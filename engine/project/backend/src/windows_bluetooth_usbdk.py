"""Explicit UsbDk Bluetooth HCI transport; never installs a driver or trust root.

The only redirect entry is UsbDkHciTransport.open(exact_pnp_instance_id).
Software inspection does not load UsbDk, enumerate USB, redirect, or send HCI.
Production accepts only a signature-verified installed helper in ProgramFiles;
there is no PATH/cwd, extracted-DLL, WinUSB, or TCP fallback.

ABI: daynix/UsbDk v1.00-22 UsbDk/UsbDkData.h and
UsbDkHelper/UsbDkHelper.h. Control buffers contain the eight-byte setup packet
followed by payload. The redirector HANDLE is an opaque helper object, NOT a
Windows file handle: only GetRedirectorSystemHandle may be used for CancelIoEx.
SCO/ISO support and physical controller/RF operation are not established.
"""
from __future__ import annotations

import ctypes as C
from pathlib import Path
import math
import os
import struct
import time
import uuid

if __package__:
    from . import windows_bluetooth_usb as usb
    from .windows_bluetooth_lease import ControllerInstanceLease, controller_key
else:
    import windows_bluetooth_usb as usb
    from windows_bluetooth_lease import ControllerInstanceLease, controller_key


SOURCE_VERSION = "daynix/UsbDk v1.00-22"
MAX_DEVICES = 512
MAX_CONFIGURATION = 65535
_QUARANTINED_DEVICES = []
REQUIRED_EXPORTS = (
    "UsbDk_GetDevicesList", "UsbDk_ReleaseDevicesList",
    "UsbDk_GetConfigurationDescriptor", "UsbDk_ReleaseConfigurationDescriptor",
    "UsbDk_StartRedirect", "UsbDk_StopRedirect", "UsbDk_GetRedirectorSystemHandle",
    "UsbDk_ReadPipe", "UsbDk_WritePipe")


class UsbDkError(usb.UsbHciError):
    pass


# WCHAR is always UTF-16 on Windows, including when ABI tests run on macOS.
class DeviceID(C.Structure):
    _fields_ = [("DeviceID", C.c_uint16 * 200), ("InstanceID", C.c_uint16 * 200)]


class DeviceDescriptor(C.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("Length", C.c_uint8), ("DescriptorType", C.c_uint8),
                ("UsbVersion", C.c_uint16), ("DeviceClass", C.c_uint8),
                ("DeviceSubClass", C.c_uint8), ("DeviceProtocol", C.c_uint8),
                ("MaximumPacketSize", C.c_uint8), ("VendorID", C.c_uint16),
                ("ProductID", C.c_uint16), ("DeviceVersion", C.c_uint16),
                ("Manufacturer", C.c_uint8), ("Product", C.c_uint8),
                ("SerialNumber", C.c_uint8), ("NumConfigurations", C.c_uint8)]


class DeviceInfo(C.Structure):
    _fields_ = [("ID", DeviceID), ("FilterID", C.c_uint64), ("Port", C.c_uint64),
                ("Speed", C.c_uint64), ("DeviceDescriptor", DeviceDescriptor)]


class ConfigurationRequest(C.Structure):
    _fields_ = [("ID", DeviceID), ("Index", C.c_uint64)]


class GeneralResult(C.Structure):
    _fields_ = [("BytesTransferred", C.c_uint64), ("UsbdStatus", C.c_uint64)]


class TransferResult(C.Structure):
    _fields_ = [("GenResult", GeneralResult), ("IsochronousResultsArray", C.c_uint64)]


class TransferRequest(C.Structure):
    _fields_ = [("EndpointAddress", C.c_uint64), ("Buffer", C.c_uint64),
                ("BufferLength", C.c_uint64), ("TransferType", C.c_uint64),
                ("IsochronousPacketsArraySize", C.c_uint64),
                ("IsochronousPacketsArray", C.c_uint64), ("Result", TransferResult)]


def abi_layout():
    """Static public-header layout, not proof that a native DLL was exercised."""
    return {"sourceVersion": SOURCE_VERSION, "processPointerBytes": C.sizeof(C.c_void_p),
            "wcharBytes": C.sizeof(C.c_uint16), "deviceIDBytes": C.sizeof(DeviceID),
            "deviceDescriptorBytes": C.sizeof(DeviceDescriptor),
            "deviceInfoBytes": C.sizeof(DeviceInfo),
            "configurationRequestBytes": C.sizeof(ConfigurationRequest),
            "transferRequestBytes": C.sizeof(TransferRequest),
            "transferResultOffset": TransferRequest.Result.offset,
            "overlappedBytes": C.sizeof(usb._Overlapped),
            "nativeABIExercised": False}


