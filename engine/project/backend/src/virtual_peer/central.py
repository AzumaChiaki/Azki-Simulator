"""Read-only GATT discovery and SAR-start acceptance from a synthetic local peer.

The peer derives characteristic and CCCD handles from guest responses. It never
fabricates a guest response, seeds an authKey or labels SAR as pairing.
"""
import struct
import sys


def crc16(data):
    value = 0
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0xA001 if value & 1 else 0)
    return value


def sar_start():
    payload = bytes.fromhex('0101030001000002020000fc03020020000402001027')
    return b'\xa5\xa5\x02\0' + struct.pack('<HH', len(payload), crc16(payload)) + payload


def sar_configuration(frame):
    if len(frame) < 9 or frame[:4] != b'\xa5\xa5\x02\0':
        raise ValueError('Unexpected SAR start response header')
    length, crc = struct.unpack_from('<HH', frame, 4)
    payload = frame[8:]
    if len(payload) != length or crc16(payload) != crc or payload[0] != 2:
        raise ValueError('Invalid SAR start response length, CRC or opcode')
    values = {}
    offset = 1
    while offset < len(payload):
        if offset + 3 > len(payload):
            raise ValueError('Truncated SAR configuration TLV')
        kind, size = struct.unpack_from('<BH', payload, offset)
        offset += 3
        if kind in values or offset + size > len(payload):
            raise ValueError('Invalid SAR configuration TLV')
        value = payload[offset:offset + size]
        offset += size
        if kind == 1 and size == 3:
            values[kind] = list(value)
        elif kind in (2, 3, 4) and size == 2:
            values[kind] = int.from_bytes(value, 'little')
        else:
            raise ValueError('Unsupported SAR configuration TLV')
    if set(values) != {1, 2, 3, 4} or any(values[k] == 0 for k in (2, 3, 4)):
        raise ValueError('Incomplete SAR configuration')
    return {'version': values[1], 'mps': values[2], 'txWindow': values[3], 'sendTimeoutMilliseconds': values[4], 'crcVerified': True}


