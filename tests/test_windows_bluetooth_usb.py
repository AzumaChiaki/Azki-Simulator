"""WinUSB protocol and owned-overlapped lifecycle; no physical radio access."""
from collections import deque
import ctypes as C
import importlib.util
import io
import json
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

PATH = Path(__file__).resolve().parents[1] / "engine/project/backend/src/windows_bluetooth_usb.py"
SPEC = importlib.util.spec_from_file_location("azki_windows_usb_tested", PATH)
usb = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = usb
SPEC.loader.exec_module(usb)

DEVICE = r"\\?\usb#vid_0a12&pid_0001#explicit-owned-device#{12345678-1234-1234-1234-123456789abc}"
PIPES = [usb.UsbPipe(0x81, 3, 16), usb.UsbPipe(0x82, 2, 64), usb.UsbPipe(0x02, 2, 64)]
RESET = bytes.fromhex("01 03 0c 00")
EVENT = bytes.fromhex("04 0e 04 01 03 0c 00")
ACL = bytes.fromhex("02 01 20 05 00 68 65 6c 6c 6f")


class FakeOperation:
    def __init__(self, kind, endpoint=None, value=None):
        self.kind, self.endpoint, self.value = kind, endpoint, value
        self.result = None


class FakeDevice:
    def __init__(self, interface=None, pipes=None):
        self.info = interface or usb.UsbInterface(0, 0, 0xE0, 1, 1)
        self.pipe_list = PIPES if pipes is None else pipes
        self.operations, self.writes, self.cancelled = [], [], []
        self.closed = 0
        self.on_wait = None
        self.short_write = False
        self.pending_write = False

    def interface(self):
        return self.info

    def pipes(self):
        return self.pipe_list

    def start_read(self, endpoint, size):
        value = FakeOperation("read", endpoint, size)
        self.operations.append(value)
        return value

    def _write(self, kind, endpoint, data, setup=None):
        self.writes.append((kind, endpoint, data, setup))
        value = FakeOperation(kind, endpoint, data)
        if not self.pending_write:
            value.result = len(data) - int(self.short_write)
        self.operations.append(value)
        return value

    def start_write(self, endpoint, data):
        return self._write("bulk", endpoint, data)

    def start_control_write(self, setup, data):
        return self._write("control", None, data, setup)

    def poll(self, operation):
        return operation.result

    def deliver(self, endpoint, data):
        for operation in self.operations:
            if operation.kind == "read" and operation.endpoint == endpoint and operation.result is None:
                operation.result = data
                return
        raise AssertionError("No pending read for endpoint")

    def wait(self, operations, timeout):
        if self.on_wait:
            self.on_wait(operations, timeout)

    def close(self):
        self.closed += 1
        self.cancelled.extend(value for value in self.operations if value.result is None)


class FakeApi:
    def __init__(self, device=None):
        self.device = device or FakeDevice()
        self.opened = []

    def open(self, path):
        self.opened.append(path)
        return self.device


