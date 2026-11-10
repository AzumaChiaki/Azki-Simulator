"""Single-session QEMU H4 bridges for an explicitly owned controller.

This module neither enumerates nor opens hardware. The caller transfers ownership
of a controller implementing send_h4/receive/close. Production uses the protected
owned-QEMU named pipe. ControllerBridge's unauthenticated loopback TCP listener
is retained for isolated protocol research and tests only.
"""
from collections import Counter, deque
import math
import os
import re
import select
import socket
import threading
import time


RESET = bytes.fromhex("01 03 0c 00")
MAX_PENDING_BYTES = 1024 * 1024
MAX_PENDING_PACKETS = 4096


class HciBridgeError(RuntimeError):
    pass


def _timeout(value, maximum):
    if (isinstance(value, bool) or not isinstance(value, (int, float)) or
            not math.isfinite(value) or not 0 < value <= maximum):
        raise ValueError("A positive bounded timeout is required")
    return float(value)


def _packet_length(packet, direction, max_acl):
    if not isinstance(packet, bytes) or not packet:
        raise HciBridgeError("A complete H4 bytes packet is required")
    kind = packet[0]
    if kind == 1 and direction == "tx":
        expected = 4 + packet[3] if len(packet) >= 4 else None
    elif kind == 4 and direction == "rx":
        expected = 3 + packet[2] if len(packet) >= 3 else None
    elif kind == 2:
        expected = 5 + int.from_bytes(packet[3:5], "little") if len(packet) >= 5 else None
        if expected is not None and expected > 5 + max_acl:
            raise HciBridgeError("H4 ACL exceeds the configured guest bound")
    else:
        raise HciBridgeError("Unsupported H4 type/direction; SCO and ISO are not implemented")
    if expected is None or len(packet) != expected:
        raise HciBridgeError("Incomplete or overlong H4 packet")
    return expected


class _GuestFrames:
    def __init__(self, max_acl):
        self.max_acl = max_acl
        self.pending = bytearray()

    def feed(self, data):
        if len(self.pending) + len(data) > MAX_PENDING_BYTES:
            raise HciBridgeError("Guest H4 stream exceeded the buffer bound")
        self.pending.extend(data)
        values = []
        while self.pending:
            kind = self.pending[0]
            if kind not in (1, 2):
                raise HciBridgeError("Guest H4 type unsupported; only Command/ACL are accepted")
            header = 4 if kind == 1 else 5
            if len(self.pending) < header:
                break
            length = self.pending[3] if kind == 1 else int.from_bytes(self.pending[3:5], "little")
            if kind == 2 and length > self.max_acl:
                raise HciBridgeError("Guest ACL payload exceeds the configured bound")
            if len(self.pending) < header + length:
                break
            if len(values) >= MAX_PENDING_PACKETS:
                raise HciBridgeError("Guest H4 packet batch exceeded the bound")
            value = bytes(self.pending[:header + length])
            del self.pending[:header + length]
            values.append(value)
        return values


