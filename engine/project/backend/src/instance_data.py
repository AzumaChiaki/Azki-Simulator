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


SETTINGS_KEY = re.compile(r'persist\.[A-Za-z0-9._-]{1,120}\Z')


def load_instance_settings(path, storage):
    """The host-materialized settings file next to this session's data image."""
    import json
    path = Path(path)
    if storage is None or path.parent != Path(storage['dataImage']).parent or path.name != 'instance-settings.json':
        raise ValueError('Instance settings must belong to this session data image')
    st = path.lstat()
    if not stat.S_ISREG(st.st_mode) or st.st_uid != os.geteuid() or st.st_mode & 0o077 or st.st_size > 1 << 20:
        raise ValueError('Instance settings file is unsafe')
    value = json.loads(path.read_text(encoding='utf-8'))
    properties, clock = value.get('properties'), value.get('clock')
    if (value.get('schema') != 1 or not isinstance(properties, dict) or len(properties) > 512
            or not isinstance(clock, dict) or clock.get('mode') not in ('host', 'offset', 'firmware')
            or (clock['mode'] == 'offset' and type(clock.get('offsetSeconds')) is not int)
            or (clock.get('timeZone') is not None and (not isinstance(clock['timeZone'], str)
                or not re.fullmatch(r'[A-Za-z][A-Za-z0-9_+/\-]{0,63}', clock['timeZone'])))):
        raise ValueError('Instance settings schema is invalid')
    for key, item in properties.items():
        if (not isinstance(key, str) or not SETTINGS_KEY.fullmatch(key)
                or (item is not None and (not isinstance(item, str) or len(item) > 1024
                                          or any(c in item for c in '\0\n\r\t')))):
            raise ValueError('Instance setting is invalid: ' + str(key)[:80])
    validate_board_settings(value)
    return value


# Board inputs a hardware preset resolved before the session was created. The
# runner only formats them into core properties; it never decides which model
# has which device, so no firmware or version knowledge lives here.
SETTINGS_CHARGERS = ('unplugged', 'charging', 'full')
SETTINGS_GAUGES = ('hp4560', 'hp4570')


def validate_board_settings(value):
    battery, health = value.get('battery'), value.get('health')
    if battery is not None:
        if (not isinstance(battery, dict)
                or set(battery) - {'channel', 'gauge', 'coulomb', 'sampleRaw', 'charger'}
                or type(battery.get('channel')) is not int or not 0 <= battery['channel'] <= 7
                or battery.get('gauge') not in SETTINGS_GAUGES
                or battery.get('coulomb') not in (None, 'cw2218')
                or (battery.get('coulomb') is not None and battery.get('gauge') != 'hp4570')
                or ('sampleRaw' in battery and (type(battery['sampleRaw']) is not int
                                                or not 0 <= battery['sampleRaw'] <= 65535))
                or ('charger' in battery and battery['charger'] not in SETTINGS_CHARGERS)):
            raise ValueError('Instance battery settings are invalid')
    if health is not None:
        # The host resolved the preset's sensor wiring (sensorBoard); the
        # runner only checks its shape and forwards it to the core.
        if (not isinstance(health, dict)
                or set(health) - {'sensorBoard', 'walkSteps', 'cadence', 'heartRate', 'worn'}
                or ('worn' in health and type(health['worn']) is not bool)
                or not isinstance(health.get('sensorBoard'), str)
                or not SENSOR_BOARD.fullmatch(health['sensorBoard'])
                or ('walkSteps' in health and (type(health['walkSteps']) is not int
                                               or not 0 <= health['walkSteps'] <= 10000000))
                or ('cadence' in health and (type(health['cadence']) is not int
                                             or not 40 <= health['cadence'] <= 200))
                or ('heartRate' in health and (type(health['heartRate']) is not int
                                               or not 0 <= health['heartRate'] <= 255))):
            raise ValueError('Instance health settings are invalid')
    return value


# imu:<chip>:<spi0|spi1|i2cN>:<cs|address>:<irq|-1>[:mount];ppg:<chip>:...[:tick Hz]
_SENSOR_PORT = r'(spi[01]|i2c[0-7]):(0x[0-9a-f]{1,2}|\d{1,2}):(-1|\d{1,2})'
_SENSOR_ENTRY = ('(imu:(lsm6dso|icm42607):' + _SENSOR_PORT + '(:([+-]?[xyz]){3})?'
                 '|ppg:hx369[57]:' + _SENSOR_PORT + r'(:\d{4,7})?)')
