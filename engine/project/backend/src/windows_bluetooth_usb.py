"""Explicit, dedicated WinUSB Bluetooth HCI transport.

Only the registered dedicated Azki interface GUID is enumerated; no driver
changes, radio discovery or synthetic HCI replies. A caller must select an
already WinUSB-bound device-interface path. USB command,
interrupt and bulk transfers do not contain UART H4 type bytes; this module
adds/removes those bytes at the QEMU boundary. SCO/ISO are not implemented.
"""
from collections import deque
import ctypes as C
from dataclasses import dataclass
import math
import os
import re
import threading
import time
import uuid


MAX_ACL_PAYLOAD = 1024  # Current BES guest transport bound, not a USB packet size.
MAX_QUEUED_BYTES = 1024 * 1024
MAX_QUEUED_PACKETS = 256
_CANCEL_DRAIN_SECONDS = 5.0
ERROR_IO_PENDING = 997
ERROR_IO_INCOMPLETE = 996
ERROR_OPERATION_ABORTED = 995
ERROR_NOT_FOUND = 1168
WAIT_OBJECT_0 = 0
WAIT_TIMEOUT = 258
WAIT_FAILED = 0xFFFFFFFF
AZKI_USB_INTERFACE_GUID = "{a4b85fea-0ae3-4be1-b238-9dca9ed299a8}"


class UsbHciError(RuntimeError):
    pass


class UsbHciProtocolError(UsbHciError):
    pass


class UsbHciCancelled(UsbHciError):
    pass


def _failed_open_cleanup(device, original):
    try:
        device.close()
    except BaseException as cleanup:
        if not any(item is device for item in _QUARANTINED_DEVICES):
            _QUARANTINED_DEVICES.append(device)
        error = UsbHciError("Controller setup failed: " + str(original) + "; owned cleanup unverified: " + str(cleanup))
        lease = getattr(device, "lease", None)
        error.raw_cleanup = {"controllerIdentity": getattr(device, "identity", None),
                             "exclusiveLease": lease.descriptor if lease is not None else None,
                             "resourcesQuarantined": True, "controllerClosed": False,
                             "error": str(error)[:1024]}
        raise error from original


def list_dedicated_controllers(*, api=None):
    """List only present registered Azki interfaces, without opening one.

    A registration is discovery metadata, not proof of endpoint/HCI support.
    Opening the explicitly selected interface still validates its real class
    and endpoints. No Bluetooth/system USB class enumeration is used here.
    """
    base = {"devices": [], "unavailableDevices": [], "interfaceGUID": AZKI_USB_INTERFACE_GUID,
            "inspectionOnly": True, "deviceOpened": False, "radioOpened": False,
            "hciCommandsSent": 0, "driverBindingPerformed": False,
            "guestFirmwareAccepted": False, "driverBindingRequired": None}
    if api is None and os.name != "nt":
        return {**base, "platformSupported": False, "reason": "Windows is required"}
    provider = api if api is not None else _RegisteredAzkiInterfaces()
    rows = provider.list_interfaces(AZKI_USB_INTERFACE_GUID)
    if not isinstance(rows, list) or len(rows) > 128:
        raise UsbHciError("Dedicated interface enumeration exceeded its bound")
    for source in rows:
        row = dict(source)
        row["devicePath"] = _device_path(row.get("devicePath"))
        if not isinstance(row.get("instanceID"), str) or not 1 <= len(row["instanceID"]) <= 4096:
            raise UsbHciError("Dedicated interface has no bounded instance ID")
        bound = isinstance(row.get("driver"), str) and row["driver"].casefold() == "winusb"
        row["winusbBound"] = bound
        base["devices" if bound else "unavailableDevices"].append(row)
    base["driverBindingRequired"] = False if base["devices"] else True if rows else None
    return {**base, "platformSupported": True,
            "reason": None if base["devices"] else "No present WinUSB interface is registered with the dedicated Azki GUID; binding and/or GUID registration may be required"}


def resolve_device_interface(device_path, *, api=None):
    """Resolve one explicitly supplied opaque interface path; no device open.

    Unlike the selector this can resolve an explicitly supplied advanced GUID.
    The PnP identity comes from SetupAPI, never from parsing a symbolic link.
    """
    path = _device_path(device_path)
    provider = api if api is not None else _RegisteredAzkiInterfaces()
    row = dict(provider.resolve_interface(path))
    if (not isinstance(row.get("devicePath"), str) or row["devicePath"].casefold() != path.casefold() or
            not isinstance(row.get("instanceID"), str) or not 1 <= len(row["instanceID"]) <= 4096 or
            not row["instanceID"].casefold().startswith("usb\\") or any(ord(c) < 32 for c in row["instanceID"])):
        raise UsbHciError("SetupAPI did not resolve the exact USB interface identity")
    return {**row, "metadataOnly": True, "deviceOpened": False, "hciCommandsSent": 0}


def dispatch_inspection(args):
    """Host/guest-independent public runtime route for read-only USB actions."""
    if not isinstance(args, dict):
        raise ValueError("USB inspection action must be an object")
    action = args.get("action")
    if action == "hci.usb.list":
        if set(args) != {"action"}:
            raise ValueError("Dedicated interface listing takes no device path or driver options")
        return list_dedicated_controllers()
    if action != "hci.usb.inspect" or set(args) - {"action", "devicePath"}:
        raise ValueError("Unknown USB inspection action or field")
    if "devicePath" in args:
        path = _device_path(args["devicePath"])
        result = inspect_winusb_device(path)
    else:
        result = inspect_backend()
    return {**result, "inspectionOnly": True, "hciCommandsSent": 0,
            "driverBindingPerformed": False, "guestFirmwareAccepted": False}


class _Guid(C.Structure):
    _fields_ = [("data1", C.c_uint32), ("data2", C.c_uint16), ("data3", C.c_uint16), ("data4", C.c_ubyte * 8)]


class _DeviceInfo(C.Structure):
    _fields_ = [("size", C.c_uint32), ("class_guid", _Guid), ("devinst", C.c_uint32), ("reserved", C.c_size_t)]