def _instance(value):
    controller_key(value)  # Same case-insensitive identity domain as WinUSB.
    if len(value) >= 400 or len(value.split("\\")) != 3:
        raise ValueError("One exact bounded USB PnP instance ID is required")
    return value


def _utf16(array):
    data = bytes(array)
    for offset in range(0, len(data), 2):
        if data[offset:offset + 2] == b"\0\0":
            return data[:offset].decode("utf-16le", errors="strict")
    raise UsbDkError("UsbDk returned an unterminated device identity")


def _identity(device_id):
    device, instance = _utf16(device_id.DeviceID), _utf16(device_id.InstanceID)
    if (not device.casefold().startswith("usb\\") or len(device.split("\\")) != 2 or
            not instance or "\\" in instance):
        raise UsbDkError("UsbDk device identifiers do not represent one USB devnode")
    return _instance(device + "\\" + instance)


def parse_configuration(data):
    """Validate all descriptor boundaries and select exactly one HCI alt-zero."""
    if not isinstance(data, bytes) or not 9 <= len(data) <= MAX_CONFIGURATION:
        raise usb.UsbHciProtocolError("Invalid USB configuration size")
    if data[0:2] != b"\x09\x02" or int.from_bytes(data[2:4], "little") != len(data) or not data[5]:
        raise usb.UsbHciProtocolError("Invalid USB configuration header")
    interfaces, current, candidates, seen = {}, None, [], set()
    offset = 9
    while offset < len(data):
        if offset + 2 > len(data):
            raise usb.UsbHciProtocolError("Truncated USB descriptor header")
        length, kind = data[offset:offset + 2]
        if length < 2 or offset + length > len(data):
            raise usb.UsbHciProtocolError("USB descriptor exceeds its configuration")
        value = data[offset:offset + length]
        if kind == 4:
            if length != 9:
                raise usb.UsbHciProtocolError("Invalid USB interface descriptor")
            key = (value[2], value[3])
            if key in seen:
                raise usb.UsbHciProtocolError("Duplicate USB interface alternate")
            seen.add(key)
            current = {"interface": usb.UsbInterface(value[2], value[3], *value[5:8]),
                       "endpointCount": value[4], "pipes": []}
            interfaces[key] = current
        elif kind == 5:
            if current is None or length < 7:
                raise usb.UsbHciProtocolError("Endpoint without a complete interface")
            packet = int.from_bytes(value[4:6], "little")
            if packet & 0xf800:
                raise usb.UsbHciProtocolError("High bandwidth endpoint encoding is unsupported")
            # UsbDk enum bulk=1/interrupt=2 differs from USBD_PIPE_TYPE used by
            # the shared HCI framing layer: USB attributes map to bulk=2/int=3.
            current["pipes"].append(usb.UsbPipe(value[2], value[3] & 3, packet))
        offset += length
    if len({key[0] for key in interfaces}) != data[4]:
        raise usb.UsbHciProtocolError("USB interface count does not match descriptors")
    for row in interfaces.values():
        if len(row["pipes"]) != row["endpointCount"]:
            raise usb.UsbHciProtocolError("USB endpoint count does not match descriptors")
        interface = row["interface"]
        if (interface.alternate == 0 and
                (interface.device_class, interface.subclass, interface.protocol) == (0xE0, 1, 1)):
            try:
                usb._select_pipes(interface, row["pipes"])
            except usb.UsbHciProtocolError:
                # Bluetooth's second interface can carry only isochronous SCO
                # endpoints with the same E0/01/01 class. Do not mistake it for
                # a second command/event/ACL interface or claim SCO support.
                if row["pipes"] and not all(pipe.kind == 1 for pipe in row["pipes"]):
                    raise
            else:
                candidates.append((interface, row["pipes"]))
    if len(candidates) != 1:
        raise usb.UsbHciProtocolError("Exactly one Bluetooth HCI alternate-zero interface is required")
    interface, pipes = candidates[0]
    usb._select_pipes(interface, pipes)
    return interface, pipes


