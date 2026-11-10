"""Meaningful host-independent contract checks for the Windows controller."""
from pathlib import Path
import json
import hashlib
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "engine/project/backend"))
from src.windows_runtime import WindowsRuntime, RuntimeFailure
from src.windows_frame import decode_header


class FrameProtocol(unittest.TestCase):
    def test_native_rgb888_precision_and_geometry(self):
        header = struct.pack("<8s6I", b"Q66FRAME", 1, 4, 432, 514, 1296, 2)
        state = decode_header(header, 32 + 432 * 514 * 3, (432, 514))
        self.assertEqual(state["pixelFormat"], "rgb888")
        self.assertEqual(state["stride"], 1296)
        with self.assertRaises(ValueError):
            decode_header(header, 32 + 432 * 514 * 3, (212, 520))

    def test_capacity_and_stride_are_not_inferred(self):
        header = struct.pack("<8s6I", b"Q66FRAME", 1, 2, 192, 490, 384, 1)
        self.assertEqual(decode_header(header, 32 + 1024 * 1024 * 2)["width"], 192)
        for length in (31, 32, 32 + 192 * 490 * 2 + 1):
            with self.assertRaises(ValueError):
                decode_header(header, length)
        for stride, fmt in ((385, 1), (576, 1), (384, 3)):
            broken = struct.pack("<8s6I", b"Q66FRAME", 1, 2, 192, 490, stride, fmt)
            with self.assertRaises(ValueError):
                decode_header(broken, 32 + 192 * 490 * 2)


class RuntimeContract(unittest.TestCase):
    def runtime(self, directory):
        return WindowsRuntime(ROOT / "engine/project", Path(directory) / "absent.exe", Path(directory) / "state")

    def test_offline_profiles_match_every_catalog_version(self):
        with tempfile.TemporaryDirectory() as directory:
            runtime = self.runtime(directory)
            result = runtime.profiles()
            catalog = json.loads((ROOT / "engine/catalog.json").read_text(encoding="utf-8"))
            actual = {row["id"] for row in result["profiles"] if not row["importOnly"]}
            self.assertEqual(actual, {row["id"] for row in catalog["profiles"]})
            self.assertEqual(result["hardwarePresets"], 17)
            self.assertFalse((Path(directory) / "state").exists())

    def test_absent_core_does_not_create_session(self):
        with tempfile.TemporaryDirectory() as directory:
            runtime = self.runtime(directory)
            with self.assertRaises(RuntimeFailure) as raised:
                runtime.prepare({"profileID": "hardware:miwear.watch.q65", "firmwarePath": "unused"})
            self.assertEqual(raised.exception.code, "core_missing")
            self.assertFalse((Path(directory) / "state").exists())

    def test_stop_continues_when_bluetooth_and_frame_cleanup_fail(self):
        events = []

        class Process:
            pid = 123
            returncode = None

            def poll(self): return self.returncode

            def wait(self, timeout):
                events.append("wait")
                if self.returncode is None:
                    raise subprocess.TimeoutExpired("owned-fake", timeout)
                return self.returncode

        class Broken:
            def __init__(self, name): self.name = name

            def close(self):
                events.append(self.name)
                raise RuntimeError(self.name + " failed")

        class QMP:
            def execute(self, command): events.append(command)
            def close(self): events.append("qmp-close")

        class Job:
            def close(self):
                events.append("job-close")
                process.returncode = -1

        with tempfile.TemporaryDirectory() as directory:
            runtime = self.runtime(directory)
            process = Process()
            runtime.process = process
            runtime.bluetooth_service = Broken("bluetooth")
            runtime.guest_peer = Broken("guest-peer")
            runtime.reader = Broken("frame")
            runtime.job, runtime.qmp = Job(), QMP()
            result = runtime.stop()
            self.assertTrue(result["ownedChildClosed"])
            self.assertFalse(result["cleanupComplete"])
            self.assertEqual([row["step"] for row in result["cleanupErrors"]], ["bluetooth", "guest_peer", "frame_close"])
            self.assertIn("quit", events)
            self.assertIn("job-close", events)
            self.assertLess(events.index("bluetooth"), events.index("job-close"))

    def test_real_stdio_profiles_and_duplicate_field_rejection(self):
        with tempfile.TemporaryDirectory() as directory:
            command = [sys.executable, "-B", "-m", "src.windows_runtime", "--project-root", str(ROOT / "engine/project"),
                       "--qemu", str(Path(directory) / "absent.exe"), "--workspace", str(Path(directory) / "state")]
            payload = json.dumps({"id": 1, "command": "profiles", "args": {}}) + "\n"
            payload += '{"id":2,"command":"profiles","args":{},"args":{}}\n'
            child = subprocess.run(command, input=payload, text=True, encoding="utf-8", capture_output=True,
                                   cwd=ROOT / "engine/project/backend", timeout=10)
            self.assertEqual(child.returncode, 0, child.stderr)
            lines = [json.loads(line) for line in child.stdout.splitlines()]
            self.assertEqual(len(lines), 2)
            self.assertTrue(lines[0]["ok"])
            self.assertFalse(lines[1]["ok"])

    def test_raw_controller_cleanup_and_reset_receipt_are_independent(self):
        class Service:
            def __init__(self, closed):
                self.close_report = {"cleanupComplete": closed, "radioTeardownVerified": False,
                                     "dedicatedController": {"controllerClosed": closed}}

            def close(self):
                return self.close_report

        with tempfile.TemporaryDirectory() as directory:
            for closed in (False, True):
                runtime = self.runtime(directory)
                runtime.bluetooth_service = Service(closed)
                result = runtime.stop()
                self.assertEqual(result["cleanupComplete"], closed)
                self.assertFalse(result["bluetoothCleanup"]["radioTeardownVerified"])
                self.assertEqual(bool(result["cleanupErrors"]), not closed)

    def test_stop_before_process_creation_reprepares_closed_guest_state(self):
        with tempfile.TemporaryDirectory() as directory:
            runtime = self.runtime(directory)
            image = Path(directory) / "image.exe"
            image.write_bytes(b"owned-test-image")
            identity = hashlib.sha256(image.read_bytes()).hexdigest()
            runtime.qemu = image
            prepared = {"session": Path(directory), "coreSHA256": identity,
                        "ap": image, "recipe": {"ap": {"sha256": identity}}}
            runtime.prepared = dict(prepared)
            runtime.last_prepare_args = {"profileID": "owned-fixture", "firmwarePath": str(image)}
            self.assertTrue(runtime.stop()["cleanupComplete"])
            calls = []
            def prepare(args):
                calls.append(args)
                runtime.prepared = dict(prepared)
            runtime.prepare = prepare
            runtime._start_prepared = lambda args: {"running": True}
            self.assertTrue(runtime.start({})["running"])
            self.assertEqual(calls, [runtime.last_prepare_args])

    def test_usb_selection_uses_inspection_without_a_winrt_or_guest_service(self):
        with tempfile.TemporaryDirectory() as directory:
            runtime = self.runtime(directory)
            runtime._bluetooth_service = mock.Mock(side_effect=AssertionError("USB inspection must not create a service"))
            for action in ("hci.usb.list", "hci.usb.inspect"):
                request = {"action": action}
                expected = {"devices": [], "inspectionOnly": True, "deviceOpened": False}
                with mock.patch("src.windows_bluetooth_usb.dispatch_inspection", return_value=expected, create=True) as inspect:
                    self.assertIs(runtime.bluetooth(request), expected)
                    inspect.assert_called_once_with(request)
            self.assertIsNone(runtime.bluetooth_service)


if __name__ == "__main__":
    unittest.main()