class ControllerBridge:
    """One listener, one accepted guest and one terminal controller session.

    All controller I/O, including optional RF teardown, runs on the worker.
    There is no reconnect or automatic guest-command retry. Stop waits for this
    worker only, never kills a process or cancels unrelated handles.
    """
    def _initialize(self, controller, *, reset_on_stop, teardown_timeout=2.0,
                    max_acl_payload=1024):
        if type(reset_on_stop) is not bool:
            raise ValueError("reset_on_stop must be explicitly True or False")
        teardown_timeout = _timeout(teardown_timeout, 2.0)
        if type(max_acl_payload) is not int or not 1 <= max_acl_payload <= 65535:
            raise ValueError("Invalid ACL payload bound")
        self._controller = controller
        self._reset_on_stop = reset_on_stop
        self._teardown_timeout = teardown_timeout
        self._max_acl = max_acl_payload
        self._lock = threading.RLock()
        self._stop = threading.Event()
        self._finished = threading.Event()
        self._socket = None
        self._server = None
        self._controller_failed = False
        self._thread = None
        self._outstanding = Counter()
        self._command_credit = None
        self._vendor_events = deque(maxlen=128)
        self._teardown_command_pending = False
        self._stats = {
            "backend": "loopback-controller-h4", "connected": False,
            "connectionAccepted": False, "closed": False, "error": None,
            "txPackets": 0, "txBytes": 0, "txAttempts": 0,
            "txOutcomeUncertain": False,
            "rxPackets": 0, "rxBytes": 0, "controllerRxPackets": 0,
            "guestWireBytesReceived": 0, "guestWireBytesSent": 0,
            "discardedTxPackets": 0, "discardedRxPackets": 0,
            "resetOnStop": reset_on_stop, "resetAttempted": False,
            "resetSent": False, "resetResponseMatched": False, "resetStatus": None,
            "radioTeardownVerified": False, "teardownError": None,
            "teardownEvidence": "none", "overAirObservation": False,
            "controllerClosed": False, "pipeClosed": None, "sco": False, "iso": False,
            "commandCredits": None,
            "commandScheduling": "one-physical-command-at-a-time-with-real-credits",
            "vendorCommandPolicy": "local-unsupported-vendor",
            "vendorCommandsForwarded": 0, "localVendorRejections": 0,
            "localVendorEventsOmitted": 0,
            "observedAddress": None, "observedAddressSource": "not-queried",
        }

    def __init__(self, controller, *, reset_on_stop, teardown_timeout=2.0,
                 max_acl_payload=1024):
        self._initialize(controller, reset_on_stop=reset_on_stop,
                         teardown_timeout=teardown_timeout, max_acl_payload=max_acl_payload)
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            if os.name == "nt":
                server.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            server.bind(("127.0.0.1", 0))
            server.listen(1)
            server.setblocking(False)
            self._port = server.getsockname()[1]
            self._server = server
            self._thread = threading.Thread(target=self._run, name="azki-owned-hci", daemon=True)
            self._thread.start()
        except BaseException:
            server.close()
            controller.close()
            raise

    @property
    def descriptor(self):
        return {"transport": "loopback-controller-h4", "host": "127.0.0.1", "port": self._port,
                "qemuChardev": "socket,id=radio,host=127.0.0.1,port=" + str(self._port),
                "machineOption": "bt-hci-chardev=radio", "singleConnection": True,
                "loopbackOnly": True, "peerAuthenticated": False,
                "guestTxH4Types": [1, 2], "guestRxH4Types": [2, 4],
                "vendorCommandPolicy": "local-unsupported-vendor",
                "resetOnStop": self._reset_on_stop}

    @property
    def stats(self):
        with self._lock:
            return {**self._stats, "localVendorEvents": [dict(row) for row in self._vendor_events],
                    "outstandingCommands": [{"opcode": f"0x{opcode:04x}", "count": count}
                        for opcode, count in sorted(self._outstanding.items()) if count > 0],
                    "teardownCommandPending": self._teardown_command_pending,
                    "commandOutcomeUncertain": self._controller_failed or self._teardown_command_pending or any(self._outstanding.values()),
                    "threadAlive": self._thread is not None and self._thread.is_alive()}

    def _set(self, **values):
        with self._lock:
            self._stats.update(values)

    def _increment(self, **values):
        with self._lock:
            for name, value in values.items():
                self._stats[name] += value

    def _error(self, message):
        with self._lock:
            if self._stats["error"] is None:
                self._stats["error"] = str(message)[:1024]

    def _send_controller(self, packet):
        if self._is_vendor(packet):
            raise HciBridgeError("Vendor firewall forbids forwarding an unapproved vendor opcode")
        try:
            self._controller.send_h4(packet)
            if packet[0] == 1 and self._command_credit is not None:
                with self._lock:
                    self._command_credit -= 1
                    self._set(commandCredits=self._command_credit)
        except BaseException:
            self._controller_failed = True
            self._set(txOutcomeUncertain=True)
            raise

    @staticmethod
    def _is_vendor(packet):
        return packet[0] == 1 and int.from_bytes(packet[1:3], "little") >> 10 == 0x3F

    def _tx_ready(self, packet):
        if packet[0] != 1:
            return True  # ACL flow-control is independent of HCI command credits.
        if any(self._outstanding.values()):
            return False
        if self._command_credit is None and self._is_vendor(packet):
            raise HciBridgeError("Vendor command rejected before real command credits were observed; no controller command sent")
        return self._command_credit is None or self._command_credit > 0

    def _dispatch_tx(self, packet):
        """Return a local rejection frame, or send exactly once to hardware.

        A foreign vendor opcode may modify a different vendor's NVM. It is never
        forwarded. Local Unknown Command responses are serialized after real
        completions and cannot replenish the physical credit counter.
        """
        if self._is_vendor(packet):
            if any(self._outstanding.values()) or self._command_credit is None or self._command_credit <= 0:
                raise HciBridgeError("Cannot issue a local vendor completion with unresolved physical command credits")
            opcode = int.from_bytes(packet[1:3], "little")
            with self._lock:
                if len(self._vendor_events) == self._vendor_events.maxlen:
                    self._stats["localVendorEventsOmitted"] += 1
                self._stats["localVendorRejections"] += 1
                self._vendor_events.append({"index": self._stats["localVendorRejections"],
                    "opcode": f"0x{opcode:04x}", "parameterBytes": packet[3], "status": 1,
                    "source": "local-unsupported-vendor", "physicalSent": False,
                    "physicalCommandCredits": self._command_credit})
            # One conservative guest credit is no greater than actual observed
            # availability. This synthetic failure is not a physical radio ACK.
            return bytes((4, 0x0E, 4, 1, packet[1], packet[2], 1))
        self._increment(txAttempts=1)
        self._send_controller(packet)
        if packet[0] == 1:
            with self._lock:
                self._outstanding[int.from_bytes(packet[1:3], "little")] += 1
        self._increment(txPackets=1, txBytes=len(packet))
        return None

    def _next_tx(self, pending):
        if self._tx_ready(pending[0]):
            return pending.popleft()
        # Keep command order and ACL order separately. A pending command or
        # local rejection must not block independent ACL traffic behind it.
        for index, packet in enumerate(pending):
            if packet[0] == 2:
                del pending[index]
                return packet
        return None

    @staticmethod
    def _send_guest(stream, data):
        return stream.send(data)

    def _receive_controller(self, timeout):
        try:
            packet = self._controller.receive(timeout)
            if packet is not None:
                _packet_length(packet, "rx", self._max_acl)
                self._increment(controllerRxPackets=1)
                self._observe_completion(packet)
            return packet
        except BaseException:
            self._controller_failed = True
            raise

    def _observe_completion(self, packet):
        if packet[0] != 4:
            return
        opcode = None
        if packet[1] == 0x0E and len(packet) >= 6:
            opcode = int.from_bytes(packet[4:6], "little")
            self._command_credit = packet[3]
        elif packet[1] == 0x0F:
            if len(packet) != 7 or packet[2] != 4:
                raise HciBridgeError("Malformed HCI Command Status; no command credit inferred")
            opcode = int.from_bytes(packet[5:7], "little")
            self._command_credit = packet[4]
        if opcode is not None:
            self._set(commandCredits=self._command_credit)
        if opcode == 0x0C03 and not (packet[1] == 0x0E and len(packet) == 7 and packet[2] == 4):
            return  # Reset requires its actual complete opcode+status, not CS/short CC.
        if (opcode == 0x1009 and self._outstanding[opcode] > 0 and
                packet[1] == 0x0E and len(packet) == 13 and packet[2] == 10 and packet[6] == 0):
            self._set(observedAddress=":".join(f"{octet:02X}" for octet in reversed(packet[7:13])),
                      observedAddressSource="physical-hci-read-bdaddr")
        if opcode is not None and self._outstanding[opcode] > 0:
            with self._lock:
                self._outstanding[opcode] -= 1

    def _teardown(self):
        if not self._reset_on_stop:
            self._set(teardownError="Reset disabled by explicit caller choice")
            return
        if self._controller_failed:
            self._set(teardownError="Controller I/O failed; no Reset retry attempted")
            return
        deadline = time.monotonic() + self._teardown_timeout
        try:
            # A prior guest Reset reply must not be mistaken for our own Reset.
            def blocked():
                return (any(self._outstanding.values()) or
                        (self._command_credit is not None and self._command_credit <= 0))
            while blocked() and time.monotonic() < deadline:
                self._receive_controller(min(.005, max(0, deadline - time.monotonic())))
            if self._outstanding[0x0C03] > 0:
                self._set(teardownError="Unresolved guest Reset; cannot attribute a new Reset reply")
                return
            if any(self._outstanding.values()):
                self._set(teardownError="Unresolved physical guest command; no teardown Reset sent without an ordered drain")
                return
            if blocked():
                self._set(teardownError="No HCI command credit available before teardown deadline; no Reset retry")
                return
            self._set(resetAttempted=True)
            with self._lock:
                self._teardown_command_pending = True
            self._send_controller(RESET)
            self._set(resetSent=True)
            while time.monotonic() < deadline:
                packet = self._receive_controller(min(.005, max(0, deadline - time.monotonic())))
                if packet is None or packet[0] != 4 or packet[1] != 0x0E:
                    continue
                # Reset Command Complete has exactly num-packets, opcode, status.
                if len(packet) == 7 and packet[2] == 4 and int.from_bytes(packet[4:6], "little") == 0x0C03:
                    status = packet[6]
                    with self._lock:
                        self._teardown_command_pending = False
                    self._set(resetResponseMatched=True, resetStatus=status,
                              radioTeardownVerified=status == 0,
                              teardownEvidence="controller-command-complete",
                              teardownError=None if status == 0 else "Controller rejected teardown Reset")
                    return
            self._set(teardownError="Teardown Reset response timed out; unplug the dedicated dongle if RF state must be certain")
        except BaseException as error:
            self._set(teardownError="Teardown failed without retry: " + str(error)[:768])

    def _run(self):
        transmit = deque()
        receive = deque()
        tx_bytes = rx_bytes = 0
        parser = _GuestFrames(self._max_acl)
        try:
            while not self._stop.is_set():
                stream = self._socket
                readers = [stream] if stream is not None else [self._server]
                writers = [stream] if stream is not None and receive else []
                readable, writable, exceptional = select.select(readers, writers, readers, 0 if transmit else .005)
                if exceptional:
                    raise HciBridgeError("Owned guest socket failed")
                if stream is None:
                    if self._server in readable:
                        stream, address = self._server.accept()
                        if address[0] != "127.0.0.1":
                            stream.close()
                            raise HciBridgeError("Non-loopback peer rejected")
                        stream.setblocking(False)
                        stream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                        self._socket = stream
                        self._server.close(); self._server = None
                        self._set(connected=True, connectionAccepted=True)
                    continue
                if stream in readable:
                    data = stream.recv(65536)
                    if not data:
                        if parser.pending:
                            self._error("Guest disconnected with an incomplete H4 packet")
                        break
                    self._increment(guestWireBytesReceived=len(data))
                    for packet in parser.feed(data):
                        if tx_bytes + len(packet) > MAX_PENDING_BYTES or len(transmit) >= MAX_PENDING_PACKETS:
                            raise HciBridgeError("Guest TX queue exceeded the bound")
                        transmit.append(packet); tx_bytes += len(packet)
                # Bounded TX batch; service controller events between batches.
                for _ in range(min(16, len(transmit))):
                    if self._stop.is_set():
                        break
                    packet = self._next_tx(transmit)
                    if packet is None:
                        break
                    tx_bytes -= len(packet)
                    local = self._dispatch_tx(packet)
                    if local is not None:
                        if rx_bytes + len(local) > MAX_PENDING_BYTES or len(receive) >= MAX_PENDING_PACKETS:
                            raise HciBridgeError("Guest RX queue exceeded the bound")
                        receive.append([local, 0]); rx_bytes += len(local)
                if self._stop.is_set():
                    break
                packet = self._receive_controller(.005)
                if packet is not None:
                    if rx_bytes + len(packet) > MAX_PENDING_BYTES or len(receive) >= MAX_PENDING_PACKETS:
                        raise HciBridgeError("Guest RX queue exceeded the bound")
                    receive.append([packet, 0]); rx_bytes += len(packet)
                if stream in writable and receive:
                    packet, offset = receive[0]
                    try:
                        count = self._send_guest(stream, memoryview(packet)[offset:])
                    except BlockingIOError:
                        count = None
                    if count is not None:
                        if not 0 < count <= len(packet) - offset:
                            raise HciBridgeError("Guest socket send made invalid progress")
                        receive[0][1] += count
                        self._increment(guestWireBytesSent=count)
                        if receive[0][1] == len(packet):
                            receive.popleft(); rx_bytes -= len(packet)
                            self._increment(rxPackets=1, rxBytes=len(packet))
        except BaseException as error:
            self._error(error)
        finally:
            self._increment(discardedTxPackets=len(transmit), discardedRxPackets=len(receive))
            self._set(connected=False)
            for owned in (self._socket, self._server):
                if owned is not None:
                    try:
                        owned.close()
                    except OSError:
                        pass
            self._socket = self._server = None
            self._finish_controller()

    def _finish_controller(self, allow_reset=True):
        if allow_reset:
            self._teardown()
        else:
            self._set(teardownError="No verified guest channel was attached; no radio command sent")
        try:
            self._controller.close()
            self._set(controllerClosed=True)
        except BaseException as error:
            self._error("Owned controller close failed: " + str(error))
        self._set(closed=True)
        self._finished.set()

    def stop(self, timeout=15.0):
        timeout = _timeout(timeout, 30.0)
        self._stop.set()
        self._thread.join(timeout)
        if self._thread.is_alive():
            self._error("Owned bridge worker did not stop; no unowned process/handle was interrupted")
            raise HciBridgeError(self.stats["error"])
        return self.stats

    def close(self):
        return self.stop()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.stop()


