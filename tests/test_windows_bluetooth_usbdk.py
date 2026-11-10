"""UsbDk metadata/ABI, H4 mapping, and owned lifecycle using fixtures only.

No installed driver, redirect, native helper DLL, radio, or external USB device
is used by these tests, including when this suite runs on Windows.
"""
from collections import deque
import ctypes as C
import importlib.util
import io
from pathlib import Path
import struct
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[1] / "engine/project/backend/src"
sys.path.insert(0, str(SOURCE))
import windows_bluetooth_usbdk as dk


INSTANCE = r"USB\VID_1234&PID_5678\EXPLICIT-OWNED-FIXTURE"
CONFIGURATION = bytes.fromhex(
    "09 02 27 00 01 01 00 80 32 "
    "09 04 00 00 03 e0 01 01 00 "
    "07 05 81 03 10 00 01 "
    "07 05 82 02 40 00 00 "
    "07 05 02 02 40 00 00")
RESET = bytes.fromhex("01 03 0c 00")
EVENT = bytes.fromhex("04 0e 04 01 03 0c 00")
ACL = bytes.fromhex("02 01 20 05 00 68 65 6c 6c 6f")


def native_row(instance=INSTANCE):
    value = dk.DeviceInfo()
    device, suffix = instance.rsplit("\\", 1)
    for field, text in (("DeviceID", device), ("InstanceID", suffix)):
        raw = text.encode("utf-16le") + b"\0\0"
        C.memmove(C.addressof(value.ID) + getattr(dk.DeviceID, field).offset, raw, len(raw))
    value.FilterID, value.Port, value.Speed = 9, 3, 2
    descriptor = value.DeviceDescriptor
    descriptor.Length, descriptor.DescriptorType = 18, 1
    descriptor.VendorID, descriptor.ProductID, descriptor.DeviceClass = 0x1234, 0x5678, 0xe0
    descriptor.NumConfigurations = 1
    return {"instanceID": instance, "native": value, "vendorID": 0x1234, "productID": 0x5678,
            "deviceClass": 0xe0, "configurationCount": 1}


def pe_fixture(exports=dk.REQUIRED_EXPORTS):
    data = bytearray(4096)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3c, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HH", data, 0x84, 0x8664, 1)
    struct.pack_into("<HH", data, 0x94, 240, 0x2000)
    optional = 0x98
    struct.pack_into("<H", data, optional, 0x20b)
    struct.pack_into("<I", data, optional + 108, 16)
    struct.pack_into("<II", data, optional + 112, 0x1000, 0x600)
    section = optional + 240
    struct.pack_into("<IIII", data, section + 8, 0x800, 0x1000, 0x800, 0x200)
    struct.pack_into("<I", data, 0x200 + 24, len(exports))
    struct.pack_into("<I", data, 0x200 + 32, 0x1100)
    location = 0x400
    for index, name in enumerate(exports):
        struct.pack_into("<I", data, 0x300 + index * 4, location - 0x200 + 0x1000)
        raw = name.encode("ascii") + b"\0"
        data[location:location + len(raw)] = raw
        location += len(raw)
    return bytes(data)


class FakeLease:
    def __init__(self, log):
        self.log, self.closed = log, False

    @property
    def descriptor(self):
        return {"controllerInstanceID": INSTANCE, "exclusiveLease": not self.closed}

    def close(self):
        self.log.append("lease.close")
        self.closed = True


