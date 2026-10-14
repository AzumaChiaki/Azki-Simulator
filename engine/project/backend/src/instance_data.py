"""Validate private working paths and exact NAND/NOR geometry at launch.

Snapshot contents are checked by the host during preparation and commit.
"""
import hashlib
import os
from pathlib import Path
import re
import stat


def _private_image(path, expected_size, expected_sha):
    path = Path(path)
    if not path.is_absolute() or path.resolve() != path:
        raise ValueError('Instance image must be an absolute non-symlink path')
    if not isinstance(expected_sha, str) or not re.fullmatch('[0-9a-f]{64}', expected_sha):
        raise ValueError('Instance image requires an initial SHA-256')
    parent = path.parent.stat()
    if parent.st_uid != os.geteuid() or parent.st_mode & 0o077:
        raise ValueError('Instance image parent must be private to the current user')
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode) or before.st_uid != os.geteuid() or \
                before.st_nlink != 1 or before.st_mode & 0o077 or before.st_size != expected_size:
            raise ValueError('Instance image must be a private regular file of exact partition size')
        after = os.fstat(fd)
        identity = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns)
        if identity(before) != identity(after) or identity(after) != identity(path.lstat()):
            raise ValueError('Instance image changed during validation')
    finally:
        os.close(fd)
    return str(path)