def _known_helper():
    if os.name != "nt" or C.sizeof(C.c_void_p) != 8:
        raise OSError("UsbDk native backend requires a 64-bit Windows process")
    shell, ole = C.WinDLL("shell32", use_last_error=True), C.WinDLL("ole32", use_last_error=True)
    shell.SHGetKnownFolderPath.argtypes = [C.c_void_p, C.c_uint32, C.c_void_p, C.POINTER(C.c_wchar_p)]
    shell.SHGetKnownFolderPath.restype = C.c_int32
    ole.CoTaskMemFree.argtypes, ole.CoTaskMemFree.restype = [C.c_void_p], None
    # FOLDERID_ProgramFilesX64. No environment override or current-directory DLL.
    guid = C.create_string_buffer(uuid.UUID("6d809377-6af0-444b-8957-a3773f02200e").bytes_le, 16)
    pointer = C.c_wchar_p()
    status = shell.SHGetKnownFolderPath(guid, 0, None, C.byref(pointer))
    if status < 0:
        raise OSError("Resolve Windows ProgramFiles KnownFolder failed")
    try:
        if not pointer.value:
            raise OSError("Windows returned an empty ProgramFiles KnownFolder")
        return Path(pointer.value) / "UsbDk Runtime Library" / "UsbDkHelper.dll"
    finally:
        ole.CoTaskMemFree(C.cast(pointer, C.c_void_p))


def _inspect_pe_exports(data):
    """Read PE metadata without mapping/executing a DLL or resolving imports."""
    if not isinstance(data, bytes) or not 256 <= len(data) <= 64 * 1024 * 1024 or data[:2] != b"MZ":
        raise UsbDkError("Installed helper is not a bounded PE image")
    def number(offset, size):
        if not 0 <= offset <= len(data) - size:
            raise UsbDkError("Truncated installed-helper PE metadata")
        return int.from_bytes(data[offset:offset + size], "little")
    pe = number(0x3c, 4)
    if (data[pe:pe + 4] != b"PE\0\0" or number(pe + 4, 2) != 0x8664 or
            not number(pe + 22, 2) & 0x2000):
        raise UsbDkError("Installed helper must be a native AMD64 PE image")
    count, optional = number(pe + 6, 2), pe + 24
    optional_size = number(pe + 20, 2)
    if (not 1 <= count <= 96 or optional_size < 120 or
            number(optional, 2) != 0x20b or number(optional + 108, 4) < 1):
        raise UsbDkError("Invalid installed-helper PE32+ metadata")
    sections = optional + optional_size
    def offset(rva, size):
        for index in range(count):
            entry = sections + index * 40
            virtual, start = number(entry + 8, 4), number(entry + 12, 4)
            raw, position = number(entry + 16, 4), number(entry + 20, 4)
            if start <= rva and rva + size <= start + min(virtual, raw):
                value = position + rva - start
                if value + size <= len(data):
                    return value
        raise UsbDkError("Installed-helper export RVA is outside file-backed sections")
    export_rva, export_size = number(optional + 112, 4), number(optional + 116, 4)
    if export_size < 40:
        raise UsbDkError("Installed helper has no complete export directory")
    export = offset(export_rva, 40)
    names, names_rva = number(export + 24, 4), number(export + 32, 4)
    if not 1 <= names <= 4096:
        raise UsbDkError("Installed-helper export count is invalid")
    table = offset(names_rva, names * 4)
    found = set()
    for index in range(names):
        string_rva = number(table + index * 4, 4)
        value = bytearray()
        for character in range(256):
            byte = data[offset(string_rva + character, 1)]
            if not byte:
                break
            value.append(byte)
        else:
            raise UsbDkError("Installed-helper export name exceeded its bound")
        try:
            found.add(bytes(value).decode("ascii"))
        except UnicodeError as error:
            raise UsbDkError("Invalid installed-helper export name") from error
    missing = sorted(set(REQUIRED_EXPORTS) - found)
    if missing:
        raise UsbDkError("Installed helper is missing required exports: " + ", ".join(missing))
    return {"machine": "AMD64", "requiredExportsPresent": list(REQUIRED_EXPORTS),
            "exportInspectionOnly": True, "nativeABIExercised": False}


class _TrustFile(C.Structure):
    _fields_ = [("size", C.c_uint32), ("path", C.c_wchar_p),
                ("file", C.c_void_p), ("subject", C.c_void_p)]


class _TrustData(C.Structure):
    _fields_ = [("size", C.c_uint32), ("callback", C.c_void_p), ("sip", C.c_void_p),
                ("ui", C.c_uint32), ("revocation", C.c_uint32), ("choice", C.c_uint32),
                ("info", C.c_void_p), ("stateAction", C.c_uint32), ("state", C.c_void_p),
                ("url", C.c_wchar_p), ("flags", C.c_uint32), ("context", C.c_uint32),
                ("signatureSettings", C.c_void_p)]