class _DeviceInterface(C.Structure):
    _fields_ = [("size", C.c_uint32), ("interface_guid", _Guid), ("flags", C.c_uint32), ("reserved", C.c_size_t)]


class _DevicePropertyKey(C.Structure):
    _fields_ = [("guid", _Guid), ("pid", C.c_uint32)]


class _RegisteredAzkiInterfaces:
    """SetupAPI enumeration/property APIs only; no CreateFile/WinUSB handle."""
    def __init__(self):
        if os.name != "nt":
            raise OSError("Windows SetupAPI is required")
        self.api = C.WinDLL("setupapi.dll", use_last_error=True)
        DWORD = C.c_uint32
        handle = C.c_void_p
        guid = C.POINTER(_Guid)
        info = C.POINTER(_DeviceInfo)
        interface = C.POINTER(_DeviceInterface)
        self.api.SetupDiGetClassDevsW.argtypes = [guid, C.c_wchar_p, handle, DWORD]
        self.api.SetupDiGetClassDevsW.restype = handle
        self.api.SetupDiCreateDeviceInfoList.argtypes = [guid, handle]
        self.api.SetupDiCreateDeviceInfoList.restype = handle
        self.api.SetupDiOpenDeviceInterfaceW.argtypes = [handle, C.c_wchar_p, DWORD, interface]
        self.api.SetupDiOpenDeviceInterfaceW.restype = C.c_int
        self.api.SetupDiEnumDeviceInterfaces.argtypes = [handle, info, guid, DWORD, interface]
        self.api.SetupDiEnumDeviceInterfaces.restype = C.c_int
        self.api.SetupDiGetDeviceInterfaceDetailW.argtypes = [handle, interface, handle, DWORD, C.POINTER(DWORD), info]
        self.api.SetupDiGetDeviceInterfaceDetailW.restype = C.c_int
        self.api.SetupDiGetDeviceInstanceIdW.argtypes = [handle, info, C.POINTER(C.c_wchar), DWORD, C.POINTER(DWORD)]
        self.api.SetupDiGetDeviceInstanceIdW.restype = C.c_int
        self.api.SetupDiGetDeviceRegistryPropertyW.argtypes = [handle, info, DWORD, C.POINTER(DWORD), handle, DWORD, C.POINTER(DWORD)]
        self.api.SetupDiGetDeviceRegistryPropertyW.restype = C.c_int
        self.api.SetupDiGetDevicePropertyW.argtypes = [handle, info, C.POINTER(_DevicePropertyKey), C.POINTER(DWORD), handle, DWORD, C.POINTER(DWORD), DWORD]
        self.api.SetupDiGetDevicePropertyW.restype = C.c_int
        self.api.SetupDiDestroyDeviceInfoList.argtypes = [handle]
        self.api.SetupDiDestroyDeviceInfoList.restype = C.c_int

    def _detail(self, handle, item):
        needed = C.c_uint32()
        if self.api.SetupDiGetDeviceInterfaceDetailW(handle, C.byref(item), None, 0, C.byref(needed), None):
            raise UsbHciError("Unexpected empty device detail success")
        error = C.get_last_error()
        if error != 122 or not 8 <= needed.value <= 65536:
            raise C.WinError(error)
        detail = C.create_string_buffer(needed.value)
        C.c_uint32.from_buffer(detail).value = 8 if C.sizeof(C.c_void_p) == 8 else 6
        info = _DeviceInfo()
        info.size = C.sizeof(info)
        if not self.api.SetupDiGetDeviceInterfaceDetailW(handle, C.byref(item), detail, len(detail), C.byref(needed), C.byref(info)):
            raise C.WinError(C.get_last_error())
        if needed.value > len(detail) or needed.value % 2:
            raise UsbHciError("SetupAPI returned an invalid device detail length")
        path = bytes(detail[4:needed.value]).decode("utf-16le").rstrip("\0")
        identity = C.create_unicode_buffer(4096)
        required = C.c_uint32()
        if not self.api.SetupDiGetDeviceInstanceIdW(handle, C.byref(info), identity, len(identity), C.byref(required)):
            raise C.WinError(C.get_last_error())
        name = self._property(handle, info, 12) or self._property(handle, info, 0)
        return {"devicePath": path, "instanceID": identity.value, "name": name,
                "interfaceGUID": "{" + str(uuid.UUID(bytes_le=bytes(item.interface_guid))) + "}",
                "driver": self._property(handle, info, 4),
                "driverInfPath": self._property(handle, info, 5, device_property=True)}

    def resolve_interface(self, path):
        _device_path(path)
        handle = self.api.SetupDiCreateDeviceInfoList(None, None)
        if handle in (None, C.c_void_p(-1).value):
            raise C.WinError(C.get_last_error())
        try:
            item = _DeviceInterface()
            item.size = C.sizeof(item)
            if not self.api.SetupDiOpenDeviceInterfaceW(handle, path, 0, C.byref(item)):
                raise C.WinError(C.get_last_error())
            if not item.flags & 1 or item.flags & 4:  # SPINT_ACTIVE / REMOVED
                raise UsbHciError("The explicitly selected device interface is not active")
            return self._detail(handle, item)
        finally:
            if not self.api.SetupDiDestroyDeviceInfoList(handle):
                raise C.WinError(C.get_last_error())

    def _property(self, handle, info, property_id, *, device_property=False):
        size, kind = C.c_uint32(), C.c_uint32()
        key = _DevicePropertyKey(_Guid.from_buffer_copy(uuid.UUID("a8b865dd-2e3d-4094-ad97-e593a70c75d6").bytes_le), property_id)
        def read(buffer, length):
            if device_property:
                return self.api.SetupDiGetDevicePropertyW(handle, C.byref(info), C.byref(key), C.byref(kind), buffer, length, C.byref(size), 0)
            return self.api.SetupDiGetDeviceRegistryPropertyW(handle, C.byref(info), property_id, C.byref(kind), buffer, length, C.byref(size))
        if read(None, 0):
            raise UsbHciError("Unexpected empty SetupAPI property success")
        error = C.get_last_error()
        if error in (13, ERROR_NOT_FOUND):
            return None
        if error != 122 or not 2 <= size.value <= 65536 or size.value % 2:
            raise C.WinError(error)
        data = C.create_string_buffer(size.value)
        if not read(data, len(data)):
            raise C.WinError(C.get_last_error())
        if kind.value != (0x12 if device_property else 1):
            raise UsbHciError("Unexpected non-string SetupAPI property")
        return bytes(data[:size.value]).decode("utf-16le").rstrip("\0")

    def list_interfaces(self, interface_guid):
        if interface_guid != AZKI_USB_INTERFACE_GUID:
            raise ValueError("Only the dedicated Azki interface GUID can be enumerated")
        guid_bytes = uuid.UUID(interface_guid).bytes_le
        guid = _Guid.from_buffer_copy(guid_bytes)
        handle = self.api.SetupDiGetClassDevsW(C.byref(guid), None, None, 2 | 16)
        if handle in (None, C.c_void_p(-1).value):
            raise C.WinError(C.get_last_error())
        result = []
        try:
            for index in range(129):
                item = _DeviceInterface()
                item.size = C.sizeof(item)
                if not self.api.SetupDiEnumDeviceInterfaces(handle, None, C.byref(guid), index, C.byref(item)):
                    error = C.get_last_error()
                    if error == 259:
                        return result
                    raise C.WinError(error)
                if index == 128:
                    raise UsbHciError("Dedicated interface enumeration exceeded128 entries")
                if bytes(item.interface_guid) != guid_bytes:
                    raise UsbHciError("SetupAPI returned an unexpected interface GUID")
                result.append(self._detail(handle, item))
        finally:
            if not self.api.SetupDiDestroyDeviceInfoList(handle):
                raise C.WinError(C.get_last_error())


