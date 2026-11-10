"""Windows-owned H4 guest link and synchronous external-* ATT RPC.

QEMU owns a nonce-named, ACL-restricted pipe server. This host is its client and
verifies the server PID against the actual owned Popen handle before any H4 data.
The unchanged H4/SMP/ATT engines carry original guest traffic; WinRT remains a
separate physical ATT/security hop, not a raw controller passthrough claim.
"""
from __future__ import annotations

from collections import OrderedDict, deque
import copy
import ctypes
from ctypes import wintypes as w
from dataclasses import dataclass, field
import json
import os
import queue
import threading
import time
import uuid

from .windows_runtime_files import WindowsFiles
from .virtual_peer.external import ExternalCentral, external_request
from .virtual_peer.link import H4Link
from .virtual_peer.secure_link import SecureLinkController
from .virtual_peer.state import canonical_uuid, strict_json, validate_state

MAX_REQUEST = 2048
MAX_RESPONSE = 65536


class WindowsGuestPeerError(RuntimeError):
    def __init__(self, message, code="guest_peer_error"):
        super().__init__(message)
        self.code = code


class GuestClock:
    def __init__(self, source=time.monotonic):
        self.source = source
        self.lock = threading.Lock()
        self.paused_since = None
        self.paused_total = 0.0

    def __call__(self):
        with self.lock:
            now = self.source()
            return now - self.paused_total - (now - self.paused_since if self.paused_since is not None else 0)

    def set_running(self, running):
        with self.lock:
            now = self.source()
            if running and self.paused_since is not None:
                self.paused_total += now - self.paused_since
                self.paused_since = None
            elif not running and self.paused_since is None:
                self.paused_since = now