class PublicContractTests(unittest.TestCase):
    def make(self, device=None, **kwargs):
        api = FakeApi(device)
        value = usb.WinUsbHciTransport.open(DEVICE, api=api, **kwargs)
        self.addCleanup(value.close)
        return value, api.device

    def test_explicit_path_and_inspect_do_not_start_radio_io(self):
        api = FakeApi()
        result = usb.inspect_winusb_device(DEVICE, api=api)
        self.assertEqual(api.opened, [DEVICE])
        self.assertEqual(api.device.operations, [])
        self.assertEqual(api.device.closed, 1)
        self.assertFalse(result["physicalRadioVerified"])
        self.assertFalse(result["driverBindingManaged"])
        self.assertFalse(result["sco"])
        self.assertFalse(result["iso"])
        for path in ("", "COM3", r"\\.\COM3", r"\\?\GLOBALROOT\Device\x", DEVICE + "\0", DEVICE + "\\extra", None):
            with self.subTest(path=path), self.assertRaises(ValueError):
                usb.WinUsbHciTransport.open(path, api=api)
        self.assertEqual(api.opened, [DEVICE])

    def test_no_device_inspection_only_loads_dlls_and_never_opens(self):
        api = FakeApi()
        with patch.object(usb.os, "name", "nt"), patch.object(usb, "_WinUsbApi", return_value=api) as factory:
            value = usb.inspect_backend()
        factory.assert_called_once_with()
        self.assertTrue(value["winUsbDllAvailable"])
        self.assertFalse(value["deviceOpened"])
        self.assertEqual(api.opened, [])
        self.assertEqual(api.device.operations, [])

    def test_dedicated_guid_list_uses_only_registration_metadata_never_winusb_open(self):
        calls = []
        rows = [{"devicePath": DEVICE, "instanceID": r"USB\VID_0A12&PID_0001\OWNED", "name": "Owned", "driver": "WinUSB", "driverInfPath": "winusb.inf"},
                {"devicePath": DEVICE, "instanceID": r"USB\VID_0A12&PID_0001\NOT-BOUND", "name": "Needs setup", "driver": "BTHUSB", "driverInfPath": "bth.inf"}]
        def enumerate_interfaces(guid):
            calls.append(guid)
            return rows
        with patch.object(usb, "_WinUsbApi") as radio_factory:
            result = usb.list_dedicated_controllers(api=SimpleNamespace(list_interfaces=enumerate_interfaces))
        self.assertEqual(calls, [usb.AZKI_USB_INTERFACE_GUID])
        radio_factory.assert_not_called()
        self.assertEqual(len(result["devices"]), 1)
        self.assertEqual(len(result["unavailableDevices"]), 1)
        self.assertFalse(result["deviceOpened"])
        self.assertFalse(result["radioOpened"])
        self.assertFalse(result["driverBindingPerformed"])
        self.assertFalse(result["driverBindingRequired"])
        self.assertEqual(result["hciCommandsSent"], 0)

    def test_empty_interface_list_does_not_invent_driver_compatibility_or_fallback(self):
        result = usb.list_dedicated_controllers(api=SimpleNamespace(list_interfaces=lambda guid: []))
        self.assertEqual(result["devices"], [])
        self.assertIsNone(result["driverBindingRequired"])
        self.assertFalse(result["guestFirmwareAccepted"])
        with patch.object(usb.os, "name", "posix"), patch.object(usb, "_RegisteredAzkiInterfaces") as native:
            self.assertFalse(usb.list_dedicated_controllers()["platformSupported"])
        native.assert_not_called()

    def test_inspection_route_validates_before_accessing_native_resources(self):
        with patch.object(usb, "list_dedicated_controllers", return_value={"devices": []}) as listing, \
                patch.object(usb, "inspect_winusb_device") as inspect_device, patch.object(usb, "inspect_backend") as backend:
            self.assertEqual(usb.dispatch_inspection({"action": "hci.usb.list"}), {"devices": []})
            listing.assert_called_once_with()
            for value in ({"action": "hci.usb.list", "devicePath": DEVICE}, {"action": "hci.usb.inspect", "devicePath": ""},
                          {"action": "hci.usb.inspect", "devicePath": None}, {"action": "hci.usb.inspect", "apply": True},
                          {"action": "hci.usb.bind"}, []):
                with self.subTest(value=value), self.assertRaises(ValueError):
                    usb.dispatch_inspection(value)
            inspect_device.assert_not_called()
            backend.assert_not_called()

    def test_interface_list_rejects_unbounded_or_non_interface_metadata(self):
        for rows in ([{}], [{"devicePath": DEVICE, "instanceID": ""}], [{}] * 129):
            with self.subTest(rows=rows[:1]), self.assertRaises((ValueError, usb.UsbHciError)):
                usb.list_dedicated_controllers(api=SimpleNamespace(list_interfaces=lambda guid: rows))

    def test_opaque_path_resolution_uses_returned_pnp_identity_not_path_parsing(self):
        calls = []
        row = {"devicePath": DEVICE.upper(), "instanceID": r"USB\VID_0A12&PID_0001\ACTUAL-PNP-ID", "driver": "WinUSB"}
        def resolve(path):
            calls.append(path)
            return row
        with patch.object(usb, "_WinUsbApi") as open_factory:
            value = usb.resolve_device_interface(DEVICE, api=SimpleNamespace(resolve_interface=resolve))
        open_factory.assert_not_called()
        self.assertEqual(calls, [DEVICE])
        self.assertEqual(value["instanceID"], row["instanceID"])
        self.assertFalse(value["deviceOpened"])
        self.assertEqual(value["hciCommandsSent"], 0)

    def test_resolver_rejects_a_different_interface_or_non_usb_instance(self):
        for row in ({"devicePath": DEVICE + "other", "instanceID": "USB\\x\\y"},
                    {"devicePath": DEVICE, "instanceID": "PCI\\x\\y"}):
            with self.subTest(row=row), self.assertRaises(usb.UsbHciError):
                usb.resolve_device_interface(DEVICE, api=SimpleNamespace(resolve_interface=lambda path: row))

    def test_inspect_cli_does_not_open_a_device_by_default(self):
        output = io.StringIO()
        with patch.object(usb, "inspect_backend", return_value={"platformSupported": True}), \
                patch.object(usb, "inspect_winusb_device") as inspect_device, patch("sys.stdout", output):
            result = usb.main(["--inspect"])
        self.assertEqual(result, 0)
        inspect_device.assert_not_called()
        self.assertEqual(json.loads(output.getvalue())["hciCommandsSent"], 0)

    def test_class_alternate_endpoint_validation_closes_only_supplied_device(self):
        for interface in (usb.UsbInterface(0, 0, 0xFF, 1, 1), usb.UsbInterface(0, 1, 0xE0, 1, 1),
                          usb.UsbInterface(0, 0, 0xE0, 2, 1), usb.UsbInterface(0, 0, 0xE0, 1, 2)):
            device = FakeDevice(interface)
            with self.assertRaises(usb.UsbHciProtocolError):
                usb.WinUsbHciTransport.open(DEVICE, api=FakeApi(device))
            self.assertEqual(device.closed, 1)
        for pipes in (PIPES[:2], PIPES + [usb.UsbPipe(0x83, 2, 64)],
                      PIPES + [PIPES[0]], [usb.UsbPipe(0x80, 3, 16)] + PIPES[1:],
                      [usb.UsbPipe(0x181, 3, 16)] + PIPES[1:]):
            with self.subTest(pipes=pipes), self.assertRaises(usb.UsbHciProtocolError):
                usb.WinUsbHciTransport.open(DEVICE, api=FakeApi(FakeDevice(pipes=pipes)))

    def test_command_uses_class_control_without_h4_byte(self):
        value, device = self.make()
        value.send_h4(RESET)
        self.assertEqual(device.writes, [("control", None, RESET[1:], usb.UsbSetup(0x20, 0, 0, 0, 3))])
        self.assertEqual(device.operations[0].kind, "control")

    def test_nonzero_interface_and_explicit_recipient(self):
        value, device = self.make(FakeDevice(usb.UsbInterface(3, 0, 0xE0, 1, 1)))
        value.send_h4(RESET)
        self.assertEqual(device.writes[0][3], usb.UsbSetup(0x21, 0, 0, 3, 3))
        other, second = self.make(command_recipient="interface")
        other.send_h4(RESET)
        self.assertEqual(second.writes[0][3], usb.UsbSetup(0x21, 0, 0, 0, 3))

    def test_acl_uses_bulk_without_h4_byte_and_rejects_other_types(self):
        value, device = self.make()
        value.send_h4(ACL)
        self.assertEqual(device.writes, [("bulk", 0x02, ACL[1:], None)])
        for packet in (b"", b"\x01", RESET + b"x", ACL[:-1], b"\x03\0\0\0", EVENT,
                       b"\x05\0\0\0\0", bytearray(RESET), b"\x02\0\0\x01\x04" + bytes(1025)):
            with self.subTest(packet=packet[:8]), self.assertRaises((ValueError, usb.UsbHciProtocolError)):
                value.send_h4(packet)
        self.assertEqual(len(device.writes), 1)

    def test_two_pipes_are_armed_and_timeout_preserves_the_same_reads(self):
        value, device = self.make()
        self.assertIsNone(value.receive(0))
        first = list(device.operations)
        self.assertEqual({operation.endpoint for operation in first}, {0x81, 0x82})
        self.assertIsNone(value.receive(0))
        self.assertEqual(device.operations, first)
        self.assertEqual(device.cancelled, [])
        device.deliver(0x81, EVENT[1:])
        device.deliver(0x82, ACL[1:])
        self.assertEqual({value.receive(0), value.receive(0)}, {EVENT, ACL})
        value.close(); value.close()
        self.assertEqual(device.closed, 1)

    def test_event_and_acl_reassembly_at_every_boundary(self):
        for packet in (EVENT, ACL):
            for split in range(1, len(packet) - 1):
                with self.subTest(packet=packet[0], split=split):
                    value, device = self.make()
                    self.assertIsNone(value.receive(0))
                    endpoint = 0x81 if packet[0] == 4 else 0x82
                    body = packet[1:]
                    device.deliver(endpoint, body[:split])
                    self.assertIsNone(value.receive(0))
                    device.deliver(endpoint, body[split:])
                    self.assertEqual(value.receive(0), packet)

    def test_multiple_frames_in_one_usb_completion_are_not_merged(self):
        value, device = self.make()
        value.receive(0)
        device.deliver(0x81, EVENT[1:] * 3)
        self.assertEqual([value.receive(0) for _ in range(3)], [EVENT] * 3)

    def test_pending_completion_is_polled_after_wait(self):
        value, device = self.make()
        device.on_wait = lambda operations, timeout: device.deliver(0x81, EVENT[1:])
        self.assertEqual(value.receive(0.05), EVENT)

    def test_oversized_acl_fails_without_truncation_or_hci_success(self):
        value, device = self.make()
        value.receive(0)
        device.deliver(0x82, bytes.fromhex("01 20 01 04"))
        with self.assertRaises(usb.UsbHciProtocolError):
            value.receive(0)
        with self.assertRaises(usb.UsbHciError):
            value.send_h4(RESET)
        self.assertEqual(device.writes, [])

    def test_short_write_latches_failure_without_retry(self):
        value, device = self.make()
        device.short_write = True
        with self.assertRaises(usb.UsbHciProtocolError):
            value.send_h4(RESET)
        with self.assertRaises(usb.UsbHciError):
            value.send_h4(RESET)
        self.assertEqual(len(device.writes), 1)

    def test_owned_close_failure_is_retained_and_not_retried(self):
        device = FakeDevice()
        value = usb.WinUsbHciTransport.open(DEVICE, api=FakeApi(device))
        def fail_close():
            device.closed += 1
            raise usb.UsbHciError("Owned cancellation remains incomplete")
        device.close = fail_close
        with self.assertRaisesRegex(usb.UsbHciError, "cancellation"):
            value.close()
        with self.assertRaisesRegex(usb.UsbHciError, "Previous owned USB close failed"):
            value.close()
        with self.assertRaises(usb.UsbHciError):
            value.send_h4(RESET)
        self.assertEqual(device.closed, 1)
        self.assertEqual(device.writes, [])

    def test_receive_poll_arm_and_wait_failure_are_terminal_without_tx_retry(self):
        for failure in ("arm-first", "arm-second", "poll", "wait"):
            with self.subTest(failure=failure):
                value, device = self.make()
                previous_start = device.start_read
                count = [0]
                def start(endpoint, size):
                    count[0] += 1
                    if failure == "arm-first" or (failure == "arm-second" and count[0] == 2):
                        raise OSError("USB arm failed")
                    return previous_start(endpoint, size)
                device.start_read = start
                if failure == "poll":
                    device.poll = lambda operation: (_ for _ in ()).throw(usb.UsbHciError("USB poll failed"))
                if failure == "wait":
                    device.wait = lambda operations, timeout: (_ for _ in ()).throw(OSError("USB wait failed"))
                with self.assertRaises((OSError, usb.UsbHciError)):
                    value.receive(.05)
                self.assertEqual(device.closed, 1)
                operations = list(device.operations)
                for command in (lambda: value.receive(0), lambda: value.send_h4(RESET)):
                    with self.assertRaises(usb.UsbHciError):
                        command()
                self.assertEqual(device.operations, operations)
                self.assertEqual(device.writes, [])
                self.assertEqual(len(value._queue), 0)

    def test_receive_error_discards_queued_frame_and_cancels_remaining_reads(self):
        value, device = self.make()
        value.receive(0)
        device.deliver(0x81, EVENT[1:])
        original = device.poll
        def poll(operation):
            if operation.endpoint == 0x82:
                raise OSError("Controller disconnected")
            return original(operation)
        device.poll = poll
        value._next_first = "event"
        with self.assertRaises(OSError):
            value.receive(.05)
        self.assertEqual(device.closed, 1)
        self.assertEqual(list(value._queue), [])
        self.assertTrue(any(item.endpoint == 0x82 for item in device.cancelled))
        with self.assertRaises(usb.UsbHciError):
            value.send_h4(RESET)
        self.assertEqual(device.writes, [])

    def test_queue_bound_and_timeout_validation(self):
        value, device = self.make()
        for timeout in (-1, float("inf"), float("nan"), True, None, 61):
            with self.assertRaises(ValueError):
                value.receive(timeout)
        for limit in (0, 65536, True):
            with self.assertRaises(ValueError):
                usb.WinUsbHciTransport.open(DEVICE, api=FakeApi(), max_acl_payload=limit)
        frames = usb._Frames(4, 1024)
        with self.assertRaises(usb.UsbHciProtocolError):
            frames.feed(bytes.fromhex("0e00") * (usb.MAX_QUEUED_PACKETS + 1))


