"""Isolated BEST1502P launch recipe using only the selected Q65 OTA."""
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time

from .control import WatchControl
from .qmp import QMP
from .q65_factory import local_desktop_image

FLAGS = ('bt-peer', 'bt-tx-capture', 'bt-hci-reset-fixture',
         'panel-id-fixture', 'lcd-frame-capture', 'panel-te-fixture',
         'spi0-unconnected', 'gpu-probe', 'gpu-id-fixture',
         'gpu-software-renderer', 'touch', 'power-adc-fixture',
         'bt-hci-reject-fixture')

def scb_fault(dump):
    words = {}
    for line in (dump or '').splitlines():
        match = re.match(r'\s*(?:0x)?([0-9a-fA-F]+):\s*(.*)', line)
        if match:
            for index, value in enumerate(re.findall(r'0x([0-9a-fA-F]{8})', match[2])):
                words[int(match[1], 16) + index * 4] = int(value, 16)
    return words.get(0xe000ed28, 0) or ((words.get(0xe000ed04, 0) & 511) in (3, 4, 5, 6))

def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')
    path.chmod(0o600)


def board_profile(profile):
    """Return the exact Q65 storage contract derived from its own AP.

    Q65 is a BEST1502P product and its NOR layout is not interchangeable
    with the BES1503 band layouts. The descriptor is explicit so instance
    preparation binds private images to this exact AP/profile.
    """
    from device_catalog import inspect_ap
    from multi_device import partition_records
    from .instance_data import validate_nor_geometry
    # Imported records from older apps lack BEST1502P header fields. Reparse
    # the selected AP rather than rewriting the user's firmware library.
    ap = inspect_ap(profile['ap_path'])
    if (profile.get('model') != 'miwear.watch.q65'
            or ap.get('chip') != 'best1502p'
            or ap.get('sha256') != profile['ap']['sha256']
            or ap.get('size') != profile['ap']['size']
            or ap.get('flash_bytes') != 0x1000000
            or ap.get('ap_offset') != 0xb0000):
        raise ValueError('Q65 requires its exact BEST1502P AP layout')
    ap_bytes = ap.get('size')
    if type(ap_bytes) is not int or ap_bytes <= 0 or ap['ap_offset'] + ap_bytes > 0xfc0000:
        raise ValueError('Q65 AP overlaps its writable NOR tail')
    geometry = {
        'ap-offset': 0xb0000, 'reset-sp': ap.get('reset_sp'),
        'flash-bytes': 0x1000000, 'nv-offset': 0xfc0000,
        'factory-offset': 0xfe0000, 'nand-data-offset': 240 * 1024**2,
        'nand-data-bytes': 232 * 1024**2, 'nand-audio-offset': 235 * 1024**2,
        'ram-bytes': 0x1a0000,
    }
    if type(geometry['reset-sp']) is not int or not 0x20000000 <= geometry['reset-sp'] <= 0x201a0000:
        raise ValueError('Q65 AP reset stack pointer is outside SRAM')
    records = partition_records(Path(profile['ap_path']).read_bytes())
    required = {('nv', 32256, 128), ('nvflash0', 32384, 128),
                ('misc', 32512, 256), ('resource', 0, 120320),
                ('audio_img', 120320, 2560), ('nand_data', 122880, 118784)}
    if not required <= {(r['name'], r['start'], r['count']) for r in records}:
        raise ValueError('Q65 AP partition table differs from the implemented 512-MiB board')
    descriptor = {
        'profileID': profile['id'], 'apSHA256': ap['sha256'],
        'apBytes': ap_bytes, 'apOffset': 0xb0000,
        'flashBytes': 0x1000000, 'offset': 0xfc0000,
        'bytes': 0x40000, 'factoryOffset': 0xfe0000,
    }
    board = {
        'profile_id': profile['id'], 'chip': 'best1502p',
        'machine': 'azki-q65', 'ap_sha256': ap['sha256'],
        'local_setup_bypass': True, 'geometry': geometry,
        'nor_geometry': descriptor, 'factory_bytes': 128 * 1024,
        'evidence': {'source': 'Q65 AP partition table: nv/nvflash0/misc/nand_data'},
    }
    validate_nor_geometry(board, ap_sha256=ap['sha256'], ap_bytes=ap_bytes)
    return board


