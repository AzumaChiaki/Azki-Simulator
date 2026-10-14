"""Trusted, bounded BIN/RPK inspection and private staging for virtual peer install.

Standard library plus packages.py only; safe to copy beside the peer modules.
No caller-supplied inspection, guest execution, QMP, or compatibility assertion.
The unchanged guest installer is the final format/signature/acceptance authority.
"""
from __future__ import annotations
from contextlib import contextmanager
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path
import re
import stat
import sys
import uuid
import zipfile
import zlib

if __package__:
    from .packages import PackageError, _bin_header, _json
else:
    from packages import PackageError, _bin_header, _json

# Package storage binds the runner-selected profile without another model list.
# Authentication, firmware hashes and supported capabilities remain runner-owned.
DEFAULT_PROFILE = 'q66@4.100.177'
MAX_PACKAGE = 32 * 1024 * 1024
MAX_PACKAGES = 16
MAX_TOTAL_BYTES = 128 * 1024 * 1024
MAX_METADATA = 16384
MAX_MANIFEST = 1024 * 1024
FILE_PATTERN = re.compile(r'[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\.bin\Z')


def _require(value, code, message):
    if not value:
        raise PackageError(code, message)


def require_install_profile(profile_id):
    _require(type(profile_id) is str and 0 < len(profile_id.encode('utf-8')) <= 128 and
             not any(ord(character) < 32 or ord(character) == 127 for character in profile_id),
             'invalid_profile', '安装会话的固件标识无效。')


def _absolute(path):
    value = os.fspath(path)
    _require(isinstance(value, str) and os.path.isabs(value) and
             all(part not in ('.', '..') for part in value.split('/')),
             'unsafe_path', '需要不含相对路径的完整文件路径。')
    return value


def _directory(path):
    value = _absolute(path)
    if sys.platform == 'darwin':
        fd = os.open(value, os.O_RDONLY | os.O_DIRECTORY | 0x20000000)  # O_NOFOLLOW_ANY
    else:
        fd = os.open('/', os.O_RDONLY | os.O_DIRECTORY)
        try:
            for part in value.split('/')[1:]:
                if not part: continue
                child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
                os.close(fd); fd = child
        except BaseException:
            os.close(fd); raise
    try:
        info = os.fstat(fd)
        _require(info.st_uid == os.getuid() and stat.S_IMODE(info.st_mode) == 0o700,
                 'session_permissions', '运行目录必须属于当前用户且权限为 0700。')
        return fd
    except BaseException:
        os.close(fd); raise


def _read_fd(fd, limit, *, private):
    before = os.fstat(fd)
    _require(stat.S_ISREG(before.st_mode) and before.st_uid == os.getuid() and before.st_nlink == 1,
             'source_type', '包必须是当前用户的普通单链接文件。')
    _require((stat.S_IMODE(before.st_mode) == 0o600 if private else not before.st_mode & 0o022),
             'source_permissions', '暂存包必须为 0600；源文件不可由其他用户写入。')
    _require(0 < before.st_size <= limit, 'quota', '包为空或超过大小限制。')
    data = bytearray()
    while len(data) <= limit:
        chunk = os.read(fd, min(1024 * 1024, limit + 1 - len(data)))
        if not chunk: break
        data.extend(chunk)
    after = os.fstat(fd)
    identity = lambda item: (item.st_dev, item.st_ino, item.st_size, item.st_mtime_ns, item.st_ctime_ns, item.st_nlink, item.st_mode)
    _require(identity(before) == identity(after) and len(data) == before.st_size,
             'source_changed', '读取期间文件发生变化，请重新选择。')
    return bytes(data)


def _read_source(path):
    value = _absolute(path)
    if sys.platform == 'darwin':
        fd = os.open(value, os.O_RDONLY | os.O_NONBLOCK | 0x20000000)
    else:
        # Source parents need not be private, but no component may be a link.
        fd = os.open('/', os.O_RDONLY | os.O_DIRECTORY)
        try:
            parts = Path(value).parts[1:]
            for part in parts[:-1]:
                child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
                os.close(fd); fd = child
            child = os.open(parts[-1], os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW, dir_fd=fd)
        finally: os.close(fd)
        fd = child
    try: return _read_fd(fd, MAX_PACKAGE, private=False)
    finally: os.close(fd)