class _VerifiedHelper:
    """Hold the exact installed file against replacement during verify/load."""
    def __init__(self):
        self.path, self.handle = _known_helper(), None
        self.kernel = C.WinDLL("kernel32", use_last_error=True)
        ptr, u32 = C.c_void_p, C.c_uint32
        for name, args, result in (
            ("CreateFileW", [C.c_wchar_p, u32, u32, ptr, u32, u32, ptr], ptr),
            ("CloseHandle", [ptr], C.c_int),
            ("GetFileAttributesW", [C.c_wchar_p], u32),
            ("GetFinalPathNameByHandleW", [ptr, C.POINTER(C.c_wchar), u32, u32], u32)):
            function = getattr(self.kernel, name)
            function.argtypes, function.restype = args, result
        for path in (self.path.parent, self.path):
            attributes = self.kernel.GetFileAttributesW(str(path))
            if attributes == 0xffffffff:
                raise FileNotFoundError("Installed UsbDk helper is absent: " + str(self.path))
            if attributes & 0x400:
                raise PermissionError("Installed UsbDk helper cannot be a reparse-point path")
        self.handle = self.kernel.CreateFileW(str(self.path), 0x80000000, 1, None, 3, 0x00200080, None)
        if self.handle in (None, 0, C.c_void_p(-1).value):
            self.handle = None
            raise OSError("Open installed UsbDk helper for signature inspection failed")
        try:
            final = C.create_unicode_buffer(32768)
            length = self.kernel.GetFinalPathNameByHandleW(self.handle, final, len(final), 0)
            if (not length or length >= len(final) or
                    os.path.normcase(final.value.removeprefix("\\\\?\\")) != os.path.normcase(str(self.path))):
                raise PermissionError("Installed UsbDk helper resolved outside its KnownFolder location")
            trust = C.WinDLL("wintrust", use_last_error=True)
            trust.WinVerifyTrust.argtypes = [ptr, ptr, C.POINTER(_TrustData)]
            trust.WinVerifyTrust.restype = C.c_int32
            info = _TrustFile(C.sizeof(_TrustFile), str(self.path), self.handle, None)
            data = _TrustData()
            data.size, data.ui, data.choice = C.sizeof(data), 2, 1
            data.info = C.cast(C.pointer(info), ptr)
            data.stateAction, data.flags = 1, 0x1000 | 0x2000  # Offline cache, disable MD2/MD4; do not disable revocation policy.
            action = C.create_string_buffer(uuid.UUID("00aac56b-cd44-11d0-8cc2-00c04fc295ee").bytes_le, 16)
            try:
                result = trust.WinVerifyTrust(C.c_void_p(-1), action, C.byref(data))
            finally:
                data.stateAction = 2
                trust.WinVerifyTrust(C.c_void_p(-1), action, C.byref(data))
            if result:
                raise PermissionError("Installed UsbDk helper signature was not trusted by unchanged Windows policy: 0x%08x" % (result & 0xffffffff))
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.handle is not None:
            if not self.kernel.CloseHandle(self.handle):
                raise OSError("Close installed helper inspection handle failed")
            self.handle = None


def inspect_backend():
    """Read-only software presence/signature/ABI; never load or redirect USB."""
    result = {"backend": "windows-usbdk-hci", "platformSupported": os.name == "nt" and C.sizeof(C.c_void_p) == 8,
              "inspectionOnly": True, "dllLoaded": False, "deviceOpened": False,
              "redirectStarted": False, "hciCommandsSent": 0, "driverInstallationPerformed": False,
              "available": False, "installedHelperTrusted": False, "abi": abi_layout(),
              "freshRevocationNotProven": True,
              "sco": False, "iso": False, "physicalRadioVerified": False,
              "fullFirmwareBluetooth": False}
    if not result["platformSupported"]:
        return {**result, "reason": "64-bit Windows is required"}
    verified = None
    try:
        result["helperPath"] = str(_known_helper())
        verified = _VerifiedHelper()
        result["helperPE"] = _inspect_pe_exports(verified.path.read_bytes())
        result.update(available=True, installedHelperTrusted=True,
                      signaturePolicy="unchanged-Windows-Authenticode-offline-cache")
    except (OSError, UsbDkError) as error:
        result["reason"] = str(error)
    finally:
        if verified is not None:
            verified.close()
    return result


