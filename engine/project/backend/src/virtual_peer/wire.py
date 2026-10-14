"""Bounded MiWear install messages and streaming MASS segments.

Field facts are read from the existing Studio Messages.swift/Mass.swift; no
Bluetooth, account, credential, firmware mutation or installation occurs here.
"""
import struct
import zlib

MAX_MESSAGE = 65535
MAX_ITEMS = 256


def varint(value):
    if type(value) is not int or not 0 <= value < 2**64:
        raise ValueError('invalid unsigned integer')
    result = bytearray()
    while value > 127:
        result.append((value & 127) | 128)
        value >>= 7
    return bytes(result + bytes((value,)))


def integer(number, value):
    return varint(number << 3) + varint(value)


def blob(number, value):
    if isinstance(value, str):
        value = value.encode('utf-8')
    if not isinstance(value, bytes) or len(value) > MAX_MESSAGE:
        raise ValueError('invalid bytes field')
    return varint(number << 3 | 2) + varint(len(value)) + value


class Fields:
    def __init__(self, data):
        if not isinstance(data, bytes) or len(data) > MAX_MESSAGE:
            raise ValueError('protobuf size bound')
        self.values = {}
        offset = 0

        def read_varint():
            nonlocal offset
            value = 0
            for index in range(10):
                if offset >= len(data):
                    raise ValueError('truncated protobuf varint')
                byte = data[offset]
                offset += 1
                if index == 9 and byte > 1:
                    raise ValueError('protobuf integer overflow')
                value |= (byte & 127) << (index * 7)
                if not byte & 128:
                    return value
            raise ValueError('protobuf varint bound')

        count = 0
        while offset < len(data):
            tag = read_varint()
            number, kind = tag >> 3, tag & 7
            count += 1
            if not 1 <= number <= 0x1fffffff or count > 2048:
                raise ValueError('protobuf field bound')
            if kind == 0:
                value = read_varint()
            elif kind in (1, 2, 5):
                length = read_varint() if kind == 2 else (8 if kind == 1 else 4)
                if offset + length > len(data):
                    raise ValueError('truncated protobuf data')
                value = data[offset:offset + length]
                offset += length
            else:
                raise ValueError('unsupported protobuf wire type')
            self.values.setdefault(number, []).append((kind, value))

    def one(self, number, kind, default=None):
        values = self.values.get(number, [])
        if not values:
            return default
        if len(values) != 1 or values[0][0] != kind:
            raise ValueError('duplicate scalar or wrong protobuf field type')
        return values[0][1]

    def number(self, field, default=0):
        return self.one(field, 0, default)

    def data(self, field, default=None):
        return self.one(field, 2, default)

    def text(self, field, default=''):
        data = self.data(field)
        if data is None:
            return default
        if len(data) > 512:
            raise ValueError('text field bound')
        value = data.decode('utf-8')
        if any(ord(c) < 32 for c in value):
            raise ValueError('control character in text')
        return value

    def message(self, field):
        value = self.data(field)
        if value is None:
            raise ValueError('required protobuf message missing')
        return Fields(value)

    def messages(self, field):
        values = self.values.get(field, [])
        if len(values) > MAX_ITEMS or any(kind != 2 for kind, _ in values):
            raise ValueError('inventory item bound/type')
        return [Fields(value) for _, value in values]


def envelope(kind, subtype, field=None, body=None):
    return integer(1, kind) + integer(2, subtype) + (blob(field, body) if field else b'')


def route(payload):
    fields = Fields(payload)
    return (fields.number(1), fields.number(2)), fields


def body(payload, kind, subtype, field):
    identity, fields = route(payload)
    if identity != (kind, subtype):
        raise ValueError('unexpected business response')
    if fields.number(100) != 0:
        raise ValueError('guest business error code %d' % fields.number(100))
    return fields.message(field)


def inventory_request(kind, cursor=0):
    if kind == 'quickapp':
        return envelope(20, 0)
    return envelope(4, 0, 6, blob(16, integer(1, cursor)))


def inventory_response(kind, payload):
    items = []
    contents = body(payload, 20 if kind == 'quickapp' else 4, 0, 22 if kind == 'quickapp' else 6).message(1)
    for entry in contents.messages(1):
        item = {'id': entry.text(1), 'version': entry.number(3 if kind == 'quickapp' else 5)}
        if not item['id'] or len(item['id'].encode()) > 128:
            raise ValueError('invalid inventory identity')
        if kind == 'quickapp':
            fingerprint = entry.data(2, b'')
            if len(fingerprint) > 64:
                raise ValueError('fingerprint bound')
            item.update(name=entry.text(5), fingerprint=fingerprint.hex())
        else:
            item.update(name=entry.text(2), current=entry.number(3) != 0)
        items.append(item)
    if len({item['id'] for item in items}) != len(items):
        raise ValueError('duplicate inventory identity')
    return items, (0 if kind == 'quickapp' else contents.number(2))