def _device_path(value):
    # A WinUSB interface path, not a COM port, arbitrary file, or GLOBALROOT.
    if (not isinstance(value, str) or not 1 <= len(value) <= 4096 or
            not re.fullmatch(r"\\\\\?\\usb#vid_[0-9a-f]{4}&pid_[0-9a-f]{4}[^\\\x00]*"
                             r"#\{[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}\}",
                             value, re.IGNORECASE)):
        raise ValueError("An explicit WinUSB USB device-interface path is required")
    return value


def _seconds(value, maximum=60.0):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError("A finite timeout in seconds is required")
    value = float(value)
    if not math.isfinite(value) or not 0 <= value <= maximum:
        raise ValueError("Timeout is outside the bounded range")
    return value


@dataclass(frozen=True)
class UsbInterface:
    number: int
    alternate: int
    device_class: int
    subclass: int
    protocol: int


@dataclass(frozen=True)
class UsbPipe:
    address: int
    kind: int  # USBD_PIPE_TYPE: bulk=2, interrupt=3.
    maximum_packet_size: int


@dataclass(frozen=True)
class UsbSetup:
    request_type: int
    request: int
    value: int
    index: int
    length: int


def _select_pipes(interface, pipes):
    if (interface.alternate != 0 or
            (interface.device_class, interface.subclass, interface.protocol) != (0xE0, 1, 1)):
        raise UsbHciProtocolError("Interface is not Bluetooth HCI class E0/01/01 alternate0")
    if not 0 <= interface.number <= 255:
        raise UsbHciProtocolError("Invalid USB interface number")
    groups = {"event": [], "acl_in": [], "acl_out": []}
    seen = set()
    for pipe in pipes:
        if (type(pipe.address) is not int or not 0 <= pipe.address <= 255 or
                pipe.address in seen or not 1 <= (pipe.address & 15) <= 15 or
                pipe.address & 0x70 or not 1 <= pipe.maximum_packet_size <= 1024):
            raise UsbHciProtocolError("Invalid or duplicate USB endpoint")
        seen.add(pipe.address)
        if pipe.kind == 3 and pipe.address & 0x80:
            groups["event"].append(pipe)
        elif pipe.kind == 2:
            groups["acl_in" if pipe.address & 0x80 else "acl_out"].append(pipe)
    if any(len(value) != 1 for value in groups.values()):
        raise UsbHciProtocolError("Exactly one interrupt-IN, bulk-IN and bulk-OUT endpoint required")
    return {key: value[0] for key, value in groups.items()}


class _Frames:
    """Reassemble the endpoint stream, without assuming one read == one HCI PDU."""
    def __init__(self, packet_type, max_acl):
        self.packet_type = packet_type
        self.max_acl = max_acl
        self.pending = bytearray()

    def feed(self, data):
        if len(self.pending) + len(data) > MAX_QUEUED_BYTES:
            raise UsbHciProtocolError("USB HCI stream exceeds the buffer bound")
        self.pending.extend(data)
        frames = []
        header = 2 if self.packet_type == 4 else 4
        while len(self.pending) >= header:
            length = self.pending[1] if self.packet_type == 4 else int.from_bytes(self.pending[2:4], "little")
            if self.packet_type == 2 and length > self.max_acl:
                raise UsbHciProtocolError("Controller ACL payload exceeds the configured guest bound")
            size = header + length
            if len(self.pending) < size:
                break
            if len(frames) >= MAX_QUEUED_PACKETS:
                raise UsbHciProtocolError("USB HCI batch exceeds the packet bound")
            frames.append(bytes((self.packet_type,)) + bytes(self.pending[:size]))
            del self.pending[:size]
        return frames