def _rpk_metadata(data):
    """Read only the standard install manifest; payload validity belongs to the guest.

    AzkiDeck RpkManifest.parse needs package and versionCode for pre-check.
    ZIP entries never become host paths and no app/page/resource is decoded.
    """
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            entry = archive.getinfo('manifest.json')
            _require(entry.file_size <= MAX_MANIFEST, 'quota', '快应用清单超过读取上限。')
            with archive.open(entry) as stream:
                payload = stream.read(MAX_MANIFEST + 1)
            _require(len(payload) <= MAX_MANIFEST, 'quota', '快应用清单超过读取上限。')
        item = json.loads(payload)
    except (OSError, ValueError, KeyError, RuntimeError, RecursionError, zipfile.BadZipFile, zlib.error) as error:
        if isinstance(error, PackageError):
            raise
        raise PackageError('manifest', '无法读取快应用 manifest.json。') from error
    _require(isinstance(item, dict) and isinstance(item.get('package'), str) and item['package'] and
             type(item.get('versionCode')) is int and 0 <= item['versionCode'] <= 0x7fffffffffffffff,
             'manifest', '快应用清单缺少包名或有效版本号。')
    text = lambda key: item[key] if isinstance(item.get(key), str) else ''
    devices = item.get('deviceTypeList')
    platform = item.get('minPlatformVersion')
    return {'packageName': item['package'], 'versionCode': item['versionCode'],
            'name': text('name'), 'versionName': text('versionName'),
            'minPlatformVersion': platform if type(platform) is int else 0,
            'deviceTypes': devices if isinstance(devices, list) and all(isinstance(x, str) for x in devices) else []}


def _report(format_name, metadata, size, sha256, profile_id):
    return {'schemaVersion': 1, 'status': 'inspected', 'profileID': profile_id,
            'format': format_name, 'bytes': size, 'sha256': sha256, 'metadata': metadata,
            'validation': 'install_identity', 'guestInstalled': False, 'signatureVerified': False,
            'runtimeCompatibility': 'unverified', 'acceptanceAuthority': 'original-guest-installer'}


def _inspect_bytes(data, profile_id):
    require_install_profile(profile_id)
    _require(0 < len(data) <= MAX_PACKAGE, 'quota', '包为空或超过大小限制。')
    if data.startswith(b'PK\x03\x04'):
        format_name, metadata = 'rpk', _rpk_metadata(data)
    else:
        _require(data[:4] == b'\x5a\xa5\x34\x12', 'unsupported_format',
                 '请选择原始 BIN 表盘或 RPK 快应用。')
        header = _bin_header(data)
        format_name = 'watchface_bin_candidate'
        metadata = {'faceID': header['faceID'], 'protocolWord': header['protocolWord']}
    return _report(format_name, metadata, len(data), hashlib.sha256(data).hexdigest(), profile_id)


def inspect_watchface(source_path, *, profile_id, expected_source_sha256=None):
    """Inspect exact BIN/RPK bytes; retained name is a Runtime API compatibility alias."""
    try:
        data = _read_source(source_path)
        report = _inspect_bytes(data, profile_id)
        _expected(report['sha256'], expected_source_sha256)
        return report
    except OSError as error:
        raise PackageError('source_io', '无法安全读取包文件。') from error


def _expected(actual, expected):
    if expected is not None:
        _require(isinstance(expected, str) and re.fullmatch('[0-9a-f]{64}', expected) and actual == expected,
                 'source_changed', '文件与已选择或检查的散列不一致。')


@contextmanager
def _packages(run_directory, *, create=False):
    root = _directory(run_directory)
    folder = None
    try:
        if create:
            try: os.mkdir('packages', 0o700, dir_fd=root)
            except FileExistsError: pass
        folder = os.open('packages', os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=root)
        info = os.fstat(folder)
        _require(info.st_uid == os.getuid() and stat.S_IMODE(info.st_mode) == 0o700,
                 'session_permissions', '包暂存目录必须为当前用户的 0700 目录。')
        yield folder
    finally:
        if folder is not None: os.close(folder)
        os.close(root)


@contextmanager
def _lock(folder):
    fd = os.open('.lock', os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600, dir_fd=folder)
    try:
        info = os.fstat(fd)
        _require(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid() and info.st_nlink == 1 and stat.S_IMODE(info.st_mode) == 0o600,
                 'session_permissions', '包暂存锁文件无效。')
        try: fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error: raise PackageError('session_busy', '正在暂存另一个包，请稍后重试。') from error
        yield
    finally: os.close(fd)


def _usage(folder):
    count, total = 0, 0
    entries = 0
    with os.scandir(folder) as files:
        for entry in files:
            name = entry.name
            if name == '.lock': continue
            entries += 1
            _require(entries <= MAX_PACKAGES * 2, 'session_quota', '运行会话的包暂存额度已满。')
            _require(FILE_PATTERN.fullmatch(name) or (name.endswith('.inspection.json') and FILE_PATTERN.fullmatch(name[:-16] + '.bin')),
                     'session_state', '暂存目录含有未知文件。')
            info = entry.stat(follow_symlinks=False)
            _require(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid() and info.st_nlink == 1 and stat.S_IMODE(info.st_mode) == 0o600,
                     'session_state', '暂存目录包含不安全文件。')
            count += int(name.endswith('.bin')); total += info.st_size
            _require(count <= MAX_PACKAGES and total <= MAX_TOTAL_BYTES, 'session_quota', '运行会话的包暂存额度已满。')
    return count, total


