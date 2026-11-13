"""Native sibling helper for the single-USB ESP32 controller."""
from pathlib import Path
import contextlib
import io
import json
import os
import re
import select
import signal
import socket
import stat
import sys
import time
import uuid


def dependencies(engine):
    engine = Path(engine).resolve()
    sys.path[:0] = [str(engine / 'python/lib/python3.12/site-packages'),
                   str(engine / 'project/backend')]
    return engine


def write_json(path, value):
    path = Path(path)
    temporary = path.with_suffix('.tmp')
    with temporary.open('w') as stream:
        temporary.chmod(0o600)
        json.dump(value, stream, indent=2)
        stream.write('\n')
    temporary.replace(path)


def devices(engine):
    dependencies(engine)
    from serial.tools.list_ports import comports
    return {'ok': True, 'devices': [{'port': p.device, 'description': p.description,
        'vid': p.vid, 'pid': p.pid, 'serialNumber': p.serial_number, 'location': p.location}
        for p in comports() if p.device.startswith('/dev/cu.') and p.vid is not None]}


def selected_port(port):
    if not re.fullmatch(r'/dev/cu\.[A-Za-z0-9_.-]+', port):
        raise ValueError('Choose an attached USB serial port')
    if not stat.S_ISCHR(os.stat(port).st_mode):
        raise ValueError('Selected serial device is unavailable')
    return port


def read_ready(path):
    path = Path(path)
    value = json.loads(path.read_text())
    root = path.parent
    if (path.name != 'ready.json' or not path.is_absolute() or value.get('ready') is not True
            or value.get('schema') != 1 or value.get('mode') != 'esp32'
            or value.get('hciSocket') != str(root / 'hci.sock')
            or re.fullmatch(r'[0-9A-F]{2}(:[0-9A-F]{2}){5}', value.get('controllerAddress', '')) is None
            or not (root / 'hci.sock').is_socket()):
        raise ValueError('ESP32 controller is not ready')
    return {key: value[key] for key in ('mode', 'port', 'controllerAddress', 'hciSocket')} | {'readyFile': str(path)}


class SerialLease:
    """pySerial's exclusive POSIX descriptor lock is held for its full lifetime."""
    def __init__(self, port):
        self.instance_id = port
        self.descriptor = {'port': port, 'ownerPID': os.getpid(), 'exclusive': True}


