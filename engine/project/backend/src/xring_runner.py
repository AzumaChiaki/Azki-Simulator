"""Run the original XRing TEE/AP with instance-owned eMMC and shared display."""
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import threading
import time

from .control import WatchControl
from .qmp import QMP
from .xring_media import EMMC_BYTES, board_profile, extract_boot, initialize_media, machine_options


class XRingControl(WatchControl):
    """Deliver the final held coordinate before releasing the CST9220 contact."""
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.touch_lock = threading.Lock()
        self.last_contact = None
        self.last_touch_time = 0.0

    def touch(self, x, y, down):
        if type(x) is not int or type(y) is not int or type(down) is not bool:
            raise ValueError('x/y must be integers and down must be boolean')
        width, height = self.frame_geometry()
        if not 0 <= x < width or not 0 <= y < height:
            raise ValueError(f'Touch coordinates must be inside {width} x {height}')
        with self.touch_lock:
            reports = [(x, y, True), (x, y, False)] if not down and self.last_contact is not None and self.last_contact != (x,y) else [(x,y,down)]
            for px, py, held in reports:
                time.sleep(max(0.0, .04 - (time.monotonic()-self.last_touch_time)))
                super().touch(px, py, held)
                self.last_touch_time = time.monotonic()
                self.last_contact = (px,py) if held else None


def _json(path, value):
    path.write_text(json.dumps(value, indent=2)+'\n')
    path.chmod(0o600)


def _tail(path):
    with path.open('rb') as stream:
        stream.seek(0, 2)
        stream.seek(max(0, stream.tell()-65536))
        return stream.read().decode(errors='replace')


def run(profile, args):
    from device_catalog import ROOT
    logdir = (args.log_dir or ROOT/'results/devices'/profile['id']).resolve()
    logdir.mkdir(mode=0o700, parents=True, exist_ok=True)
    try:
        return _run(profile, args, logdir)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        result = {'outcome':'host_error', 'returncode':None, 'qmp_error':str(error),
                  'emmc_writeback':False, 'storage_writeback_error':str(error)}
        _json(logdir/'result.json', result)
        print(json.dumps(result), flush=True)
        return 2


