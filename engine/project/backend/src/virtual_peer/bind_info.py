"""Initial BindPsk information request for a fresh synthetic virtual identity.

This exchanges fresh non-secret nonces, not credentials. It does not complete
binding or provision an authKey. Parsed bytes stay in memory; reports compare
synthetic identifiers without logging raw protobuf, keys or device nonces.
"""
import secrets
import struct
from .central import crc16


SYNTHETIC_USER_ID = b'azki-synthetic-user'


def request(random_bytes=secrets.token_bytes, dynamic_code=0, user_id=None):
    if type(dynamic_code) is not int or dynamic_code not in (0, 1):
        raise ValueError('Dynamic-code check flag must be 0 or 1')
    # P67 compares this optional field with MD5(miwear_user_id). It is
    # an account selector, not a fresh challenge nonce.
    if user_id is not None:
        import hashlib
        nonce = hashlib.md5(user_id).digest()
    else:
        nonce = random_bytes(16)
    if not isinstance(nonce, bytes) or len(nonce) != 16:
        raise ValueError('Fresh synthetic nonce generation failed')
    payload = bytes.fromhex('0101080110111a165a1408') + bytes((dynamic_code,)) + b'\x12\x10' + nonce
    return encode(3, 0, payload)


def encode(kind, sequence, payload):
    if kind not in range(4) or not 0 <= sequence <= 255 or len(payload) > 65535:
        raise ValueError('SAR frame bounds')
    return b'\xa5\xa5' + struct.pack('<BBHH', kind, sequence, len(payload), crc16(payload)) + payload


class Frames:
    def __init__(self, maximum=4096):
        self.pending = bytearray()
        if type(maximum) is not int or not 1 <= maximum <= 65535:raise ValueError('SAR frame bound')
        self.maximum = maximum

    def feed(self, chunk):
        if not isinstance(chunk, bytes) or len(chunk) > 244 or len(self.pending) + len(chunk) > self.maximum + 8:
            raise ValueError('SAR receive buffer bound')
        self.pending.extend(chunk)
        frames = []
        while len(self.pending) >= 8:
            if self.pending[:2] != b'\xa5\xa5' or self.pending[2] & 0xEF not in range(4):
                raise ValueError('Invalid SAR frame header')
            length, crc = struct.unpack_from('<HH', self.pending, 4)
            if length > self.maximum:
                raise ValueError('SAR frame length bound')
            if len(self.pending) < length + 8:
                break
            payload = bytes(self.pending[8:length + 8])
            if crc16(payload) != crc:
                raise ValueError('SAR frame CRC mismatch')
            frames.append((self.pending[2] & 0x0F, self.pending[3], payload))
            del self.pending[:length + 8]
        return frames


def fields(data):
    """Small strict protobuf reader for observed envelope wire types 0 and 2."""
    if not isinstance(data, bytes) or len(data) > 4096:
        raise ValueError('Protobuf size bound')
    result, offset = {}, 0

    def varint():
        nonlocal offset
        value = 0
        for index in range(10):
            if offset >= len(data):
                raise ValueError('Truncated protobuf varint')
            byte = data[offset]
            offset += 1
            if index == 9 and byte > 1:
                raise ValueError('Protobuf integer overflow')
            value |= (byte & 127) << (index * 7)
            if not byte & 128:
                return value
        raise ValueError('Protobuf varint bound')

    while offset < len(data):
        tag = varint()
        number, kind = tag >> 3, tag & 7
        if not number or number > 0x1FFFFFFF or number in result or len(result) >= 64:
            raise ValueError('Invalid or repeated protobuf field')
        if kind == 0:
            value = varint()
        elif kind == 2:
            length = varint()
            if offset + length > len(data):
                raise ValueError('Truncated protobuf field')
            value = data[offset:offset + length]
            offset += length
        else:
            raise ValueError('Unsupported protobuf wire type')
        result[number] = value
    return result


def inspect(payload, serial, address, did=None, *, model='miwear.watch.q66nfc', model_aliases=()):
    if type(model) is not str or not 1 <= len(model.encode('utf-8')) < 32:
        raise ValueError('Invalid expected synthetic model')
    import re
    models=(model,)+tuple(model_aliases)
    if len(models)>15 or any(type(item) is not str or not re.fullmatch(r'miwear\.watch\.[a-z0-9]{1,12}',item) for item in models):
        raise ValueError('Invalid firmware model alias')
    if payload[:2] != b'\x01\x01':
        raise ValueError('BindInfo requires unencrypted protobuf channel response')
    outer = fields(payload[2:])
    if outer.get(1) != 1 or outer.get(2) != 17:
        raise ValueError('Unexpected BindInfo command response')
    body = fields(outer.get(3, b''))
    failure = body.get(3, outer.get(100))
    if failure is not None:
        if type(failure) is not int:
            raise ValueError('Invalid BindInfo error')
        return {'received': True, 'accepted': False, 'guestError': failure}
    info = fields(body.get(12, b''))
    expected_types = {1: int, 2: bytes, 3: bytes, 4: bytes, 5: int, 6: bytes, 7: bytes}
    for key, value in info.items():
        if key in expected_types and type(value) is not expected_types[key]:
            raise ValueError('Invalid BindInfo field type')
    if not all(key in info for key in (2, 3, 4, 6)) or len(info[6]) != 16:
        raise ValueError('Incomplete guest BindInfo')
    return {'received': True, 'accepted': True, 'serialMatchesSyntheticSeed': info[2] == serial.encode(),
            'addressMatchesSyntheticSeed': info[3].lower() == address.lower().encode(),
            'modelMatchesSyntheticSeed': info[4] == model.encode(),
            'modelMatchesFirmwareProfile': info[4] in {item.encode() for item in models},
            'reportedModel': info[4].decode('ascii') if re.fullmatch(rb'miwear\.watch\.[a-z0-9]{1,12}',info[4]) else None,
            'expectedModel': model, 'deviceNonceBytes': len(info[6]),
            'didPresent': bool(info.get(7)), 'verifyMode': info.get(1), 'oobMode': info.get(5),
            'didMatchesSyntheticSeed': did is not None and info.get(7) == did.encode(),
            'freshBindingVerified': False, 'authKeyEstablished': False}