def prepare_request(package):
    if package.kind == 'quickapp':
        request = blob(1, package.identifier) + integer(2, package.version) + integer(3, len(package.payload))
        return envelope(20, 1, 22, blob(2, request))
    entitlement = blob(1, '') + blob(2, '')
    request = blob(1, package.identifier) + integer(2, len(package.payload))
    if package.version:
        request += integer(3, package.version)
    return envelope(4, 4, 6, blob(6, request + blob(5, entitlement)))


def prepare_response(kind, payload):
    if kind == 'quickapp':
        result = body(payload, 20, 1, 22).message(3)
        return {'code': result.number(1), 'slice': result.number(2), 'id': '', 'requiresSpace': False}
    value = body(payload, 4, 4, 6)
    if value.data(9) is not None:
        result = value.message(9)
        return {'code': result.number(2), 'slice': result.number(4), 'id': result.text(1), 'requiresSpace': result.number(5) != 0}
    return {'code': value.number(5, 255), 'slice': 0, 'id': '', 'requiresSpace': False}


def mass_prepare_request(package):
    value = integer(1, package.data_type) + blob(2, package.md5) + integer(3, len(package.payload))
    return envelope(22, 0, 24, blob(1, value))


def mass_prepare_response(payload):
    value = body(payload, 22, 0, 24).message(2)
    return {'id': value.data(1, b''), 'code': value.number(2), 'offset': value.number(4), 'slice': value.number(5)}


def terminal_response(kind, payload):
    if kind == 'quickapp':
        result = body(payload, 20, 2, 22).message(4)
        identifier = result.text(2)
        if result.data(3) is not None:
            nested = result.message(3).text(1)
            if identifier and nested and identifier != nested:
                raise ValueError('conflicting terminal identities')
            identifier = nested or identifier
        code = result.number(1, 255)
        return {'id': identifier, 'code': code, 'accepted': code == 0}
    result = body(payload, 4, 5, 6).message(7)
    code = result.number(2, 255)
    return {'id': result.text(1), 'code': code, 'accepted': code in (2, 3)}


def activate_watchface_request(identifier):
    """Original guest selector, only to be sent after installation evidence."""
    return envelope(4, 1, 6, blob(2, identifier))


def activate_watchface_response(payload):
    return body(payload, 4, 1, 6).number(4) != 0


def mass_segments(package, sent_offset, slice_length, remote_mps=4096):
    """Yield one segment at a time; never construct another whole-file wire copy."""
    if type(sent_offset) is not int or not 0 <= sent_offset <= len(package.payload):
        raise ValueError('resume offset outside package')
    if type(remote_mps) is not int or not 64 <= remote_mps <= MAX_MESSAGE:
        raise ValueError('invalid negotiated SAR MPS')
    if type(slice_length) is not int or slice_length < 0 or slice_length > MAX_MESSAGE:
        raise ValueError('invalid MASS slice hint')
    # Conservative stop-and-wait slices, bounded by the negotiated SAR L2 size.
    effective = min(slice_length or 4096, remote_mps - 2)
    capacity = effective - 4
    if capacity <= 0:
        raise ValueError('MASS slice too short')
    remaining = len(package.payload) - sent_offset
    total = (22 + remaining + 4 + capacity - 1) // capacity
    if total > 65535:
        raise ValueError('MASS segment count bound')
    pending = bytearray(b'\0' + bytes((package.data_type,)) + package.md5 + struct.pack('<I', remaining))
    crc = zlib.crc32(pending)
    index = 1
    for offset in range(sent_offset, len(package.payload), capacity):
        chunk = package.payload[offset:offset + capacity]
        crc = zlib.crc32(chunk, crc)
        pending.extend(chunk)
        while len(pending) >= capacity:
            yield struct.pack('<HH', total, index) + bytes(pending[:capacity])
            del pending[:capacity]
            index += 1
    pending.extend(struct.pack('<I', crc & 0xffffffff))
    while pending:
        yield struct.pack('<HH', total, index) + bytes(pending[:capacity])
        del pending[:capacity]
        index += 1
    if index != total + 1:
        raise ValueError('MASS length accounting failure')