class LocalCentral:
    def __init__(self, controller, delay=1.0, bind_info=False, bind_dynamic_code=0, connection_timeout=30):
        if not 0 <= delay <= 20:
            raise ValueError('Virtual central bounds')
        self.controller = controller
        self.delay = delay
        self.link = None
        self.sessions = []
        self.session = None
        self.state = 'idle'
        self.next_at = delay
        self.deadline = None
        self.request = None
        self.cursor = 1
        self.notify = self.write = self.cccd = None
        self.sar_pending = bytearray()
        self.requests = 0
        self.indications_confirmed = 0
        self.smp_rejected = 0
        self.errors = []
        self.bind_info = bind_info
        self.bind_dynamic_code = bind_dynamic_code
        self.bind_frames = None
        self.reconnect_delay = .5
        self.connection_timeout = 30
        self.set_connection_timeout(connection_timeout)
        self.transport_service='fe95'
        self.notify_uuid='005e'
        self.write_uuid='005f'

    def set_connection_timeout(self, seconds):
        if type(seconds) is not int or not 1 <= seconds <= 300:
            raise ValueError('Connection timeout must be 1–300 seconds')
        if self.deadline is not None:
            self.deadline += seconds - self.connection_timeout
        self.connection_timeout = seconds
        self.sar_timeout = seconds

    def _fail(self, message):
        self.errors.append(message)
        self.state = 'failed'
        if self.session is not None:
            self.session['error'] = message
        self.deadline = None

    def _send_request(self, pdu, state, now):
        self.requests += 1
        if self.requests > 256:
            raise ValueError('GATT request limit')
        self.request = pdu[0]
        self.state = state
        self.deadline = now + self.connection_timeout
        return self.link.from_peer(4, pdu)

    def _sar_writes(self, wire):
        limit = self.session['mtu'] - 3
        return [(4, b'\x52' + struct.pack('<H', self.write) + wire[offset:offset + limit])
                for offset in range(0, len(wire), limit)]

    def tick(self, now):
        if self.state in ('done', 'failed'):
            return []
        if self.session is not None and not self.controller.connected and self.state not in ('idle', 'reconnect'):
            self.session['guestDisconnected'] = True
            self._fail('Guest disconnected during ' + self.state)
            return []
        if self.deadline is not None and now > self.deadline:
            self._fail('Timed out awaiting guest ' + self.state)
            return []
        if now < self.next_at:
            return []
        if self.state in ('idle', 'reconnect'):
            if not self.controller.local_name.rstrip(b'\0').startswith(b'Azki Virtual'):
                try:
                    events = self.controller.connect_local()
                except ValueError as error:
                    self._fail(str(error))
                    return []
                self.session = {'index': len(self.sessions) + 1, 'connectionMode': 'explicit-local-central',
                                'services': [], 'characteristics': [], 'descriptors': [], 'sarVerified': False}
                self.sessions.append(self.session)
                self.cursor = 1
                self.notify = self.write = self.cccd = None
                self.sar_pending.clear()
                self.state = 'mtu'
                self.next_at = now + 0.25
                return events
            return []
        if self.state == 'mtu':
            return self._send_request(b'\x02' + struct.pack('<H', 247), 'wait_mtu', now)
        if self.state == 'services':
            return self._send_request(b'\x10' + struct.pack('<HHH', self.cursor, 0xFFFF, 0x2800), 'wait_services', now)
        if self.state == 'characteristics':
            return self._send_request(b'\x08' + struct.pack('<HHH', self.cursor, 0xFFFF, 0x2803), 'wait_characteristics', now)
        if self.state == 'descriptors':
            return self._send_request(b'\x04' + struct.pack('<HH', self.cursor, self.descriptor_end), 'wait_descriptors', now)
        if self.state == 'subscribe':
            if self.cccd is None:
                self._fail('Guest notification CCCD was not discovered')
                return []
            return self._send_request(b'\x12' + struct.pack('<H', self.cccd) + b'\x01\0', 'wait_subscribe', now)
        if self.state == 'sar':
            frames = []
            for cid, pdu in self._sar_writes(sar_start()):
                frames += self.link.from_peer(cid, pdu)
            self.state = 'wait_sar'
            self.deadline = now + self.sar_timeout
            return frames
        if self.state == 'bind_info':
            from .bind_info import Frames, request
            self.bind_frames = Frames()
            self.session['bindInfo'] = {'requested': True, 'received': False, 'transportAcknowledged': False, 'dynamicCodeCheck': self.bind_dynamic_code}
            frames = []
            for cid, pdu in self._sar_writes(request(dynamic_code=self.bind_dynamic_code)):
                frames += self.link.from_peer(cid, pdu)
            self.state = 'wait_bind_info'
            self.deadline = now + self.connection_timeout
            return frames
        if self.state == 'disconnect':
            events = self.controller.disconnect_local()
            self.session['disconnectEventSent'] = True
            self.state = 'reconnect'
            self.next_at = now + self.reconnect_delay
            return events
        return []

    def _discover_transport(self):
        chars = self.session['characteristics']
        service = next((s for s in self.session['services'] if s['uuid'] == self.transport_service), None)
        if service is None:
            raise ValueError('Guest selected transport service missing')
        within = [c for c in chars if service['start'] <= c['decl'] <= service['end']]
        notify = next((c for c in within if c['uuid'] == self.notify_uuid and c['properties'] & 0x10), None)
        write = next((c for c in within if c['uuid'] == self.write_uuid and c['properties'] & 0x04), None)
        if notify is None or write is None:
            raise ValueError('Guest MiWear characteristics missing or incompatible')
        self.notify, self.write = notify['value'], write['value']
        next_decl = min([c['decl'] for c in within if c['decl'] > notify['decl']] + [service['end'] + 1])
        self.cursor = self.notify + 1
        self.descriptor_end = next_decl - 1
        if self.cursor > self.descriptor_end:
            raise ValueError('Guest notification characteristic has no descriptor range')
        self.session['transportHandles'] = {'notify': self.notify, 'write': self.write}
        self.session['transportService'] = self.transport_service
        self.state = 'descriptors'

    def consume(self, cid, pdu):
        if cid == 6:
            self.smp_rejected += 1
            # Pairing Failed / Pairing Not Supported. No fake encryption event.
            return [(6, b'\x05\x05')]
        if cid == 5:
            if len(pdu) < 4 or struct.unpack_from('<H', pdu, 2)[0] != len(pdu) - 4:
                raise ValueError('Malformed LE signaling request')
            code, identifier = pdu[:2]
            if code == 0x12 and len(pdu) == 12:
                return [(5, struct.pack('<BBHH', 0x13, identifier, 2, 1))]  # parameters rejected
            if code & 1:  # unsupported responses have no further response
                return []
            return [(5, struct.pack('<BBHH', 1, identifier, 2, 0))]
        if not pdu or len(pdu) > 247:
            raise ValueError('ATT PDU outside negotiated bound')
        op = pdu[0]
        if op == 0x1D:
            if len(pdu) < 3:
                raise ValueError('Short ATT indication')
            self.indications_confirmed += 1
            return [(4, b'\x1e')]
        if op == 0x02:
            if len(pdu) != 3:
                raise ValueError('Malformed guest MTU request')
            return [(4, b'\x03\xf7\0')]
        if op == 0x1B:
            if len(pdu) < 3:
                raise ValueError('Short ATT notification')
            handle = struct.unpack_from('<H', pdu, 1)[0]
            if handle != self.notify:
                return []
            if self.state == 'wait_bind_info':
                from .bind_info import encode, inspect
                outgoing = []
                for kind, sequence, payload in self.bind_frames.feed(pdu[3:]):
                    if kind == 1 and sequence == 0 and not payload:
                        self.session['bindInfo']['transportAcknowledged'] = True
                    elif kind == 3:
                        address = ':'.join(f'{v:02x}' for v in self.controller.address)
                        self.session['bindInfo'].update(inspect(payload, 'AZKI-Q66-VIRTUAL01', address, '9906600000001'))
                        outgoing += self._sar_writes(encode(1, sequence, b''))
                        self.state = 'disconnect'
                        self.deadline = None
                    else:
                        raise ValueError('Unexpected SAR response to BindInfo')
                return outgoing
            if self.state != 'wait_sar':
                return []
            self.sar_pending.extend(pdu[3:])
            if len(self.sar_pending) > 1024:
                raise ValueError('SAR handshake buffer bound')
            if len(self.sar_pending) < 8:
                return []
            wanted = 8 + struct.unpack_from('<H', self.sar_pending, 4)[0]
            if wanted > 1024:
                raise ValueError('SAR handshake length bound')
            if len(self.sar_pending) < wanted:
                return []
            self.session['sarConfiguration'] = sar_configuration(bytes(self.sar_pending))
            self.session['sarVerified'] = True
            self.deadline = None
            self.state = 'bind_info' if self.bind_info else 'disconnect'
            return []
        if self.session is None:
            raise ValueError('ATT response before local connection')
        if op == 0x01:
            if len(pdu) != 5 or pdu[1] != self.request:
                raise ValueError('Unmatched ATT error response')
            if pdu[4] == 0x12 and self.state in (
                    'wait_services', 'wait_characteristics', 'wait_descriptors', 'wait_subscribe'):
                # GATT 2.5.2.1: Database Out Of Sync invalidates discovered
                # handles. Rediscover on this same ATT bearer; reconnecting
                # here would leave the peer change-unaware again.
                rediscoveries = self.session.get('databaseRediscoveries', 0)
                if rediscoveries < 2:
                    self.session['databaseRediscoveries'] = rediscoveries + 1
                    for field in ('services', 'characteristics', 'descriptors'):
                        self.session[field] = []
                    self.session.pop('transportHandles', None)
                    self.session.pop('transportService', None)
                    self.notify = self.write = self.cccd = None
                    self.cursor = 1
                    self.state = 'services'
                    self.deadline = None
                    return []
            if pdu[4] != 0x0A:
                print('Virtual peer ATT error: request=0x%02x handle=0x%04x code=0x%02x state=%s' %
                      (pdu[1], struct.unpack_from('<H', pdu, 2)[0], pdu[4], self.state), file=sys.stderr, flush=True)
                self._fail('Guest ATT error 0x%02x for request 0x%02x' % (pdu[4], pdu[1]))
                return []
            if self.state == 'wait_services':
                self.state, self.cursor = 'characteristics', 1
            elif self.state == 'wait_characteristics':
                self._discover_transport()
            elif self.state == 'wait_descriptors':
                self.state = 'subscribe'
            else:
                self._fail('Unexpected ATT attribute-not-found')
            self.deadline = None
            return []
        if op == 3 and self.state == 'wait_mtu' and len(pdu) == 3:
            mtu = struct.unpack_from('<H', pdu, 1)[0]
            if not 23 <= mtu <= 517:
                raise ValueError('Invalid guest ATT MTU')
            self.session['mtu'] = min(247, mtu)
            self.state, self.cursor = 'services', 1
        elif op in (0x11, 0x09) and self.state in ('wait_services', 'wait_characteristics'):
            services = self.state == 'wait_services'
            sizes = (6, 20) if services else (7, 21)
            if len(pdu) < 2 or pdu[1] not in sizes or not (len(pdu) - 2) or (len(pdu) - 2) % pdu[1]:
                raise ValueError('Malformed GATT discovery response')
            if op != (0x11 if services else 0x09):
                raise ValueError('Unmatched GATT discovery response')
            size = pdu[1]
            previous = self.cursor - 1
            for offset in range(2, len(pdu), size):
                if services:
                    start, end = struct.unpack_from('<HH', pdu, offset)
                    if not previous < start <= end:
                        raise ValueError('Nonmonotonic GATT service handles')
                    self.session['services'].append({'start': start, 'end': end, 'uuid': pdu[offset + 4:offset + size][::-1].hex()})
                    previous = end
                else:
                    decl, properties, value = struct.unpack_from('<HBH', pdu, offset)
                    if not previous < decl < value:
                        raise ValueError('Nonmonotonic GATT characteristic handles')
                    self.session['characteristics'].append({'decl': decl, 'properties': properties, 'value': value, 'uuid': pdu[offset + 5:offset + size][::-1].hex()})
                    previous = decl
            if len(self.session['characteristics']) + len(self.session['services']) > 256:
                raise ValueError('GATT discovery item limit')
            self.cursor = previous + 1
            if self.cursor > 0xFFFF:
                if services:
                    self.state, self.cursor = 'characteristics', 1
                else:
                    self._discover_transport()
            else:
                self.state = 'services' if services else 'characteristics'
        elif op == 0x05 and self.state == 'wait_descriptors':
            if len(pdu) < 6 or pdu[1] != 1 or (len(pdu) - 2) % 4:
                raise ValueError('Malformed CCCD discovery response')
            previous = self.cursor - 1
            for offset in range(2, len(pdu), 4):
                handle, uuid = struct.unpack_from('<HH', pdu, offset)
                if not previous < handle <= self.descriptor_end:
                    raise ValueError('Invalid GATT descriptor handle')
                self.session['descriptors'].append({'handle': handle, 'uuid': f'{uuid:04x}'})
                if uuid == 0x2902:
                    if self.cccd is not None:
                        raise ValueError('Ambiguous notification CCCD')
                    self.cccd = handle
                    self.session['transportHandles']['cccd'] = handle
                previous = handle
            self.cursor = previous + 1
            self.state = 'subscribe' if self.cursor > self.descriptor_end else 'descriptors'
        elif op == 0x13 and self.state == 'wait_subscribe' and len(pdu) == 1:
            self.session['cccdWriteAcknowledged'] = True
            self.state = 'sar'
        elif op in (0x0A, 0x08, 0x10, 0x04, 0x12):
            # This central does not expose a server attribute database.
            handle = pdu[1:3] if len(pdu) >= 3 else b'\0\0'
            return [(4, bytes((1, op)) + handle + b'\x06')]
        else:
            raise ValueError('Unexpected ATT response')
        self.deadline = None
        return []

    def report(self):
        return {'state': self.state, 'sessions': self.sessions, 'errors': self.errors,
                'requests': self.requests, 'indicationsConfirmed': self.indications_confirmed,
                'smpRequestsRejected': self.smp_rejected, 'pairingVerified': False,
                'authKeyEstablished': False, 'physicalRadioExercised': False}