class WinUsbHciTransport:
    """Serialized public API, with two concurrent overlapped endpoint reads.

    A receive timeout leaves the reads alive for the next receive; no bytes are
    discarded by cancellation/reissue. Public calls are serialized. Use short
    receive timeouts in a duplex pump so TX and close are serviced promptly.
    """
    @classmethod
    def open(cls, device_path, *, api=None, max_acl_payload=MAX_ACL_PAYLOAD,
             command_recipient=None, io_timeout=5.0):
        path = _device_path(device_path)
        if type(max_acl_payload) is not int or not 1 <= max_acl_payload <= 65535:
            raise ValueError("Invalid ACL payload bound")
        io_timeout = _seconds(io_timeout)
        if not io_timeout:
            raise ValueError("I/O timeout must be positive")
        if command_recipient not in (None, "device", "interface"):
            raise ValueError("Invalid USB HCI command recipient")
        provider = api if api is not None else _WinUsbApi()
        device = provider.open(path)
        try:
            interface = device.interface()
            selected = _select_pipes(interface, device.pipes())
            return cls(device, path, interface, selected, max_acl_payload,
                       command_recipient, io_timeout)
        except BaseException as error:
            _failed_open_cleanup(device, error)
            raise

    def __init__(self, device, path, interface, pipes, max_acl, recipient, io_timeout):
        self._device, self._path, self._interface, self._pipes = device, path, interface, pipes
        self._max_acl, self._io_timeout = max_acl, io_timeout
        self._recipient = recipient or ("interface" if interface.number else "device")
        self._lock = threading.RLock()
        self._closed = False
        self._close_error = None
        self._failed = None
        self._reads = {}
        self._frames = {"event": _Frames(4, max_acl), "acl_in": _Frames(2, max_acl)}
        self._queue = deque()
        self._queued_bytes = 0
        self._next_first = "event"

    @property
    def capabilities(self):
        lease = getattr(self._device, "lease", None)
        identity = getattr(self._device, "identity", None)
        return {"backend": "windows-winusb-hci", "rawHciTransport": True,
                "physicalRadioVerified": False, "driverBindingManaged": False,
                "devicePath": self._path, "interfaceNumber": self._interface.number,
                "commandRecipient": self._recipient,
                "guestTxH4Types": [1, 2], "guestRxH4Types": [2, 4],
                "sco": False, "iso": False, "maxAclPayloadBytes": self._max_acl,
                "endpoints": {key: pipe.address for key, pipe in self._pipes.items()},
                "syntheticHciReplies": False, "automaticControllerInitialization": False,
                "controllerIdentity": identity,
                "controllerInstanceID": identity.get("instanceID") if identity else None,
                "controllerAddress": None, "addressSource": "not-queried",
                "exclusiveLease": lease.descriptor if lease is not None else None}

    def _check(self):
        if self._closed:
            raise UsbHciError("USB HCI transport is closed")
        if self._failed:
            raise UsbHciError("USB HCI transport failed: " + self._failed)

    def _terminal_io(self, error):
        self._failed = self._failed or str(error)
        try:
            self.close()
        except BaseException as cleanup:
            raise UsbHciError(self._failed + "; owned USB cleanup failed: " + str(cleanup)) from error

    def send_h4(self, packet):
        if not isinstance(packet, bytes) or not packet:
            raise ValueError("One complete H4 bytes packet is required")
        kind = packet[0]
        if kind == 1:
            valid = len(packet) >= 4 and len(packet) == 4 + packet[3]
        elif kind == 2:
            valid = (len(packet) >= 5 and len(packet) == 5 + int.from_bytes(packet[3:5], "little")
                     and len(packet) <= 5 + self._max_acl)
        else:
            valid = False
        if not valid:
            raise UsbHciProtocolError("Only complete bounded H4 Command/ACL packets may be sent")
        with self._lock:
            self._check()
            body = packet[1:]
            try:
                if kind == 1:
                    setup = UsbSetup(0x21 if self._recipient == "interface" else 0x20, 0, 0,
                                     self._interface.number if self._recipient == "interface" else 0,
                                     len(body))
                    operation = self._device.start_control_write(setup, body)
                else:
                    operation = self._device.start_write(self._pipes["acl_out"].address, body)
                deadline = time.monotonic() + self._io_timeout
                result = self._device.poll(operation)
                while result is None:
                    left = deadline - time.monotonic()
                    if left <= 0:
                        self._failed = "USB TX timed out; command outcome may be uncertain; do not retry"
                        self.close()  # Exact owned request cancellation + completion drain.
                        raise TimeoutError(self._failed)
                    self._device.wait([operation], left)
                    result = self._device.poll(operation)
                if result != len(body):
                    raise UsbHciProtocolError("Short USB HCI write; do not automatically retry")
            except BaseException as error:
                self._terminal_io(error)
                raise

    def _arm_reads(self):
        for name in ("event", "acl_in"):
            if name not in self._reads:
                pipe = self._pipes[name]
                minimum = 257 if name == "event" else self._max_acl + 4
                size = math.ceil(minimum / pipe.maximum_packet_size) * pipe.maximum_packet_size
                self._reads[name] = self._device.start_read(pipe.address, size)

    def receive(self, timeout=0.05):
        timeout = _seconds(timeout)
        with self._lock:
            self._check()
            try:
                return self._receive(timeout)
            except (OSError, UsbHciError) as error:
                # A native completion error may have retired its OVERLAPPED.
                # Never retry a stale _reads entry, queued output or guest TX.
                self._terminal_io(error)
                raise

    def _receive(self, timeout):
        timeout = _seconds(timeout)
        with self._lock:
            self._check()
            self._arm_reads()
            if self._queue:
                value = self._queue.popleft(); self._queued_bytes -= len(value)
                return value
            deadline = time.monotonic() + timeout
            empty_completions = 0
            while True:
                self._arm_reads()
                names = (self._next_first, "acl_in" if self._next_first == "event" else "event")
                self._next_first = names[1]
                for name in names:
                    operation = self._reads[name]
                    data = self._device.poll(operation)
                    if data is None:
                        continue
                    del self._reads[name]
                    if not isinstance(data, bytes):
                        raise UsbHciProtocolError("Invalid USB read completion")
                    if not data:
                        empty_completions += 1
                    try:
                        frames = self._frames[name].feed(data)
                        for value in frames:
                            if (len(self._queue) >= MAX_QUEUED_PACKETS or
                                    self._queued_bytes + len(value) > MAX_QUEUED_BYTES):
                                raise UsbHciProtocolError("USB HCI receive queue exceeded")
                            self._queue.append(value); self._queued_bytes += len(value)
                    except UsbHciProtocolError as error:
                        self._failed = str(error)
                        raise
                # Keep both endpoints armed even when this poll expires or
                # returns a queued frame. A partial header must not stall until
                # some future caller happens to start another receive.
                self._arm_reads()
                if self._queue:
                    value = self._queue.popleft(); self._queued_bytes -= len(value)
                    return value
                left = deadline - time.monotonic()
                if left <= 0:
                    return None
                if empty_completions >= 16:
                    raise UsbHciProtocolError("Repeated empty USB reads")
                self._arm_reads()
                self._device.wait(list(self._reads.values()), left)

    def close(self):
        with self._lock:
            if self._closed:
                if self._close_error is not None:
                    raise UsbHciError("Previous owned USB close failed: " + self._close_error)
                return
            self._closed = True
            try:
                self._device.close()
            except BaseException as error:
                self._close_error = str(error)
                raise
            self._reads.clear(); self._queue.clear(); self._queued_bytes = 0

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def inspect_winusb_device(device_path, *, api=None):
    """Open/query/close the explicit already-bound interface; send no HCI."""
    with WinUsbHciTransport.open(device_path, api=api) as transport:
        pass
    return {**transport.capabilities, "transportClosed": True}


