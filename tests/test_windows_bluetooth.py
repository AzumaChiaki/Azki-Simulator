import importlib.util
import os
from pathlib import Path
import queue
import tempfile
import threading
import time
import types
import unittest
from unittest import mock

PATH = Path(__file__).resolve().parents[1] / "engine/project/backend/src/windows_bluetooth.py"
SPEC = importlib.util.spec_from_file_location("windows_bluetooth_under_test", PATH)
BT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BT)

SERVICE = {"key": "s10", "uuid": "fe95", "characteristics": [
    {"key": "h12", "uuid": "0001", "readable": True, "writable": True,
     "notifiable": True, "writeWithResponse": True, "notifies": True}]}


class Host:
    dropped_scan_events = 0

    def __init__(self):
        self.calls = []
        self.event_queue = queue.Queue()
        self.closed = False
        self.generation = 0

    def request(self, command, **fields):
        self.calls.append((command, fields))
        if command == "status":
            return {"available": True, "central": True, "peripheral": True, "rawHci": False}
        if command == "peripheral.publish":
            self.generation += 1
            return {"generation": self.generation, "published": True}
        return {"accepted": True}

    def events(self, limit=128):
        result = []
        while len(result) < limit:
            try:
                result.append(self.event_queue.get_nowait())
            except queue.Empty:
                return result
        return result

    def close(self):
        self.closed = True


class Guest:
    def __init__(self):
        self.calls = []
        self.results = {}
        self.services = [SERVICE]
        self.notifications = []
        self.value = bytes.fromhex("001122ff")

    def __call__(self, value):
        self.calls.append(value)
        command = value["command"]
        if command == "external-open":
            return {"bridgeID": "test-bridge"}
        if command == "external-status":
            notifications, self.notifications = self.notifications, []
            return {"state": "ready", "mtu": 23, "services": self.services, "notifications": notifications}
        if command == "external-request":
            operation = "op" + str(len(self.results))
            self.results[operation] = {"done": True, "value": self.value.hex() if value["operation"] == "read" else ""}
            return {"operationID": operation}
        if command == "external-result":
            return self.results[value["operationID"]]
        if command == "external-close":
            return {"closed": True}
        raise AssertionError(command)


def request(host, *, peer="peerA", key="h12", operation="read", value="", offset=0, identity="physical1", generation=None):
    return {"event": "peripheral.request", "data": {
        "operationID": identity, "operation": operation, "peerID": peer, "key": key,
        "offset": offset, "value": value, "maximumLength": 3,
        "generation": generation if generation is not None else host.generation}}