def _windows_pipe_factory():
    if __package__:
        from .windows_guest_peer import WindowsHCIPipe
    else:
        from windows_guest_peer import WindowsHCIPipe
    return WindowsHCIPipe()


class NamedPipeControllerBridge(ControllerBridge):
    """Production Windows bridge: inert until attached to an owned Popen.

    QEMU creates the server with the Windows overlay's user/SYSTEM ACL and
    client-PID check. WindowsHCIPipe verifies file type, ACL and owned server
    PID. No unauthenticated TCP fallback exists here.
    """
    def __init__(self, controller, *, reset_on_stop, teardown_timeout=2.0,
                 max_acl_payload=1024, pipe_factory=None, connect_timeout=10.0):
        self._initialize(controller, reset_on_stop=reset_on_stop,
                         teardown_timeout=teardown_timeout, max_acl_payload=max_acl_payload)
        self._connect_timeout = _timeout(connect_timeout, 30.0)
        self._pipe = (pipe_factory or _windows_pipe_factory)()
        if not re.fullmatch(r"pipe,id=radio,path=AzkiSimulator-HCI-[0-9a-f]{32}", self._pipe.chardev_argument):
            self._pipe.close()
            raise HciBridgeError("A private Windows HCI pipe nonce is required")
        self._core = None
        self._set(backend="named-pipe-controller-h4", peerAuthenticated=False, pipeClosed=False)

    @property
    def descriptor(self):
        return {"transport": "owned-qemu-named-pipe-h4", "qemuChardev": self._pipe.chardev_argument,
                "machineOption": "bt-hci-chardev=radio",
                "qemuEnvironment": {"AZKI_QEMU_PIPE_CLIENT_PID": str(os.getpid())},
                "singleConnection": True, "resetOnStop": self._reset_on_stop,
                "peerAuthentication": "owned-QEMU-server-PID/private-ACL/client-PID",
                "peerAuthenticated": self.stats["peerAuthenticated"],
                "vendorCommandPolicy": "local-unsupported-vendor",
                "guestTxH4Types": [1, 2], "guestRxH4Types": [2, 4]}

    def attach_core(self, owned_process):
        with self._lock:
            if self._thread is not None or self._stop.is_set() or self._stats["closed"]:
                raise HciBridgeError("Owned QEMU can be attached exactly once")
            # Metadata only; the pipe's connect/read/flush/close all run on its
            # one worker. An external JSON PID must not be substituted here.
            self._pipe.attach_core(owned_process)
            self._core = owned_process
            self._thread = threading.Thread(target=self._run_pipe, name="azki-owned-hci-pipe", daemon=True)
            self._thread.start()
        return self

    def _run_pipe(self):
        transmit = deque()
        pending = bytearray()
        receive_frames = deque()
        tx_bytes = 0
        parser = _GuestFrames(self._max_acl)
        connected = False
        try:
            deadline = time.monotonic() + self._connect_timeout
            while not self._stop.is_set():
                if self._pipe.connect():
                    connected = True
                    self._set(connected=True, connectionAccepted=True, peerAuthenticated=True)
                    break
                if time.monotonic() >= deadline:
                    raise TimeoutError("Owned QEMU HCI pipe was not ready before the deadline")
                self._stop.wait(.005)
            while connected and not self._stop.is_set():
                data = self._pipe.read()  # b'' means no available/complete data.
                if data:
                    self._increment(guestWireBytesReceived=len(data))
                    for packet in parser.feed(data):
                        if tx_bytes + len(packet) > MAX_PENDING_BYTES or len(transmit) >= MAX_PENDING_PACKETS:
                            raise HciBridgeError("Guest TX queue exceeded the bound")
                        transmit.append(packet); tx_bytes += len(packet)
                for _ in range(min(16, len(transmit))):
                    if self._stop.is_set():
                        break
                    packet = self._next_tx(transmit)
                    if packet is None:
                        break
                    tx_bytes -= len(packet)
                    local = self._dispatch_tx(packet)
                    if local is not None:
                        if len(pending) + len(local) > MAX_PENDING_BYTES or len(receive_frames) >= MAX_PENDING_PACKETS:
                            raise HciBridgeError("Guest RX queue exceeded the bound")
                        pending.extend(local)
                        receive_frames.append([len(local), len(local)])
                if self._stop.is_set():
                    break
                packet = self._receive_controller(.005)
                if packet is not None:
                    if len(pending) + len(packet) > MAX_PENDING_BYTES or len(receive_frames) >= MAX_PENDING_PACKETS:
                        raise HciBridgeError("Guest RX queue exceeded the bound")
                    pending.extend(packet)
                    receive_frames.append([len(packet), len(packet)])
                count = self._pipe.flush(pending)
                if type(count) is not int or not 0 <= count <= len(pending):
                    raise HciBridgeError("Invalid owned pipe write completion")
                if count:
                    # A pending prefix is never changed until the pipe confirms
                    # completion. Appending a tail is explicitly permitted.
                    del pending[:count]
                    self._increment(guestWireBytesSent=count)
                    left = count
                    while left:
                        fragment = receive_frames[0]
                        taken = min(left, fragment[0])
                        fragment[0] -= taken; left -= taken
                        if fragment[0] == 0:
                            receive_frames.popleft()
                            self._increment(rxPackets=1, rxBytes=fragment[1])
        except BaseException as error:
            self._error(error)
        finally:
            self._increment(discardedTxPackets=len(transmit), discardedRxPackets=len(receive_frames))
            self._set(connected=False)
            try:
                self._pipe.close()
            except BaseException as error:
                self._error("Owned HCI pipe close failed: " + str(error))
            else:
                self._set(pipeClosed=True)
            self._finish_controller(allow_reset=connected)

    def _close_unattached(self):
        try:
            self._pipe.close()
        except BaseException as error:
            self._error("Unattached owned HCI pipe close failed: " + str(error))
        else:
            self._set(pipeClosed=True)
        self._finish_controller(allow_reset=False)

    def stop(self, timeout=15.0):
        _timeout(timeout, 30.0)
        with self._lock:
            if self._thread is None:
                self._stop.set()
                self._thread = threading.Thread(target=self._close_unattached, name="azki-hci-unattached-close", daemon=True)
                self._thread.start()
        return super().stop(timeout)