def inspect_backend():
    """Resolve Windows DLL exports only. Never enumerate or open a device."""
    result = {"backend": "windows-winusb-hci", "platformSupported": os.name == "nt",
              "winUsbDllAvailable": False, "deviceOpened": False,
              "hciCommandsSent": 0, "physicalRadioVerified": False,
              "requiresExplicitWinUsbInterfacePath": True,
              "driverBindingManaged": False, "guestTxH4Types": [1, 2],
              "guestRxH4Types": [2, 4], "sco": False, "iso": False}
    if os.name == "nt":
        try:
            _WinUsbApi()
            result["winUsbDllAvailable"] = True
        except (OSError, AttributeError) as error:
            result["error"] = str(error)
    else:
        result["error"] = "WinUSB backend requires Windows"
    return result


_PROBE_COMMANDS = {
    0x0C03: ("Reset", 1),
    0x1001: ("Read_Local_Version_Information", 9),
    0x1002: ("Read_Local_Supported_Commands", 65),
    0x1003: ("Read_Local_Supported_Features", 9),
    0x1005: ("Read_Buffer_Size", 8),
    0x1009: ("Read_BD_ADDR", 7),
    0x2002: ("LE_Read_Buffer_Size_v1", 4),
    0x2003: ("LE_Read_Local_Supported_Features", 9),
    0x201C: ("LE_Read_Supported_States", 9),
}
_PROBE_LE_BITS = {0x2002: (25, 1), 0x2003: (25, 2), 0x201C: (28, 3)}


def _probe_fields(opcode, values):
    """Only successful exact-length return parameters are decoded."""
    data = values[1:]
    if opcode == 0x1001:
        return {"hciVersion": data[0], "hciRevision": int.from_bytes(data[1:3], "little"),
                "lmpVersion": data[3], "companyIdentifier": int.from_bytes(data[4:6], "little"),
                "lmpSubversion": int.from_bytes(data[6:8], "little")}
    if opcode in (0x1002, 0x1003, 0x2003, 0x201C):
        return {"hex": data.hex()}
    if opcode == 0x1005:
        return {"aclPayloadBytes": int.from_bytes(data[:2], "little"), "synchronousPayloadBytes": data[2],
                "aclPacketCredits": int.from_bytes(data[3:5], "little"),
                "synchronousPacketCredits": int.from_bytes(data[5:7], "little")}
    if opcode == 0x1009:
        return {"address": ":".join("%02X" % octet for octet in reversed(data)),
                "littleEndianHex": data.hex(), "isZero": not any(data)}
    if opcode == 0x2002:
        return {"leAclPayloadBytes": int.from_bytes(data[:2], "little"), "leAclPacketCredits": data[2]}
    return {}