def _resolve_instance(instance_id):
    """Read actual SetupAPI identity, never parse a device-interface alias."""
    instance_id = _instance(instance_id)
    resolver = usb._RegisteredAzkiInterfaces()
    api = resolver.api
    api.SetupDiOpenDeviceInfoW.argtypes = [C.c_void_p, C.c_wchar_p, C.c_void_p, C.c_uint32, C.POINTER(usb._DeviceInfo)]
    api.SetupDiOpenDeviceInfoW.restype = C.c_int
    handle = api.SetupDiCreateDeviceInfoList(None, None)
    if handle in (None, C.c_void_p(-1).value):
        raise C.WinError(C.get_last_error())
    try:
        info = usb._DeviceInfo()
        info.size = C.sizeof(info)
        if not api.SetupDiOpenDeviceInfoW(handle, instance_id, None, 0, C.byref(info)):
            raise C.WinError(C.get_last_error())
        actual, required = C.create_unicode_buffer(4096), C.c_uint32()
        if not api.SetupDiGetDeviceInstanceIdW(handle, C.byref(info), actual, len(actual), C.byref(required)):
            raise C.WinError(C.get_last_error())
        if actual.value.casefold() != instance_id.casefold():
            raise UsbDkError("SetupAPI did not confirm the exact selected PnP instance")
        return {"instanceID": actual.value, "driver": resolver._property(handle, info, 4),
                "name": resolver._property(handle, info, 12) or resolver._property(handle, info, 0),
                "identitySource": "UsbDk-device-ID-confirmed-by-SetupAPI"}
    finally:
        if not api.SetupDiDestroyDeviceInfoList(handle):
            raise C.WinError(C.get_last_error())


class _UsbDkApi:
    """No installer exports or global driver/filter configuration bindings."""
    def __init__(self):
        self.verified = _VerifiedHelper()
        try:
            _inspect_pe_exports(self.verified.path.read_bytes())
        except BaseException:
            self.verified.close()
            raise
        # DLL_LOAD_DIR + SYSTEM32 only; no PATH/current-directory dependency search.
        try:
            self.helper = C.WinDLL(str(self.verified.path), use_last_error=True, winmode=0x900)
        except BaseException:
            self.verified.close()
            raise
        self.kernel = self.verified.kernel
        ptr, u32, boolean = C.c_void_p, C.c_uint32, C.c_int32
        for name, args, result in (
            ("UsbDk_GetDevicesList", [C.POINTER(C.POINTER(DeviceInfo)), C.POINTER(u32)], boolean),
            ("UsbDk_ReleaseDevicesList", [C.POINTER(DeviceInfo)], None),
            ("UsbDk_GetConfigurationDescriptor", [C.POINTER(ConfigurationRequest), C.POINTER(ptr), C.POINTER(u32)], boolean),
            ("UsbDk_ReleaseConfigurationDescriptor", [ptr], None),
            ("UsbDk_StartRedirect", [C.POINTER(DeviceID)], ptr),
            ("UsbDk_StopRedirect", [ptr], boolean),
            ("UsbDk_GetRedirectorSystemHandle", [ptr], ptr),
            ("UsbDk_ReadPipe", [ptr, C.POINTER(TransferRequest), C.POINTER(usb._Overlapped)], C.c_int32),
            ("UsbDk_WritePipe", [ptr, C.POINTER(TransferRequest), C.POINTER(usb._Overlapped)], C.c_int32)):
            function = getattr(self.helper, name)
            function.argtypes, function.restype = args, result
        for name, args, result in (
            ("CreateEventW", [ptr, boolean, boolean, C.c_wchar_p], ptr),
            ("CancelIoEx", [ptr, C.POINTER(usb._Overlapped)], boolean),
            ("GetOverlappedResult", [ptr, C.POINTER(usb._Overlapped), C.POINTER(u32), boolean], boolean),
            ("WaitForMultipleObjects", [u32, C.POINTER(ptr), boolean, u32], u32)):
            function = getattr(self.kernel, name)
            function.argtypes, function.restype = args, result

    def close(self):
        self.verified.close()

    def devices(self):
        pointer, count = C.POINTER(DeviceInfo)(), C.c_uint32()
        if not self.helper.UsbDk_GetDevicesList(C.byref(pointer), C.byref(count)):
            raise UsbDkError("UsbDk_GetDevicesList failed; installed filter may be unavailable")
        try:
            if count.value > MAX_DEVICES or (count.value and not pointer):
                raise UsbDkError("UsbDk device list exceeded its bound")
            result = []
            for index in range(count.value):
                native = DeviceInfo.from_buffer_copy(bytes(pointer[index]))
                descriptor = native.DeviceDescriptor
                if descriptor.Length != 18 or descriptor.DescriptorType != 1:
                    raise UsbDkError("UsbDk returned an invalid device descriptor")
                result.append({"instanceID": _identity(native.ID), "native": native,
                               "vendorID": descriptor.VendorID, "productID": descriptor.ProductID,
                               "deviceClass": descriptor.DeviceClass,
                               "configurationCount": descriptor.NumConfigurations})
            return result
        finally:
            if pointer:
                self.helper.UsbDk_ReleaseDevicesList(pointer)

    def configuration(self, row):
        if row["configurationCount"] != 1:
            raise UsbDkError("Only an explicit single-configuration Bluetooth controller is supported")
        request = ConfigurationRequest(row["native"].ID, 0)
        pointer, length = C.c_void_p(), C.c_uint32()
        if not self.helper.UsbDk_GetConfigurationDescriptor(C.byref(request), C.byref(pointer), C.byref(length)):
            raise UsbDkError("UsbDk_GetConfigurationDescriptor failed")
        try:
            if not pointer or not 9 <= length.value <= MAX_CONFIGURATION:
                raise UsbDkError("UsbDk configuration exceeded its bound")
            return C.string_at(pointer, length.value)
        finally:
            if pointer:
                self.helper.UsbDk_ReleaseConfigurationDescriptor(pointer)

    def open(self, instance_id):
        instance_id = _instance(instance_id)
        rows = [row for row in self.devices() if row["instanceID"].casefold() == instance_id.casefold()]
        if len(rows) != 1:
            raise UsbDkError("The exact selected USB controller is absent or ambiguous")
        row = rows[0]
        identity = _resolve_instance(instance_id)
        interface, pipes = parse_configuration(self.configuration(row))
        lease = ControllerInstanceLease.acquire(identity["instanceID"])
        try:
            # Recheck bus identity after taking the cross-backend lease, before
            # the first redirect side effect. Never choose the first VID/PID.
            current = [item for item in self.devices() if item["instanceID"].casefold() == instance_id.casefold()]
            def stable(item):
                native = item["native"]
                return (item["instanceID"].casefold(), native.FilterID, native.Port,
                        native.Speed, bytes(native.DeviceDescriptor))
            if len(current) != 1 or stable(current[0]) != stable(row):
                raise UsbDkError("Selected UsbDk controller changed before exclusive open")
            _resolve_instance(instance_id)
        except BaseException:
            lease.close()
            raise
        return _RedirectedDevice(self, row, identity, interface, pipes, lease)


