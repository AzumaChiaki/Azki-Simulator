"""Private 4 GiB eMMC transactions for the XRing O62M firmware."""
from pathlib import Path
import errno
import os
import re
import shutil
import uuid

EMMC_BYTES = 4 * 1024**3
MODEL = 'miwear.watch.o62m'
KIND = 'xring-emmc'


def is_plan(plan):
    identity = plan.get('identity', {})
    return identity.get('profile', {}).get('model') == MODEL and identity.get('profile', {}).get('ap', {}).get('chip') == 'xring'


def copy_image(source, target):
    """Clone on APFS; preserve sparse regions when copying between volumes."""
    from . import instance_storage as store
    source, target = Path(source), Path(target)
    store._private(source, size=EMMC_BYTES)
    if store._clone_file(source, target):
        return
    with source.open('rb') as src:
        fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        try:
            os.ftruncate(fd, EMMC_BYTES)
            offset = 0
            sparse = hasattr(os, 'SEEK_DATA')
            while offset < EMMC_BYTES:
                end = EMMC_BYTES
                if sparse:
                    try:
                        offset = os.lseek(src.fileno(), offset, os.SEEK_DATA)
                        end = min(os.lseek(src.fileno(), offset, os.SEEK_HOLE), EMMC_BYTES)
                    except OSError as error:
                        if error.errno == errno.ENXIO:
                            break
                        if error.errno not in (errno.EINVAL, errno.ENOTSUP):
                            raise
                        sparse = False
                while offset < end:
                    chunk = os.pread(src.fileno(), min(1024 * 1024, end - offset), offset)
                    if not chunk:
                        raise ValueError('Incomplete eMMC image')
                    if chunk.strip(b'\0'):
                        written = 0
                        while written < len(chunk):
                            amount = os.pwrite(fd, chunk[written:], offset + written)
                            if amount <= 0:
                                raise OSError('Short eMMC image write')
                            written += amount
                    offset += len(chunk)
            os.fsync(fd)
        except BaseException:
            os.close(fd); target.unlink(); raise
        os.close(fd)


def begin(store, firmware, previous):
    """Use the existing revision/HEAD protocol with a single whole-media file."""
    from . import instance_storage as storage
    if previous and previous[2].data_format != KIND:
        raise ValueError('此实例不是 XRing eMMC 存储，请新建实例')
    store._make_room(protect={previous[0].name} if previous else set())
    tx = 'r-' + uuid.uuid4().hex
    folder = store.root / 'revisions' / tx
    folder.mkdir(mode=0o700)
    marker = {'revision': tx, 'previous': previous[0].name if previous else None,
              'firmware': firmware.record, 'phase': 'preparing'}
    storage._atomic_json(store.root / 'pending.json', marker)
    store.active = folder
    try:
        image = folder / 'emmc.img'
        if previous:
            copy_image(previous[0] / 'emmc.img', image)
        else:
            fd = os.open(image, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            try:
                os.ftruncate(fd, EMMC_BYTES); os.fsync(fd)
            finally:
                os.close(fd)
        storage._atomic_json(folder / 'revision.json', {**marker,
            'origin': 'accepted-instance-revision' if previous else 'fresh-emmc',
            'compatibility': 'same-device eMMC; firmware decides compatibility'})
        storage._atomic_json(store.root / 'pending.json', {**marker, 'phase': 'guest'})
        return {'previous_revision': marker['previous'], 'emmc_initialize': previous is None}
    except BaseException:
        shutil.rmtree(folder); (store.root / 'pending.json').unlink()
        store.active = None
        raise


def prepare(library_root, session_root, instance_id, plan):
    from . import instance_storage as storage
    from .maintenance import reset_instance
    instance_id = storage._uuid(instance_id)
    session = storage._path(session_root)
    firmware = storage.Firmware(MODEL, plan['profileID'], 0, EMMC_BYTES, KIND)
    firmware.validate()
    reset_instance(library_root, instance_id)
    store = storage._open_or_create(storage._instance_root(library_root, instance_id), MODEL)
    try:
        tx = store.begin(firmware)
        work = session / 'instance-data'
        work.mkdir(mode=0o700)
        copy_image(store.active / 'emmc.img', work / 'emmc.img')
        binding = {'available': True, 'kind': KIND, 'instanceID': instance_id,
            'sessionRoot': str(session), 'model': MODEL, 'profileID': firmware.profile,
            'qemuSHA256': plan['identity']['qemu']['sha256'],
            'transactionID': store.active.name, 'previousRevision': tx['previous_revision'],
            'emmcImage': str(work / 'emmc.img'), 'emmcBytes': EMMC_BYTES,
            'emmcInitialize': tx['emmc_initialize'], 'norPersistent': False,
            'virtualPeerEnabled': False, 'virtualPeerReason': 'XRing 不使用虚拟对端绑定状态',
            'consistency': 'crash-consistent', 'reason': '独立 eMMC 会话快照；正常关闭后保存'}
        pending = storage._json(store.root / 'pending.json')
        storage._atomic_json(store.root / 'pending.json', {**pending, 'sessionRoot': str(session)})
        storage._atomic_json(session / 'instance-storage.json', binding)
        return binding
    except BaseException:
        if store.active is not None:
            store.finish(success=False)
        raise
    finally:
        store.close()


def validate(binding, *, project, profile_id, model, qemu_sha256):
    from . import instance_storage as storage
    storage._uuid(binding.get('instanceID'))
    expected = storage._path(project).parent.parent / 'session'
    if (binding.get('available') is not True or binding.get('kind') != KIND or model != MODEL
            or binding.get('model') != MODEL or binding.get('profileID') != profile_id
            or binding.get('sessionRoot') != str(expected)
            or binding.get('qemuSHA256') != qemu_sha256 or not storage._hex(qemu_sha256, 64)
            or re.fullmatch(r'r-[0-9a-f]{32}', binding.get('transactionID', '')) is None
            or binding.get('emmcImage') != str(expected / 'instance-data/emmc.img')
            or type(binding.get('emmcBytes')) is not int or binding['emmcBytes'] != EMMC_BYTES
            or type(binding.get('emmcInitialize')) is not bool
            or binding.get('emmcInitialize') != (binding.get('previousRevision') is None)
            or binding.get('norPersistent') is not False or binding.get('virtualPeerEnabled') is not False
            or binding.get('consistency') != 'crash-consistent'
            or any(k in binding for k in (*storage.VIRTUAL_FIELDS, 'physicalBluetooth', 'dataImage', 'spareImage', 'norImage'))):
        raise ValueError('XRing eMMC binding differs from this instance/session/core')
    storage._private(expected / 'instance-data/emmc.img', size=EMMC_BYTES)
    if storage._json(expected / 'instance-storage.json') != binding:
        raise ValueError('XRing instance binding record changed')
    firmware = storage.Firmware(MODEL, profile_id, 0, EMMC_BYTES, KIND)
    firmware.validate()
    return firmware


def arguments(binding):
    argv = ['--instance-emmc-image', binding['emmcImage']]
    if binding['emmcInitialize']:
        argv.append('--instance-emmc-initialize')
    return argv