def probe_controller(device_path, *, transport_factory=None, timeout=2.0):
    """Explicit Reset-once + serial standard read-only controller queries.

    Never called by inspect/open/close. No firmware download, vendor commands,
    advertising, scanning or connection is started. A busy exclusive device is
    not stolen from an existing runtime. Fake transport injection is for tests.
    """
    path = _device_path(device_path)
    timeout = _seconds(timeout, maximum=5.0)
    if not timeout:
        raise ValueError("Controller query timeout must be positive")
    result = {"backend": "windows-winusb-hci", "operation": "explicit-controller-probe",
              "devicePath": path, "queries": [], "skipped": [], "resetSucceeded": False,
              "success": False, "error": None, "commandCredits": None,
              "leSupported": None, "transportClosed": False, "radioStateUnknown": True,
              "controllerAddress": None,
              "advertisingStarted": False, "scanningStarted": False, "connectionsStarted": False,
              "vendorCommandsSent": False, "resetCommandsAttempted": 0,
              "extraResetOnClose": False, "completeFirmwareBluetoothAccepted": False}
    controller = None
    credits = None
    event_count = 0

    def event(deadline):
        nonlocal credits, event_count
        while time.monotonic() < deadline:
            packet = controller.receive(min(.01, max(0, deadline - time.monotonic())))
            if packet is None:
                continue
            event_count += 1
            if event_count > 512:
                raise UsbHciProtocolError("Controller probe event bound exceeded")
            if not isinstance(packet, bytes) or not packet:
                raise UsbHciProtocolError("Invalid controller probe packet")
            if packet[0] == 2:
                if len(packet) < 5 or len(packet) != 5 + int.from_bytes(packet[3:5], "little"):
                    raise UsbHciProtocolError("Malformed ACL during controller probe")
                continue  # Never log unrelated payloads or turn them into replies.
            if packet[0] != 4 or len(packet) < 3 or len(packet) != 3 + packet[2]:
                raise UsbHciProtocolError("Malformed HCI event during controller probe")
            if packet[1] == 0x0E:
                if len(packet) < 6:
                    raise UsbHciProtocolError("Truncated HCI Command Complete")
                credits = packet[3]
                result["commandCredits"] = credits
                return "commandComplete", int.from_bytes(packet[4:6], "little"), packet[6:]
            if packet[1] == 0x0F:
                if len(packet) != 7 or packet[2] != 4:
                    raise UsbHciProtocolError("Truncated HCI Command Status")
                credits = packet[4]
                result["commandCredits"] = credits
                return "commandStatus", int.from_bytes(packet[5:7], "little"), packet[3:4]
        raise TimeoutError("Controller probe response/command-credit deadline expired")

    def query(opcode, bootstrap=False):
        nonlocal credits
        name, expected = _PROBE_COMMANDS[opcode]
        row = {"opcode": "0x%04x" % opcode, "name": name, "commandAttempted": False,
               "commandSent": False, "response": None, "status": None, "fields": None}
        result["queries"].append(row)
        deadline = time.monotonic() + timeout
        # Only the explicit bootstrap Reset may precede an observed credit.
        while not bootstrap and (credits is None or credits <= 0):
            event(deadline)
        if time.monotonic() >= deadline:
            raise TimeoutError("No command credit for " + name)
        row["commandAttempted"] = True
        if opcode == 0x0C03:
            result["resetCommandsAttempted"] += 1
        controller.send_h4(bytes((1, opcode & 255, opcode >> 8, 0)))
        row["commandSent"] = True
        if credits is not None:
            credits -= 1
            result["commandCredits"] = credits
        while True:
            kind, actual_opcode, values = event(deadline)
            if actual_opcode != opcode:
                continue
            if not values:
                raise UsbHciProtocolError("Missing status for " + name)
            if kind == "commandStatus" and values[0] == 0:
                continue  # Accepted does not mean this standard query completed.
            row.update(response=kind, status=values[0], commandCredits=credits,
                       returnParametersHex=values.hex())
            if kind == "commandStatus":
                return row  # Explicit rejection; no fake return fields.
            if values[0] == 0:
                if len(values) != expected:
                    raise UsbHciProtocolError("Unexpected successful return length for " + name)
                row["fields"] = _probe_fields(opcode, values)
            elif opcode == 0x0C03 and len(values) != 1:
                raise UsbHciProtocolError("Malformed Reset Command Complete")
            return row

    try:
        factory = transport_factory or WinUsbHciTransport.open
        controller = factory(path, io_timeout=timeout)
        reset = query(0x0C03, bootstrap=True)
        if reset["response"] != "commandComplete" or reset["status"] != 0:
            raise UsbHciError("Controller rejected initial Reset; no retry")
        result["resetSucceeded"] = True
        rows = {}
        for opcode in (0x1001, 0x1002, 0x1003, 0x1005):
            rows[opcode] = query(opcode)
        if rows[0x1003]["status"] == 0:
            features = bytes.fromhex(rows[0x1003]["fields"]["hex"])
            result["leSupported"] = bool(features[4] & 0x40)
        commands = bytes.fromhex(rows[0x1002]["fields"]["hex"]) if rows[0x1002]["status"] == 0 else None
        if commands is None:
            reason = "Supported-command bitmap unavailable"
        elif not commands[15] & (1 << 1):
            reason = "Supported-command bitmap does not advertise this query"
        else:
            address = query(0x1009)
            if address["status"] == 0:
                result["controllerAddress"] = address["fields"]["address"]
            reason = None
        if reason is not None:
            result["skipped"].append({"opcode": "0x1009", "reason": reason})
        for opcode, (byte, bit) in _PROBE_LE_BITS.items():
            if result["leSupported"] is not True:
                reason = "Controller LE feature absent or unknown"
            elif commands is None:
                reason = "Supported-command bitmap unavailable"
            elif not commands[byte] & (1 << bit):
                reason = "Supported-command bitmap does not advertise this query"
            else:
                query(opcode)
                continue
            result["skipped"].append({"opcode": "0x%04x" % opcode, "reason": reason})
        result["success"] = all(row["status"] == 0 and row["response"] == "commandComplete"
                                for row in result["queries"])
        if not result["success"]:
            result["error"] = "One or more standard queries were rejected; no fields fabricated"
    except (OSError, ValueError, UsbHciError) as error:
        result["error"] = str(error)
        result["success"] = False
    finally:
        if controller is not None:
            try:
                controller.close()  # Transport close sends no extra HCI Reset.
                result["transportClosed"] = True
            except BaseException as error:
                result["error"] = "Owned transport close failed: " + str(error)
                result["success"] = False
        result["radioStateUnknown"] = not (result["success"] and result["resetSucceeded"])
    return result


# Windows ABI types use fixed-width integers even when this module is imported
# on a Unix test host, where ctypes.c_ulong may be 64 bits.
class _UsbInterfaceDescriptor(C.Structure):
    _pack_ = 1
    _fields_ = [(name, C.c_uint8) for name in (
        "Length", "DescriptorType", "InterfaceNumber", "AlternateSetting",
        "NumEndpoints", "InterfaceClass", "InterfaceSubClass", "InterfaceProtocol", "InterfaceString")]


class _PipeInformation(C.Structure):
    _fields_ = [("PipeType", C.c_int32), ("PipeId", C.c_uint8),
                ("MaximumPacketSize", C.c_uint16), ("Interval", C.c_uint8)]


class _SetupPacket(C.Structure):
    _pack_ = 1
    _fields_ = [("RequestType", C.c_uint8), ("Request", C.c_uint8),
                ("Value", C.c_uint16), ("Index", C.c_uint16), ("Length", C.c_uint16)]


class _Offsets(C.Structure):
    _fields_ = [("Offset", C.c_uint32), ("OffsetHigh", C.c_uint32)]


class _OffsetUnion(C.Union):
    _fields_ = [("offsets", _Offsets), ("Pointer", C.c_void_p)]


class _Overlapped(C.Structure):
    _anonymous_ = ("location",)
    _fields_ = [("Internal", C.c_size_t), ("InternalHigh", C.c_size_t),
                ("location", _OffsetUnion), ("hEvent", C.c_void_p)]


class _Operation:
    def __init__(self, event, size, data=None):
        self.event = event
        self.buffer = (C.c_uint8 * max(1, size))()
        if data:
            C.memmove(self.buffer, data, len(data))
        self.overlapped = _Overlapped()
        self.overlapped.hEvent = event
        self.size, self.is_read = size, data is None
        self.result = None
        self.complete = False


