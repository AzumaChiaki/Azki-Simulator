"""H4 packets over the owned ESP32 UART0/VHCI link; no host Bluetooth stack."""
import struct
import time
import threading

BANNER = b"AZKI_VHCI_SERIAL_V1 READY\n"
HCI, START, STOP, READY, CLOSED, PING, FATAL = 1, 0x10, 0x11, 0x12, 0x13, 0x14, 0x7f
MAX_PACKET = 1029

class SerialBridgeError(OSError):
    pass

def validate_h4(packet, outbound):
    if not isinstance(packet, bytes) or not packet or len(packet) > MAX_PACKET:
        raise SerialBridgeError("Invalid H4 packet size")
    kind = packet[0]
    if kind == 1 and outbound and len(packet) >= 4:
        expected = 4 + packet[3]
    elif kind == 2 and len(packet) >= 5:
        expected = 5 + int.from_bytes(packet[3:5], "little")
    elif kind == 4 and not outbound and len(packet) >= 3:
        expected = 3 + packet[2]
    else:
        raise SerialBridgeError("Unexpected H4 packet direction or type")
    if len(packet) != expected:
        raise SerialBridgeError("H4 length mismatch")

class Esp32SerialTransport:
    def __init__(self, port, identity, lease, *, serial_factory=None):
        self.identity = dict(identity)
        self.lease = lease
        self._serial = None
        self._tx_lock = threading.RLock()
        self._rx_lock = threading.RLock()
        self._closing = False
        self.cleanup_errors = []
        self._buffer = bytearray()
        self._error = None
        self._ready = False
        self._closed = False
        self._last_tx = time.monotonic()
        self.close_packets = []
        self.controller_address_hint = None
        self.boot_diagnostics = {'receivedBytes': 0, 'tailHex': '', 'truncated': False, 'bannerMatched': False}
        if identity.get("instanceID", "").casefold() != lease.instance_id.casefold():
            raise SerialBridgeError("Serial identity differs from owned lease")
        if serial_factory is None:
            import serial
            serial_factory = serial.Serial
        try:
            handle = serial_factory(port=None, baudrate=115200, timeout=.01,
                                    write_timeout=2, rtscts=False, dsrdtr=False)
            self._serial = handle
            handle.dtr = False
            handle.rts = False
            handle.port = port
            handle.open()
            # The identified board uses RTS for EN, not UART hardware flow control.
            handle.rts = True
            time.sleep(.1)
            handle.rts = False
            self._boot_banner(time.monotonic() + 10)
            self._write_frame(START, b"")
            reply = self._frame(time.monotonic() + 10)
            if reply is None or reply[0] != READY or len(reply[1]) != 6:
                raise SerialBridgeError("Controller did not become ready")
            self.controller_address_hint = ":".join(f"{x:02X}" for x in reply[1])
            self._ready = True
        except BaseException as error:
            self._error = str(error)
            if self._serial is not None:
                try:
                    self._serial.close()
                except BaseException as cleanup:
                    self.cleanup_errors.append(str(cleanup))
            error.raw_cleanup = {"controllerClosed": self._serial is None or
                                 not self._serial.is_open,
                                 "cleanupErrors": list(self.cleanup_errors),
                                 "bootDiagnostics": dict(self.boot_diagnostics)}
            raise

    def _boot_banner(self, deadline):
        # Only pre-controller boot text is skipped. No HCI exists before START.
        boot = bytearray()
        diagnostic_tail = bytearray()
        while time.monotonic() < deadline:
            chunk = self._serial.read(256)
            boot.extend(chunk)
            diagnostic_tail.extend(chunk)
            del diagnostic_tail[:-2048]
            self.boot_diagnostics['receivedBytes'] += len(chunk)
            self.boot_diagnostics['tailHex'] = diagnostic_tail.hex()
            self.boot_diagnostics['truncated'] = self.boot_diagnostics['receivedBytes'] > len(diagnostic_tail)
            index = boot.find(BANNER)
            if index >= 0:
                self.boot_diagnostics["bannerMatched"] = True
                self._buffer.extend(boot[index + len(BANNER):])
                return
            if len(boot) > 4096:
                del boot[:-len(BANNER)]
        raise SerialBridgeError("ESP32 bridge boot banner absent")

    def _write_frame(self, kind, data):
        with self._tx_lock:
            if self._closing and kind != STOP:
                raise SerialBridgeError("Serial link closing")
            return self._write_frame_locked(kind, data)

    def _write_frame_locked(self, kind, data):
        if self._error or self._closed:
            raise SerialBridgeError(self._error or "Serial link closed")
        wire = struct.pack("<BBH", 0xa5, kind, len(data)) + data
        try:
            if self._serial.write(wire) != len(wire):
                raise SerialBridgeError("Incomplete serial write")
        except BaseException as error:
            self._error = str(error)
            raise
        self._last_tx = time.monotonic()

    def _frame(self, deadline):
        while True:
            if len(self._buffer) >= 4:
                magic, kind, size = struct.unpack_from("<BBH", self._buffer)
                if magic != 0xa5 or size > MAX_PACKET:
                    raise SerialBridgeError("Serial framing lost")
                if len(self._buffer) >= 4 + size:
                    data = bytes(self._buffer[4:4 + size])
                    del self._buffer[:4 + size]
                    if kind == FATAL:
                        raise SerialBridgeError("ESP32 fatal: " + data.hex())
                    return kind, data
            if time.monotonic() >= deadline:
                return None
            self._buffer.extend(self._serial.read(4096))

    def send_h4(self, packet):
        if not self._ready:
            raise SerialBridgeError("Controller not ready")
        validate_h4(packet, True)
        self._write_frame(HCI, packet)

    def receive(self, timeout=.05):
        with self._rx_lock:
            if self._closing:
                raise SerialBridgeError("Serial link closing")
            return self._receive_locked(timeout)

    def _receive_locked(self, timeout):
        if self._error or self._closed:
            raise SerialBridgeError(self._error or "Serial link closed")
        try:
            if self._ready and time.monotonic() - self._last_tx > 2:
                self._write_frame(PING, b"")
            frame = self._frame(time.monotonic() + timeout)
            if frame is None:
                return None
            kind, data = frame
            if kind != HCI:
                raise SerialBridgeError("Unexpected serial control frame")
            validate_h4(data, False)
            return data
        except BaseException as error:
            self._error = str(error)
            raise

    @property
    def capabilities(self):
        return {"backend": "esp32-uart0-vhci", "rawHciTransport": True,
                "controllerInstanceID": self.identity["instanceID"],
                "controllerIdentity": self.identity, "exclusiveLease": self.lease.descriptor,
                "ready": self._ready, "closed": self._closed, "firstError": self._error,
                "cleanupErrors": list(self.cleanup_errors),
                "bootDiagnostics": dict(self.boot_diagnostics),
                "addressHint": self.controller_address_hint,
                "addressHintSource": "firmware-esp-read-mac-not-hci-1009",
                "physicalRadioVerified": False, "syntheticHciReplies": False,
                "sco": False, "iso": False, "maxAclPayloadBytes": 1024}

    def close(self):
        # The bridge stops its worker first; this lock excludes an in-flight read.
        with self._tx_lock:
            self._closing = True
        with self._rx_lock:
            return self._close_locked()

    def _close_locked(self):
        if self._closed:
            return
        error = None
        try:
            if self._ready and not self._error:
                self._write_frame(STOP, b"")
                deadline = time.monotonic() + 3
                while True:
                    frame = self._frame(deadline)
                    if frame is None:
                        raise SerialBridgeError("Controller stop not confirmed")
                    kind, data = frame
                    if kind == CLOSED and not data:
                        break
                    if kind == HCI:
                        validate_h4(data, False)
                        self.close_packets.append(data)
                    else:
                        raise SerialBridgeError("Unexpected frame during stop")
                if self._buffer or self.close_packets:
                    raise SerialBridgeError("Real HCI or partial bytes remain at close")
            elif self._error:
                raise SerialBridgeError("Failed link; controller stop cannot be confirmed")
        except BaseException as caught:
            self._error = self._error or str(caught)
            error = caught
        finally:
            try:
                if self._serial is not None:
                    self._serial.close()
                self._closed = True
            except BaseException as cleanup:
                self.cleanup_errors.append(str(cleanup))
                error = error or cleanup
                self._error = self._error or str(cleanup)
            self._ready = False
        if error:
            raise error