def complete(opcode, values=b"\0", credits=1):
    return bytes((4, 0x0e, 3 + len(values), credits, opcode & 255, opcode >> 8)) + values


class FakeProbeTransport:
    def __init__(self, *, le=False, le_commands=True, bd_addr=False):
        commands = bytearray(64)
        if bd_addr:
            commands[15] |= 1 << 1
        if le_commands:
            for byte, bit in usb._PROBE_LE_BITS.values():
                commands[byte] |= 1 << bit
        features = bytearray(8)
        if le:
            features[4] |= 0x40
        self.responses = {
            0x0c03: [complete(0x0c03)],
            0x1001: [complete(0x1001, bytes.fromhex("00 09 34 12 09 0a 00 78 56"))],
            0x1002: [complete(0x1002, b"\0" + commands)],
            0x1003: [complete(0x1003, b"\0" + features)],
            0x1005: [complete(0x1005, bytes.fromhex("00 00 04 3c 08 00 02 00"))],
            0x1009: [complete(0x1009, bytes.fromhex("00 f6 e5 d4 c3 b2 a1"))],
            0x2002: [complete(0x2002, bytes.fromhex("00 fb 00 04"))],
            0x2003: [complete(0x2003, bytes.fromhex("00 01 02 03 04 05 06 07 08"))],
            0x201c: [complete(0x201c, bytes.fromhex("00 08 07 06 05 04 03 02 01"))],
        }
        self.pending, self.sent, self.opened, self.closed = deque(), [], [], 0
        self.close_error = None

    def factory(self, path, **kwargs):
        self.opened.append((path, kwargs))
        return self

    def send_h4(self, packet):
        self.sent.append(packet)
        opcode = int.from_bytes(packet[1:3], "little")
        self.pending.extend(self.responses.get(opcode, []))

    def receive(self, timeout):
        return self.pending.popleft() if self.pending else None

    def close(self):
        self.closed += 1
        if self.close_error:
            raise self.close_error