# Only used if a broken driver never completes cancellation. Keep OVERLAPPED,
# buffers, events and handles alive rather than freeing kernel-visible memory.
# No retries or operations are performed on these quarantined devices.
_QUARANTINED_DEVICES = []


class _WinUsbApi:
    def __init__(self):
        if os.name != "nt":
            raise OSError("WinUSB backend requires Windows")
        self.kernel = C.WinDLL("kernel32", use_last_error=True)
        self.usb = C.WinDLL("winusb", use_last_error=True)
        ptr, u32, u8, boolean = C.c_void_p, C.c_uint32, C.c_uint8, C.c_int32
        signatures = [
            (self.kernel, "CreateFileW", [C.c_wchar_p, u32, u32, ptr, u32, u32, ptr], ptr),
            (self.kernel, "CloseHandle", [ptr], boolean),
            (self.kernel, "CreateEventW", [ptr, boolean, boolean, C.c_wchar_p], ptr),
            (self.kernel, "CancelIoEx", [ptr, C.POINTER(_Overlapped)], boolean),
            (self.kernel, "WaitForMultipleObjects", [u32, C.POINTER(ptr), boolean, u32], u32),
            (self.usb, "WinUsb_Initialize", [ptr, C.POINTER(ptr)], boolean),
            (self.usb, "WinUsb_Free", [ptr], boolean),
            (self.usb, "WinUsb_GetCurrentAlternateSetting", [ptr, C.POINTER(u8)], boolean),
            (self.usb, "WinUsb_QueryInterfaceSettings", [ptr, u8, C.POINTER(_UsbInterfaceDescriptor)], boolean),
            (self.usb, "WinUsb_QueryPipe", [ptr, u8, u8, C.POINTER(_PipeInformation)], boolean),
            (self.usb, "WinUsb_ReadPipe", [ptr, u8, ptr, u32, C.POINTER(u32), C.POINTER(_Overlapped)], boolean),
            (self.usb, "WinUsb_WritePipe", [ptr, u8, ptr, u32, C.POINTER(u32), C.POINTER(_Overlapped)], boolean),
            (self.usb, "WinUsb_ControlTransfer", [ptr, _SetupPacket, ptr, u32, C.POINTER(u32), C.POINTER(_Overlapped)], boolean),
            (self.usb, "WinUsb_GetOverlappedResult", [ptr, C.POINTER(_Overlapped), C.POINTER(u32), boolean], boolean),
        ]
        for library, name, arguments, result in signatures:
            function = getattr(library, name)
            function.argtypes, function.restype = arguments, result

    def open(self, path):
        identity = resolve_device_interface(path)
        if not isinstance(identity.get("driver"), str) or identity["driver"].casefold() != "winusb":
            raise UsbHciError("Selected controller does not use WinUSB; no driver changed or device opened")
        if __package__:
            from .windows_bluetooth_lease import ControllerInstanceLease
        else:
            from windows_bluetooth_lease import ControllerInstanceLease
        lease = ControllerInstanceLease.acquire(identity["instanceID"])
        # The native device owns the lease from the first CreateFile attempt;
        # constructor/close failure retains it alongside any unclosed handle.
        return _WinUsbDevice(self, path, identity=identity, lease=lease)


def _win_error(operation, code=None):
    code = C.get_last_error() if code is None else code
    return OSError(code, operation + " failed (Windows error " + str(code) + ")")