def _run(profile, args, logdir):
    from device_catalog import ROOT
    board = board_profile(profile)
    if args.virtual_peer_state or args.bluetooth_mode == 'esp32':
        raise ValueError('XRing Bluetooth transport is not implemented')
    if args.trace:
        raise ValueError('XRing instruction traces are disabled for the secure firmware')
    if args.gdb is not None:
        raise ValueError('XRing secure-firmware debugger export is not enabled')
    if any((args.instance_data_image, args.instance_spare_image, args.instance_nor_image)):
        raise ValueError('XRing uses an eMMC instance, not BES NAND/NOR images')
    if args.instance_settings:
        raise ValueError('XRing live instance settings are not implemented')
    if args.seconds is not None and args.seconds <= 0:
        raise ValueError('Run duration must be positive')
    persistent = args.instance_emmc_image is not None
    if args.instance_emmc_initialize and not persistent:
        raise ValueError('eMMC initialization requires its instance image')
    media = Path(args.instance_emmc_image).resolve() if persistent else None
    if persistent and (not media.is_file() or media.stat().st_size != EMMC_BYTES):
        raise ValueError('XRing requires a 4-GiB instance eMMC transaction')
    if persistent and media.is_relative_to(logdir):
        raise ValueError('Instance eMMC belongs outside the run log directory')
    board.update(skip_oobe_requested=bool(args.skip_oobe), emmc_initialize=bool(args.instance_emmc_initialize))
    if persistent:
        board['instance_storage'] = {'kind':'xring-emmc', 'emmcImage':str(media),
            'emmcBytes':EMMC_BYTES, 'emmcInitialize':bool(args.instance_emmc_initialize)}
    _json(logdir/'board.json', board)
    _json(logdir/'identity.json', {'profile_id':profile['id'], 'ap_sha256':profile['ap']['sha256'],
                                  'ota_sha256':profile.get('ota_sha256'), 'firmware_modified':False,
                                  'emmc_image':str(media) if persistent else None,
                                  'emmc_initialize':bool(args.instance_emmc_initialize)})
    images = extract_boot(profile, logdir)
    frame = logdir/'lcd-frame.raw'
    interrupted = timed_out = False
    error = None
    control = connection = proc = None
    frame_count = 0
    started = time.monotonic()
    def request_stop(*_):
        nonlocal interrupted
        interrupted = True
    previous_signals = {sig:signal.signal(sig, request_stop) for sig in (signal.SIGINT,signal.SIGTERM)}
    try:
        with tempfile.TemporaryDirectory(prefix='q66-qmp-') as temporary:
            directory = Path(temporary)
            if media is None:
                media = directory/'emmc.img'
            if not persistent or args.instance_emmc_initialize:
                initialize_media(profile, media, directory, skip_oobe=args.skip_oobe)
            machine = machine_options(images, media, directory, frame, writeback=persistent)
            qmp_path = directory/'qmp.sock'
            stderr_path = logdir/'stderr.log'
            command = [str(ROOT/'backend/build/qemu-system-arm'), '-M',machine,
                       '-smp','3','-accel','tcg,thread=single','-display','none','-monitor','none',
                       '-serial','stdio','-nic','none','-no-reboot','-kernel',str(images['vela_tee.bin']),
                       '-qmp',f'unix:{qmp_path},server=on,wait=off']
            _json(logdir/'launch.json', {'command':command})
            with stderr_path.open('wb') as stderr:
                proc = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stderr, stderr=stderr, umask=0o077)
                try:
                    deadline = time.monotonic()+60
                    while not qmp_path.exists():
                        if proc.poll() is not None or time.monotonic() >= deadline or interrupted:
                            raise RuntimeError('XRing core control socket did not start; see stderr.log')
                        time.sleep(.02)
                    qmp_path.chmod(0o600)
                    connection = QMP(qmp_path, timeout=2)
                    control = XRingControl(connection, directory, log_directory=logdir, frame_file=frame)
                    input_path = directory/'input.sock'
                    control.serve_input(input_path)
                    descriptor = {'qmp':str(qmp_path), 'framebuffer':str(frame), 'pid':proc.pid,
                                  'input_socket':str(input_path)}
                    if not args.headless:
                        descriptor['url'] = control.serve(0)
                    _json(logdir/'control.json', descriptor)
                    print('XRing O62M running with the selected original TEE/AP and eMMC', flush=True)
                    while proc.poll() is None and not interrupted:
                        if args.seconds is not None and time.monotonic()-started >= args.seconds:
                            timed_out = True
                            break
                        sizes = [p.stat().st_size for p in logdir.iterdir() if p.is_file()]
                        if max(sizes, default=0) > 512*1024**2 or sum(sizes) > 1024**3:
                            raise RuntimeError('XRing run output quota reached')
                        time.sleep(.1)
                except (OSError, ValueError, RuntimeError, ConnectionError) as problem:
                    error = str(problem)
                finally:
                    if control is not None:
                        try:
                            control.close()
                            if frame.exists():
                                control.save_frame(logdir/'lcd-frame.png')
                                frame_count = (control.frame_sequence or 0) // 2
                            _json(logdir/'touch-input.json', control.events)
                        except (OSError, ValueError, RuntimeError, ConnectionError) as problem:
                            error = error or str(problem)
                    if connection is not None and proc.poll() is None:
                        try:
                            connection.execute('quit')
                        except (OSError, ValueError, RuntimeError, ConnectionError) as problem:
                            error = error or str(problem)
                    if connection is not None:
                        connection.close()
                    try:
                        proc.wait(timeout=30)
                    except subprocess.TimeoutExpired:
                        proc.terminate()
                        try:
                            proc.wait(timeout=3)
                        except subprocess.TimeoutExpired:
                            proc.kill()
                            proc.wait()
                        error = error or 'XRing core did not shut down cleanly'
                    (logdir/'control.json').unlink(missing_ok=True)
            tail = _tail(stderr_path)
            completed = bool(re.search(r'XRING_EMMC_WRITEBACK completed=1 dirty-sectors=\d+', tail))
            storage_error = None
            if persistent and (proc.returncode != 0 or not completed):
                storage_error = 'XRing eMMC writeback did not complete'
            outcome = ('host_error' if error else 'guest_fault' if proc.returncode else
                       'interrupted' if interrupted else 'time_limit' if timed_out else 'qemu_stopped')
            result = {'outcome':outcome, 'returncode':proc.returncode, 'qmp_error':error,
                      'runtime_seconds':time.monotonic()-started, 'time_limit_reached':timed_out,
                      'display_frame_count':frame_count, 'display_frame_output':frame_count > 0,
                      'display_framebuffer':str(frame), 'firmware_modified':False,
                      'emmc_image':str(media) if persistent else None, 'emmc_bytes':EMMC_BYTES,
                      'emmc_initialized':bool(args.instance_emmc_initialize),
                      'emmc_writeback':persistent and proc.returncode == 0 and completed,
                      'storage_writeback_error':storage_error}
            _json(logdir/'result.json', result)
            print(json.dumps(result), flush=True)
            return 0 if outcome in ('interrupted','time_limit','qemu_stopped') and not storage_error else 2
    finally:
        for sig, handler in previous_signals.items():
            signal.signal(sig, handler)
