"""Bounded local LE ACL transport layered on the real-crypto command plane.

Only an explicit same-process virtual central can open a connection. This does
not advertise on a radio, perform SMP or pretend that an encrypted link exists.
"""
import struct

from .hci import Command, VirtualController, complete, supported_commands

ACL_BYTES = 251
ACL_CREDITS = 8
L2CAP_BYTES = 4096
HANDLE = 0x0040
UNKNOWN_CONNECTION = 0x02
COMMAND_DISALLOWED = 0x0C
LINK_COMMANDS = {
    0x0406: Command('Disconnect', 3, 0, 5, 0),
    0x041D: Command('Read_Remote_Version_Information', 2, 2, 7, 0),
    0x2016: Command('LE_Read_Remote_Features', 2, 27, 5, 0),
}


def event(code, body):
    if len(body) > 255:
        raise ValueError('HCI event bound')
    return bytes((4, code, len(body))) + body


def status(opcode, value=0):
    return event(0x0F, struct.pack('<BBH', value, 1, opcode))


class LinkController(VirtualController):
    def reset(self):
        super().reset()
        self.connected = False
        self.closed_handle_pending = False
        self.closed_acl_remaining = 0
        self.disconnect_reason = None
        self.guest_disconnect_reason = None
        self.link_events = []
        self.generation = getattr(self, 'generation', 0) + 1

    def command_bits(self):
        bits = bytearray(supported_commands())
        for entry in LINK_COMMANDS.values():
            bits[entry.supported_byte] |= 1 << entry.supported_bit
        return bytes(bits)

    def enabled_event(self, code, body, subevent=None):
        if not int.from_bytes(self.event_mask, 'little') & (1 << (code - 1)):
            return []
        if subevent is not None:
            if not int.from_bytes(self.le_event_mask, 'little') & (1 << (subevent - 1)):
                return []
            body = bytes((subevent,)) + body
        return [event(code, body)]

    def connect_local(self):
        if self.connected:
            raise ValueError('One virtual connection is already active')
        # Synthetic peer 02:00:00:00:00:A0, deliberately not a real identity.
        body = struct.pack('<BHBB', 0, HANDLE, 1, 0) + bytes.fromhex('a00000000002')
        body += struct.pack('<HHHB', 24, 0, 200, 0)
        events = self.enabled_event(0x3E, body, 1)
        if not events:
            raise ValueError('Host has not enabled LE connection events')
        self.connected = True
        self.closed_handle_pending = False
        self.closed_acl_remaining = 0
        self.disconnect_reason = None
        self.guest_disconnect_reason = None
        self.generation += 1
        return events

    def disconnect_local(self, reason=0x13):
        if not self.connected:
            raise ValueError('No virtual connection')
        self.connected = False
        self.closed_handle_pending = True
        self.closed_acl_remaining = ACL_CREDITS
        self.disconnect_reason = reason
        self.guest_disconnect_reason = None
        self.generation += 1
        return self.enabled_event(0x05, struct.pack('<BHB', 0, HANDLE, reason))

    def respond(self, opcode, params):
        if opcode == 0x1002 and not params:
            return complete(opcode, 0, self.command_bits())
        if opcode == 0x2002 and not params:
            return complete(opcode, 0, struct.pack('<HB', ACL_BYTES, ACL_CREDITS))
        if opcode == 0x201C and not params:
            return complete(opcode, 0, (1 << 7).to_bytes(8, 'little'))  # peripheral connection only
        if opcode not in LINK_COMMANDS:
            return super().respond(opcode, params)
        if len(params) != LINK_COMMANDS[opcode].length:
            return status(opcode, 0x12)
        handle = struct.unpack_from('<H', params)[0]
        if not self.connected or handle != HANDLE:
            return status(opcode, UNKNOWN_CONNECTION)
        if opcode == 0x0406:
            if params[2] not in (0x05, 0x13, 0x14, 0x15, 0x1A, 0x29, 0x3B):
                return status(opcode, 0x12)
            self.link_events += self.disconnect_local(0x16)  # local host terminated
            self.guest_disconnect_reason = params[2]
        elif opcode == 0x041D:
            self.link_events += self.enabled_event(0x0C, struct.pack('<BHBHH', 0, HANDLE, 6, 0xFFFF, 1))
        elif opcode == 0x2016:
            self.link_events += self.enabled_event(0x3E, struct.pack('<BH', 0, HANDLE) + bytes(8), 4)
        return status(opcode)

    def drain_events(self):
        events, self.link_events = self.link_events, []
        return events

    def transfer_air(self,direction,pb,data):
        return data


