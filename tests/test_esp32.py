"""ESP32 framing, command ownership, storage isolation and lifecycle tests."""
from collections import deque
from pathlib import Path
import json
import socket
import sys
import tempfile
import unittest
from unittest.mock import patch, Mock
ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'engine/project/backend'), str(ROOT / 'Sources/AzkiSimulator/Runtime')]
from src.esp32_hci import Controller, H4Stream, PROTECTED
from src.esp32_transport import SerialBridgeError, Esp32SerialTransport, BANNER
from azki_simulator import esp32, instance_storage

def cc(opcode, result=b'\0'):
    return bytes((4, 14, 3 + len(result), 1)) + opcode.to_bytes(2, 'little') + result

class Fake:
    controller_address_hint = '01:02:03:04:05:06'
    def __init__(self):
        self.sent = []; self.replies = deque()
    def send_h4(self, packet):
        self.sent.append(packet)
    def receive(self, timeout):
        return self.replies.popleft() if self.replies else None

class ESP32Tests(unittest.TestCase):
    def test_fragmented_and_coalesced_h4(self):
        stream = H4Stream()
        wire = b'\x01\x03\x0c\x00\x02\x01\x00\x03\x00abc'
        self.assertEqual(stream.feed(wire[:2]), [])
        self.assertEqual(stream.feed(wire[2:]), [wire[:4], wire[4:]])
        self.assertFalse(stream.buffer)
        with self.assertRaises(SerialBridgeError): stream.feed(b'\x04')

    def test_protected_failure_does_not_create_physical_credit(self):
        transport = Fake(); controller = Controller(transport); controller.credits = 2
        for opcode in (*PROTECTED, 0xfc01):
            controller.queue.append(b'\x01' + opcode.to_bytes(2, 'little') + b'\0')
        replies = controller.dispatch()
        self.assertEqual(len(replies), 6)
        self.assertFalse(transport.sent)
        self.assertEqual(controller.credits, 2)
        self.assertTrue(all(p[-1] == 1 for p in replies))

    def test_acl_passes_while_command_pending(self):
        transport = Fake(); controller = Controller(transport); controller.credits = 1
        first = b'\x01\x03\x0c\0'; second = b'\x01\x09\x10\0'; acl = b'\x02\0\0\x01\0x'
        controller.queue.extend([first, second, acl]); controller.dispatch()
        self.assertEqual(transport.sent, [first, acl])
        transport.replies.append(cc(0xc03)); controller.receive(); controller.dispatch()
        self.assertEqual(transport.sent[-1], second)

    def test_unowned_completion_stops_link(self):
        transport = Fake(); controller = Controller(transport)
        transport.replies.append(cc(0xc03))
        with self.assertRaises(SerialBridgeError): controller.receive()
        self.assertTrue(controller.failed)

    def test_teardown_retains_real_async_tail(self):
        transport = Fake(); controller = Controller(transport); controller.credits = 1
        disconnect = bytes.fromhex('04050400000013')
        transport.replies.extend([disconnect, cc(0xc03)])
        self.assertEqual(controller.teardown(), [disconnect.hex()])
        self.assertEqual(transport.sent, [b'\x01\x03\x0c\0'])

    def test_initialize_uses_real_address(self):
        transport = Fake(); controller = Controller(transport)
        transport.replies.extend([cc(0xc03), cc(0x1009, b'\0\x06\x05\x04\x03\x02\x01'), cc(0x1001, b'\0' * 9)])
        self.assertEqual(controller.initialize(), transport.controller_address_hint)

    def test_boot_diagnostics_bound_only_pre_start_bytes(self):
        transport = Esp32SerialTransport.__new__(Esp32SerialTransport)
        transport._buffer = bytearray()
        transport.boot_diagnostics = {'receivedBytes': 0, 'tailHex': '', 'truncated': False, 'bannerMatched': False}
        transport._serial = Mock()
        transport._serial.read.side_effect = [b'x' * 3000, BANNER]
        import time
        transport._boot_banner(time.monotonic() + 1)
        self.assertEqual(transport.boot_diagnostics['receivedBytes'], 3000 + len(BANNER))
        self.assertEqual(len(bytes.fromhex(transport.boot_diagnostics['tailHex'])), 2048)
        self.assertTrue(transport.boot_diagnostics['truncated'])
        self.assertTrue(transport.boot_diagnostics['bannerMatched'])
        transport._serial.write.assert_not_called()

    def test_existing_virtual_storage_cannot_become_physical(self):
        store = Mock()
        store.info = {'model': 'miwear.watch.q66tc'}
        store.head.return_value = (Path('/unused'), {})
        store.active = None
        firmware = Mock(model='miwear.watch.q66tc', profile='q66', nor_bytes=131072)
        plan = {'identity': {'runtimeCapabilities': {'nandPersistence': {}}}}
        with patch.object(esp32, 'read_ready', return_value={'mode': 'esp32'}), \
                patch.object(instance_storage, '_firmware', return_value=firmware), \
                patch.object(instance_storage, '_open_or_create', return_value=store), \
                patch('azki_simulator.maintenance.reset_instance'):
            with self.assertRaisesRegex(ValueError, '另一种蓝牙方式'):
                instance_storage.prepare_session_storage('/private/tmp/library', '/private/tmp/session',
                    '5a0bde5a-018d-42e1-baf5-ff1089944eda', plan, bluetooth_mode='esp32', esp32_ready_file='/unused')
        store.begin.assert_not_called()
        store.close.assert_called_once()

    def test_physical_mode_requires_storage_capability(self):
        with patch.object(esp32, 'read_ready', return_value={'mode': 'esp32'}):
            with self.assertRaisesRegex(ValueError, '独立实例存储'):
                instance_storage.prepare_session_storage('/unused', '/private/tmp/session',
                    '5a0bde5a-018d-42e1-baf5-ff1089944eda', {'identity': {}},
                    bluetooth_mode='esp32', esp32_ready_file='/unused')

    def test_native_launcher_runs_as_script_with_physical_arguments(self):
        import runpy
        module = runpy.run_path(str(ROOT / 'Sources/AzkiSimulator/Runtime/azki_simulator/minative_launch.py'),
                               run_name='native_script_probe')
        with tempfile.TemporaryDirectory(dir='/private/tmp', prefix='esp-native-') as folder:
            base = Path(folder)
            ready = base / 'ready.json'
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as endpoint:
                endpoint.bind(str(base / 'hci.sock'))
                ready.write_text(json.dumps({'schema': 1, 'mode': 'esp32', 'ready': True,
                    'port': '/dev/cu.test', 'controllerAddress': '01:02:03:04:05:06',
                    'hciSocket': str(base / 'hci.sock')}))
                run = base / 'session/m-123456789abc'
                disks = base / 'session/instance-data'
                arguments = ['--native', '180', str(run / 'runner-process.json'), '123',
                    str(base / 'engine'), str(base / 'prepared/project'), 'q66@4.100.177', str(run),
                    '--instance-data-image', str(disks / 'nand-data.bin'),
                    '--instance-spare-image', str(disks / 'nand-spare.bin'),
                    '--instance-data-sha256', 'a' * 64, '--instance-spare-sha256', 'b' * 64,
                    '--instance-nor-image', str(disks / 'nor-config.bin'), '--instance-nor-sha256', 'c' * 64,
                    '--bluetooth-mode', 'esp32', '--esp32-ready-file', str(ready), '--no-skip-oobe']
                _, _, _, command = module['native_invocation'](arguments)
                self.assertEqual(command[-5:], arguments[-5:])

    def test_closed_ready_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'ready.json'
            path.write_text(json.dumps({'ready': False}))
            with self.assertRaises(ValueError): esp32.read_ready(path)

    def test_physical_mode_rejects_virtual_key_before_storage(self):
        with patch.object(esp32, 'read_ready', return_value={'mode': 'esp32'}):
            with self.assertRaisesRegex(ValueError, '虚拟蓝牙身份'):
                instance_storage.prepare_session_storage('/unused', '/unused', 'unused', {},
                    bluetooth_mode='esp32', esp32_ready_file='/unused', virtual_key_file='/key')

if __name__ == '__main__': unittest.main()
