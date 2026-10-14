"""ATT proxy for a CoreBluetooth peripheral. Authentication bytes stay end-to-end.

CoreBluetooth terminates the physical BLE link. The independent virtual hop
uses the existing cryptographic SMP central; phone application authentication
bytes still pass through unchanged. No physical-hop key is imported or invented.
"""
from collections import deque
import struct
import time
import uuid

HOST_SERVICES = {'1800', '1801', '00001800-0000-1000-8000-00805f9b34fb',
                 '00001801-0000-1000-8000-00805f9b34fb'}

def uuid_text(raw):
    text = raw[::-1].hex()
    return str(uuid.UUID(hex=text)) if len(raw) == 16 else text


class ExternalCentral:
    def __init__(self, controller, timeout=30, smp=None):
        self.controller = controller
        self.link = None
        self.bridge_id = str(uuid.uuid4())
        self.timeout = timeout
        self.state = 'connecting'
        self.error = None
        self.mtu = 247
        self.cursor = 1
        self.services = []
        self.characteristics = []
        self.descriptor_index = 0
        self.pending = None
        self.queue = deque()
        self.results = {}
        self.notifications = deque()
        self.started = time.monotonic()
        self.outgoing = []
        if smp is None:
            from .smp import LegacyCentral
            smp = LegacyCentral(controller)
        self.smp = smp
        self.smp.events = deque(maxlen=128)
        self.smp_failed = 0
        self.next_connect = 0
        self.resume_encryption = False
        self.subscriptions = set()
        self.restore_subscriptions = False
        self.next_discovery = 0

    def _fail(self, message):
        self.error = message
        self.state = 'failed'
        if self.pending and self.pending.get('id'):
            self.results[self.pending['id']] = {'done': True, 'error': message}
        self.pending = None
        while self.queue:
            item = self.queue.popleft()
            self.results[item['id']] = {'done': True, 'error': message}

    def _request(self, pdu, purpose, now, **extra):
        self.pending = dict(op=pdu[0], purpose=purpose, deadline=now + self.timeout, **extra)
        return self.link.from_peer(4, pdu)

    def reconnect(self, now):
        self._fail('Guest connection restarted')
        self.error = None
        # Match the existing working AuthenticationCentral lifecycle: the guest
        # finishes binding/disconnect work before accepting the next connection.
        self.state = 'connecting'; self.next_connect = now + 3.0; self.started = now
        self.services.clear(); self.characteristics.clear(); self.descriptor_index = 0; self.cursor = 1
        self.notifications.clear(); self.restore_subscriptions = True

    def tick(self, now):
        if self.state == 'failed': return []
        if self.state == 'connecting':
            if now < self.next_connect: return []
            if self.controller.local_name.rstrip(b'\0').startswith(b'Azki Virtual'):
                if now - self.started > self.timeout: self._fail('Guest Bluetooth did not become ready')
                return []
            self.state = 'mtu'
            self.smp.reset()
            self.resume_encryption = self.smp.bond is not None
            return self.controller.connect_local()
        if not self.controller.connected:
            self.reconnect(now)
            return []
        if self.resume_encryption:
            self.resume_encryption = False
            self.smp.resume_bond()
            return self.controller.drain_events()
        if self.pending:
            if now >= self.pending['deadline']: self._fail('Guest ATT request timed out')
            return []
        if self.state != 'ready' and now-self.started >= self.timeout:
            self._fail('Guest Bluetooth services did not become ready'); return []
        if now < self.next_discovery: return []
        if self.state == 'mtu': return self._request(b'\x02\xf7\0', 'mtu', now)
        if self.state == 'services': return self._request(b'\x10' + struct.pack('<HHH', self.cursor, 65535, 0x2800), 'services', now)
        if self.state == 'characteristics': return self._request(b'\x08' + struct.pack('<HHH', self.cursor, 65535, 0x2803), 'characteristics', now)
        if self.state == 'descriptors':
            while self.descriptor_index < len(self.characteristics):
                char = self.characteristics[self.descriptor_index]
                if char['properties'] & 0x30 and char['value'] < char['end']:
                    self.cursor = max(self.cursor, char['value'] + 1)
                    if self.cursor <= char['end']:
                        return self._request(b'\x04' + struct.pack('<HH', self.cursor, char['end']), 'descriptors', now)
                self.descriptor_index += 1
                self.cursor = 1
            self.state = 'ready'
            if self.restore_subscriptions:
                self.restore_subscriptions = False
                for key in self.subscriptions.copy():
                    self.submit('subscribe', key, enabled=True)
                    self.queue[-1]['internal'] = True
        if self.state == 'ready' and self.queue:
            item = self.queue.popleft()
            if item['pdu'][0] == 0x52:
                # Write Command has no ATT response. This is submission, never
                # an authentication or business-installation success claim.
                self.results[item['id']] = {'done': True, 'submitted': True}
                return self.link.from_peer(4, item['pdu'])
            return self._request(item['pdu'], 'operation', now, id=item['id'], key=item['key'], kind=item['kind'], enabled=item['enabled'], internal=item.get('internal',False))
        return []

    def _finish_characteristics(self):
        if not any(s['uuid'] not in HOST_SERVICES and
                   any(s['start'] <= c['decl'] <= s['end'] for c in self.characteristics)
                   for s in self.services):
            # The controller name becomes available before firmware registers
            # its GATT database. Keep discovery inside the original deadline.
            self.services.clear(); self.characteristics.clear(); self.descriptor_index = 0
            self.state, self.cursor = 'services', 1
            self.next_discovery = time.monotonic() + .25
            return
        for index, char in enumerate(self.characteristics):
            service = next(s for s in self.services if s['start'] <= char['decl'] <= s['end'])
            char['end'] = min([c['decl'] - 1 for c in self.characteristics[index + 1:] if c['decl'] <= service['end']] + [service['end']])
        self.state, self.cursor = 'descriptors', 1

    def consume(self, cid, pdu):
        if cid == 6:
            try: return self.smp.receive(pdu)
            except ValueError as error:
                self.smp_failed += 1
                self._fail(str(error))
                return [] if pdu[:1] == b'\x05' else [(6, b'\x05\x08')]
        if cid == 5:
            if len(pdu) >= 4 and pdu[0] == 0x12: return [(5, struct.pack('<BBHH', 0x13, pdu[1], 2, 0))]
            return []
        if not pdu: raise ValueError('Empty guest ATT response')
        op = pdu[0]
        if op in (0x1b, 0x1d):
            if len(pdu) < 3: raise ValueError('Short notification')
            if len(self.notifications) >= 256: self._fail('External notification queue full'); return []
            self.notifications.append({'key': 'h' + str(struct.unpack_from('<H', pdu, 1)[0]), 'value': pdu[3:].hex(), 'indication': op == 0x1d})
            # ATT bearer is terminated here; this confirms local receipt only.
            return [(4, b'\x1e')] if op == 0x1d else []
        if op == 2:
            if len(pdu) != 3 or not 23 <= int.from_bytes(pdu[1:], 'little') <= 517: raise ValueError('Invalid guest MTU')
            self.mtu = min(247, struct.unpack_from('<H', pdu, 1)[0])
            return [(4, b'\x03\xf7\0')]
        if op in (4, 6, 8, 10, 12, 16, 18):
            return [(4, bytes((1, op)) + pdu[1:3].ljust(2,b'\0') + b'\x06')]
        item = self.pending
        if item is None: return []
        purpose = item['purpose']
        if op == 1:
            if len(pdu) != 5 or pdu[1] != item['op']: raise ValueError('Unmatched ATT error')
            self.pending = None
            code = pdu[4]
            if purpose == 'operation':
                if item.get('internal'): self._fail('Guest subscription could not resume')
                else: self.results[item['id']] = {'done': True, 'attError': code}
                return []
            if code == 0x12 and purpose in ('services', 'characteristics', 'descriptors'):
                self.services.clear(); self.characteristics.clear(); self.descriptor_index = 0
                self.state, self.cursor = 'services', 1; return []
            if code != 0x0a: self._fail('Guest discovery ATT error 0x%02x' % code); return []
            if purpose == 'services': self.state, self.cursor = 'characteristics', 1
            elif purpose == 'characteristics': self._finish_characteristics()
            elif purpose == 'descriptors': self.descriptor_index += 1; self.cursor = 1
            else: self._fail('Unexpected guest ATT error')
            return []
        expected = {'mtu': 3, 'services': 0x11, 'characteristics': 9, 'descriptors': 5}.get(purpose, item['op'] + 1)
        if op != expected: raise ValueError('Unexpected guest ATT response')
        self.pending = None
        if purpose == 'operation':
            if item.get('kind') == 'subscribe':
                if item['enabled']: self.subscriptions.add(item['key'])
                else: self.subscriptions.discard(item['key'])
            if not item.get('internal'): self.results[item['id']] = {'done': True, 'value': pdu[1:].hex()}
            return []
        if purpose == 'mtu':
            if len(pdu) != 3 or not 23 <= int.from_bytes(pdu[1:], 'little') <= 517: raise ValueError('Invalid guest MTU')
            self.mtu = min(247, struct.unpack_from('<H', pdu, 1)[0]); self.state = 'services'; return []
        if purpose in ('services', 'characteristics'):
            if len(pdu) < 2: raise ValueError('Short discovery response')
            size = pdu[1]
            if size not in ((6, 20) if purpose == 'services' else (7, 21)) or len(pdu) <= 2 or (len(pdu) - 2) % size: raise ValueError('Malformed guest discovery')
            for offset in range(2, len(pdu), size):
                if purpose == 'services':
                    start, end = struct.unpack_from('<HH', pdu, offset)
                    self.services.append(dict(start=start, end=end, uuid=uuid_text(pdu[offset+4:offset+size])))
                    self.cursor = end + 1
                else:
                    decl, properties, value = struct.unpack_from('<HBH', pdu, offset)
                    self.characteristics.append(dict(decl=decl, properties=properties, value=value, uuid=uuid_text(pdu[offset+5:offset+size]), cccd=None))
                    self.cursor = decl + 1
            if self.cursor > 65535:
                if purpose == 'services': self.state, self.cursor = 'characteristics', 1
                else: self._finish_characteristics()
        elif purpose == 'descriptors':
            if len(pdu) < 2: raise ValueError('Short descriptor response')
            size = {1: 4, 2: 18}.get(pdu[1])
            if size is None or (len(pdu)-2) % size: raise ValueError('Malformed descriptors')
            for offset in range(2, len(pdu), size):
                handle = struct.unpack_from('<H', pdu, offset)[0]
                if uuid_text(pdu[offset+2:offset+size]) == '2902': self.characteristics[self.descriptor_index]['cccd'] = handle
                self.cursor = handle + 1
        return []

    def submit(self, operation, key, value='', offset=0, enabled=False):
        if self.state != 'ready': raise ValueError('Guest GATT is not ready')
        if len(self.queue) + len(self.results) >= 128: raise ValueError('External operation queue full')
        char = next((c for c in self.characteristics if key == 'h' + str(c['value'])), None)
        if char is None: raise ValueError('Unknown guest characteristic')
        handle = char['value']
        if operation == 'read': pdu = (b'\x0c' + struct.pack('<HH', handle, offset)) if offset else b'\x0a' + struct.pack('<H', handle)
        elif operation == 'write':
            data = bytes.fromhex(value)
            if len(data) > self.mtu - 3: raise ValueError('Write exceeds guest MTU')
            pdu = bytes((0x12 if char['properties'] & 8 else 0x52,)) + struct.pack('<H', handle) + data
        elif operation == 'subscribe':
            if char['cccd'] is None: raise ValueError('Guest CCCD missing')
            pdu = b'\x12' + struct.pack('<HH', char['cccd'], (1 if char['properties'] & 0x10 else 2) if enabled else 0)
        else: raise ValueError('Unknown ATT operation')
        identity = str(uuid.uuid4())
        self.queue.append(dict(id=identity, pdu=pdu, key=key, kind=operation, enabled=enabled))
        return {'operationID': identity}

    def status(self):
        services = []
        for service in self.services:
            # GAP and GATT belong to the physical CoreBluetooth server itself.
            if service['uuid'] in HOST_SERVICES: continue
            chars = [dict(key='h'+str(c['value']), uuid=c['uuid'], readable=bool(c['properties'] & 2), writable=bool(c['properties'] & 0x0c), writeWithoutResponse=bool(c['properties'] & 4), writeWithResponse=bool(c['properties'] & 8), notifies=bool(c['properties'] & 0x10), notifiable=bool(c['properties'] & 0x30), indicates=bool(c['properties'] & 0x20)) for c in self.characteristics if service['start'] <= c['decl'] <= service['end']]
            if chars: services.append(dict(key='s'+str(service['start']), uuid=service['uuid'], characteristics=chars))
        notifications = list(self.notifications); self.notifications.clear()
        return dict(bridgeID=self.bridge_id, state=self.state, error=self.error, mtu=self.mtu, services=services, notifications=notifications, guestSMPFailures=self.smp_failed, guestLinkEncrypted=getattr(self.controller,'air',None) is not None, guestBondEstablished=self.smp.bond is not None, authentication='forwarded-to-guest; not inspected', physicalLinkSecurity='managed-by-macOS')