class WindowsGuestProtocol:
    """Single-thread protocol owner, independently testable without pipe/radio."""
    def __init__(self, private_state, *, clock=None, bond_writer=None, controller=None):
        self.private_state = validate_state(copy.deepcopy(private_state))
        self.clock = clock or GuestClock()
        self.bond_writer = bond_writer
        self.controller = controller or SecureLinkController(self.private_state["identity"]["address"])
        self.controller.encryption_events = deque(maxlen=128)
        self.protocol = H4Link(self.controller, self._no_external, max_packets=None)
        self.external = None
        self.tx = bytearray()
        self.connected = False
        self.replay = OrderedDict()
        self.output_sha256 = None
        self.failure = None
        self.diagnostics = None
        self.bond_write_failed = False

    @staticmethod
    def _no_external(cid, payload):
        raise ValueError("No external peer owns this guest connection")

    def _send(self, packets):
        for packet in packets:
            if len(self.tx) + len(packet) > 1048576:
                raise ValueError("HCI pending output exceeds its bound")
            self.tx.extend(packet)

    def feed(self, data):
        self._send(self.protocol.feed(data))

    def _save_bond(self):
        if self.bond_write_failed:
            raise RuntimeError("Private bond writer failed; no automatic retry")
        if self.external is None or self.external.smp.bond is None:
            return
        ltk, random, ediv = self.external.smp.bond
        bond = {"ltk": ltk.hex(), "random": random.hex(), "ediv": ediv}
        if self.private_state.get("guestLinkBond") == bond:
            return
        candidate = copy.deepcopy(self.private_state)
        candidate["guestLinkBond"] = bond
        if candidate["binding"] is not None:
            candidate["binding"].update(bond)
        validate_state(candidate)
        if self.bond_writer is not None:
            try:
                result = self.bond_writer(candidate)
            except Exception:
                self.bond_write_failed = True
                self.failure = "private-bond-write-failed"
                raise
            if not isinstance(result, str) or len(result) != 64 or any(c not in "0123456789abcdef" for c in result):
                self.bond_write_failed = True
                self.failure = "private-bond-write-failed"
                raise ValueError("Private bond writer did not return its exact SHA256")
            self.output_sha256 = result
        self.private_state = candidate

    def tick(self):
        if self.connected and self.external is not None:
            self._send(self.external.tick(self.clock()))
            self._save_bond()

    def status(self):
        return {"schema": 1, "transport": "windows-private-pipe-virtual-hci",
                "hciConnected": self.connected, "physicalRadio": False,
                "physicalLinkSecurity": "managed-by-Windows-WinRT",
                "rawHCIControllerPassthrough": False,
                "applicationAuthentication": "forwarded-to-original-guest; not-inspected",
                "guestBusinessAuthenticationImplemented": False,
                "stateWritten": self.output_sha256 is not None,
                "bondPersistence": "private-callback" if self.bond_writer else "in-memory-only",
                "failure": self.failure,
                "diagnostics": self.diagnostics,
                "metrics": {"hciCommands": self.protocol.count,
                            "aclPackets": self.protocol.acl_count,
                            "l2capPackets": self.protocol.l2cap_count},
                "externalBridge": None if self.external is None else
                    {"bridgeID": self.external.bridge_id, "state": self.external.state,
                     "error": self.external.error}}

    def _dispatch(self, value):
        command = value["command"]
        if command == "status":
            return self.status()
        if command == "external-open":
            if self.external is not None:
                raise ValueError("External bridge already open")
            if not self.connected:
                raise ValueError("Original guest HCI transport is not connected")
            if self.controller.connected:
                self._send(self.controller.disconnect_local())
            self.external = ExternalCentral(self.controller, value.get("connectionTimeoutSeconds", 30),
                                             clock=self.clock,
                                             physical_link_security="managed-by-Windows-WinRT")
            bond = self.private_state.get("guestLinkBond") or self.private_state["binding"]
            if bond:
                self.external.smp.bond = (bytes.fromhex(bond["ltk"]), bytes.fromhex(bond["random"]), bond["ediv"])
            self.external.link = self.protocol
            self.protocol.on_l2cap = self.external.consume
            return {"bridgeID": self.external.bridge_id}
        bridge = self.external
        if bridge is None or value["bridgeID"] != bridge.bridge_id:
            raise ValueError("Stale external bridge")
        if command == "external-status":
            return bridge.status()
        if command == "external-reconnect":
            self._save_bond()
            if self.controller.connected:
                self._send(self.controller.disconnect_local())
            bridge.reconnect(self.clock())
            return {"reconnecting": True}
        if command == "external-request":
            if value["operation"] == "write" and value["offset"]:
                raise ValueError("Prepared/offset writes are not implemented")
            return bridge.submit(value["operation"], value["key"], value["value"], value["offset"], value["enabled"])
        if command == "external-result":
            return bridge.results.pop(value["operationID"], {"done": False})
        if command == "external-close":
            self._save_bond()
            if self.controller.connected:
                self._send(self.controller.disconnect_local())
            self.external = None
            self.protocol.on_l2cap = self._no_external
            return {"closed": True}
        raise ValueError("Unsupported Windows guest peer command")

    def process_request(self, raw):
        identity = None
        try:
            if not isinstance(raw, bytes) or not 1 <= len(raw) <= MAX_REQUEST:
                raise ValueError("Guest request byte bound")
            value = strict_json(raw)
            if value.get("command") == "status":
                if (set(value) != {"schema", "requestID", "command"} or type(value["schema"]) is not int or
                        value["schema"] != 1 or not canonical_uuid(value["requestID"])):
                    raise ValueError("Invalid status schema")
            else:
                external_request(value)
            identity = value["requestID"]
            if identity in self.replay:
                previous, response = self.replay[identity]
                if previous != raw:
                    raise ValueError("Request replay has different bytes")
                return response
            result = self._dispatch(value)
            response = {"schema": 1, "requestID": identity, "ok": True, "result": result}
        except Exception:
            response = {"schema": 1, "requestID": identity, "ok": False,
                        "error": {"code": "request_rejected", "message": "Guest peer request rejected"}}
        encoded = json.dumps(response, separators=(",", ":"), ensure_ascii=True, allow_nan=False).encode()
        if len(encoded) > MAX_RESPONSE:
            encoded = json.dumps({"schema": 1, "requestID": identity, "ok": False,
                                  "error": {"code": "response_bound", "message": "Guest response exceeds its bound"}}).encode()
        if identity is not None and identity not in self.replay:
            self.replay[identity] = (bytes(raw), encoded)
            while len(self.replay) > 64:
                self.replay.popitem(last=False)
        return encoded

    def close(self):
        if self.external is not None:
            self.external._fail("Guest peer closed")
        self.connected = False
        close = getattr(self.controller.crypto, "close", None)
        if close is not None:
            close()


class OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_size_t), ("InternalHigh", ctypes.c_size_t),
                ("Offset", w.DWORD), ("OffsetHigh", w.DWORD), ("hEvent", w.HANDLE)]


class WindowsHCIPipe:
    """Single-worker client of one owned QEMU server; all I/O remains bounded."""
    def __init__(self, api=None):
        self.api = api or WindowsFiles()
        self.name = "AzkiSimulator-HCI-" + uuid.uuid4().hex
        self.path = "\\\\.\\pipe\\" + self.name
        self.handle = None
        self.process = None
        self.read_operation = self.write_operation = None
        self.read_buffer = self.write_buffer = None
        k = self.api.kernel
        for name, result, args in (
            ("GetNamedPipeServerProcessId", w.BOOL, [w.HANDLE, ctypes.POINTER(w.ULONG)]),
            ("PeekNamedPipe", w.BOOL, [w.HANDLE, w.LPVOID, w.DWORD, w.LPVOID, ctypes.POINTER(w.DWORD), w.LPVOID]),
            ("ReadFile", w.BOOL, [w.HANDLE, w.LPVOID, w.DWORD, w.LPVOID, ctypes.POINTER(OVERLAPPED)]),
            ("WriteFile", w.BOOL, [w.HANDLE, w.LPVOID, w.DWORD, w.LPVOID, ctypes.POINTER(OVERLAPPED)]),
            ("GetOverlappedResult", w.BOOL, [w.HANDLE, ctypes.POINTER(OVERLAPPED), ctypes.POINTER(w.DWORD), w.BOOL]),
            ("CreateEventW", w.HANDLE, [w.LPVOID, w.BOOL, w.BOOL, w.LPCWSTR]),
            ("CancelIoEx", w.BOOL, [w.HANDLE, ctypes.POINTER(OVERLAPPED)]),
        ):
            function = getattr(k, name)
            function.restype, function.argtypes = result, args

    @property
    def chardev_argument(self):
        return "pipe,id=radio,path=" + self.name

    def attach_core(self, process):
        if self.process is not None or type(getattr(process, "pid", None)) is not int or process.pid <= 0 or not callable(getattr(process, "poll", None)):
            raise ValueError("Owned core process handle must be assigned exactly once")
        if process.poll() is not None:
            raise RuntimeError("Owned QEMU has already exited")
        self.process = process

    def connect(self):
        if self.handle is not None:
            return True
        if self.process is None:
            return False
        if self.process.poll() is not None:
            raise ConnectionError("Owned QEMU exited before guest HCI connected")
        handle = self.api.kernel.CreateFileW(self.path, 0xc0020000, 0, None, 3,
                                             0x40000000 | 0x00100000 | 0x00010000, None)
        if handle == self.api.INVALID:
            code = ctypes.get_last_error()
            if code in (2, 231):
                return False
            raise ctypes.WinError(code, "Open original guest HCI pipe")
        try:
            actual = w.ULONG()
            if not self.api.kernel.GetNamedPipeServerProcessId(handle, ctypes.byref(actual)):
                raise self.api.error("Identify original guest HCI server")
            if actual.value != self.process.pid or self.process.poll() is not None:
                raise PermissionError("HCI server is not this runtime's owned QEMU")
            if self.api.kernel.GetFileType(handle) != 3:
                raise PermissionError("Guest HCI endpoint is not a named pipe")
            self.api.check_private(handle)
            self.handle = handle
            return True
        except BaseException:
            self.api.close(handle)
            raise

    def _operation(self):
        operation = OVERLAPPED()
        operation.hEvent = self.api.kernel.CreateEventW(None, True, False, None)
        if not operation.hEvent:
            raise self.api.error("Create guest HCI operation event")
        return operation

    def _result(self, operation):
        count = w.DWORD()
        if not self.api.kernel.GetOverlappedResult(self.handle, ctypes.byref(operation), ctypes.byref(count), False):
            code = ctypes.get_last_error()
            if code == 996:
                return None
            raise ctypes.WinError(code, "Complete original guest HCI I/O")
        return count.value

    def read(self):
        if self.read_operation is None:
            available = w.DWORD()
            if not self.api.kernel.PeekNamedPipe(self.handle, None, 0, None, ctypes.byref(available), None):
                raise self.api.error("Inspect original guest HCI pipe")
            if not available.value:
                return b""
            self.read_buffer = ctypes.create_string_buffer(min(4096, available.value))
            self.read_operation = self._operation()
            if not self.api.kernel.ReadFile(self.handle, self.read_buffer, len(self.read_buffer), None,
                                             ctypes.byref(self.read_operation)) and ctypes.get_last_error() != 997:
                raise self.api.error("Read original guest HCI")
        count = self._result(self.read_operation)
        if count is None:
            return b""
        result = self.read_buffer.raw[:count]
        self.api.close(self.read_operation.hEvent)
        self.read_operation = self.read_buffer = None
        if not result:
            raise ConnectionError("Original guest HCI pipe closed")
        return result

    def flush(self, pending):
        if self.write_operation is None:
            if not pending:
                return 0
            self.write_buffer = ctypes.create_string_buffer(bytes(pending[:65536]))
            self.write_operation = self._operation()
            if not self.api.kernel.WriteFile(self.handle, self.write_buffer, len(self.write_buffer) - 1, None,
                                              ctypes.byref(self.write_operation)) and ctypes.get_last_error() != 997:
                raise self.api.error("Write original guest HCI; outcome may be unknown")
        count = self._result(self.write_operation)
        if count is None:
            return 0
        self.api.close(self.write_operation.hEvent)
        self.write_operation = self.write_buffer = None
        if not count or count > len(pending):
            raise ConnectionError("Invalid guest HCI write completion")
        return count

    def close(self):
        if self.handle is not None:
            for operation in (self.read_operation, self.write_operation):
                if operation is not None:
                    self.api.kernel.CancelIoEx(self.handle, ctypes.byref(operation))
                    count = w.DWORD()
                    # Keep buffers/events alive until Windows completes cancellation.
                    self.api.kernel.GetOverlappedResult(self.handle, ctypes.byref(operation), ctypes.byref(count), True)
                    self.api.close(operation.hEvent)
            self.api.close(self.handle)
            self.handle = None
            self.read_operation = self.write_operation = self.read_buffer = self.write_buffer = None