class ControllerProbeTests(unittest.TestCase):
    def probe(self, fake, **kwargs):
        return usb.probe_controller(DEVICE, transport_factory=fake.factory, timeout=.001, **kwargs)

    def test_once_reset_and_read_only_queries_decode_real_fields(self):
        fake = FakeProbeTransport(le=True)
        result = self.probe(fake)
        self.assertTrue(result["success"], result)
        self.assertEqual([int.from_bytes(p[1:3], "little") for p in fake.sent],
                         [0x0c03, 0x1001, 0x1002, 0x1003, 0x1005, 0x2002, 0x2003, 0x201c])
        self.assertTrue(all(len(packet) == 4 and packet[0] == 1 and packet[3] == 0 for packet in fake.sent))
        self.assertEqual(result["resetCommandsAttempted"], 1)
        self.assertEqual(fake.closed, 1)
        self.assertTrue(result["transportClosed"])
        self.assertFalse(result["radioStateUnknown"])
        self.assertFalse(result["extraResetOnClose"])
        self.assertFalse(result["completeFirmwareBluetoothAccepted"])
        rows = {row["opcode"]: row for row in result["queries"]}
        self.assertEqual(rows["0x1001"]["fields"], {"hciVersion": 9, "hciRevision": 0x1234,
                         "lmpVersion": 9, "companyIdentifier": 10, "lmpSubversion": 0x5678})
        self.assertEqual(rows["0x1005"]["fields"], {"aclPayloadBytes": 1024,
                         "synchronousPayloadBytes": 60, "aclPacketCredits": 8,
                         "synchronousPacketCredits": 2})
        self.assertEqual(rows["0x2002"]["fields"], {"leAclPayloadBytes": 251, "leAclPacketCredits": 4})
        self.assertEqual(rows["0x2003"]["fields"]["hex"], "0102030405060708")
        self.assertEqual(rows["0x201c"]["fields"]["hex"], "0807060504030201")
        self.assertEqual(fake.opened, [(DEVICE, {"io_timeout": .001})])

    def test_le_queries_require_both_real_feature_and_command_bits(self):
        for le, advertised in ((False, True), (True, False)):
            with self.subTest(le=le, advertised=advertised):
                fake = FakeProbeTransport(le=le, le_commands=advertised)
                result = self.probe(fake)
                self.assertTrue(result["success"], result)
                self.assertEqual(result["leSupported"], le)
                self.assertEqual(len(fake.sent), 5)
                self.assertEqual(len(result["skipped"]), 4)
        fake = FakeProbeTransport(le=True)
        fake.responses[0x1002] = [complete(0x1002, b"\1")]
        result = self.probe(fake)
        self.assertFalse(result["success"])
        self.assertEqual(result["queries"][2]["status"], 1)
        self.assertIsNone(result["queries"][2]["fields"])
        self.assertEqual(len(fake.sent), 5)
        self.assertTrue(all(row["reason"] == "Supported-command bitmap unavailable" for row in result["skipped"]))

    def test_read_bd_addr_is_bitmap_gated_and_decodes_six_little_endian_octets(self):
        fake = FakeProbeTransport(bd_addr=True)
        result = self.probe(fake)
        self.assertTrue(result["success"], result)
        self.assertEqual(result["controllerAddress"], "A1:B2:C3:D4:E5:F6")
        row = next(row for row in result["queries"] if row["opcode"] == "0x1009")
        self.assertEqual(row["fields"], {"address": "A1:B2:C3:D4:E5:F6",
                         "littleEndianHex": "f6e5d4c3b2a1", "isZero": False})
        self.assertEqual([int.from_bytes(packet[1:3], "little") for packet in fake.sent],
                         [0x0c03, 0x1001, 0x1002, 0x1003, 0x1005, 0x1009])
        self.assertEqual(fake.sent[-1], bytes.fromhex("01 09 10 00"))
        self.assertEqual(fake.sent.count(RESET), 1)
        self.assertEqual(fake.closed, 1)
        for bitmap in ("unsupported", "unavailable"):
            with self.subTest(bitmap=bitmap):
                fake = FakeProbeTransport(bd_addr=False)
                if bitmap == "unavailable":
                    fake.responses[0x1002] = [complete(0x1002, b"\1")]
                result = self.probe(fake)
                self.assertIsNone(result["controllerAddress"])
                self.assertNotIn(bytes.fromhex("01 09 10 00"), fake.sent)
                skipped = next(row for row in result["skipped"] if row["opcode"] == "0x1009")
                self.assertIn("bitmap", skipped["reason"])

    def test_read_bd_addr_rejection_short_success_and_zero_address_are_truthful(self):
        for values in (b"\1", b"\0" + bytes(5)):
            with self.subTest(values=values.hex()):
                fake = FakeProbeTransport(bd_addr=True)
                fake.responses[0x1009] = [complete(0x1009, values)]
                result = self.probe(fake)
                self.assertFalse(result["success"])
                self.assertIsNone(result["controllerAddress"])
                row = next(row for row in result["queries"] if row["opcode"] == "0x1009")
                self.assertIsNone(row["fields"])
                self.assertEqual(row["status"], values[0])
                self.assertEqual(fake.sent.count(bytes.fromhex("01 09 10 00")), 1)
                self.assertEqual(fake.sent.count(RESET), 1)
        fake = FakeProbeTransport(bd_addr=True)
        fake.responses[0x1009] = [complete(0x1009, bytes(7))]
        result = self.probe(fake)
        self.assertTrue(result["success"], result)
        self.assertEqual(result["controllerAddress"], "00:00:00:00:00:00")
        row = next(row for row in result["queries"] if row["opcode"] == "0x1009")
        self.assertEqual(row["fields"], {"address": "00:00:00:00:00:00",
                         "littleEndianHex": "000000000000", "isZero": True})

    def test_matching_opcode_and_complete_are_required_not_status_acceptance(self):
        fake = FakeProbeTransport()
        fake.responses[0x1001] = [ACL, complete(0x1234), bytes.fromhex("04 0f 04 00 01 01 10"),
                                   fake.responses[0x1001][0]]
        result = self.probe(fake)
        self.assertTrue(result["success"], result)
        self.assertEqual(result["queries"][1]["response"], "commandComplete")
        fake = FakeProbeTransport()
        fake.responses[0x1001] = [bytes.fromhex("04 0f 04 01 01 01 10")]
        result = self.probe(fake)
        self.assertFalse(result["success"])
        self.assertEqual(result["queries"][1]["response"], "commandStatus")
        self.assertEqual(result["queries"][1]["status"], 1)
        self.assertIsNone(result["queries"][1]["fields"])

    def test_actual_command_credits_gate_next_command_without_replay(self):
        fake = FakeProbeTransport()
        fake.responses[0x0c03] = [complete(0x0c03, credits=0)]
        result = self.probe(fake)
        self.assertFalse(result["success"])
        self.assertTrue(result["resetSucceeded"])
        self.assertEqual(fake.sent, [RESET])
        self.assertFalse(result["queries"][1]["commandAttempted"])
        self.assertEqual(result["commandCredits"], 0)
        fake = FakeProbeTransport()
        fake.responses[0x0c03] = [complete(0x0c03, credits=0), complete(0, b"", credits=1)]
        result = self.probe(fake)
        self.assertTrue(result["success"], result)
        self.assertEqual(fake.sent.count(RESET), 1)

    def test_reset_rejection_timeout_malformed_or_io_error_never_retries(self):
        for replies in ([complete(0x0c03, b"\1")], [], [complete(0x0c03, b"\0\0")],
                        [b"\x04\x0e\x04\x01\x03"], [complete(0x1234)]):
            with self.subTest(replies=replies):
                fake = FakeProbeTransport()
                fake.responses[0x0c03] = replies
                result = self.probe(fake)
                self.assertFalse(result["success"])
                self.assertTrue(result["radioStateUnknown"])
                self.assertEqual(fake.sent, [RESET])
                self.assertEqual(fake.closed, 1)
        fake = FakeProbeTransport()
        def fail_receive(_):
            raise OSError("USB disconnected")
        fake.receive = fail_receive
        result = self.probe(fake)
        self.assertFalse(result["success"])
        self.assertEqual(fake.sent, [RESET])
        self.assertIn("USB disconnected", result["error"])

    def test_wrong_successful_query_length_is_not_decoded_or_retried(self):
        fake = FakeProbeTransport()
        fake.responses[0x1001] = [complete(0x1001, b"\0")]
        result = self.probe(fake)
        self.assertFalse(result["success"])
        self.assertIsNone(result["queries"][1]["fields"])
        self.assertEqual(len(fake.sent), 2)
        self.assertIn("length", result["error"])

    def test_failed_close_is_truthful_even_after_successful_queries(self):
        fake = FakeProbeTransport()
        fake.close_error = usb.UsbHciError("Owned buffers retained")
        result = self.probe(fake)
        self.assertFalse(result["success"])
        self.assertFalse(result["transportClosed"])
        self.assertTrue(result["radioStateUnknown"])
        self.assertIn("Owned transport close failed", result["error"])
        self.assertEqual(fake.sent.count(RESET), 1)

    def test_probe_is_explicit_cli_operation_requiring_path(self):
        with patch.object(usb, "probe_controller") as probe, patch("sys.stderr", io.StringIO()):
            with self.assertRaises(SystemExit):
                usb.main(["--probe-controller"])
        probe.assert_not_called()
        output = io.StringIO()
        with patch.object(usb, "probe_controller", return_value={"success": True}) as probe, \
                patch.object(usb, "inspect_backend") as inspect, patch("sys.stdout", output):
            self.assertEqual(usb.main(["--probe-controller", "--device-path", DEVICE, "--query-timeout", "1"]), 0)
        probe.assert_called_once_with(DEVICE, timeout=1)
        inspect.assert_not_called()
        with patch.object(usb, "inspect_backend", return_value={"platformSupported": True}), \
                patch.object(usb, "probe_controller") as probe, patch("sys.stdout", io.StringIO()):
            self.assertEqual(usb.main(["--inspect"]), 0)
        probe.assert_not_called()