def prepare_instance_storage(board, args, virtual_state):
    """Validate the private transaction and seed only a fresh Q65 misc image."""
    from .instance_data import (validate_instance_images, validate_instance_nor,
                                initialize_instance_nor)
    storage = nor = None
    if any((args.instance_data_image, args.instance_spare_image,
            args.instance_data_sha256, args.instance_spare_sha256)):
        storage = validate_instance_images(args.instance_data_image, args.instance_spare_image,
            args.instance_data_sha256, args.instance_spare_sha256, board['geometry'])
        board['instance_storage'] = storage
    if args.instance_nor_image or args.instance_nor_sha256 or args.instance_nor_initialize:
        nor = validate_instance_nor(args.instance_nor_image, args.instance_nor_sha256,
                                    args.instance_nor_initialize, board, storage)
        board['instance_nor'] = nor
    if virtual_state is not None:
        if (nor is None or Path(args.virtual_peer_state).parent != Path(storage['dataImage']).parent):
            raise ValueError('Q65 virtual peer requires the same private NAND/NOR transaction')
    if nor is not None and nor['norInitialize']:
        if virtual_state is not None:
            from .virtual_peer.state import factory_image
            initialize_instance_nor(nor, factory_image(virtual_state, skip_oobe=args.skip_oobe))
        elif args.skip_oobe:
            initialize_instance_nor(nor, local_desktop_image())
    # Existing NOR and guest-confirmed pairing are never reseeded.
    return storage, nor

def run(profile, args):
    from device_catalog import ROOT
    logdir = (args.log_dir or ROOT / 'results/devices' / profile['id']).resolve()
    logdir.mkdir(mode=0o700, parents=True, exist_ok=True)
    try:
        return _run(profile, args, logdir)
    except (OSError, ValueError, RuntimeError) as problem:
        # Preparation failures must also publish a useful startup result.
        result = dict(outcome='host_error', returncode=None, qmp_error=str(problem),
                      storage_writeback_error=None, diagnostic_errors=[str(problem)])
        write_json(logdir / 'result.json', result)
        print(json.dumps(result), flush=True)
        return 2