def list_controllers(*, api=None):
    """Read installed-filter metadata only; never redirect/initialize any radio."""
    if api is None and os.name != "nt":
        return {"devices": [], "platformSupported": False, "inspectionOnly": True,
                "redirectStarted": False, "hciCommandsSent": 0}
    provider = api if api is not None else _UsbDkApi()
    result = []
    try:
        for row in provider.devices():
            if row["deviceClass"] not in (0, 0xE0):
                continue
            try:
                interface, pipes = parse_configuration(provider.configuration(row))
                identity = _resolve_instance(row["instanceID"])
            except (OSError, usb.UsbHciError):
                continue
            result.append({**identity, "vendorID": row["vendorID"], "productID": row["productID"],
                           "interfaceNumber": interface.number,
                           "endpoints": {key: pipe.address for key, pipe in usb._select_pipes(interface, pipes).items()}})
    finally:
        if api is None:
            provider.close()
    return {"devices": result, "platformSupported": True, "inspectionOnly": True,
            "redirectStarted": False, "hciCommandsSent": 0, "driverInstallationPerformed": False}


class UsbDkHciTransport(usb.WinUsbHciTransport):
    """Share only H4 framing/duplex scheduling, not the native WinUSB adapter."""
    @classmethod
    def open(cls, instance_id, *, api=None, max_acl_payload=usb.MAX_ACL_PAYLOAD,
             command_recipient=None, io_timeout=5.0):
        instance_id = _instance(instance_id)
        if type(max_acl_payload) is not int or not 1 <= max_acl_payload <= 65535:
            raise ValueError("Invalid ACL payload bound")
        io_timeout = usb._seconds(io_timeout)
        if not io_timeout or command_recipient not in (None, "device", "interface"):
            raise ValueError("Invalid I/O timeout or USB HCI command recipient")
        provider = api if api is not None else _UsbDkApi()
        device = provider.open(instance_id)
        try:
            interface = device.interface()
            selected = usb._select_pipes(interface, device.pipes())
            return cls(device, instance_id, interface, selected, max_acl_payload,
                       command_recipient, io_timeout)
        except BaseException as error:
            _cleanup_failed_open(device, error)
            raise

    @property
    def capabilities(self):
        result = super().capabilities
        result.pop("devicePath", None)
        return {**result, "backend": "windows-usbdk-hci", "controllerInstanceID": self._path,
                "selectorType": "exact-PnP-instance-ID", "usesWinUsbAPI": False,
                "redirectsWindowsDevice": True, "driverInstallationManaged": False,
                "fullFirmwareBluetooth": False, "physicalRadioVerified": False}

    def _terminal_io(self, error):
        self._failed = self._failed or str(error)
        try:
            self.close()
        except BaseException as cleanup:
            failure = UsbDkError(self._failed + "; owned UsbDk cleanup failed: " + str(cleanup))
            if hasattr(cleanup, "raw_cleanup"):
                failure.raw_cleanup = cleanup.raw_cleanup
            raise failure from error