class H4Link:
    """One connection, capped H4/L2CAP buffers and immediately returned credits.

    An ACL credit is returned only after the packet has been copied to the
    bounded reassembly buffer or delivered to the peer. Invalid packets fail
    before a credit is emitted. The caller must flush responses after feed.
    Host-directed data uses HCI's default disabled controller-to-host flow
    control; it is still capped by the peer's one-outstanding-ATT procedure.
    """
    def __init__(self, controller, on_l2cap, max_packets=8192):
        self.controller = controller
        self.on_l2cap = on_l2cap
        self.max_packets = max_packets
        self.pending = bytearray()
        self.reassembly = bytearray()
        self.expected = None
        self.generation = controller.generation
        self.count = 0
        self.acl_count = 0
        self.l2cap_count = 0
        self.disconnected_acl_dropped = 0
        self.summary = {}
        self.events = []

    def _sync_connection(self):
        if self.generation != self.controller.generation:
            self.reassembly.clear()
            self.expected = None
            self.generation = self.controller.generation

    def from_peer(self, cid, data):
        self._sync_connection()
        if not self.controller.connected:
            raise ValueError('Peer ACL without virtual connection')
        if cid not in (4, 5, 6) or not 1 <= len(data) <= L2CAP_BYTES:
            raise ValueError('Peer L2CAP bound or channel')
        frame = struct.pack('<HH', len(data), cid) + data
        packets = []
        for offset in range(0, len(frame), ACL_BYTES):
            chunk = frame[offset:offset + ACL_BYTES]
            chunk = self.controller.transfer_air(0,2 if offset==0 else 1,chunk)
            flags = HANDLE | (0x2000 if offset == 0 else 0x1000)
            packets.append(b'\x02' + struct.pack('<HH', flags, len(chunk)) + chunk)
        return packets

    def _consume_acl(self, packet):
        self._sync_connection()
        flags, size = struct.unpack_from('<HH', packet, 1)
        pb = flags >> 12 & 3
        if flags >> 14 or pb not in (0, 1, 2):
            raise ValueError('Unsupported ACL flags')
        if flags & 0xFFF != HANDLE:
            raise ValueError('ACL for unknown virtual connection')
        if not self.controller.connected:
            # Host may already have queued its advertised ACL window when our
            # Disconnect Complete crosses the wire. Reclaiming credits is a
            # host-side disconnect operation; do not emit stale completion
            # events or deliver these packets to another connection.
            if self.controller.closed_handle_pending and self.controller.closed_acl_remaining:
                self.controller.closed_acl_remaining -= 1
                self.acl_count += 1
                self.disconnected_acl_dropped += 1
                return []
            raise ValueError('ACL for unknown virtual connection')
        data = self.controller.transfer_air(1,pb,packet[5:])
        if pb in (0, 2):
            if self.reassembly or len(data) < 4:
                raise ValueError('Unexpected ACL start')
            length, cid = struct.unpack_from('<HH', data)
            if not 1 <= length <= L2CAP_BYTES or cid not in (4, 5, 6):
                raise ValueError('L2CAP bound or channel')
            self.expected = 4 + length
        elif not self.reassembly:
            raise ValueError('ACL continuation without start')
        if len(self.reassembly) + size > self.expected:
            raise ValueError('ACL exceeds declared L2CAP length')
        self.reassembly.extend(data)
        outgoing = []
        if len(self.reassembly) == self.expected:
            _, cid = struct.unpack_from('<HH', self.reassembly)
            payload = bytes(self.reassembly[4:])
            self.reassembly.clear()
            self.expected = None
            for reply_cid, reply in self.on_l2cap(cid, payload):
                outgoing.extend(self.from_peer(reply_cid, reply))
            outgoing.extend(self.controller.drain_events())
            self.l2cap_count += 1
        self.acl_count += 1
        # This event is mandatory and exempt from Set Event Mask.
        return [event(0x13, struct.pack('<BHH', 1, HANDLE, 1))] + outgoing

    def feed(self, chunk):
        if not isinstance(chunk, bytes) or len(chunk) > 4096:
            raise ValueError('H4 read size limit')
        responses = []
        acl_in_batch = 0
        for byte in chunk:
            self.pending.append(byte)
            kind = self.pending[0]
            if kind not in (1, 2):
                raise ValueError('Unsupported H4 packet type')
            header = 4 if kind == 1 else 5
            if len(self.pending) < header:
                continue
            size = self.pending[3] if kind == 1 else struct.unpack_from('<H', self.pending, 3)[0]
            if kind == 2 and not 1 <= size <= ACL_BYTES:
                raise ValueError('ACL packet size limit')
            if len(self.pending) < header + size:
                continue
            if self.max_packets is not None and self.count + self.acl_count >= self.max_packets:
                raise ValueError('H4 packet quota exceeded')
            packet = bytes(self.pending)
            self.pending.clear()
            if kind == 2:
                acl_in_batch += 1
                if acl_in_batch > ACL_CREDITS:
                    raise ValueError('Host exceeded advertised ACL credits before response flush')
                responses.extend(self._consume_acl(packet))
            else:
                opcode = struct.unpack_from('<H', packet, 1)[0]
                response = self.controller.respond(opcode, packet[4:])
                value = response[3] if response[1] == 0x0F else response[6]
                self.count += 1
                key = (opcode, value)
                self.summary[key] = self.summary.get(key, 0) + 1
                if len(self.events) < 256:
                    row = {'index': self.count, 'opcode': f'0x{opcode:04x}', 'parameterBytes': size, 'status': value}
                    if opcode == 0xFC32 and size == 6:
                        row['matchesSyntheticAddressLE'] = packet[4:] == self.controller.address[::-1]
                    if opcode == 0xFC72 and size == 7:
                        row['matchesSyntheticAddressLE'] = packet[5:] == self.controller.address[::-1]
                    if opcode == 0x0406 and size == 3:
                        row['requestedDisconnectReason'] = packet[6]
                    self.events.append(row)
                responses.append(response)
                responses.extend(self.controller.drain_events())
                self._sync_connection()
        return responses

    def finish(self):
        if self.pending:
            raise ValueError('Truncated H4 packet')
        if self.controller.connected and self.reassembly:
            raise ValueError('Truncated L2CAP packet')

    def report(self):
        return [{'opcode': f'0x{opcode:04x}', 'status': value, 'count': count}
                for (opcode, value), count in sorted(self.summary.items())]
