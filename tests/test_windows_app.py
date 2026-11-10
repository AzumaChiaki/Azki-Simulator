"""Desktop-client integration and panel-coordinate regression checks."""
import base64
from contextlib import ExitStack
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest

APP = Path(__file__).resolve().parents[1] / 'engine/windows-app'
sys.path.insert(0, str(APP))
from frames import decode_frame, fit_panel, panel_point
from rpc import RuntimeClient, RuntimeErrorResponse
from bluetooth_panel import BluetoothPanel
from app import default_layout, launch_arguments
from sensors_panel import integer_value, pose_value


class SensorInputTests(unittest.TestCase):
    def test_adc_and_integer_fields_keep_full_range(self):
        self.assertEqual(integer_value('65535', 0, 65535), 65535)
        self.assertEqual(integer_value('0', 0, 65535), 0)
        for value in ('-1', '65536', '1.5', 'nan'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                integer_value(value, 0, 65535)

    def test_body_pose_rejects_nonfinite_or_outside_range(self):
        self.assertEqual(pose_value('-180', '90.5'), {'rollDegrees': -180.0, 'pitchDegrees': 90.5})
        for roll, pitch in (('nan', '0'), ('0', 'inf'), ('181', '0'), ('0', '-181')):
            with self.subTest(pose=(roll, pitch)), self.assertRaises(ValueError):
                pose_value(roll, pitch)


class LaunchTests(unittest.TestCase):
    def test_development_layout(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            project, core = default_layout(root / 'engine/windows-app/app.py')
            self.assertEqual(project, root / 'engine/project')
            self.assertEqual(core, root / 'dist/windows/qemu-system-arm.exe')

    def test_portable_layout(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            (root / 'python').mkdir()
            project, core = default_layout(root / 'windows-app/app.py')
            self.assertEqual(project, root / 'project')
            self.assertEqual(core, root / 'qemu-system-arm.exe')

    def test_usb_mode_preserves_explicit_interface(self):
        path = r'\\?\usb#vid_0a12&pid_0001#instance#{a4b85fea-0ae3-4be1-b238-9dca9ed299a8}'
        result = launch_arguments('hardware:miwear.watch.q65', {'firmwarePath': ' C:/watch.bin '}, False, 'usb', path)
        self.assertEqual(result['hciDevicePath'], path)
        self.assertEqual(result['firmwarePath'], 'C:/watch.bin')
        self.assertEqual(result['bluetooth'], 'usb')
        self.assertFalse(result['skipOOBE'])

    def test_missing_usb_cannot_silently_fallback(self):
        with self.assertRaises(ValueError):
            launch_arguments('q66', {}, True, 'usb')
        with self.assertRaises(ValueError):
            launch_arguments('q66', {}, True, 'serial')

    def test_host_mode_does_not_leak_stale_usb_selection(self):
        result = launch_arguments('q66', {'firmwarePath': 'a.bin'}, True, 'winrt', 'stale-usb-path')
        self.assertNotIn('hciDevicePath', result)

    def test_saved_instance_does_not_reseed_from_current_form(self):
        result = launch_arguments('different-profile', {'firmwarePath': 'other.bin'}, False,
                                  'winrt', instance_id='saved-instance', initialized=True)
        self.assertEqual(result, {'instanceID': 'saved-instance', 'bluetooth': 'winrt'})

    def test_first_instance_prepare_retains_initial_setup_choice(self):
        result = launch_arguments('q66', {'firmwarePath': 'new.bin'}, False,
                                  'reject', instance_id='new-instance', initialized=False)
        self.assertEqual(result['instanceID'], 'new-instance')
        self.assertEqual(result['firmwarePath'], 'new.bin')
        self.assertFalse(result['skipOOBE'])


class FrameTests(unittest.TestCase):
    def test_rgb565_panel_and_capture_colors(self):
        frame = decode_frame(dict(sequence=2, width=2, height=2, pixelFormat='rgb565le',
                                  pixelsBase64=base64.b64encode(bytes.fromhex('00f8e0071f00ffff')).decode()))
        self.assertEqual(list(frame.image().getdata()), [(248, 0, 0), (0, 252, 0), (0, 0, 248), (248, 252, 248)])

    def test_stride_and_rgb888(self):
        frame = decode_frame(dict(sequence=0, width=1, height=2, stride=4, pixelFormat='rgb888',
                                  pixelsBase64=base64.b64encode(bytes.fromhex('ff00000000ff0000')).decode()))
        self.assertEqual(list(frame.image().getdata()), [(255, 0, 0), (0, 255, 0)])

    def test_geometry_cannot_expand_frame_payload(self):
        value = dict(sequence=2, width=2, height=2, pixelFormat='rgb888', pixelsBase64='AAAA')
        for changes in ({}, {'width': True}, {'height': 2048}, {'stride': -1},
                        {'pixelFormat': 'rgba'}, {'pixelsBase64': '%%%'}, {'sequence': -1}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                decode_frame(dict(value, **changes))

    def test_letterbox_and_release_map_for_all_panel_shapes(self):
        for width, height in ((192, 490), (212, 520), (336, 480), (432, 514), (466, 466)):
            rect = fit_panel(width, height, 600, 700)
            x, y, w, h = rect
            with self.subTest(panel=(width, height)):
                self.assertEqual(panel_point(x + w / 2, y + h / 2, rect, width, height), (width // 2, height // 2))
                self.assertIsNone(panel_point(x - 1, y, rect, width, height))
                self.assertEqual(panel_point(x + w + 100, y + h + 100, rect, width, height, clamp=True), (width - 1, height - 1))

    def test_bluetooth_hex_input_is_exact_bytes(self):
        self.assertEqual(BluetoothPanel.hex_value('41 5a\n4B49'), '415a4b49')
        for invalid in ('1', 'xx', '0x41', 'aa' * 4097):
            with self.subTest(invalid=invalid[:30]), self.assertRaises(ValueError):
                BluetoothPanel.hex_value(invalid)


class RuntimeClientTests(unittest.TestCase):
    def service(self, directory, body):
        path = Path(directory) / 'service.py'
        path.write_text(body)
        client = RuntimeClient([sys.executable, '-u', str(path)], cwd=directory, log_file=subprocess.DEVNULL)
        self.addCleanup(client.close)
        return client

    def test_jsonl_roundtrip_and_owned_eof_cleanup(self):
        with tempfile.TemporaryDirectory() as directory, ExitStack() as stack:
            client = self.service(directory, '''import json,sys
for line in sys.stdin:
    v=json.loads(line)
    print(json.dumps({'id':v['id'],'ok':True,'result':{'command':v['command'],'args':v['args']}}),flush=True)
''')
            stack.callback(client.close)
            result = client.request('touch', dict(x=191, y=489, down=False))
            self.assertEqual(result, dict(command='touch', args=dict(x=191, y=489, down=False)))
            client.close()
            self.assertIsNotNone(client.process.poll())

    def test_server_error_does_not_corrupt_transport(self):
        with tempfile.TemporaryDirectory() as directory, ExitStack() as stack:
            client = self.service(directory, '''import json,sys
for line in sys.stdin:
    v=json.loads(line)
    if v['command']=='bad': out={'id':v['id'],'ok':False,'error':{'code':'bad_input','message':'unsupported'}}
    else: out={'id':v['id'],'ok':True,'result':{'running':False}}
    print(json.dumps(out),flush=True)
''')
            stack.callback(client.close)
            with self.assertRaises(RuntimeErrorResponse) as error:
                client.request('bad')
            self.assertEqual(error.exception.code, 'bad_input')
            self.assertEqual(client.request('status'), {'running': False})

    def test_mismatched_response_closes_only_owned_service(self):
        with tempfile.TemporaryDirectory() as directory, ExitStack() as stack:
            client = self.service(directory, '''import json,sys
for line in sys.stdin:
    v=json.loads(line)
    print(json.dumps({'id':v['id']+1,'ok':True,'result':{}}),flush=True)
''')
            stack.callback(client.close)
            with self.assertRaises(RuntimeErrorResponse):
                client.request('status')
            self.assertTrue(client.broken)
            client.close()
            self.assertIsNotNone(client.process.poll())

    def test_invalid_json_also_reaps_owned_service(self):
        with tempfile.TemporaryDirectory() as directory, ExitStack() as stack:
            client = self.service(directory, '''import sys
for line in sys.stdin: print('invalid json',flush=True)
''')
            stack.callback(client.close)
            with self.assertRaises(ValueError):
                client.request('status')
            self.assertTrue(client.broken)
            client.close()
            self.assertIsNotNone(client.process.poll())

    def test_close_waits_for_preparation_then_requests_clean_stop(self):
        with tempfile.TemporaryDirectory() as directory, ExitStack() as stack:
            client = self.service(directory, '''import json,sys,time
from pathlib import Path
for line in sys.stdin:
    value=json.loads(line)
    if value['command']=='prepare':
        Path('preparing').write_text('ready')
        deadline=time.monotonic()+5
        while not Path('release').exists() and time.monotonic()<deadline: time.sleep(.01)
    result={'running':False,'command':value['command']}
    print(json.dumps({'id':value['id'],'ok':True,'result':result}),flush=True)
''')
            stack.callback(client.close)
            results = []
            worker = threading.Thread(target=lambda: results.append(client.request('prepare', timeout=10)))
            worker.start()
            deadline = time.monotonic() + 3
            while not (Path(directory) / 'preparing').exists() and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertTrue((Path(directory) / 'preparing').exists())
            closer = threading.Thread(target=client.close)
            closer.start()
            (Path(directory) / 'release').write_text('finish preparation')
            worker.join(timeout=5)
            closer.join(timeout=5)
            self.assertFalse(worker.is_alive())
            self.assertFalse(closer.is_alive())
            self.assertEqual(results, [{'running': False, 'command': 'prepare'}])
            self.assertTrue(client.close_report['stopAcknowledged'])
            self.assertEqual(client.close_report['stopResult']['command'], 'stop')
            self.assertFalse(client.close_report['forced'])


if __name__ == '__main__':
    unittest.main()