def validate_instance_images(data, spare, data_sha, spare_sha, geometry):
    if not all((data, spare, data_sha, spare_sha)):
        raise ValueError('Instance storage requires both image paths and both initial SHA-256 values')
    offset, size = geometry.get('nand-data-offset'), geometry.get('nand-data-bytes')
    if type(offset) is not int or type(size) is not int or offset < 0 or size <= 0 or \
            (offset | size) & 131071 or offset + size > 512 * 1024 * 1024:
        raise ValueError('Instance NAND geometry is unsupported')
    data_path = _private_image(data, size, data_sha)
    spare_path = _private_image(spare, size // 2048 * 64, spare_sha)
    if data_path == spare_path:
        raise ValueError('Instance data and spare images must be different files')
    return dict(schema=1, dataImage=data_path, spareImage=spare_path,
                dataSHA256=data_sha, spareSHA256=spare_sha, dataOffset=offset, dataBytes=size)


def instance_machine_options(storage):
    """No caller-controlled option names; paths use QEMU comma escaping."""
    return (',data-image=' + storage['dataImage'].replace(',', ',,') +
            ',data-spare-image=' + storage['spareImage'].replace(',', ',,') +
            ',data-writeback=on')


def initialize_instance_data(storage, seed_path, *, initialize):
    """Apply a legacy UI seed only to a newly created, entirely erased disk.

    YAFFS uses in-band tags here, so the separate spare image stays erased.
    A restored or already initialized disk is never replaced with the seed.
    """
    import fcntl
    if initialize is not True:
        raise ValueError('Data seed is limited to a first-run transaction')
    seed_path = Path(seed_path)
    if not seed_path.is_absolute() or seed_path.resolve() != seed_path:
        raise ValueError('Data seed must be an absolute non-symlink file')
    size = storage['dataBytes']
    descriptors = []
    try:
        for key, expected_size in (('dataImage', size), ('spareImage', size // 2048 * 64)):
            fd = os.open(storage[key], os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK)
            descriptors.append(fd)
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            st = os.fstat(fd)
            if (not stat.S_ISREG(st.st_mode) or st.st_uid != os.geteuid() or st.st_nlink != 1
                    or st.st_mode & 0o077 or st.st_size != expected_size):
                raise ValueError('Data initialization file is unsafe')
            while chunk := os.read(fd, 1024 * 1024):
                if chunk != b'\xff' * len(chunk):
                    return False
        seed_fd = os.open(seed_path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        descriptors.append(seed_fd)
        before = os.fstat(seed_fd)
        if not stat.S_ISREG(before.st_mode) or before.st_size != size:
            raise ValueError('Data seed must have the exact original partition size')
        offset = 0
        while chunk := os.read(seed_fd, 1024 * 1024):
            done = 0
            while done < len(chunk):
                count = os.pwrite(descriptors[0], chunk[done:], offset + done)
                if count <= 0:
                    raise OSError('Short data seed write')
                done += count
            offset += len(chunk)
        after = os.fstat(seed_fd)
        identity = lambda st: (st.st_dev, st.st_ino, st.st_size, st.st_mtime_ns, st.st_ctime_ns)
        if offset != size or identity(before) != identity(after):
            raise ValueError('Data seed changed while initializing the private transaction')
        os.fsync(descriptors[0])
        return True
    finally:
        for fd in reversed(descriptors):
            os.close(fd)


NOR_PROFILE = 'q66@4.100.177'
NOR_AP_SHA256 = 'ff74c6f467529963b6afd40669437d8ec39a02e48c10d0893739847b3ba3dc2b'
NOR_OFFSET, NOR_BYTES, FACTORY_OFFSET = 0xFB8000, 0x48000, 0xFC0000


def validate_nor_geometry(board, *, ap_sha256=None, ap_bytes=None):
    """Check a descriptor derived from this profile's original AP partitions.

    This is a storage contract, not a claim that the selected QEMU implements
    the layout or that a factory seed/protocol is known for the profile.
    """
    if type(board) is not dict or type(board.get('geometry')) is not dict:
        raise ValueError('NOR requires a board geometry')
    descriptor = board.get('nor_geometry')
    required = {'profileID', 'apSHA256', 'apBytes', 'apOffset', 'flashBytes',
                'offset', 'bytes', 'factoryOffset'}
    config = {'configOffset', 'configBytes'}
    if (type(descriptor) is not dict or not required <= set(descriptor)
            or set(descriptor) - required - config
            or bool(set(descriptor) & config) != (config <= set(descriptor))):
        raise ValueError('NOR requires an explicit complete exact-profile descriptor')
    profile = descriptor['profileID']
    digest = descriptor['apSHA256']
    if (type(profile) is not str or not 1 <= len(profile) <= 256
            or profile != board.get('profile_id')
            or type(digest) is not str or not re.fullmatch('[0-9a-f]{64}', digest)):
        raise ValueError('NOR descriptor does not match the exact profile/AP identity')
    numbers = required - {'profileID', 'apSHA256'}
    if any(type(descriptor[k]) is not int for k in numbers | (set(descriptor) & config)):
        raise ValueError('NOR descriptor geometry must use integer byte counts')
    flash, offset, size = (descriptor[k] for k in ('flashBytes', 'offset', 'bytes'))
    config_offset, config_bytes = (descriptor.get(k, 0) for k in ('configOffset', 'configBytes'))
    tail_bytes = size - config_bytes
    factory = descriptor['factoryOffset']
    geometry = board['geometry']
    ap_offset = geometry.get('ap-offset')
    factory_bytes = board.get('factory_bytes')
    if (flash not in (8 * 1024 * 1024, 16 * 1024 * 1024)
            or offset <= 0 or not 0 < size <= flash or tail_bytes <= 0 or offset + tail_bytes != flash
            or (offset | size | factory) & 511
            or type(ap_offset) is not int or ap_offset < 0 or descriptor['apOffset'] != ap_offset
            or descriptor['apBytes'] <= 0 or ap_offset + descriptor['apBytes'] > offset
            or (ap_bytes is not None and descriptor['apBytes'] != ap_bytes)
            or type(factory_bytes) is not int or factory_bytes <= 0 or factory_bytes & 511
            or not offset <= factory < factory + factory_bytes <= flash):
        raise ValueError('NOR descriptor overlaps AP or has unsupported partition bounds')
    for key, wanted in {'flash-bytes': flash, 'nv-offset': offset,
                        'factory-offset': factory}.items():
        if type(geometry.get(key)) is not int or geometry[key] != wanted:
            raise ValueError('NOR descriptor differs from the original board geometry')
    for key, board_key in (('configOffset', 'config-offset'), ('configBytes', 'config-bytes')):
        value = geometry.get(board_key, 0)
        if type(value) is not int or value != descriptor.get(key, 0):
            raise ValueError('NOR configuration extent differs from the board geometry')
    if (config_offset < 0 or config_bytes < 0 or bool(config_offset) != bool(config_bytes)
            or (config_offset | config_bytes) & 8191
            or config_offset + config_bytes > ap_offset):
        raise ValueError('NOR configuration extent overlaps AP or is not erase-block aligned')
    return dict(descriptor)


def validate_instance_nor(path, sha256, initialize, board, nand):
    if type(initialize) is not bool or not path or not sha256 or nand is None:
        raise ValueError('NOR requires a complete NAND transaction and explicit initialization mode')
    descriptor = validate_nor_geometry(board)
    size = descriptor['bytes']
    path = Path(_private_image(path, size, sha256))
    if path != Path(nand['dataImage']).parent / 'nor-config.bin':
        raise ValueError('NOR must be the exact sibling working file')
    if initialize and path.read_bytes() != b'\xff' * size:
        raise ValueError('First NOR initialization requires an erased working image')
    result = dict(schema=1, profileID=descriptor['profileID'], apSHA256=descriptor['apSHA256'],
                apBytes=descriptor['apBytes'], apOffset=descriptor['apOffset'], flashBytes=descriptor['flashBytes'],
                factoryOffset=descriptor['factoryOffset'], factoryBytes=board['factory_bytes'],
                norImage=str(path), norSHA256=sha256, norOffset=descriptor['offset'],
                norBytes=size, norInitialize=initialize)
    if 'configBytes' in descriptor:
        result.update(configOffset=descriptor['configOffset'], configBytes=descriptor['configBytes'])
    return result


def initialize_instance_nor(storage, image, config_image=None):
    """Seed only a fresh erased transaction's factory and optional config.

    The compact file stores the tail first and the config extent second. Both
    writes belong to the same disposable transaction; accepted data is never
    overlaid, and the caller must discard this transaction on any write error.
    """
    import fcntl
    size = storage['norBytes']
    tail_bytes = size - storage.get('configBytes', 0)
    relative = storage['factoryOffset'] - storage['norOffset']
    if (storage['norInitialize'] is not True or type(image) is not bytes
            or len(image) != storage['factoryBytes'] or relative < 0
            or relative + len(image) > tail_bytes):
        raise ValueError('NOR initialization requires a first-run seed of the exact factory partition size')
    if config_image is not None and (type(config_image) is not bytes
            or not storage.get('configBytes', 0) or len(config_image) != storage['configBytes']):
        raise ValueError('NOR configuration seed must match its exact compact-file extent')
    path = storage['norImage']
    fd = os.open(path, os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        st = os.fstat(fd)
        if (not stat.S_ISREG(st.st_mode) or st.st_uid != os.geteuid() or st.st_nlink != 1
                or st.st_mode & 0o077 or st.st_size != size):
            raise ValueError('NOR initialization file is unsafe')
        original = os.pread(fd, size, 0)
        if original != b'\xff' * size or hashlib.sha256(original).hexdigest() != storage['norSHA256']:
            raise ValueError('NOR changed before first initialization')
        seeds = [(relative, image)]
        if config_image is not None:
            seeds.append((tail_bytes, config_image))
        for offset, seed in seeds:
            done = 0
            while done < len(seed):
                count = os.pwrite(fd, seed[done:], offset + done)
                if count <= 0:
                    raise OSError('Short NOR seed write')
                done += count
        os.fsync(fd)
    finally:
        os.close(fd)


def instance_nor_machine_options(storage):
    return ',nor-image=' + storage['norImage'].replace(',', ',,') + ',nor-writeback=on'


def instance_nor_result_geometry(storage):
    """Echo the compact file mapping for the host's stop-time identity check."""
    result = dict(nor_offset=storage['norOffset'] if storage else None,
                  nor_bytes=storage['norBytes'] if storage else None)
    if storage and storage.get('configBytes', 0):
        result.update(nor_config_offset=storage['configOffset'], nor_config_bytes=storage['configBytes'])
    return result