def _quarantine(device, message):
    device._close_error = message
    if not any(value is device for value in _QUARANTINED_DEVICES):
        _QUARANTINED_DEVICES.append(device)
    error = UsbDkError(message)
    error.raw_cleanup = {"controllerIdentity": device.identity,
                         "exclusiveLease": device.lease.descriptor,
                         "resourcesQuarantined": True, "controllerClosed": False,
                         "redirectStopVerified": False, "radioTeardownVerified": False,
                         "error": message[:1024]}
    return error


def _cleanup_failed_open(device, original):
    try:
        device.close()
    except BaseException as cleanup:
        error = _quarantine(device, "UsbDk setup failed: " + str(original) + "; cleanup unverified: " + str(cleanup))
        raise error from original


class _Operation:
    def __init__(self, event, size, data, endpoint, transfer_type, prefix=0):
        self.event, self.size, self.read = event, size, data is None
        self.buffer = (C.c_uint8 * max(1, size))()
        if data is not None:
            C.memmove(self.buffer, data, len(data))
        self.request = TransferRequest()
        self.request.EndpointAddress, self.request.TransferType = endpoint, transfer_type
        self.request.Buffer, self.request.BufferLength = C.addressof(self.buffer), size
        self.overlapped = usb._Overlapped()
        self.overlapped.hEvent = event
        self.prefix, self.complete, self.synchronous, self.result = prefix, False, False, None