@dataclass
class PendingCall:
    raw: bytes
    deadline: float
    event: threading.Event = field(default_factory=threading.Event)
    result: bytes | None = None
    cancelled: bool = False
    error: Exception | None = None


class WindowsGuestPeer:
    """One runtime-owned worker; guest_rpc calls never rely on the UI loop."""
    def __init__(self, private_state, *, api=None, run_directory=None, bond_writer=None):
        self.api = api or WindowsFiles()
        if run_directory is not None:
            directory = self.api.open_read(run_directory, directory=True)
            self.api.close(directory)
        self.core = WindowsGuestProtocol(private_state, bond_writer=bond_writer)
        self.pipe = WindowsHCIPipe(self.api)
        self.calls = queue.Queue(maxsize=32)
        self.stop = threading.Event()
        self.ready = threading.Event()
        self.closed = False
        self.thread = None
        self.failure = None

    @property
    def chardev_argument(self):
        return self.pipe.chardev_argument

    @property
    def required_environment(self):
        return {"AZKI_QEMU_PIPE_CLIENT_PID": str(os.getpid())}

    def start(self):
        if self.closed or self.thread is not None:
            raise RuntimeError("Guest peer cannot be restarted")
        self.thread = threading.Thread(target=self._run, name="azki-windows-guest-peer", daemon=True)
        self.thread.start()
        return self

    def attach_core(self, process):
        self.pipe.attach_core(process)

    def wait_ready(self, timeout=15.0):
        if not self.ready.wait(timeout):
            raise TimeoutError("Original guest HCI pipe did not become ready")
        if self.failure or not self.core.connected:
            raise WindowsGuestPeerError("Original guest HCI startup failed", "hci_startup_failed")

    def set_guest_running(self, running):
        self.core.clock.set_running(bool(running))

    def call(self, value, *, timeout=2.0):
        if not 0.05 <= timeout <= 30 or self.closed or self.failure or self.thread is None:
            raise WindowsGuestPeerError("Guest peer is not available", "guest_unavailable")
        raw = json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()
        if len(raw) > MAX_REQUEST:
            raise ValueError("Guest RPC request exceeds 2048 bytes")
        pending = PendingCall(raw, time.monotonic() + timeout)
        try:
            self.calls.put_nowait(pending)
        except queue.Full:
            raise WindowsGuestPeerError("Guest RPC queue is full", "queue_full") from None
        if not pending.event.wait(timeout):
            pending.cancelled = True
            raise WindowsGuestPeerError("Guest RPC timed out; mutation outcome may be unknown; do not resend", "outcome_unknown")
        if pending.error is not None:
            raise pending.error
        response = json.loads(pending.result)
        if response.get("ok") is not True:
            error = response.get("error", {})
            raise WindowsGuestPeerError(error.get("message", "Guest request rejected"), error.get("code", "request_rejected"))
        return response["result"]

    def _requests(self):
        for _ in range(32):
            try:
                pending = self.calls.get_nowait()
            except queue.Empty:
                return
            if pending.cancelled or time.monotonic() > pending.deadline:
                pending.error = WindowsGuestPeerError("Guest RPC expired before execution", "request_expired")
            else:
                pending.result = self.core.process_request(pending.raw)
            pending.event.set()

    def _run(self):
        try:
            while not self.stop.is_set():
                if self.core.failure:
                    raise RuntimeError("Guest peer protocol failed")
                if self.pipe.connect():
                    if not self.core.connected:
                        self.core.connected = True
                        self.ready.set()
                    data = self.pipe.read()
                    if data:
                        self.core.feed(data)
                    self.core.tick()
                    sent = self.pipe.flush(self.core.tx)
                    if sent:
                        del self.core.tx[:sent]
                self._requests()
                self.stop.wait(.005)
        except Exception as error:
            self.failure = self.core.failure or "windows-guest-peer-failed"
            self.core.failure = self.failure
            self.core.diagnostics = {"exceptionType": type(error).__name__,
                                     "winerror": getattr(error, "winerror", None),
                                     "errno": getattr(error, "errno", None)}
        finally:
            self.ready.set()
            self.core.close()
            self.pipe.close()
            while True:
                try:
                    pending = self.calls.get_nowait()
                except queue.Empty:
                    break
                pending.error = WindowsGuestPeerError("Guest peer stopped", "guest_unavailable")
                pending.event.set()

    def close(self):
        if self.closed:
            return
        self.stop.set()
        if self.thread is not None:
            self.thread.join(3.0)
            if self.thread.is_alive():
                raise RuntimeError("Windows guest peer did not stop within its bound")
        else:
            self.core.close()
            self.pipe.close()
        self.closed = True