def _run(profile, args, logdir):
    from device_catalog import ROOT
    from firmware_resources import checked_member
    from .instance_data import (instance_machine_options, instance_nor_machine_options,
                                instance_nor_result_geometry)
    if profile['model'] != 'miwear.watch.q65' or profile['ap']['chip'] != 'best1502p':
        raise ValueError('Q65 requires its exact BEST1502P profile')
    if args.gdb is not None or args.instance_settings:
        raise ValueError('Q65 debugger/instance settings are not implemented')
    virtual_state = peer = None
    if args.virtual_peer_state:
        from .virtual_peer.state import load_state
        virtual_state = load_state(args.virtual_peer_state, args.virtual_peer_state_sha256)
        if (virtual_state['profileID'] != profile['id']
                or virtual_state['apSHA256'] != profile['ap']['sha256']
                or Path(args.virtual_peer_output) != logdir / 'virtual-peer-state.pending.json'):
            raise ValueError('Q65 private peer identity differs from selected AP/run')
    board = board_profile(profile)
    board['local_setup_bypass'] = bool(args.skip_oobe)
    board['skip_oobe_requested'] = bool(args.skip_oobe)
    instance_storage, instance_nor = prepare_instance_storage(board, args, virtual_state)
    package = Path(profile['ota_path']).read_bytes()
    images = {}
    limits = {'vela_resource.bin': 235 * 1024**2,
              'vela_audio_img.bin': 5 * 1024**2, 'vela_sensor.bin': 4 * 1024**2}
    for name, limit in limits.items():
        records = [row for row in profile['partitions'] if row['name'] == name]
        if len(records) != 1 or not 0 < records[0]['size'] <= limit:
            raise ValueError('Q65 requires one bounded ' + name)
        data = checked_member(package, records[0])
        target = logdir / name
        target.write_bytes(data)
        target.chmod(0o600)
        images[name] = target
    del package
    identity = dict(profile_id=profile['id'], ap_sha256=profile['ap']['sha256'],
        instance_data_sha256=args.instance_data_sha256,
        instance_spare_sha256=args.instance_spare_sha256)
    if instance_nor is not None:
        identity.update(instance_nor_sha256=args.instance_nor_sha256,
                        instance_nor_initialize=args.instance_nor_initialize)
    if virtual_state is not None:
        board['virtual_peer_state_sha256'] = args.virtual_peer_state_sha256
        identity['virtual_peer_state_sha256'] = args.virtual_peer_state_sha256
    write_json(logdir / 'board.json', board)
    write_json(logdir / 'identity.json', identity)
    frame = logdir / 'lcd-frame.raw'
    flags = tuple(flag for flag in FLAGS if virtual_state is None or
                  flag not in ('bt-hci-reset-fixture', 'bt-hci-reject-fixture'))
    machine = 'azki-q65,revision-id=5,' + ','.join(flag + '=on' for flag in flags)
    for option, name in [('resource-image', 'vela_resource.bin'),
                         ('audio-image', 'vela_audio_img.bin'), ('sensor-image', 'vela_sensor.bin')]:
        machine += ',' + option + '=' + str(images[name]).replace(',', ',,')
    if instance_storage is not None:
        machine += instance_machine_options(instance_storage)
    if instance_nor is not None:
        machine += instance_nor_machine_options(instance_nor)
    elif args.skip_oobe:
        factory = logdir / 'desktop-factory.bin'
        factory.write_bytes(local_desktop_image())
        factory.chmod(0o600)
        machine += ',factory-image=' + str(factory).replace(',', ',,')
    machine += ',lcd-frame-file=' + str(frame).replace(',', ',,')
    qemu = ROOT / 'backend/build/qemu-system-arm'
    started = time.monotonic()
    error = None
    interrupted = timed_out = False
    def request_stop(*_):
        nonlocal interrupted
        interrupted = True
    # Complete QMP exchanges before shutdown; interrupted reads leave replies.
    signal.signal(signal.SIGINT, request_stop)
    scb = sensor_scb = ''
    with tempfile.TemporaryDirectory(prefix='q66-qmp-') as temporary:
        if virtual_state is not None:
            from .virtual_peer.service import PeerService
            peer = PeerService(temporary, logdir, virtual_state,
                args.virtual_peer_output, args.virtual_peer_state_sha256).start()
            machine += ',bt-hci-chardev=radio'
        qmp_path = Path(temporary) / 'qmp.sock'
        command = [str(qemu), '-accel', 'tcg,thread=multi', '-smp', '2',
            '-M', machine, '-display', 'none', '-monitor', 'none', '-no-reboot',
            '-kernel', profile['ap_path'], '-qmp', f'unix:{qmp_path},server=on,wait=off',
            '-d', 'guest_errors', '-D', str(logdir / 'qemu.log')]
        for index in range(3):
            command += ['-serial', f'file:{logdir / ("uart" + str(index) + ".log")}']
        if peer is not None:
            command += ['-chardev', f'socket,id=radio,path={peer.hci_path}']
        write_json(logdir / 'launch.json', {'command': command})
        control = connection = None
        with (logdir / 'stderr.log').open('w') as stderr:
            proc = subprocess.Popen(command, stdout=stderr, stderr=stderr, umask=0o077)
            if peer is not None:
                peer.attach_core(proc.pid)
            try:
                while not qmp_path.exists():
                    if proc.poll() is not None or time.monotonic() - started > 10:
                        raise RuntimeError('Q65 QEMU control socket did not start; see stderr.log')
                    time.sleep(.02)
                os.chmod(qmp_path, 0o600)
                connection = QMP(qmp_path)
                control = WatchControl(connection, temporary, log_directory=logdir, frame_file=frame)
                input_path = Path(temporary) / 'input.sock'
                control.serve_input(input_path)
                write_json(logdir / 'control.json', dict(qmp=str(qmp_path), framebuffer=str(frame),
                           pid=proc.pid, input_socket=str(input_path),
                           **({'virtual_peer': peer.descriptor()} if peer is not None else {})))
                print('Q65 BEST1502P running with AP/resource/audio/sensor from selected OTA', flush=True)
                while proc.poll() is None and not interrupted:
                    if peer is not None and peer.failure:
                        raise RuntimeError('Q65 virtual peer failed: ' + peer.failure)
                    if args.seconds and time.monotonic() - started >= args.seconds:
                        timed_out = True
                        break
                    scb = control.execute('human-monitor-command',
                        {'command-line': 'xp /11wx 0xe000ed00', 'cpu-index': 0})
                    if scb_fault(scb):
                        break
                    time.sleep(1)
            except KeyboardInterrupt:
                interrupted = True
            except (OSError, ValueError, RuntimeError, ConnectionError) as problem:
                error = str(problem)
            finally:
                # Stop may be requested by both frame and lifecycle workers.
                # Do not interrupt state capture/result publication a second time.
                signal.signal(signal.SIGINT, signal.SIG_IGN)
                if peer is not None:
                    peer.close()
                if control is not None:
                    control.close()
                    write_json(logdir / 'touch-input.json', control.events)
                if connection is not None and proc.poll() is None:
                    try:
                        connection.execute('stop')
                        for name, cmd, cpu in [('registers.txt', 'info registers', 0),
                                ('scb.txt', 'xp /32wx 0xe000ed00', 0),
                                ('sensor-registers.txt', 'info registers', 1),
                                ('sensor-scb.txt', 'x /32wx 0xe000ed00', 1)]:
                            output = connection.execute('human-monitor-command', {'command-line': cmd, 'cpu-index': cpu})
                            (logdir / name).write_text(output)
                            if name == 'scb.txt': scb = output
                            if name == 'sensor-scb.txt': sensor_scb = output
                        connection.execute('quit')
                    except (OSError, ValueError, RuntimeError, ConnectionError) as problem:
                        error = error or str(problem)
                if connection is not None:
                    connection.close()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.terminate()
                    try: proc.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()
                (logdir / 'control.json').unlink(missing_ok=True)
    uart = '\n'.join(path.read_text(errors='replace') for path in logdir.glob('uart*.log'))
    trace = (logdir / 'qemu.log').read_text(errors='replace') if (logdir / 'qemu.log').exists() else ''
    faults = re.findall(r'fault address (0x[0-9a-fA-F]+)', trace)
    faults += [value for value in re.findall(r'BFAR:\s*([0-9a-fA-F]{8})', uart) if int(value, 16)]
    cfsr = scb_fault(scb) or scb_fault(sensor_scb)
    outcome = ('host_error' if error or proc.returncode else 'guest_fault' if faults or cfsr
               else 'interrupted' if interrupted else 'time_limit' if timed_out else 'qemu_stopped')
    stderr = (logdir / 'stderr.log').read_text(errors='replace')
    storage_error = next((line[:512] for line in stderr.splitlines()
                          if 'Q65 storage writeback failed:' in line), None)
    result = dict(outcome=outcome, returncode=proc.returncode, qmp_error=error,
        fault_addresses=faults, cortex_fault_status=cfsr,
        ap_cortex_fault_status=scb_fault(scb), sensor_cortex_fault_status=scb_fault(sensor_scb),
        data_image=instance_storage['dataImage'] if instance_storage else None,
        data_spare_image=instance_storage['spareImage'] if instance_storage else None,
        data_writeback=instance_storage is not None,
        instance_data_sha256=args.instance_data_sha256,
        instance_spare_sha256=args.instance_spare_sha256,
        nor_image=instance_nor['norImage'] if instance_nor else None,
        nor_writeback=instance_nor is not None,
        nor_initialized=args.instance_nor_initialize if instance_nor else False,
        instance_nor_sha256=args.instance_nor_sha256,
        storage_writeback_error=storage_error, **instance_nor_result_geometry(instance_nor),
        virtual_peer_state=peer.state_receipt() if peer is not None else None,
        virtual_peer_error=peer.failure if peer is not None else None,
        time_limit_reached=timed_out, runtime_seconds=time.monotonic() - started,
        firmware_startup_completed='tag=booting-completed' in uart,
        lvgl_started='lvgl vsync start OK' in uart,
        gpu_render_rejection_count=trace.count('BES GPU render rejected:'),
        display_frame_count=trace.count('BES experimental LCD frame captured:'))
    write_json(logdir / 'result.json', result)
    print(json.dumps(result), flush=True)
    return 0 if outcome in ('interrupted', 'time_limit', 'qemu_stopped') else 2