class FakeNative:
    def __init__(self):
        self.helper = self
        self.kernel = self
        self.error = 0
        self.log, self.operations, self.writes, self.cancelled, self.closed_events = [], {}, [], [], []
        self.event = 1000
        self.stop_ok, self.start_ok, self.cancel_completes = True, True, True
        self.short_write, self.usbd_status = False, 0
        self.system_value = 0x2222
        self.lease = FakeLease(self.log)

    def UsbDk_StartRedirect(self, value):
        self.log.append("redirect.start")
        self.redirected_id = dk._identity(value._obj)
        return 0x1111 if self.start_ok else C.c_void_p(-1).value

    def UsbDk_GetRedirectorSystemHandle(self, handle):
        self.log.append(("redirect.system", handle))
        return self.system_value

    def UsbDk_StopRedirect(self, handle):
        self.log.append(("redirect.stop", handle))
        return self.stop_ok

    def CreateEventW(self, *_):
        self.event += 1
        return self.event

    def CloseHandle(self, handle):
        self.closed_events.append(handle)
        return True

    def _transfer(self, read, request, overlap):
        request, overlap = request._obj, overlap._obj
        address = C.addressof(overlap)
        self.operations[address] = {"read": read, "request": request, "overlap": overlap,
                                    "done": not read, "cancelled": False}
        if read:
            return 2
        payload = C.string_at(request.Buffer, request.BufferLength)
        self.writes.append((request.EndpointAddress, request.TransferType, payload))
        request.Result.GenResult.BytesTransferred = request.BufferLength - (8 if request.TransferType == 0 else 0) - int(self.short_write)
        request.Result.GenResult.UsbdStatus = self.usbd_status
        return 1

    def UsbDk_WritePipe(self, handle, request, overlap):
        self.assert_redirect(handle)
        return self._transfer(False, request, overlap)

    def UsbDk_ReadPipe(self, handle, request, overlap):
        self.assert_redirect(handle)
        return self._transfer(True, request, overlap)

    def assert_redirect(self, handle):
        if handle != 0x1111:
            raise AssertionError("Transfer used a system handle instead of the opaque redirect handle")

    def GetOverlappedResult(self, handle, overlap, _, wait):
        if handle != self.system_value or wait:
            raise AssertionError("Completion used an opaque handle or blocked")
        row = self.operations[C.addressof(overlap._obj)]
        if row["cancelled"]:
            self.error = dk.usb.ERROR_OPERATION_ABORTED
            return False
        if not row["done"]:
            self.error = dk.usb.ERROR_IO_INCOMPLETE
            return False
        return True

    def CancelIoEx(self, handle, overlap):
        if handle != self.system_value:
            raise AssertionError("Cancellation used the opaque redirect handle")
        address = C.addressof(overlap._obj)
        self.cancelled.append((handle, address))
        if self.cancel_completes:
            self.operations[address]["cancelled"] = True
        return True

    def WaitForMultipleObjects(self, count, handles, all_, timeout):
        return dk.usb.WAIT_TIMEOUT

    def deliver(self, endpoint, data):
        for row in self.operations.values():
            request = row["request"]
            if row["read"] and not row["done"] and not row["cancelled"] and request.EndpointAddress == endpoint:
                if len(data) > request.BufferLength:
                    raise AssertionError("Fixture data exceeds read buffer")
                C.memmove(request.Buffer, data, len(data))
                request.Result.GenResult.BytesTransferred = len(data)
                row["done"] = True
                return
        raise AssertionError("No armed fixture read")

    def close(self):
        self.log.append("api.close")

    def device(self):
        interface, pipes = dk.parse_configuration(CONFIGURATION)
        return dk._RedirectedDevice(self, native_row(), {"instanceID": INSTANCE}, interface, pipes, self.lease)