class WindowsBluetoothTests(unittest.TestCase):
    def make_bridge(self):
        host, guest = Host(), Guest()
        bridge = BT.WindowsGuestPeripheralBridge(host, guest)
        self.addCleanup(bridge.close)
        result = bridge.start()
        return host, guest, bridge, result

    def test_construction_does_not_start_process_or_radio(self):
        calls = []
        host = BT.WindowsBluetoothHost("missing.exe", process_factory=lambda *a, **k: calls.append((a, k)))
        self.assertIsNone(host._process)
        self.assertEqual(calls, [])
        host.close()

    def test_unknown_helper_is_a_clear_error(self):
        host = BT.WindowsBluetoothHost("not-an-installed-host.exe")
        with self.assertRaises(BT.BluetoothHostError) as error:
            host.request("status")
        self.assertEqual(error.exception.code, "host_unavailable")
        host.close()

    def test_bundled_runtime_environment_is_only_applied_to_owned_child(self):
        class CapturedSpawn(Exception):
            pass

        captured = []

        def capture(*args, **kwargs):
            captured.append(kwargs["env"])
            raise CapturedSpawn()

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            helper = root / "windows-bluetooth" / "AzkiBluetoothHost.exe"
            helper.parent.mkdir()
            helper.touch()
            runtime = root / "dotnet"
            runtime.mkdir()
            with mock.patch.dict(os.environ, {"DOTNET_ROOT": "existing", "DOTNET_ROOT_X64": "existing-x64"}):
                for present in (False, True):
                    if present:
                        (runtime / "dotnet.exe").touch()
                    host = BT.WindowsBluetoothHost(helper, process_factory=capture)
                    with self.assertRaises(CapturedSpawn):
                        host.start()
                    expected_root = str(runtime.resolve()) if present else "existing"
                    expected_x64 = str(runtime.resolve()) if present else "existing-x64"
                    self.assertEqual(captured[-1]["DOTNET_ROOT"], expected_root)
                    self.assertEqual(captured[-1]["DOTNET_ROOT_X64"], expected_x64)
                    self.assertEqual(os.environ["DOTNET_ROOT"], "existing")
                    self.assertEqual(os.environ["DOTNET_ROOT_X64"], "existing-x64")
                    host.close()

    def test_capabilities_do_not_claim_guest_transport_or_complete_hci(self):
        service = BT.WindowsBluetoothService(host=Host())
        self.addCleanup(service.close)
        result = service.dispatch({"action": "capabilities"})
        self.assertFalse(result["guestATTProxyAvailable"])
        self.assertFalse(result["guestRawHCI"])
        self.assertFalse(result["firmwareCompleteBluetooth"])
        with self.assertRaises(BT.BluetoothHostError) as error:
            service.dispatch({"action": "peripheral.start"})
        self.assertEqual(error.exception.code, "guest_transport_unavailable")

    def test_read_response_keeps_exact_guest_bytes_and_physical_length(self):
        host, guest, bridge, result = self.make_bridge()
        self.assertFalse(result["rawHci"])
        bridge.poll([request(host, offset=2)])
        submitted = next(c for c in guest.calls if c["command"] == "external-request")
        self.assertEqual(submitted["offset"], 2)
        self.assertEqual(host.calls[-1], ("peripheral.respond", {"operationID": "physical1", "attError": 0, "value": "001122"}))

    def test_write_bytes_are_unchanged_and_no_authentication_result_invented(self):
        host, guest, bridge, _ = self.make_bridge()
        bridge.poll([request(host, operation="write", value="ff00127f")])
        submitted = next(c for c in guest.calls if c["command"] == "external-request")
        self.assertEqual(submitted["value"], "ff00127f")
        self.assertEqual(submitted["operation"], "write")
        self.assertNotIn("authenticated", submitted)

    def test_unknown_second_peer_and_characteristic_never_mutate_guest(self):
        host, guest, bridge, _ = self.make_bridge()
        bridge.poll([request(host)])
        initial = len([c for c in guest.calls if c["command"] == "external-request"])
        bridge.poll([request(host, peer="peerB", operation="write", identity="second"),
                     request(host, key="unknown", operation="write", identity="unknown")])
        self.assertEqual(len([c for c in guest.calls if c["command"] == "external-request"]), initial)
        replies = [f for c, f in host.calls if c == "peripheral.respond"]
        self.assertEqual([r["attError"] for r in replies[-2:]], [0x11, 0x11])

    def test_write_offset_and_guest_mtu_overflow_are_rejected_before_guest(self):
        host, guest, bridge, _ = self.make_bridge()
        bridge.poll([request(host, operation="write", offset=1, value="00"),
                     request(host, operation="write", value="00" * 21, identity="oversize")])
        self.assertFalse(any(c["command"] == "external-request" for c in guest.calls))
        replies = [f for c, f in host.calls if c == "peripheral.respond"]
        self.assertEqual([r["attError"] for r in replies], [0x07, 0x0d])

    def test_old_generation_requests_do_not_reach_new_guest_session(self):
        host, guest, bridge, _ = self.make_bridge()
        bridge.poll([request(host, generation=host.generation - 1, operation="write", value="12")])
        self.assertFalse(any(c["command"] == "external-request" for c in guest.calls))

    def test_windows_reserved_service_omission_requires_explicit_flag_and_reports_it(self):
        host, guest = Host(), Guest()
        guest.services = [{**SERVICE, "key": "reserved", "uuid": "180a"}, SERVICE]
        bridge = BT.WindowsGuestPeripheralBridge(host, guest)
        with self.assertRaises(BT.BluetoothHostError) as error:
            bridge.start()
        self.assertEqual(error.exception.code, "reserved_services")
        self.assertFalse(any(c == "peripheral.publish" for c, _ in host.calls))
        self.assertIsNone(bridge.bridge_id)
        result = bridge.start(allow_service_omissions=True)
        self.addCleanup(bridge.close)
        self.assertEqual(result["omittedServices"][0]["uuid"], "180a")
        self.assertEqual(len(result["services"]), 1)

    def test_notification_is_not_silently_truncated(self):
        host, guest, bridge, _ = self.make_bridge()
        bridge.poll([{"event": "peripheral.subscription", "data": {"peerID": "peerA", "key": "h12", "enabled": True,
                     "maximumLength": 3, "generation": host.generation}}])
        guest.notifications = [{"key": "h12", "value": "00112233"}]
        with self.assertRaises(BT.BluetoothHostError) as error:
            bridge.poll([])
        self.assertEqual(error.exception.code, "notification_length")
        self.assertIsNone(bridge.bridge_id)
        self.assertFalse(any(c == "peripheral.notify" for c, _ in host.calls))

    def test_guest_att_pump_runs_without_any_ui_events_poll(self):
        host, guest = Host(), Guest()
        service = BT.WindowsBluetoothService(host=host, guest_rpc=guest)
        self.addCleanup(service.close)
        service.dispatch({"action": "peripheral.start"})
        host.event_queue.put(request(host, operation="write", value="0102"))
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline and not any(c == "peripheral.respond" for c, _ in host.calls):
            time.sleep(0.01)
        self.assertTrue(any(c == "peripheral.respond" for c, _ in host.calls), "Background pump did not forward ATT without UI polling")
        self.assertTrue(any(c["command"] == "external-request" for c in guest.calls))
        service.dispatch({"action": "peripheral.stop"})
        self.assertIsNone(service.bridge)
        self.assertIsNone(service._pump_thread)
        self.assertEqual(guest.calls[-1]["command"], "external-close")

    def test_service_validates_action_and_bounded_event_limit(self):
        service = BT.WindowsBluetoothService(host=Host())
        self.addCleanup(service.close)
        for request_value in ({}, {"action": "unknown"}, {"action": "events", "limit": 0}, {"action": "events", "limit": True}):
            with self.assertRaises(ValueError):
                service.dispatch(request_value)

    def test_raw_mode_preparation_uses_private_pipe_and_never_starts_winrt(self):
        host = Host()
        controller = types.SimpleNamespace(capabilities={"backend": "mock-winusb", "sco": False, "iso": False})
        controller.close = mock.Mock()
        bridge = types.SimpleNamespace(
            descriptor={"qemuChardev": "pipe,id=radio,path=AzkiSimulator-HCI-" + "0" * 32,
                        "qemuEnvironment": {"AZKI_QEMU_PIPE_CLIENT_PID": "owned"}},
            stats={"closed": False, "peerAuthenticated": False, "connected": False},
            attach_core=mock.Mock(), stop=mock.Mock())
        usb = types.SimpleNamespace(WinUsbHciTransport=types.SimpleNamespace(open=mock.Mock(return_value=controller)))
        hci = types.SimpleNamespace(NamedPipeControllerBridge=mock.Mock(return_value=bridge))
        with mock.patch.dict("sys.modules", {"windows_bluetooth_usb": usb, "windows_bluetooth_hci": hci}):
            service = BT.WindowsBluetoothService(host=host)
            result = service.prepare_raw_hci("an-explicit-interface", reset_on_stop=True)
            self.assertTrue(result["qemuChardev"].startswith("pipe,"))
            self.assertEqual(host.calls, [])
            owned_process = object()
            service.attach_core(owned_process)
            bridge.attach_core.assert_called_once_with(owned_process)
            state = service.dispatch({"action": "capabilities"})
            self.assertFalse(state["guestRawHCI"])
            self.assertFalse(state["firmwareCompleteBluetooth"])
            self.assertEqual(host.calls, [])
            with self.assertRaises(BT.BluetoothHostError):
                service.dispatch({"action": "peripheral.start"})
            with self.assertRaises(ValueError):
                service.dispatch({"action": "attach_core", "pid": 123})
            service.close()
            bridge.stop.assert_called_once()
            self.assertTrue(host.closed)

    def test_raw_pipe_construction_failure_releases_only_selected_controller(self):
        controller = types.SimpleNamespace(close=mock.Mock())
        usb = types.SimpleNamespace(WinUsbHciTransport=types.SimpleNamespace(open=mock.Mock(return_value=controller)))
        hci = types.SimpleNamespace(NamedPipeControllerBridge=mock.Mock(side_effect=RuntimeError("private pipe unavailable")))
        with mock.patch.dict("sys.modules", {"windows_bluetooth_usb": usb, "windows_bluetooth_hci": hci}):
            service = BT.WindowsBluetoothService(host=Host())
            self.addCleanup(service.close)
            with self.assertRaises(RuntimeError):
                service.prepare_raw_hci("an-explicit-interface")
            controller.close.assert_called_once()
            self.assertIsNone(service.raw_hci_status())

    def test_final_controller_failure_and_unverified_radio_are_preserved_on_close(self):
        for controller_closed, pipe_closed in ((True, True), (False, True), (True, False)):
            with self.subTest(controller_closed=controller_closed, pipe_closed=pipe_closed):
                host = Host()
                state = {"closed": True, "controllerClosed": controller_closed, "pipeClosed": pipe_closed,
                         "radioTeardownVerified": False, "teardownError": "Reset completion timed out",
                         "error": None if controller_closed else "Owned controller close failed"}
                service = BT.WindowsBluetoothService(host=host)
                service._raw_bridge = types.SimpleNamespace(descriptor={"transport": "authenticated-pipe"},
                                                            stats=state, stop=mock.Mock())
                result = service.close()
                self.assertEqual(result["cleanupComplete"], controller_closed and pipe_closed)
                self.assertFalse(result["radioTeardownVerified"])
                self.assertEqual(result["dedicatedController"]["state"], state)
                self.assertIs(service.close_report, result)
                self.assertTrue(host.closed)

    def test_cleanup_report_survives_stop_error_and_host_is_still_closed(self):
        host = Host()
        service = BT.WindowsBluetoothService(host=host)
        service._raw_bridge = types.SimpleNamespace(descriptor={},
            stats={"closed": False, "controllerClosed": False, "radioTeardownVerified": False},
            stop=mock.Mock(side_effect=RuntimeError("worker still stopping")))
        with self.assertRaisesRegex(RuntimeError, "worker still stopping"):
            service.close()
        self.assertTrue(host.closed)
        self.assertFalse(service.close_report["cleanupComplete"])
        self.assertEqual(service.close_report["cleanupErrors"][0]["step"], "dedicated_controller")

    def test_controller_identity_is_trusted_metadata_and_address_is_unmeasured(self):
        controller = types.SimpleNamespace(capabilities={"controllerInstanceID": "USB\\OWNED\\A", "controllerAddress": None, "addressSource": "not-queried"}, close=mock.Mock())
        bridge = types.SimpleNamespace(descriptor={"qemuChardev": "pipe,id=radio,path=owned"}, stats={}, stop=mock.Mock())
        modules = {"windows_bluetooth_usb": types.SimpleNamespace(WinUsbHciTransport=types.SimpleNamespace(open=lambda path: controller)),
                   "windows_bluetooth_hci": types.SimpleNamespace(NamedPipeControllerBridge=lambda *a, **k: bridge)}
        with mock.patch.dict("sys.modules", modules):
            service = BT.WindowsBluetoothService(host=Host())
            descriptor = service.prepare_raw_hci("an-explicit-interface")
            self.assertEqual(descriptor["controllerInstanceID"], "USB\\OWNED\\A")
            self.assertIsNone(descriptor["controllerAddress"])
            self.assertEqual(descriptor["addressSource"], "not-queried")
            service.close()

    def test_raw_open_cleanup_quarantine_is_not_reported_as_success(self):
        failure = RuntimeError("native release failed")
        failure.raw_cleanup = {"controllerIdentity": {"instanceID": "USB\\OWNED\\A"}, "resourcesQuarantined": True, "error": "native cleanup unverified"}
        usb = types.SimpleNamespace(WinUsbHciTransport=types.SimpleNamespace(open=mock.Mock(side_effect=failure)))
        with mock.patch.dict("sys.modules", {"windows_bluetooth_usb": usb, "windows_bluetooth_hci": types.SimpleNamespace(NamedPipeControllerBridge=mock.Mock())}):
            host = Host()
            service = BT.WindowsBluetoothService(host=host)
            with self.assertRaises(RuntimeError):
                service.prepare_raw_hci("an-explicit-interface")
            result = service.close()
            self.assertFalse(result["cleanupComplete"])
            self.assertFalse(result["radioTeardownVerified"])
            self.assertEqual(result["dedicatedController"]["descriptor"]["controllerInstanceID"], "USB\\OWNED\\A")
            self.assertTrue(host.closed)


if __name__ == "__main__":
    unittest.main()
