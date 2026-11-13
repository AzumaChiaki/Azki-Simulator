"""Single-owner H4 scheduling for the ESP32 controller transport."""
from collections import deque
import time
from .esp32_transport import SerialBridgeError, validate_h4

PROTECTED = frozenset((0x0c05, 0x0c0d, 0x0c12, 0x0c5a, 0x0c5b))

class H4Stream:
    def __init__(self):
        self.buffer = bytearray()

    def feed(self, data):
        self.buffer.extend(data)
        result = []
        while self.buffer:
            kind = self.buffer[0]
            header = {1: 4, 2: 5}.get(kind)
            if header is None:
                raise SerialBridgeError('Guest H4 type is unsupported')
            if len(self.buffer) < header:
                break
            size = header + (self.buffer[3] if kind == 1 else int.from_bytes(self.buffer[3:5], 'little'))
            if size > 1029:
                raise SerialBridgeError('Guest H4 packet exceeds transport capacity')
            if len(self.buffer) < size:
                break
            packet = bytes(self.buffer[:size]); del self.buffer[:size]
            validate_h4(packet, True)
            result.append(packet)
        return result

class Controller:
    def __init__(self, transport):
        self.transport = transport
        self.credits = None
        self.pending = None
        self.queue = deque()
        self.local_rejections = []
        self.address = None
        self.failed = False
        self.tx_packets = self.rx_packets = 0

    def send(self, packet):
        try:
            self.transport.send_h4(packet)
            self.tx_packets += 1
            if packet[0] == 1:
                self.pending = int.from_bytes(packet[1:3], 'little')
                if self.credits is not None:
                    self.credits -= 1
        except BaseException:
            self.failed = True
            raise

    def receive(self, timeout=.01):
        try:
            packet = self.transport.receive(timeout)
            if packet is None:
                return None
            self.rx_packets += 1
            if packet[:2] == b'\x04\x0e':
                if len(packet) < 6:
                    raise SerialBridgeError('Short command completion')
                opcode = int.from_bytes(packet[4:6], 'little')
                credit = packet[3]
            elif packet[:2] == b'\x04\x0f':
                if len(packet) != 7:
                    raise SerialBridgeError('Malformed command status')
                opcode = int.from_bytes(packet[5:7], 'little')
                credit = packet[4]
            else:
                if packet[:2] == b'\x04\x10':
                    raise SerialBridgeError('Controller reported Hardware Error')
                return packet
            if opcode != self.pending and not (opcode == 0 and len(packet) == 6):
                raise SerialBridgeError('Unowned physical command completion')
            self.credits = credit
            if opcode == self.pending:
                if opcode == 0x1009 and packet[:2] == b'\x04\x0e' and len(packet) == 13 and packet[6] == 0:
                    address = ':'.join(f'{x:02X}' for x in packet[7:13][::-1])
                    if self.address and self.address != address:
                        raise SerialBridgeError('Controller address changed')
                self.pending = None
            return packet
        except BaseException:
            self.failed = True
            raise

    def command(self, opcode, length, *, tail=None):
        if self.pending is not None or (self.credits is not None and self.credits < 1):
            raise SerialBridgeError('No observed physical command credit')
        self.send(b'\x01' + opcode.to_bytes(2, 'little') + b'\x00')
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            packet = self.receive()
            if packet is None:
                continue
            if tail is not None and not (packet[:2] == b'\x04\x0e' and len(packet) >= 6
                    and int.from_bytes(packet[4:6], 'little') == opcode):
                tail.append(packet.hex())
                continue
            if (len(packet) != length or packet[:2] != b'\x04\x0e'
                    or int.from_bytes(packet[4:6], 'little') != opcode or packet[6] != 0):
                self.failed = True
                raise SerialBridgeError('Unexpected physical initialization/stop response')
            return packet
        self.failed = True
        raise SerialBridgeError('Physical command response timed out')

    def initialize(self):
        self.command(0x0c03, 7)
        reply = self.command(0x1009, 13)
        self.address = ':'.join(f'{x:02X}' for x in reply[7:13][::-1])
        if self.address != self.transport.controller_address_hint:
            raise SerialBridgeError('HCI address differs from controller READY')
        self.command(0x1001, 15)
        return self.address

    def dispatch(self):
        if len(self.queue) > 256:
            raise SerialBridgeError('Guest transmit queue exceeded capacity')
        local = []
        while self.queue:
            index = 0
            if self.queue[0][0] == 1 and (self.pending is not None or not self.credits):
                index = next((i for i, p in enumerate(self.queue) if p[0] == 2), -1)
                if index < 0:
                    break
            packet = self.queue[index]; del self.queue[index]
            opcode = int.from_bytes(packet[1:3], 'little') if packet[0] == 1 else None
            if opcode is not None and (opcode in PROTECTED or opcode >> 10 == 0x3f):
                self.local_rejections.append({'opcode': opcode, 'status': 1,
                    'source': 'operator-protected-command' if opcode in PROTECTED else 'foreign-vendor-command',
                    'physicalSent': False})
                local.append(bytes((4, 0x0e, 4, 1, packet[1], packet[2], 1)))
            else:
                self.send(packet)
        return local

    def teardown(self):
        if self.failed:
            raise SerialBridgeError('Physical I/O failed; no teardown retry')
        # Guest shutdown can leave a command in flight. Its real completion is
        # consumed before the helper's own Reset, never mistaken for that Reset.
        tail = []
        deadline = time.monotonic() + 2
        while self.pending is not None and time.monotonic() < deadline:
            packet = self.receive()
            if packet is not None:
                tail.append(packet.hex())
        self.command(0x0c03, 7, tail=tail)
        return tail