def bridge(engine, port, state_root, expected_address=None):
    dependencies(engine)
    import serial
    from src.esp32_transport import Esp32SerialTransport
    from src.esp32_hci import Controller, H4Stream
    root = Path(state_root)
    root.mkdir(mode=0o700, exist_ok=True)
    state = {'schema': 1, 'mode': 'esp32', 'port': port, 'pid': os.getpid(),
             'ready': False, 'closed': False, 'error': None, 'cleanupErrors': [],
             'portOpened': False, 'controllerStarted': False}
    transport = controller = listener = guest = None
    stop = False
    parent = os.getppid()
    def stopped(signum, frame):
        nonlocal stop
        stop = True
    signal.signal(signal.SIGTERM, stopped)
    signal.signal(signal.SIGINT, stopped)
    stream = H4Stream()
    outbound = bytearray()
    try:
        port = selected_port(port)
        identity = {'instanceID': port, 'transport': 'esp32-uart0-vhci'}
        def serial_factory(**kw):
            handle = serial.Serial(exclusive=True, **kw)
            original_open = handle.open
            def owned_open():
                original_open()
                state['portOpened'] = True
            handle.open = owned_open
            return handle
        transport = Esp32SerialTransport(port, identity, SerialLease(port), serial_factory=serial_factory)
        state['controllerStarted'] = True
        state['bootDiagnostics'] = dict(transport.boot_diagnostics)
        controller = Controller(transport)
        address = controller.initialize()
        if expected_address and address != expected_address:
            raise ValueError('Selected controller differs from this instance')
        path = root / 'hci.sock'
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(str(path)); path.chmod(0o600); listener.listen(1); listener.setblocking(False)
        state.update(ready=True, controllerAddress=address, hciSocket=str(path),
                     statePath=str(root / 'state.json'), controllerIdentity={'path': port})
        write_json(root / 'ready.json', state)
        write_json(root / 'state.json', state)
        while not stop and os.getppid() == parent:
            if guest is None:
                readable, _, _ = select.select([listener], [], [], .01)
                if readable:
                    guest, _ = listener.accept(); guest.setblocking(False)
                    state['guestConnected'] = True
                    write_json(root / 'state.json', state)
                else:
                    packet = controller.receive(.01)
                    if packet is not None:
                        raise RuntimeError('Unexpected physical HCI before guest traffic')
                continue
            readable, writable, _ = select.select([guest], [guest] if outbound else [], [], .001)
            if readable:
                data = guest.recv(16384)
                if not data:
                    break
                controller.queue.extend(stream.feed(data))
            for packet in controller.dispatch():
                outbound.extend(packet)
            packet = controller.receive(.005)
            if packet:
                outbound.extend(packet)
            if len(outbound) > 262144:
                raise RuntimeError('Guest is not consuming real controller traffic')
            if writable and outbound:
                sent = guest.send(outbound)
                del outbound[:sent]
        state['pendingGuestBytes'] = len(stream.buffer)
        state['unsentGuestPackets'] = len(controller.queue)
        state['undeliveredControllerBytes'] = outbound.hex()
        if stream.buffer or controller.queue:
            raise RuntimeError('Guest stopped with partial or queued HCI traffic')
    except BaseException as error:
        state['error'] = str(error)
        state['openFailureCleanup'] = getattr(error, 'raw_cleanup', None)
    finally:
        if controller is not None:
            try:
                state['teardownPackets'] = controller.teardown()
                state['radioTeardownVerified'] = True
            except BaseException as error:
                state['cleanupErrors'].append(str(error))
            state.update(txPackets=controller.tx_packets, rxPackets=controller.rx_packets,
                         localRejections=controller.local_rejections)
        if transport is not None:
            try:
                transport.close()
                state['controllerClosed'] = True
            except BaseException as error:
                state['cleanupErrors'].append(str(error))
            state['serialClosed'] = transport.capabilities['closed']
            state['closePackets'] = [p.hex() for p in transport.close_packets]
        for handle in (guest, listener):
            if handle:
                handle.close()
        state.update(ready=False, closed=True)
        state['leaseReleased'] = (state.get('serialClosed') is True or not state['portOpened'] or
            bool((state.get('openFailureCleanup') or {}).get('controllerClosed')))
        state['stopConfirmed'] = state.get('controllerClosed') is True or not state['portOpened']
        state['cleanupComplete'] = state['stopConfirmed'] and state['leaseReleased'] and not state['cleanupErrors']
        state['firstError'] = state['error'] or next(iter(state['cleanupErrors']), None)
        state['message'] = state['firstError']
        write_json(root / 'closed.json', state)
        write_json(root / 'state.json', state)
        if (root / 'ready.json').exists():
            write_json(root / 'ready.json', state)
    return {'ok': not state['error'] and not state['cleanupErrors'], **state}


