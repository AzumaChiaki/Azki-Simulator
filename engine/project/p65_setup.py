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

def _find_inode(image, base, size, wanted):
    found = []

    def walk(offset, path):
        while offset:
            inode = _entry(image, base, size, offset)
            name = inode['name']
            target = path + '/' + name if path else '/' + name
            if inode['kind'] == 1 and name not in ('.', '..'):
                walk(inode['spec'], target)
            elif target == wanted and inode['kind'] == 2:
                found.append(inode)
            offset = inode['next'] & ~15

    walk(0x20, '')
    if len(found) != 1:
        raise ValueError('ROMFS does not contain exactly one ' + wanted)
    return found[0]

def _zero_space(image, base, start, need):
    """First run of zero bytes at a ROMFS-aligned offset at or after start.

    ROMFS pads entry names relative to the inode start, so a placement is only
    usable when its filesystem offset, not its absolute image offset, is
    sixteen-byte aligned.
    """
    zeros = bytes(need)
    offset = start
    while True:
        offset = image.find(zeros, offset)
        if offset < 0:
            raise ValueError('AP image has no zero-filled space for local setup files')
        aligned = base + _align16(max(0, offset - base))
        if aligned + need <= len(image) and not any(image[aligned:aligned + need]):
            return aligned
        offset = aligned + 16

def _append_inode(image, base, size, name, content):
    """Append one regular inode; fall back to the next zero-filled region.

    ROMFS only requires inodes to live inside the superblock size, so an
    out-of-line copy in firmware padding is valid once the size grows to it.
    """
    header = struct.pack('>4I', 2, 0, len(content), 0) + name.encode() + b'\0'
    header = header.ljust(_align16(len(header)), b'\0')
    body = header + content
    total = _align16(len(body))
    offset = size
    if base + offset + total > len(image) or any(image[base + offset:base + offset + total]):
        offset = _zero_space(image, base, base + size, total) - base
    if base + offset + total > len(image) or any(image[base + offset:base + offset + total]):
        raise ValueError('AP ROMFS has insufficient zero-filled padding for local setup properties')
    image[base + offset:base + offset + len(header)] = header
    image[base + offset + len(header):base + offset + len(body)] = content
    struct.pack_into('>I', image, base + offset + 12,
                     checksum(bytes(image[base + offset:base + offset + len(header)])))
    return offset, max(size, offset + total)

def _relink_inode(image, base, inode, target):
    """Turn the original inode into a ROMFS hardlink to the appended copy."""
    struct.pack_into('>4I', image, base + inode['offset'], inode['next'] & ~15, target, 0, 0)
    raw = bytes(image[base + inode['offset']:base + inode['offset'] + inode['entry_size']])
    struct.pack_into('>I', image, base + inode['offset'] + 12, checksum(raw))

def _property_script(script, properties):
    """Insert property writes after kvdb_init and before kvdbd starts.

    Rejected for now: extending /init.d/rc.sysinit in the boot copy makes the
    guest take a BusFault during early PMU init (no display, no watch face),
    even though the rewritten ROMFS still parses and every inode checksum
    verifies. Kept so the experiment is not repeated blindly.
    """
    lines = script.split(b'\n')
    injected, done = [], False
    for line in lines:
        injected.append(line)
        if not done and line.strip() == b'kvdb_init':
            for index, (key, value) in enumerate(properties.items()):
                redirect = b' > ' if index == 0 else b' >> '
                injected.append(b'echo ' + key.encode() + b'=' + value.encode()
                                + redirect + b'/tmp/factory.prop')
            done = True
    if not done:
        raise ValueError('AP rc.sysinit has no kvdb_init stage to extend')
    return b'\n'.join(injected)

def local_boot_image(original, extra_properties=None):
    if not isinstance(original, (bytes, bytearray)) or len(original) < ROMFS_MIN_SIZE:
        raise ValueError('AP image is empty or truncated')
    overrides = {}
    if extra_properties is not None:
        if type(extra_properties) is not dict or not extra_properties:
            raise ValueError('AP setup property override is not a non-empty mapping')
        for key, value in extra_properties.items():
            if (type(key) is not str or type(value) is not str
                    or not 1 <= len(key) <= 64 or not 1 <= len(value) <= 96
                    or '=' in key or any(not 33 <= ord(char) <= 126 for char in key)
                    or any(not 32 <= ord(char) <= 126 for char in value)
                    or key in LOCAL_PROPERTIES):
                raise ValueError('AP setup property override is invalid')
        overrides = dict(extra_properties)
    properties_map = dict(LOCAL_PROPERTIES)
    properties_map.update(overrides)
    image = bytearray(original)
    base, romfs_size, build_inode, content_inode = _locate_romfs(image)
    properties = bytes(image[base + content_inode['data']:base + content_inode['data'] + content_inode['length']])
    lines = set(properties.splitlines())
    if all(f'{key}={value}'.encode() in lines for key, value in properties_map.items()):
        raise ValueError('AP image already contains the local OOBE properties')
    if b'ro.product.device.devicetype=watch' not in lines:
        raise ValueError('AP ROMFS build.prop is not a watch image')
    appended = b'\n' + ''.join(f'{key}={value}\n' for key, value in properties_map.items()).encode()
    new_properties = properties + appended
    size = romfs_size
    offset, size = _append_inode(image, base, size, 'build.prop', new_properties)
    _relink_inode(image, base, build_inode, offset)
    struct.pack_into('>II', image, base + 8, size, 0)
    struct.pack_into('>I', image, base + 12, checksum(bytes(image[base:base + 512])))
    return bytes(image)