class _RedirectedDevice:
    def __init__(self, api, row, identity, interface, pipes, lease):
        self.api, self.identity, self.lease = api, identity, lease
        self._interface, self._pipes = interface, pipes
        self.handle = self.system_handle = None
        self._operations, self._closed, self._close_error = {}, False, None
        self._redirect_attempted = False
        try:
            self._redirect_attempted = True
            self.handle = api.helper.UsbDk_StartRedirect(C.byref(row["native"].ID))
            if self.handle in (None, 0, C.c_void_p(-1).value):
                self.handle = None
                raise _quarantine(self, "UsbDk_StartRedirect failed; redirect outcome is unverified; no retry/takeover")
            self.system_handle = api.helper.UsbDk_GetRedirectorSystemHandle(self.handle)
            if self.system_handle in (None, 0, C.c_void_p(-1).value):
                self.system_handle = None
                raise UsbDkError("UsbDk returned no system handle for owned I/O cancellation")
        except BaseException as error:
            if self._close_error is None:
                _cleanup_failed_open(self, error)
            raise

    def interface(self):
        return self._interface

    def pipes(self):
        return self._pipes

    def _release(self, operation):
        if operation.event is not None:
            if not self.api.kernel.CloseHandle(operation.event):
                raise _quarantine(self, "Owned UsbDk completion event did not close")
            operation.event = None
        self._operations.pop(id(operation), None)

    def _submit(self, endpoint, transfer_type, size, data=None, prefix=0):
        if self._closed or self._close_error:
            raise UsbDkError("UsbDk device is closed or quarantined")
        if (type(size) is not int or not 1 <= size <= usb.MAX_QUEUED_BYTES or
                data is not None and (not isinstance(data, bytes) or len(data) != size)):
            raise ValueError("One bounded UsbDk transfer buffer is required")
        event = self.api.kernel.CreateEventW(None, True, False, None)
        if not event:
            raise UsbDkError("Create owned UsbDk completion event failed")
        operation = _Operation(event, size, data, endpoint, transfer_type, prefix)
        self._operations[id(operation)] = operation
        function = self.api.helper.UsbDk_ReadPipe if data is None else self.api.helper.UsbDk_WritePipe
        status = function(self.handle, C.byref(operation.request), C.byref(operation.overlapped))
        if status == 0:
            operation.complete = True
            self._release(operation)
            raise UsbDkError("UsbDk transfer start failed; do not automatically retry")
        if status not in (1, 2):
            raise _quarantine(self, "Unknown UsbDk transfer-start result; request lifetime is unverified")
        operation.synchronous = status == 1
        return operation

    def start_read(self, endpoint, size):
        pipe = next((value for value in self._pipes if value.address == endpoint), None)
        if pipe is None or not endpoint & 0x80 or pipe.kind not in (2, 3):
            raise UsbDkError("Read endpoint is outside the selected HCI interface")
        return self._submit(endpoint, 1 if pipe.kind == 2 else 2, size)

    def start_write(self, endpoint, data):
        if not any(value.address == endpoint and value.kind == 2 and not endpoint & 0x80 for value in self._pipes):
            raise UsbDkError("Write endpoint is outside the selected HCI interface")
        return self._submit(endpoint, 1, len(data), data)

    def start_control_write(self, setup, data):
        if (setup.length != len(data) or setup.request_type not in (0x20, 0x21) or setup.request or setup.value or
                setup.index != (self._interface.number if setup.request_type == 0x21 else 0)):
            raise UsbDkError("Only one class HCI Command control transfer is allowed")
        payload = struct.pack("<BBHHH", setup.request_type, setup.request, setup.value, setup.index, setup.length) + data
        return self._submit(0, 0, len(payload), payload, prefix=8)

    def poll(self, operation):
        if operation.complete and operation.event is None:
            return operation.result
        if id(operation) not in self._operations:
            raise UsbDkError("Foreign UsbDk operation")
        if not operation.synchronous:
            ignored = C.c_uint32()
            if not self.api.kernel.GetOverlappedResult(self.system_handle, C.byref(operation.overlapped), C.byref(ignored), False):
                code = C.get_last_error()
                if code in (usb.ERROR_IO_PENDING, usb.ERROR_IO_INCOMPLETE):
                    return None
                if code == usb.ERROR_OPERATION_ABORTED:
                    operation.complete = True
                    self._release(operation)
                    raise usb.UsbHciCancelled("Owned UsbDk request cancelled")
                # Unknown failure is not proof that the kernel released buffers.
                raise OSError(code, "GetOverlappedResult for owned UsbDk request failed")
        operation.complete = True
        transferred = operation.request.Result.GenResult.BytesTransferred
        status = operation.request.Result.GenResult.UsbdStatus
        self._release(operation)
        if status:
            raise UsbDkError("UsbDk USB request failed with USBD status 0x%x; do not retry" % status)
        if transferred > operation.size - operation.prefix:
            raise usb.UsbHciProtocolError("UsbDk returned an impossible transfer length")
        # Control result counts only data-stage bytes, not its 8-byte setup.
        operation.result = bytes(operation.buffer[:transferred]) if operation.read else transferred
        return operation.result

    def wait(self, operations, timeout):
        active = [value for value in operations if not value.complete]
        if not active:
            return
        if len(active) > 64 or any(id(value) not in self._operations for value in active):
            raise UsbDkError("Foreign or excessive UsbDk wait operations")
        handles = (C.c_void_p * len(active))(*(value.event for value in active))
        result = self.api.kernel.WaitForMultipleObjects(len(active), handles, False, min(60000, max(0, math.ceil(timeout * 1000))))
        if result != usb.WAIT_TIMEOUT and not 0 <= result < len(active):
            raise UsbDkError("Owned UsbDk completion wait failed")

    def close(self):
        if self._close_error:
            raise _quarantine(self, self._close_error)
        if self._closed:
            return
        errors = []
        for operation in list(self._operations.values()):
            if operation.synchronous or operation.complete:
                continue
            if not self.api.kernel.CancelIoEx(self.system_handle, C.byref(operation.overlapped)):
                code = C.get_last_error()
                if code != usb.ERROR_NOT_FOUND:
                    errors.append("CancelIoEx error " + str(code))
        deadline = time.monotonic() + usb._CANCEL_DRAIN_SECONDS
        while self._operations and time.monotonic() < deadline:
            for operation in list(self._operations.values()):
                try:
                    self.poll(operation)
                except usb.UsbHciCancelled:
                    pass
                except BaseException as error:
                    errors.append(str(error))
            if self._operations:
                try:
                    self.wait(list(self._operations.values()), min(0.05, max(0, deadline - time.monotonic())))
                except BaseException as error:
                    errors.append(str(error))
                    break
        if self._operations:
            raise _quarantine(self, "Owned UsbDk cancellation did not drain; buffers, handles and controller lease retained")
        if self.handle is not None:
            try:
                stopped = self.api.helper.UsbDk_StopRedirect(self.handle)
            except BaseException as error:
                raise _quarantine(self, "Owned UsbDk StopRedirect raised: " + str(error)) from error
            if not stopped:
                raise _quarantine(self, "Owned UsbDk StopRedirect failed; controller lease retained")
            self.handle = self.system_handle = None
        if errors:
            raise _quarantine(self, "UsbDk cleanup had unverified native errors: " + "; ".join(errors)[:1024])
        try:
            close_api = getattr(self.api, "close", None)
            if close_api is not None:
                close_api()
            self.lease.close()
        except BaseException as error:
            raise _quarantine(self, "Owned UsbDk controller lease did not close: " + str(error)) from error
        self._closed = True


def main(arguments=None):
    import argparse
    import json
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inspect", action="store_true", required=True,
                        help="Read software presence/signature/ABI only; never load or redirect")
    parser.parse_args(arguments)
    result = inspect_backend()
    print(json.dumps(result, indent=2))
    return 0 if result["available"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