def flash(engine, port, backup_root, operation, backup=None):
    engine = dependencies(engine)
    import hashlib
    import serial
    import esptool.loader
    from esptool.targets.esp32 import ESP32ROM
    from esptool.cmds import attach_flash, write_flash, run_stub
    port = selected_port(port)
    root = Path(backup_root) / (time.strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:8])
    root.mkdir(mode=0o700, parents=True)
    report = {'operation': operation, 'port': port, 'error': None, 'cleanupErrors': [],
              'writeStarted': False, 'writesVerified': False, 'backupVerified': False,
              'receipt': str(root / 'receipt.json')}
    handle = esp = None
    # esptool's progress belongs in a local log; stdout remains one JSON result.
    with (root / 'rom.log').open('w') as log, contextlib.redirect_stdout(log):
        try:
            handle = serial.Serial(port, 115200, timeout=3, write_timeout=10, exclusive=True)
            esp = ESP32ROM(handle, 115200)
            esp.connect('default-reset', attempts=1)
            if esp.IS_STUB or esp.read_reg(esp.CHIP_DETECT_MAGIC_REG_ADDR) != esp.MAGIC_VALUE:
                raise RuntimeError('This firmware requires the original ESP32 chip')
            report['chipDescription'] = esp.get_chip_description()
            report['baseMac'] = ':'.join(f'{x:02X}' for x in esp.read_mac())
            if esp.get_flash_encryption_enabled() or esp.get_secure_boot_enabled():
                raise RuntimeError('Secure boot or encrypted flash is enabled')
            attach_flash(esp)
            report['flashID'] = esp.flash_id()
            size = 1 << (report['flashID'] >> 16 & 255)
            if size != 4194304:
                raise RuntimeError('The included controller firmware requires 4 MB flash')
            esp.flash_set_parameters(size)
            esp = run_stub(esp)
            if not esp.IS_STUB:
                raise RuntimeError('Official ESP32 RAM loader did not start')
            report.update(stubUploaded=True, baudrate=115200)
            esp.flash_set_parameters(size)
            restore = None
            if operation == 'restore':
                source = Path(backup)
                saved = json.loads((source.parent / 'receipt.json').read_text())
                restore = source.read_bytes()
                if (not saved.get('backupVerified') or saved['baseMac'] != report['baseMac']
                        or saved['flashID'] != report['flashID'] or len(restore) != size
                        or hashlib.sha256(restore).hexdigest() != saved['backupSHA256']):
                    raise RuntimeError('Backup does not match this board or is incomplete')
            target = root / 'flash-backup.bin'
            digest = hashlib.md5(); sha = hashlib.sha256()
            report['bytesSaved'] = 0
            with target.open('xb') as output:
                target.chmod(0o600)
                for offset in range(0, size, 65536):
                    data = esp.read_flash(offset, 65536)
                    if len(data) != 65536:
                        raise RuntimeError('Incomplete device flash read')
                    output.write(data); digest.update(data); sha.update(data)
                    report['bytesSaved'] += len(data)
                    write_json(root / 'receipt.json', report)
            if digest.hexdigest() != esp.flash_md5sum(0, size).lower():
                raise RuntimeError('Backup differs from device flash')
            report.update(backupVerified=True, backup=str(target), backupSHA256=sha.hexdigest())
            write_json(root / 'receipt.json', report)
            if operation != 'backup':
                images = [(0, restore)] if restore is not None else [
                    (offset, (engine / 'project/backend/esp32-controller/images' / name).read_bytes())
                    for offset, name in ((0x1000, 'bootloader.bin'), (0x8000, 'partition-table.bin'),
                                         (0x10000, 'azki_vhci_serial.bin'))]
                esptool.loader.WRITE_BLOCK_ATTEMPTS = 1
                esp.WRITE_FLASH_ATTEMPTS = 1
                report['writeStarted'] = True
                write_json(root / 'receipt.json', report)
                write_flash(esp, images, flash_freq='keep', flash_mode='keep', flash_size='keep',
                            erase_all=False, force=False, encrypt=False, no_compress=True, no_progress=True)
                report['writesVerified'] = True
        except BaseException as error:
            report['error'] = str(error)
        finally:
            if esp and (not report['writeStarted'] or report['writesVerified']):
                try:
                    esp.hard_reset(); report['ordinaryBootResetSent'] = True
                except BaseException as error:
                    report['cleanupErrors'].append(str(error))
            if handle:
                try:
                    handle.close(); report['serialClosed'] = not handle.is_open
                except BaseException as error:
                    report['cleanupErrors'].append(str(error))
    report['ok'] = not report['error'] and not report['cleanupErrors'] and report['backupVerified']
    write_json(root / 'receipt.json', report)
    return report