class _WinUsbDevice:
    def __init__(self, api, path, *, identity=None, lease=None):
        self.api = api
        self.identity, self.lease = identity, lease
        self.file = self.handle = None
        self._operations = {}
        self._closed = False
        try:
            self.file = api.kernel.CreateFileW(path, 0xC0000000, 0, None, 3, 0x40000080, None)
            if self.file in (None, C.c_void_p(-1).value):
                self.file = None
                raise _win_error("CreateFileW dedicated WinUSB interface")
            self.handle = C.c_void_p()
            if not api.usb.WinUsb_Initialize(self.file, C.byref(self.handle)):
                self.handle = None
                raise _win_error("WinUsb_Initialize (already WinUSB-bound interface required)")
            alternate = C.c_uint8()
            if not api.usb.WinUsb_GetCurrentAlternateSetting(self.handle, C.byref(alternate)):
                raise _win_error("WinUsb_GetCurrentAlternateSetting")
            if alternate.value != 0:
                raise UsbHciProtocolError("Active USB alternate setting is not0; no automatic switch performed")
            descriptor = _UsbInterfaceDescriptor()
            if not api.usb.WinUsb_QueryInterfaceSettings(self.handle, 0, C.byref(descriptor)):
                raise _win_error("WinUsb_QueryInterfaceSettings")
            self._descriptor = descriptor
        except BaseException as error:
            _failed_open_cleanup(self, error)
            raise

    def interface(self):
        value = self._descriptor
        if value.Length != 9 or value.DescriptorType != 4:
            raise UsbHciProtocolError("Malformed USB interface descriptor")
        return UsbInterface(value.InterfaceNumber, value.AlternateSetting,
                            value.InterfaceClass, value.InterfaceSubClass, value.InterfaceProtocol)

    def pipes(self):
        values = []
        for index in range(self._descriptor.NumEndpoints):
            pipe = _PipeInformation()
            if not self.api.usb.WinUsb_QueryPipe(self.handle, 0, index, C.byref(pipe)):
                raise _win_error("WinUsb_QueryPipe")
            values.append(UsbPipe(pipe.PipeId, pipe.PipeType, pipe.MaximumPacketSize))
        return values

    def _operation(self, size, data=None):
        if self._closed:
            raise UsbHciError("Native USB interface is closed")
        event = self.api.kernel.CreateEventW(None, True, False, None)
        if not event:
            raise _win_error("CreateEventW")
        operation = _Operation(event, size, data)
        self._operations[id(operation)] = operation
        return operation

    def _started(self, operation, result):
        if not result:
            error = C.get_last_error()
            if error != ERROR_IO_PENDING:
                self._release(operation)
                raise _win_error("WinUSB transfer start", error)
        return operation

    def start_read(self, endpoint, size):
        operation = self._operation(size)
        return self._started(operation, self.api.usb.WinUsb_ReadPipe(
            self.handle, endpoint, operation.buffer, size, None, C.byref(operation.overlapped)))

    def start_write(self, endpoint, data):
        operation = self._operation(len(data), data)
        return self._started(operation, self.api.usb.WinUsb_WritePipe(
            self.handle, endpoint, operation.buffer, len(data), None, C.byref(operation.overlapped)))

    def start_control_write(self, setup, data):
        operation = self._operation(len(data), data)
        packet = _SetupPacket(setup.request_type, setup.request, setup.value, setup.index, setup.length)
        return self._started(operation, self.api.usb.WinUsb_ControlTransfer(
            self.handle, packet, operation.buffer, len(data), None, C.byref(operation.overlapped)))

    def _release(self, operation):
        self._operations.pop(id(operation), None)
        if operation.event:
            self.api.kernel.CloseHandle(operation.event)
            operation.event = None

    def poll(self, operation):
        if operation.complete:
            return operation.result
        if id(operation) not in self._operations:
            raise UsbHciError("Foreign USB operation")
        transferred = C.c_uint32()
        if not self.api.usb.WinUsb_GetOverlappedResult(
                self.handle, C.byref(operation.overlapped), C.byref(transferred), False):
            error = C.get_last_error()
            if error in (ERROR_IO_INCOMPLETE, ERROR_IO_PENDING):
                return None
            self._release(operation)
            operation.complete = True
            if error == ERROR_OPERATION_ABORTED:
                raise UsbHciCancelled("Owned USB operation cancelled")
            raise _win_error("WinUsb_GetOverlappedResult", error)
        self._release(operation)
        operation.complete = True
        if transferred.value > operation.size:
            raise UsbHciProtocolError("USB transfer returned an impossible length")
        operation.result = bytes(operation.buffer[:transferred.value]) if operation.is_read else transferred.value
        return operation.result

    def wait(self, operations, timeout):
        active = [operation for operation in operations if not operation.complete]
        if not active:
            return
        if any(id(operation) not in self._operations for operation in active):
            raise UsbHciError("Foreign USB wait operation")
        handles = (C.c_void_p * len(active))(*(operation.event for operation in active))
        milliseconds = min(60000, max(0, math.ceil(timeout * 1000)))
        result = self.api.kernel.WaitForMultipleObjects(len(active), handles, False, milliseconds)
        if result == WAIT_FAILED:
            raise _win_error("WaitForMultipleObjects")
        if result != WAIT_TIMEOUT and not WAIT_OBJECT_0 <= result < WAIT_OBJECT_0 + len(active):
            raise UsbHciError("Unexpected USB event wait result")

    def close(self):
        if self._closed:
            return
        # Every request is cancelled by its exact OVERLAPPED on our own handle.
        # Cancellation is not completion: drain before freeing any memory.
        cancellation_error = None
        for operation in list(self._operations.values()):
            try:
                if not self.api.kernel.CancelIoEx(self.file, C.byref(operation.overlapped)):
                    error = C.get_last_error()
                    if error != ERROR_NOT_FOUND:
                        cancellation_error = _win_error("CancelIoEx", error)
            except BaseException as error:
                cancellation_error = cancellation_error or error
        deadline = time.monotonic() + _CANCEL_DRAIN_SECONDS
        while self._operations and time.monotonic() < deadline:
            for operation in list(self._operations.values()):
                try:
                    self.poll(operation)
                except UsbHciCancelled:
                    pass
                except (OSError, UsbHciProtocolError) as error:
                    cancellation_error = cancellation_error or error
            if self._operations:
                try:
                    self.wait(list(self._operations.values()), min(0.05, max(0, deadline - time.monotonic())))
                except BaseException as error:
                    cancellation_error = cancellation_error or error
                    break
        self._closed = True
        if self._operations:
            _QUARANTINED_DEVICES.append(self)
            raise UsbHciError("USB cancellation did not complete; owned buffers/handles retained for safety")
        try:
            if self.handle:
                if not self.api.usb.WinUsb_Free(self.handle):
                    raise _win_error("WinUsb_Free dedicated interface")
                self.handle = None
            if self.file:
                if not self.api.kernel.CloseHandle(self.file):
                    raise _win_error("CloseHandle dedicated interface")
                self.file = None
            if cancellation_error:
                raise cancellation_error
            if self.lease is not None:
                self.lease.close()
        except BaseException:
            # This includes failed native release, not only pending I/O. The
            # controller identity remains leased; no second guest takes over.
            _QUARANTINED_DEVICES.append(self)
            raise


def main(arguments=None):
    import argparse
    import json
    parser = argparse.ArgumentParser(description=__doc__)
    operation = parser.add_mutually_exclusive_group(required=True)
    operation.add_argument("--inspect", action="store_true", help="Inspect software or one explicit already WinUSB-bound interface; send no HCI")
    operation.add_argument("--probe-controller", action="store_true", help="Explicitly Reset once and send standard read-only HCI queries on one already WinUSB-bound controller")
    parser.add_argument("--device-path", help="Explicit WinUSB interface; never a BTHUSB adapter lookup or VID/PID search")
    parser.add_argument("--query-timeout", type=float, default=2.0)
    args = parser.parse_args(arguments)
    if args.probe_controller and not args.device_path:
        parser.error("--probe-controller requires an explicit --device-path")
    try:
        if args.probe_controller:
            result = probe_controller(args.device_path, timeout=args.query_timeout)
            print(json.dumps(result, indent=2))
            return 0 if result["success"] else 1
        result = inspect_winusb_device(args.device_path) if args.device_path else inspect_backend()
        result.update(inspectionOnly=True, hciCommandsSent=0)
        print(json.dumps(result, indent=2))
        return 0 if result.get("platformSupported", True) and not result.get("error") else 1
    except (OSError, ValueError, UsbHciError) as error:
        print(json.dumps({"backend": "windows-winusb-hci", "inspectionOnly": args.inspect,
                          "operation": "explicit-controller-probe" if args.probe_controller else "inspect",
                          "hciCommandsSent": 0, "error": str(error)}, indent=2))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