class NativeShim:
    """ctypes-shaped fake: exercise native ERROR_IO_PENDING and exact cancellation."""
    def __init__(self):
        self.error, self.next_event = 0, 3000
        self.requests, self.cancelled, self.closed, self.freed = {}, [], [], []
        self.calls = []
        self.cancel_race = False
        self.stuck = False
        self.wait_failed = False
        self.current_alternate = 0
        self.immediate = False
        self.kernel = SimpleNamespace(CreateFileW=self.create, CloseHandle=self.close,
            CreateEventW=self.event, CancelIoEx=self.cancel, WaitForMultipleObjects=self.wait)
        self.usb = SimpleNamespace(WinUsb_Initialize=self.initialize, WinUsb_Free=self.free,
            WinUsb_GetCurrentAlternateSetting=self.current,
            WinUsb_QueryInterfaceSettings=self.interface, WinUsb_QueryPipe=self.pipe,
            WinUsb_ReadPipe=self.read, WinUsb_WritePipe=self.write,
            WinUsb_ControlTransfer=self.control, WinUsb_GetOverlappedResult=self.poll)

    def create(self, *args):
        self.calls.append(("open", args))
        return 1001

    def initialize(self, handle, output):
        assert handle == 1001
        output._obj.value = 2002
        return 1

    def interface(self, handle, alternate, output):
        data = output._obj
        for name, value in (("Length", 9), ("DescriptorType", 4), ("InterfaceNumber", 0),
                            ("NumEndpoints", 3), ("InterfaceClass", 0xE0),
                            ("InterfaceSubClass", 1), ("InterfaceProtocol", 1)):
            setattr(data, name, value)
        return 1

    def current(self, handle, output):
        output._obj.value = self.current_alternate
        return 1

    def pipe(self, handle, alternate, index, output):
        value = PIPES[index]
        output._obj.PipeType, output._obj.PipeId = value.kind, value.address
        output._obj.MaximumPacketSize = value.maximum_packet_size
        return 1

    def event(self, security, manual_reset, initial, name):
        assert manual_reset and not initial
        self.next_event += 1
        return self.next_event

    def _start(self, buffer, length, overlapped, raw=None):
        address = C.addressof(overlapped._obj)
        self.requests[address] = {"buffer": buffer, "length": length, "overlapped": overlapped._obj,
                                  "state": "pending", "raw": raw}
        if self.immediate:
            self.requests[address]["state"] = "done"
            return 1
        self.error = usb.ERROR_IO_PENDING
        return 0

    def read(self, handle, endpoint, buffer, length, output, overlapped):
        return self._start(buffer, length, overlapped)

    def write(self, handle, endpoint, buffer, length, output, overlapped):
        return self._start(buffer, length, overlapped, C.string_at(buffer, length))

    def control(self, handle, setup, buffer, length, output, overlapped):
        self.calls.append(("control", (setup.RequestType, setup.Index, C.string_at(buffer, length))))
        return self._start(buffer, length, overlapped, C.string_at(buffer, length))

    def poll(self, handle, overlapped, transferred, wait):
        assert handle.value == 2002 and not wait
        item = self.requests[C.addressof(overlapped._obj)]
        if item["state"] == "pending":
            self.error = usb.ERROR_IO_INCOMPLETE
            return 0
        if item["state"] == "cancelled":
            self.error = usb.ERROR_OPERATION_ABORTED
            self.calls.append(("drained-cancel", C.addressof(overlapped._obj)))
            return 0
        raw = item["raw"] or b""
        transferred._obj.value = len(raw)
        if raw:
            C.memmove(item["buffer"], raw, len(raw))
        return 1

    def cancel(self, handle, overlapped):
        assert handle == 1001 and overlapped is not None
        address = C.addressof(overlapped._obj)
        self.cancelled.append((handle, address))
        if self.stuck:
            return 1
        if self.cancel_race:
            self.requests[address]["state"] = "done"
            self.error = usb.ERROR_NOT_FOUND
            return 0
        self.requests[address]["state"] = "cancelled"
        return 1

    def wait(self, count, handles, wait_all, milliseconds):
        if self.wait_failed:
            self.error = 6
            return usb.WAIT_FAILED
        return usb.WAIT_TIMEOUT

    def close(self, handle):
        self.closed.append(handle)
        return 1

    def free(self, handle):
        self.calls.append(("free", handle.value))
        self.freed.append(handle.value)
        return 1


