"""Strict readers for product resource paths inside locally held full OTAs."""
from __future__ import annotations

import hashlib
import io
from pathlib import PurePosixPath
import struct
import zipfile
import zlib


def checked_member(data: bytes, record: dict, *, verify_sha=True) -> bytes:
    """Read a local ZIP member at its recorded offset, preserving duplicates."""
    offset = record['header_offset']
    if offset < 0 or offset + 30 > len(data):
        raise ValueError('ZIP local header outside OTA')
    values = struct.unpack_from('<4s5H3I2H', data, offset)
    signature, _, flags, method, _, _, crc, packed, size, names, extra = values
    begin = offset + 30 + names + extra
    end = begin + packed
    if signature != b'PK\x03\x04' or flags & 9 or end > len(data):
        raise ValueError('Unsupported or truncated ZIP local member')
    name = data[offset + 30:offset + 30 + names].decode('utf-8')
    expected = (record['name'], record['data_offset'], record['compressed_size'],
                record['size'], record['compression_method'], record['crc32'])
    if (name, begin, packed, size, method, f'{crc:08x}') != expected:
        raise ValueError('ZIP member differs from selected OTA inventory')
    compressed = data[begin:end]
    if method == 0:
        output = compressed
    elif method == 8:
        decoder = zlib.decompressobj(-15)
        output = decoder.decompress(compressed, size + 1)
        if not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
            raise ValueError('Invalid DEFLATE member boundary')
    else:
        raise ValueError('Unsupported resource ZIP method')
    if len(output) != size or zlib.crc32(output) != crc:
        raise ValueError('Resource ZIP size/CRC mismatch')
    if verify_sha and hashlib.sha256(output).hexdigest() != record['sha256']:
        raise ValueError('Resource member SHA256 mismatch')
    return output


def n67_system_members(data: bytes, records: list[dict]) -> list[dict]:
    """Decode the full-path table rather than guessing duplicate ZIP names.

    Observed in every locally held N67 full OTA (3.1.171, .175, .187):
    outer +0x54 points to 0x7e5a5a5a resource container; inner +0x46 is
    uint16 count. 0x80-byte records begin at +0x50. Each stores flags,
    relative ZIP offset, ZIP length, and a zero-padded 116-byte path.
    The table has a CRC32; every ZIP is followed by its payload CRC32.
    """
    if len(data) < 0x58 or data[:4] != b'\x60ZZ~':
        raise ValueError('Not an N67 full OTA envelope')
    base = struct.unpack_from('<I', data, 0x54)[0]
    if base + 80 > len(data) or data[base:base + 4] != b'ZZZ~':
        raise ValueError('Missing N67 resource header')
    count = struct.unpack_from('<H', data, base + 70)[0]
    end = base + 80 + count * 128
    if not count or count != len(records) or end + 4 > len(data):
        raise ValueError('Resource table count/range mismatch')
    if zlib.crc32(data[base:end]) != struct.unpack_from('<I', data, end)[0]:
        raise ValueError('Resource path-table CRC mismatch')
    result = []
    seen_paths = set()
    previous_end = end + 4
    for index, record in enumerate(records):
        at = base + 80 + index * 128
        flags, relative, length = struct.unpack_from('<III', data, at)
        raw_path = data[at + 12:at + 128]
        path_bytes, separator, padding = raw_path.partition(b'\0')
        if not separator or any(padding):
            raise ValueError('Resource path is not zero-padded')
        path = path_bytes.decode('utf-8')
        parts = path.split('/')
        if not path.startswith('/') or any(x in ('', '.', '..') for x in parts[1:]):
            raise ValueError('Unsafe resource path')
        if path in seen_paths:
            raise ValueError('Duplicate full resource path')
        seen_paths.add(path)
        offset = base + relative
        zip_end = offset + length
        if offset != previous_end or offset != record['header_offset'] or zip_end + 4 > len(data):
            raise ValueError('Resource path-table ZIP offset/length mismatch')
        # ZipFile validates the central-directory boundary and member identity.
        with zipfile.ZipFile(io.BytesIO(data[offset:zip_end])) as archive:
            infos = archive.infolist()
            if len(infos) != 1 or infos[0].filename != record['name'] or infos[0].header_offset != 0:
                raise ValueError('Expected one original ZIP member per resource path')
            member = infos[0]
            if (member.file_size, member.CRC) != (record['size'], int(record['crc32'], 16)):
                raise ValueError('Resource central-directory integrity mismatch')
        if struct.unpack_from('<I', data, zip_end)[0] != int(record['crc32'], 16):
            raise ValueError('Resource trailer CRC mismatch')
        previous_end = zip_end + 4
        if PurePosixPath(path).name != record['name']:
            raise ValueError('Path basename differs from ZIP member')
        if path.startswith('/system/'):
            if flags != 0x340000:
                raise ValueError('Unknown N67 system resource flags')
            result.append(dict(record, target_path=path.removeprefix('/system/')))
        elif not path.startswith('/data/ota/') or flags not in {0x740001, 0x740002, 0x740005, 0x740006, 0x740007}:
            raise ValueError('Unknown N67 non-system resource destination')
    if not result:
        raise ValueError('Empty N67 system resource table')
    return result