SENSOR_BOARD = re.compile(_SENSOR_ENTRY + '(;' + _SENSOR_ENTRY + ')?')


def instance_settings_options(settings, *, adc_fixture, default_sample=21000, ntc_channels=0,
                              ntc_sample=8990):
    """QEMU -M options for one session's host settings.

    Values never contain a separator: the ADC vector doubles its commas the way
    QEMU's machine parser expects, and the health fixture uses ';'. The board's
    thermistor channels (geometry pmu-adc-ntc mask) keep the core's
    room-temperature sample so a charger is not stopped as too cold.
    """
    options = []
    battery = settings.get('battery')
    if battery:
        options.append('battery-gauge=' + battery['gauge'])
        # The core always boots unplugged: a charger present at power-on
        # sends the firmware into its charge-and-reboot flow. The runner
        # plugs it in after boot (instance_settings_runtime).
        if 'sampleRaw' in battery:
            if not adc_fixture:
                raise ValueError('Battery sample input requires the PMU ADC fixture')
            raw = [ntc_sample if ntc_channels >> channel & 1 else default_sample
                   for channel in range(8)]
            raw[battery['channel']] = battery['sampleRaw']
            options.append('pmu-adc-raw=' + ','.join(str(item) for item in raw).replace(',', ',,'))
    health = settings.get('health')
    if health:
        # The preset's motion/optical sensors are attached only for a session
        # that drives them: the firmware itself counts the walked steps and
        # measures the pulse from their raw samples. ';' separates fields
        # inside one machine value (a comma would split the option).
        options.append('sensor-board=' + health['sensorBoard'])
        # The walk starts later (instance_settings_walk): steps taken while
        # the firmware's pedometer service is still starting are not counted.
        options.append('health-fixture=walk=0;cadence=%d;heart-rate=%d;worn=%d'
                       % (health.get('cadence', 110), health.get('heartRate', 72),
                          health.get('worn', True)))
    return options


def instance_settings_walk(settings):
    """Steps the wearer walks once the firmware's pedometer is running."""
    return (settings.get('health') or {}).get('walkSteps', 0)


def instance_settings_runtime(settings):
    """Core properties the runner sets over QMP once the guest is running."""
    battery = settings.get('battery') or {}
    if battery.get('charger', 'unplugged') != 'unplugged':
        return [('battery-charger', battery['charger'])]
    return []


def apply_instance_settings(storage, settings, helper, temporary):
    """Write settings and the boot clock into persist.db before the core starts.

    persist.rtc_save is the firmware's own saved wall clock: with no valid RTC
    at boot it restores system time from it (miwear_rtc_checker)."""
    import subprocess
    import time
    lines = []
    for key, item in sorted(settings['properties'].items()):
        lines.append(f'D\t{key}\n' if item is None else f'S\t{key}\t{item}\n')
    clock_seconds = None
    if settings['clock']['mode'] != 'firmware':
        clock_seconds = int(time.time()) + settings['clock'].get('offsetSeconds', 0)
        lines.append(f'S\tpersist.rtc_save\t{clock_seconds}\n')
        zone = settings['clock'].get('timeZone')
        if zone and 'persist.system_tz' not in settings['properties']:
            lines.append(f'S\tpersist.system_tz\t{zone}\n')
    if not lines:
        return None
    changes = Path(temporary) / 'instance-settings-changes.txt'
    fd = os.open(changes, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w', encoding='utf-8') as out:
        out.writelines(lines)
    try:
        subprocess.run([str(helper), storage['dataImage'], storage['spareImage'], 'set', str(changes)],
                       check=True, capture_output=True, timeout=120, env={**os.environ, 'TMPDIR': str(temporary)})
    except subprocess.CalledProcessError as error:
        raise ValueError('Writing instance settings failed: ' + error.stderr.decode(errors='replace')[-300:]) from error
    finally:
        changes.unlink(missing_ok=True)
    return clock_seconds


def read_instance_properties(storage, helper, temporary):
    """The firmware's own persist.db properties after a clean stop."""
    import json
    import subprocess
    result = subprocess.run([str(helper), storage['dataImage'], storage['spareImage'], 'get'],
                            check=True, capture_output=True, timeout=120, env={**os.environ, 'TMPDIR': str(temporary)})
    value = json.loads(result.stdout)
    if not isinstance(value, dict):
        raise ValueError('Instance property dump is invalid')
    return value