class NativeLifecycleTests(unittest.TestCase):
    def native(self, shim=None):
        shim = shim or NativeShim()
        patcher = patch.object(usb.C, "get_last_error", lambda: shim.error, create=True)
        patcher.start(); self.addCleanup(patcher.stop)
        return usb._WinUsbDevice(shim, DEVICE), shim

    def test_windows_abi_sizes_and_no_non_windows_dll_load(self):
        self.assertEqual(C.sizeof(usb._UsbInterfaceDescriptor), 9)
        self.assertEqual(C.sizeof(usb._SetupPacket), 8)
        self.assertEqual(C.sizeof(usb._PipeInformation), 12)
        self.assertEqual(C.sizeof(usb._Overlapped), 32 if C.sizeof(C.c_void_p) == 8 else 20)
        with patch.object(usb.os, "name", "posix"), self.assertRaises(OSError):
            usb._WinUsbApi()

    def test_pending_read_completion_releases_event_only_after_result(self):
        device, shim = self.native()
        operation = device.start_read(0x81, 272)
        event = operation.event
        self.assertIsNone(device.poll(operation))
        self.assertNotIn(event, shim.closed)
        address = C.addressof(operation.overlapped)
        shim.requests[address].update(state="done", raw=EVENT[1:])
        self.assertEqual(device.poll(operation), EVENT[1:])
        self.assertIn(event, shim.closed)
        self.assertEqual(device.poll(operation), EVENT[1:])
        self.assertEqual(shim.closed.count(event), 1)
        device.close()
        self.assertEqual(shim.cancelled, [])

    def test_nonzero_active_alternate_is_rejected_without_switching(self):
        shim = NativeShim()
        shim.current_alternate = 1
        with self.assertRaises(usb.UsbHciProtocolError):
            self.native(shim)
        self.assertEqual(shim.requests, {})
        self.assertEqual(shim.freed, [2002])
        self.assertIn(1001, shim.closed)

    def test_close_cancels_exact_owned_overlapped_and_drains_before_free(self):
        device, shim = self.native()
        first, second = device.start_read(0x81, 272), device.start_read(0x82, 1088)
        addresses = {C.addressof(first.overlapped), C.addressof(second.overlapped)}
        device.close(); device.close()
        self.assertEqual({address for handle, address in shim.cancelled}, addresses)
        self.assertTrue(all(handle == 1001 for handle, address in shim.cancelled))
        self.assertEqual([kind for kind, value in shim.calls[-3:]], ["drained-cancel", "drained-cancel", "free"])
        self.assertEqual(shim.freed, [2002])
        self.assertEqual(shim.closed.count(1001), 1)
        self.assertEqual(shim.calls[0][1][2], 0)  # exclusive CreateFile sharing
        self.assertTrue(shim.calls[0][1][5] & 0x40000000)

    def test_cancel_race_with_normal_completion_is_drained_safely(self):
        device, shim = self.native()
        operation = device.start_read(0x81, 272)
        shim.cancel_race = True
        device.close()
        self.assertTrue(operation.complete)
        self.assertEqual(shim.freed, [2002])

    def test_immediate_usb_control_completion_uses_actual_overlapped_result(self):
        device, shim = self.native()
        shim.immediate = True
        operation = device.start_control_write(usb.UsbSetup(0x20, 0, 0, 0, 3), RESET[1:])
        self.assertEqual(device.poll(operation), 3)
        self.assertEqual(shim.calls[-1], ("control", (0x20, 0, RESET[1:])))
        self.assertEqual(len(device._operations), 0)
        device.close()
        self.assertEqual(shim.cancelled, [])

    def test_stuck_driver_and_wait_failure_quarantine_live_memory(self):
        for wait_failed in (False, True):
            with self.subTest(wait_failed=wait_failed):
                device, shim = self.native()
                operation = device.start_read(0x81, 272)
                shim.stuck, shim.wait_failed = True, wait_failed
                previous = len(usb._QUARANTINED_DEVICES)
                with patch.object(usb, "_CANCEL_DRAIN_SECONDS", 0.001), self.assertRaises(usb.UsbHciError):
                    device.close()
                self.assertEqual(len(usb._QUARANTINED_DEVICES), previous + 1)
                self.assertIs(usb._QUARANTINED_DEVICES[-1], device)
                self.assertEqual(shim.freed, [])
                self.assertNotIn(operation.event, shim.closed)
                self.assertNotIn(1001, shim.closed)
                # This is only fake memory/handles; remove our own fake lease.
                usb._QUARANTINED_DEVICES.pop()

    def test_foreign_requests_are_not_waited_or_cancelled(self):
        device, shim = self.native()
        foreign = usb._Operation(3333, 10)
        with self.assertRaises(usb.UsbHciError):
            device.poll(foreign)
        with self.assertRaises(usb.UsbHciError):
            device.wait([foreign], 0)
        device.close()
        self.assertEqual(shim.cancelled, [])

    def test_controller_lease_releases_after_native_handles_and_on_clean_setup_failure(self):
        shim = NativeShim()
        events = []
        lease = SimpleNamespace(close=lambda: events.append(("lease-close", list(shim.freed), list(shim.closed))), descriptor={})
        device = usb._WinUsbDevice(shim, DEVICE, identity={"instanceID": "USB\\OWNED\\A"}, lease=lease)
        device.close()
        self.assertEqual(events, [("lease-close", [2002], [1001])])
        shim = NativeShim()
        shim.current_alternate = 1
        events.clear()
        with self.assertRaises(usb.UsbHciProtocolError):
            usb._WinUsbDevice(shim, DEVICE, lease=lease)
        self.assertEqual(events, [("lease-close", [2002], [1001])])

    def test_failed_free_close_or_cancel_retains_lease_without_retry(self):
        for phase in ("free", "handle", "cancel"):
            with self.subTest(phase=phase):
                shim = NativeShim()
                patcher = patch.object(usb.C, "get_last_error", lambda: shim.error, create=True)
                patcher.start()
                released = []
                lease = SimpleNamespace(close=lambda: released.append(True), descriptor={"exclusiveLease": True})
                device = usb._WinUsbDevice(shim, DEVICE, lease=lease)
                if phase == "free":
                    shim.usb.WinUsb_Free = lambda handle: 0
                elif phase == "handle":
                    shim.kernel.CloseHandle = lambda handle: 0
                else:
                    device.start_read(0x81, 272)
                    shim.stuck = True
                start = len(usb._QUARANTINED_DEVICES)
                try:
                    with patch.object(usb, "_CANCEL_DRAIN_SECONDS", 0.001), self.assertRaises((OSError, usb.UsbHciError)):
                        device.close()
                    self.assertEqual(released, [])
                    self.assertIs(usb._QUARANTINED_DEVICES[-1], device)
                    self.assertEqual(len(usb._QUARANTINED_DEVICES), start + 1)
                    device.close()  # Terminal native device does no second release.
                    self.assertEqual(released, [])
                finally:
                    del usb._QUARANTINED_DEVICES[start:]
                    patcher.stop()

    def test_setup_failure_with_failed_cleanup_preserves_lease_evidence(self):
        shim = NativeShim()
        shim.current_alternate = 1
        shim.usb.WinUsb_Free = lambda handle: 0
        lease = SimpleNamespace(close=lambda: self.fail("must retain lease"), descriptor={"exclusiveLease": True})
        start = len(usb._QUARANTINED_DEVICES)
        with patch.object(usb.C, "get_last_error", lambda: 6, create=True):
            try:
                with self.assertRaises(usb.UsbHciError) as error:
                    usb._WinUsbDevice(shim, DEVICE, identity={"instanceID": "USB\\OWNED\\A"}, lease=lease)
                self.assertTrue(error.exception.raw_cleanup["resourcesQuarantined"])
                self.assertFalse(error.exception.raw_cleanup["controllerClosed"])
                self.assertEqual(len(usb._QUARANTINED_DEVICES), start + 1)
            finally:
                del usb._QUARANTINED_DEVICES[start:]

    def test_descriptor_inspection_reports_released_lease_after_native_close(self):
        class OwnedLease:
            closed = False
            @property
            def descriptor(self):
                return {"exclusiveLease": not self.closed}
            def close(self):
                self.closed = True
        owned = OwnedLease()
        shim = NativeShim()
        provider = SimpleNamespace(open=lambda path: usb._WinUsbDevice(shim, path, lease=owned))
        result = usb.inspect_winusb_device(DEVICE, api=provider)
        self.assertTrue(result["transportClosed"])
        self.assertFalse(result["exclusiveLease"]["exclusiveLease"])
        self.assertEqual(shim.requests, {})


if __name__ == "__main__":
    unittest.main()
