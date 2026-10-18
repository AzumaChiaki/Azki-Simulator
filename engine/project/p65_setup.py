"""Prepare a P65 AP boot copy with local OOBE-completion properties.

The AP image is deliberately not pinned to one firmware SHA256. P65 builds
move the embedded ROMFS and the build.prop inode, so setup first validates a
ROMFS superblock and locates /build.prop structurally.
"""
import struct

AP_SHA256 = None
ROMFS_MAGIC = b'-rom1fs-'
ROMFS_MIN_SIZE = 0x200
ROMFS_MAX_SIZE = 0x10000000
LOCAL_PROPERTIES = {
    'persist.setup_wizard': '1',
    'persist.system_novice_guidance': 'false',
    'persist.settings_novice': 'true',
    'persist.locale': 'zh_CN',
    'persist.screen.auto_screen_timeout': '0',
    'persist.screen.dimmer_timeout': '2147483647',
}

def checksum(data):
    padded = data + bytes((-len(data)) % 4)
    return (-sum(struct.unpack('>' + 'I' * (len(padded) // 4), padded))) & 0xffffffff

def _align16(value):
    return (value + 15) & ~15

def _entry(image, base, size, offset):
    if offset <= 0 or offset + 32 > size:
        raise ValueError('ROMFS inode offset is outside the image')
    absolute = base + offset
    nxt, spec, length, check = struct.unpack_from('>4I', image, absolute)
    name_end = image.find(b'\0', absolute + 16, absolute + 16 + 4096)
    if name_end < 0 or name_end + 1 > base + size:
        raise ValueError('ROMFS inode name is unterminated')
    entry_size = _align16(name_end + 1 - absolute)
    if entry_size < 32 or offset + entry_size > size:
        raise ValueError('ROMFS inode size is invalid')
    raw = image[absolute:absolute + entry_size]
    if checksum(raw) != 0:
        raise ValueError('ROMFS inode checksum is invalid')
    name = image[absolute + 16:name_end].decode('utf-8')
    data = _align16(name_end + 1 - base)
    if data + length > size:
        raise ValueError('ROMFS file exceeds the superblock size')
    return dict(offset=offset, next=nxt, spec=spec, length=length,
                checksum=check, name=name, entry_size=entry_size, data=data,
                kind=nxt & 7)

def _romfs_tree(image, base, size):
    found = []
    visited = set()
    def walk(offset, path):
        while offset:
            if offset in visited:
                raise ValueError('ROMFS directory contains a cycle')
            visited.add(offset)
            inode = _entry(image, base, size, offset)
            name = inode['name']
            target = path + '/' + name if path else '/' + name
            if inode['kind'] == 1 and name not in ('.', '..'):
                walk(inode['spec'], target)
            elif name == 'build.prop' and target == '/build.prop':
                content = inode if inode['kind'] == 2 else _entry(image, base, size, inode['spec'])
                if content['kind'] != 2:
                    raise ValueError('/build.prop is not a regular file')
                found.append((inode, content))
            offset = inode['next'] & ~15
    root = _entry(image, base, size, 0x20)
    if root['kind'] != 1 or root['name'] != '.':
        raise ValueError('ROMFS root directory is invalid')
    walk(0x20, '')
    if len(found) != 1:
        raise ValueError('ROMFS does not contain exactly one /build.prop')
    return found[0]

def _locate_romfs(image):
    candidates = []
    cursor = 0
    while True:
        base = image.find(ROMFS_MAGIC, cursor)
        if base < 0:
            break
        cursor = base + 1
        if base + 16 > len(image):
            continue
        size = struct.unpack_from('>I', image, base + 8)[0]
        if not ROMFS_MIN_SIZE <= size <= ROMFS_MAX_SIZE or base + size > len(image):
            continue
        if base + 512 > len(image) or checksum(image[base:base + 512]) != 0:
            continue
        try:
            build_inode, content_inode = _romfs_tree(image, base, size)
        except (UnicodeDecodeError, ValueError):
            continue
        candidates.append((base, size, build_inode, content_inode))
    if len(candidates) != 1:
        raise ValueError('could not identify a unique valid AP ROMFS with /build.prop')
    return candidates[0]

def local_boot_image(original):
    if not isinstance(original, (bytes, bytearray)) or len(original) < ROMFS_MIN_SIZE:
        raise ValueError('AP image is empty or truncated')
    image = bytearray(original)
    base, romfs_size, build_inode, content_inode = _locate_romfs(image)
    properties = bytes(image[base + content_inode['data']:base + content_inode['data'] + content_inode['length']])
    lines = set(properties.splitlines())
    if all(f'{key}={value}'.encode() in lines for key, value in LOCAL_PROPERTIES.items()):
        raise ValueError('AP image already contains the local OOBE properties')
    if b'ro.product.device.devicetype=watch' not in lines:
        raise ValueError('AP ROMFS build.prop is not a watch image')
    appended = b'\n' + ''.join(f'{key}={value}\n' for key, value in LOCAL_PROPERTIES.items()).encode()
    new_properties = properties + appended
    new_size = _align16(romfs_size + 32 + len(new_properties))
    if base + new_size > len(image) or any(image[base + romfs_size:base + new_size]):
        raise ValueError('AP ROMFS has insufficient zero-filled padding for local setup properties')
    new_entry = bytearray(struct.pack('>4I', 2, 0, len(new_properties), 0) + b'build.prop\0'.ljust(16, b'\0'))
    struct.pack_into('>I', new_entry, 12, checksum(bytes(new_entry)))
    image[base + romfs_size:base + romfs_size + 32] = new_entry
    image[base + romfs_size + 32:base + romfs_size + 32 + len(new_properties)] = new_properties
    next_entry = build_inode['next'] & ~15
    struct.pack_into('>4I', image, base + build_inode['offset'], next_entry, romfs_size, 0, 0)
    old_raw = bytes(image[base + build_inode['offset']:base + build_inode['offset'] + build_inode['entry_size']])
    struct.pack_into('>I', image, base + build_inode['offset'] + 12, checksum(old_raw))
    struct.pack_into('>II', image, base + 8, new_size, 0)
    struct.pack_into('>I', image, base + 12, checksum(bytes(image[base:base + 512])))
    return bytes(image)