class MetadataTests(unittest.TestCase):
    def test_static_abi_matches_official_x64_headers(self):
        result = dk.abi_layout()
        self.assertEqual({key: result[key] for key in ("wcharBytes", "deviceIDBytes", "deviceDescriptorBytes", "deviceInfoBytes",
                                                       "configurationRequestBytes", "transferRequestBytes", "transferResultOffset")},
                         {"wcharBytes": 2, "deviceIDBytes": 800, "deviceDescriptorBytes": 18, "deviceInfoBytes": 848,
                          "configurationRequestBytes": 808, "transferRequestBytes": 72, "transferResultOffset": 48})
        self.assertFalse(result["nativeABIExercised"])

    def test_utf16_identity_requires_bounded_exact_usb_devnode(self):
        self.assertEqual(dk._identity(native_row()["native"].ID), INSTANCE)
        for value in (None, "", "USB\\x", "PCI\\x\\y", INSTANCE + "\\extra", INSTANCE + "\0"):
            with self.subTest(value=value), self.assertRaises((ValueError, TypeError)):
                dk.UsbDkHciTransport.open(value, api=SimpleNamespace(open=lambda _: self.fail("Opened invalid selector")))
        value = dk.DeviceID()
        for index in range(200):
            value.DeviceID[index] = ord("x")
        with self.assertRaises(dk.UsbDkError):
            dk._identity(value)

    def test_configuration_accepts_hci_and_rejects_wrong_class_counts_and_bounds(self):
        interface, pipes = dk.parse_configuration(CONFIGURATION)
        self.assertEqual((interface.number, interface.device_class), (0, 0xe0))
        self.assertEqual([value.address for value in pipes], [0x81, 0x82, 2])
        invalid = [b"", CONFIGURATION[:-1], CONFIGURATION + b"\0", CONFIGURATION[:18] + b"\0\x05" + CONFIGURATION[20:]]
        for position, value in ((4, 2), (13, 2), (14, 0xff), (21, 2), (22, 0), (19, 0)):
            data = bytearray(CONFIGURATION)
            data[position] = value
            invalid.append(bytes(data))
        for data in invalid:
            with self.subTest(data=data.hex()), self.assertRaises(dk.usb.UsbHciProtocolError):
                dk.parse_configuration(data)

    def test_export_inspection_reads_metadata_without_loading_code(self):
        result = dk._inspect_pe_exports(pe_fixture())
        self.assertEqual(result["machine"], "AMD64")
        self.assertEqual(result["requiredExportsPresent"], list(dk.REQUIRED_EXPORTS))
        self.assertFalse(result["nativeABIExercised"])
        for data in (b"MZ", pe_fixture(dk.REQUIRED_EXPORTS[:-1]), pe_fixture()[:0x400]):
            with self.subTest(length=len(data)), self.assertRaises(dk.UsbDkError):
                dk._inspect_pe_exports(data)
        wrong = bytearray(pe_fixture())
        struct.pack_into("<H", wrong, 0x84, 0xaa64)
        with self.assertRaises(dk.UsbDkError):
            dk._inspect_pe_exports(bytes(wrong))

    def test_bluetooth_sco_interface_is_not_mistaken_for_second_acl_interface(self):
        # Bluetooth USB has an additional E0/01/01 SCO interface whose alt0
        # isochronous endpoints may have zero maximum packet size.
        other = bytes.fromhex("09 04 01 00 02 e0 01 01 00 07 05 83 01 00 00 01 07 05 03 01 00 00 01 "
                              "09 04 01 01 02 e0 01 01 00 07 05 83 01 09 00 01 07 05 03 01 09 00 01")
        data = bytearray(CONFIGURATION + other)
        data[2:4] = len(data).to_bytes(2, "little")
        data[4] = 2
        interface, pipes = dk.parse_configuration(bytes(data))
        self.assertEqual(interface.number, 0)
        self.assertEqual([pipe.address for pipe in pipes], [0x81, 0x82, 2])

    def test_nonwindows_software_inspection_does_not_load_or_enumerate(self):
        with patch.object(dk.os, "name", "posix"), patch.object(dk, "_VerifiedHelper") as verify, patch.object(dk, "_UsbDkApi") as api:
            result = dk.inspect_backend()
        verify.assert_not_called()
        api.assert_not_called()
        self.assertFalse(result["platformSupported"])
        self.assertFalse(result["dllLoaded"])
        self.assertEqual(result["hciCommandsSent"], 0)

    def test_absent_installed_helper_does_not_load_fallback_or_redirect(self):
        helper = Path("fixture-installed/UsbDkHelper.dll")
        with patch.object(dk.os, "name", "nt"), patch.object(dk, "_known_helper", return_value=helper), \
                patch.object(dk, "_VerifiedHelper", side_effect=FileNotFoundError("not installed")), patch.object(dk, "_UsbDkApi") as native:
            result = dk.inspect_backend()
        native.assert_not_called()
        self.assertFalse(result["available"])
        self.assertFalse(result["redirectStarted"])
        self.assertIn("not installed", result["reason"])

    def test_software_inspection_verifies_signature_and_pe_but_does_not_load_dll(self):
        verified = SimpleNamespace(path=SimpleNamespace(read_bytes=lambda: pe_fixture()), close=lambda: None)
        helper = Path("fixture-installed/UsbDkHelper.dll")
        with patch.object(dk.os, "name", "nt"), patch.object(dk, "_known_helper", return_value=helper), \
                patch.object(dk, "_VerifiedHelper", return_value=verified), patch.object(dk, "_UsbDkApi") as native:
            result = dk.inspect_backend()
        native.assert_not_called()
        self.assertTrue(result["available"])
        self.assertTrue(result["installedHelperTrusted"])
        self.assertFalse(result["dllLoaded"])
        self.assertTrue(result["freshRevocationNotProven"])
        self.assertFalse(result["abi"]["nativeABIExercised"])

    def test_metadata_listing_never_redirects_and_retains_exact_identity(self):
        calls = []
        provider = SimpleNamespace(devices=lambda: [native_row()], configuration=lambda row: CONFIGURATION,
                                   open=lambda _: self.fail("Metadata listing redirected a controller"))
        with patch.object(dk, "_resolve_instance", side_effect=lambda identity: calls.append(identity) or {"instanceID": identity}):
            result = dk.list_controllers(api=provider)
        self.assertEqual(calls, [INSTANCE])
        self.assertEqual(result["devices"][0]["instanceID"], INSTANCE)
        self.assertFalse(result["redirectStarted"])
        self.assertEqual(result["hciCommandsSent"], 0)