def _write(folder, name, data):
    fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=folder)
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, 'wb', closefd=False) as stream:
            stream.write(data); stream.flush(); os.fsync(fd)
    except BaseException:
        info = os.fstat(fd)
        current = os.stat(name, dir_fd=folder, follow_symlinks=False)
        if (info.st_dev, info.st_ino) == (current.st_dev, current.st_ino):
            os.unlink(name, dir_fd=folder)
        raise
    finally: os.close(fd)


def stage_watchface(source_path, run_directory, *, profile_id, expected_source_sha256=None):
    """Publish a fresh UUID.bin and trusted audit receipt; never overwrite others."""
    require_install_profile(profile_id)
    try:
        data = _read_source(source_path)
        report = _inspect_bytes(data, profile_id); _expected(report['sha256'], expected_source_sha256)
        filename = str(uuid.uuid4()) + '.bin'; receipt_name = filename[:-4] + '.inspection.json'
        receipt = {**report, 'status': 'staged', 'filename': filename, 'inspectionFile': receipt_name}
        metadata = json.dumps(receipt, sort_keys=True, ensure_ascii=False).encode('utf-8')
        _require(len(metadata) <= MAX_METADATA, 'quota', '检查记录超过上限。')
        with _packages(run_directory, create=True) as folder, _lock(folder):
            count, used = _usage(folder)
            _require(count < MAX_PACKAGES and used + len(data) + len(metadata) <= MAX_TOTAL_BYTES,
                     'session_quota', '运行会话的包暂存额度已满。')
            created = []
            try:
                # Receipt publication is last. UUID.bin is a container name for
                # either byte format; peers bind the receipt to its exact bytes.
                _write(folder, filename, data); created.append(filename)
                _write(folder, receipt_name, metadata); created.append(receipt_name)
                os.fsync(folder)
            except BaseException:
                for name in created: os.unlink(name, dir_fd=folder)
                raise
        return receipt
    except OSError as error:
        raise PackageError('staging_io', '包暂存失败，既有包未被覆盖。') from error


# Existing callers keep working; new callers may use format-neutral names.
inspect_package = inspect_watchface
stage_package = stage_watchface


def load_staged(run_directory, filename, expected_sha256, *, profile_id):
    """Bind private incoming bytes to their receipt without repeating format inspection.

    Callers provide only a UUID filename and SHA, never a receipt or external
    path. Receipt metadata feeds guest pre-check; it cannot authorize host code
    execution, select another path, or declare installation/signature success.
    """
    require_install_profile(profile_id)
    _require(isinstance(filename, str) and FILE_PATTERN.fullmatch(filename), 'unsafe_path', '包文件名必须为 UUID.bin。')
    _require(isinstance(expected_sha256, str) and re.fullmatch('[0-9a-f]{64}', expected_sha256),
             'source_changed', '安装请求必须包含完整 SHA-256。')
    try:
        with _packages(run_directory) as folder:
            fd = os.open(filename, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=folder)
            try: data = _read_fd(fd, MAX_PACKAGE, private=True)
            finally: os.close(fd)
            receipt_name = filename[:-4] + '.inspection.json'
            fd = os.open(receipt_name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=folder)
            try: receipt = _json(_read_fd(fd, MAX_METADATA, private=True), MAX_METADATA)
            finally: os.close(fd)
            digest = hashlib.sha256(data).hexdigest(); _expected(digest, expected_sha256)
            _require(isinstance(receipt, dict) and all(receipt.get(key) == value for key, value in
                     {'filename': filename, 'inspectionFile': receipt_name, 'profileID': profile_id,
                      'bytes': len(data), 'sha256': digest}.items()),
                     'staged_binding', '暂存检查记录与当前文件不一致。')
            format_name, metadata = receipt.get('format'), receipt.get('metadata')
            _require(format_name in ('rpk', 'watchface_bin_candidate') and isinstance(metadata, dict),
                     'staged_binding', '暂存记录缺少安装标识。')
            fields = ('packageName', 'versionCode', 'name', 'versionName', 'minPlatformVersion', 'deviceTypes') if format_name == 'rpk' else ('faceID', 'protocolWord')
            report = _report(format_name, {key: metadata[key] for key in fields if key in metadata},
                             len(data), digest, profile_id)
            return {**report, 'filename': filename, 'inspectionFile': receipt_name}
    except OSError as error:
        raise PackageError('staged_io', '暂存包缺失或无法安全读取，请重新导入。') from error