class LifecycleTests(unittest.TestCase):
    def setUp(self):
        self.api = FakeNative()
        self.errors = patch.object(dk.C, "get_last_error", side_effect=lambda: self.api.error, create=True)
        self.errors.start()
        self.addCleanup(self.errors.stop)
        # Only fake objects can enter quarantine from this fixture suite.
        self.addCleanup(dk._QUARANTINED_DEVICES.clear)

    def transport(self):
        device = self.api.device()
        value = dk.UsbDkHciTransport.open(INSTANCE, api=SimpleNamespace(open=lambda instance: device))
        self.addCleanup(lambda: value.close() if not device._close_error else None)
        return value, device

    def test_command_uses_setup_plus_data_and_acl_bulk_excludes_h4(self):
        value, device = self.transport()
        value.send_h4(RESET)
        value.send_h4(ACL)
        self.assertEqual(self.api.writes, [(0, 0, bytes.fromhex("20 00 00 00 00 00 03 00") + RESET[1:]),
                                          (2, 1, ACL[1:])])
        self.assertEqual(value.capabilities["backend"], "windows-usbdk-hci")
        self.assertFalse(value.capabilities["usesWinUsbAPI"])
        self.assertFalse(value.capabilities["sco"])
        self.assertFalse(value.capabilities["iso"])
        self.assertFalse(value.capabilities["physicalRadioVerified"])

    def test_duplex_reads_preserve_fragmentation_and_add_h4_only_at_guest_boundary(self):
        value, device = self.transport()
        self.assertIsNone(value.receive(0))
        first = dict(device._operations)
        self.assertIsNone(value.receive(0))
        self.assertEqual(first, device._operations)
        self.api.deliver(0x81, EVENT[1:3])
        self.assertIsNone(value.receive(0))
        self.api.deliver(0x82, ACL[1:])
        self.api.deliver(0x81, EVENT[3:])
        received = {value.receive(0), value.receive(0)}
        self.assertEqual(received, {EVENT, ACL})

    def test_h4_sco_iso_event_tx_and_short_packets_are_rejected_without_native_io(self):
        value, _ = self.transport()
        for packet in (EVENT, b"\x03\0\0\0", b"\x05\0\0\0\0", RESET[:-1], ACL[:-1]):
            with self.subTest(packet=packet), self.assertRaises(dk.usb.UsbHciProtocolError):
                value.send_h4(packet)
        self.assertEqual(self.api.writes, [])

    def test_cancel_uses_exact_overlapped_system_handle_then_stop_then_lease_release(self):
        value, device = self.transport()
        value.receive(0)
        requests = set(self.api.operations)
        value.close()
        self.assertEqual({address for handle, address in self.api.cancelled}, requests)
        self.assertTrue(all(handle == 0x2222 for handle, _ in self.api.cancelled))
        self.assertNotIn(0x1111, self.api.closed_events)
        self.assertNotIn(0x2222, self.api.closed_events)
        stop = self.api.log.index(("redirect.stop", 0x1111))
        self.assertLess(stop, self.api.log.index("api.close"))
        self.assertLess(stop, self.api.log.index("lease.close"))
        self.assertTrue(self.api.lease.closed)
        value.close()
        self.assertEqual(self.api.log.count(("redirect.stop", 0x1111)), 1)

    def test_failed_drain_keeps_buffers_redirect_and_lease_quarantined_without_retry(self):
        value, device = self.transport()
        value.receive(0)
        self.api.cancel_completes = False
        with patch.object(dk.usb, "_CANCEL_DRAIN_SECONDS", 0), self.assertRaises(dk.UsbDkError) as caught:
            value.close()
        self.assertTrue(caught.exception.raw_cleanup["resourcesQuarantined"])
        self.assertFalse(self.api.lease.closed)
        self.assertEqual(len(device._operations), 2)
        self.assertIn(device, dk._QUARANTINED_DEVICES)
        self.assertNotIn(("redirect.stop", 0x1111), self.api.log)
        with self.assertRaises(dk.usb.UsbHciError):
            value.close()
        self.assertEqual(len(self.api.cancelled), 2)

    def test_stop_failure_keeps_exact_controller_lease_and_does_not_retry(self):
        value, device = self.transport()
        self.api.stop_ok = False
        with self.assertRaises(dk.UsbDkError):
            value.close()
        self.assertFalse(self.api.lease.closed)
        self.assertIn(device, dk._QUARANTINED_DEVICES)
        with self.assertRaises(dk.usb.UsbHciError):
            value.close()
        self.assertEqual(self.api.log.count(("redirect.stop", 0x1111)), 1)

    def test_failed_start_has_uncertain_outcome_and_holds_lease(self):
        self.api.start_ok = False
        with self.assertRaises(dk.UsbDkError) as caught:
            self.api.device()
        self.assertTrue(caught.exception.raw_cleanup["resourcesQuarantined"])
        self.assertFalse(self.api.lease.closed)
        self.assertEqual(len(dk._QUARANTINED_DEVICES), 1)
        self.assertEqual(self.api.closed_events, [])

    def test_missing_system_handle_stops_owned_redirect_before_releasing_lease(self):
        self.api.system_value = None
        with self.assertRaises(dk.UsbDkError):
            self.api.device()
        self.assertIn(("redirect.stop", 0x1111), self.api.log)
        self.assertTrue(self.api.lease.closed)

    def test_short_write_is_terminal_without_resending(self):
        value, device = self.transport()
        self.api.short_write = True
        with self.assertRaises(dk.usb.UsbHciProtocolError):
            value.send_h4(RESET)
        with self.assertRaises(dk.usb.UsbHciError):
            value.send_h4(RESET)
        self.assertEqual(len(self.api.writes), 1)
        self.assertTrue(self.api.lease.closed)

    def test_usbd_status_failure_is_not_a_successful_hci_ack(self):
        value, _ = self.transport()
        self.api.usbd_status = 0xc0000001
        with self.assertRaises(dk.UsbDkError):
            value.send_h4(RESET)
        self.assertEqual(len(self.api.writes), 1)
        self.assertTrue(self.api.lease.closed)

    def test_lease_is_acquired_by_resolved_pnp_id_before_redirect_and_second_open_is_denied(self):
        provider = object.__new__(dk._UsbDkApi)
        provider.helper, provider.kernel, provider.close = self.api, self.api, self.api.close
        provider.devices, provider.configuration = lambda: [native_row()], lambda _: CONFIGURATION
        acquired = []
        def acquire(identity):
            acquired.append(identity)
            if len(acquired) > 1:
                raise dk.UsbDkError("busy fixture lease")
            self.api.log.append("lease.acquire")
            return self.api.lease
        with patch.object(dk, "_resolve_instance", side_effect=lambda identity: {"instanceID": INSTANCE}), \
                patch.object(dk.ControllerInstanceLease, "acquire", side_effect=acquire):
            device = provider.open(INSTANCE.lower())
            with self.assertRaises(dk.UsbDkError):
                provider.open(INSTANCE)
        self.assertEqual(acquired, [INSTANCE, INSTANCE])
        self.assertLess(self.api.log.index("lease.acquire"), self.api.log.index("redirect.start"))
        self.assertEqual(self.api.log.count("redirect.start"), 1)
        device.close()

    def test_identity_change_after_lease_releases_without_redirect(self):
        provider = object.__new__(dk._UsbDkApi)
        provider.helper, provider.kernel = self.api, self.api
        changed = native_row()
        changed["native"].Port += 1
        rows = deque(([native_row()], [changed]))
        provider.devices, provider.configuration = lambda: rows.popleft(), lambda _: CONFIGURATION
        with patch.object(dk, "_resolve_instance", return_value={"instanceID": INSTANCE}), \
                patch.object(dk.ControllerInstanceLease, "acquire", return_value=self.api.lease), self.assertRaises(dk.UsbDkError):
            provider.open(INSTANCE)
        self.assertTrue(self.api.lease.closed)
        self.assertNotIn("redirect.start", self.api.log)


if __name__ == "__main__":
    unittest.main()
