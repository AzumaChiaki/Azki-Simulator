"""Ephemeral, same-user discovery metadata; never connects to a helper.

Advertisements are hints, not authorization or liveness evidence. Every result
is notConnected and may be stale, including when a Unix socket inode exists.
The helper must publish after listening and withdraw in its own finally block.
No PID, credentials, input paths or persistent access material are advertised.
"""
from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import stat
import sys
import threading
import uuid


PROTOCOL = "azki.simulator.session.v1"
VERSION = 1
MAX_ADVERTISEMENT = 4096
MAX_DESCRIPTOR = 65536
MAX_SCAN = 256
MAX_RESULTS = 64
CAPABILITIES = frozenset(("status", "start", "stop", "input", "screenshot", "logs",
                          "inspect-package", "stage-package"))
_FIELDS = frozenset(("id", "directory", "protocol", "version", "capabilities"))
_ENTRY = re.compile(r"session-([0-9a-f-]{36})\.json\Z")
_LOCK = threading.RLock()


class DiscoveryError(ValueError):
    def __init__(self, code, message):
        self.code = code
        super().__init__(message)


@dataclass
class _Publication:
    pid: int
    directory_fd: int
    file_fd: int  # Keeps the original inode alive: unlink/recreate cannot reuse it.
    directory_identity: tuple
    file_identity: tuple
    payload: bytes


_OWNED = {}


def _registry_path():
    # Session roots live in the platform temp directory (Darwin exposes /tmp as
    # a link to /private/tmp; Linux uses /tmp directly).
    return Path("/private/tmp" if sys.platform == "darwin" else "/tmp") / ("azki-simulator-" + str(os.getuid()))


def _path(value):
    if not isinstance(value, (str, Path)):
        raise DiscoveryError("unsafe_discovery", "Expected an absolute canonical path")
    path = Path(value)
    text = str(path)
    if (path.anchor != "/" or ".." in path.parts or
            any(ord(char) < 32 or ord(char) == 127 for char in text) or
            len(os.fsencode(text)) > 1024):
        raise DiscoveryError("unsafe_discovery", "Expected an absolute canonical path")
    return path


def _identity(info):
    return info.st_dev, info.st_ino


def _private_directory(info):
    if (not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or
            stat.S_IMODE(info.st_mode) != 0o700):
        raise DiscoveryError("unsafe_discovery", "Expected this user's private 0700 directory")


def _open_directory(path, private=True):
    """Walk all components with NOFOLLOW, pinning each parent while opening."""
    path = _path(path)
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
    fd = os.open("/", flags)
    try:
        for component in path.parts[1:]:
            child = os.open(component, flags, dir_fd=fd)
            os.close(fd)
            fd = child
        if private:
            _private_directory(os.fstat(fd))
        return fd
    except BaseException:
        os.close(fd)
        raise


def _open_registry(create=False):
    path = _path(_registry_path())
    parent = _open_directory(path.parent, private=False)
    try:
        if create:
            try:
                os.mkdir(path.name, 0o700, dir_fd=parent)
            except FileExistsError:
                pass
        fd = os.open(path.name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW |
                     os.O_CLOEXEC, dir_fd=parent)
        try:
            _private_directory(os.fstat(fd))
        except BaseException:
            os.close(fd)
            raise
        return path, fd
    finally:
        os.close(parent)


def _regular(info, maximum):
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or
            stat.S_IMODE(info.st_mode) != 0o600 or info.st_nlink != 1 or
            not 0 < info.st_size <= maximum):
        raise DiscoveryError("invalid_advertisement", "Expected a private, bounded, singly linked regular file")


def _unchanged(before, after):
    return (before.st_dev, before.st_ino, before.st_uid, before.st_mode,
            before.st_nlink, before.st_size, before.st_mtime_ns, before.st_ctime_ns) == (
                after.st_dev, after.st_ino, after.st_uid, after.st_mode,
                after.st_nlink, after.st_size, after.st_mtime_ns, after.st_ctime_ns)


def _read_at(directory_fd, name, maximum):
    fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC,
                 dir_fd=directory_fd)
    try:
        before = os.fstat(fd)
        _regular(before, maximum)
        chunks, size = [], 0
        while size <= maximum:
            chunk = os.read(fd, min(4096, maximum + 1 - size))
            if not chunk:
                break
            chunks.append(chunk)
            size += len(chunk)
        if size != before.st_size or not _unchanged(before, os.fstat(fd)):
            raise DiscoveryError("invalid_advertisement", "Discovery metadata changed while reading")
        return b"".join(chunks)
    finally:
        os.close(fd)


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise DiscoveryError("invalid_advertisement", "Duplicate JSON key")
        result[key] = value
    return result


def _nonfinite(value):
    raise DiscoveryError("invalid_advertisement", "Non-finite JSON value")


def _json(data):
    value = json.loads(data.decode("utf-8"), object_pairs_hook=_object, parse_constant=_nonfinite)
    if not isinstance(value, dict):
        raise DiscoveryError("invalid_advertisement", "Expected a JSON object")
    return value


def _projection(value, root=None, advertisement=False):
    if not isinstance(value, dict) or (advertisement and set(value) != _FIELDS):
        raise DiscoveryError("invalid_advertisement", "Unsupported discovery fields")
    identifier = value.get("id")
    if not isinstance(identifier, str) or str(uuid.UUID(identifier)) != identifier:
        raise DiscoveryError("invalid_advertisement", "Expected a canonical UUID")
    if value.get("protocol") != PROTOCOL or type(value.get("version")) is not int or value["version"] != VERSION:
        raise DiscoveryError("invalid_advertisement", "Unsupported session protocol")
    directory = _path(value.get("directory"))
    if str(directory) != value["directory"] or (root is not None and directory != root):
        raise DiscoveryError("invalid_advertisement", "Session directory does not match")
    if len(os.fsencode(directory / "control.sock")) > 103:
        raise DiscoveryError("unsafe_discovery", "Session path exceeds Unix socket limit")
    capabilities = value.get("capabilities")
    if (not isinstance(capabilities, list) or len(capabilities) > len(CAPABILITIES) or
            any(not isinstance(item, str) or item not in CAPABILITIES for item in capabilities) or
            len(set(capabilities)) != len(capabilities)):
        raise DiscoveryError("invalid_advertisement", "Unsupported capability declaration")
    if not advertisement and value.get("socket") != "control.sock":
        raise DiscoveryError("invalid_advertisement", "Session control socket must be control.sock")
    return {"id": identifier, "directory": str(directory), "protocol": PROTOCOL,
            "version": VERSION, "capabilities": list(capabilities)}


def _session_matches(directory_fd, projected):
    descriptor = _json(_read_at(directory_fd, "session.json", MAX_DESCRIPTOR))
    if _projection(descriptor, Path(projected["directory"])) != projected:
        raise DiscoveryError("invalid_advertisement", "Advertisement does not match session descriptor")


def _socket_present(directory_fd):
    try:
        info = os.stat("control.sock", dir_fd=directory_fd, follow_symlinks=False)
    except FileNotFoundError:
        return False
    if (not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid() or
            stat.S_IMODE(info.st_mode) != 0o600 or info.st_nlink != 1):
        raise DiscoveryError("invalid_advertisement", "Unsafe session control socket")
    return True  # An inode is not evidence of a listening/live helper.


def _unlink_exact(directory_fd, name, identity):
    try:
        info = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
        if _identity(info) != identity or not stat.S_ISREG(info.st_mode):
            return False
        os.unlink(name, dir_fd=directory_fd)
        return True
    except FileNotFoundError:
        return False


def publish(root, session):
    """Create a minimal advertisement exclusively; never replace a stale entry.

    Call withdraw(returned_path) in the publishing helper's finally block.
    An unresponsive/crashed helper's advertisement is deliberately not removed
    by discover(), nor treated as proof that the helper is still alive.
    """
    root = _path(root)
    projected = _projection(session, root)
    root_fd = _open_directory(root)
    try:
        _session_matches(root_fd, projected)
        _socket_present(root_fd)
    finally:
        os.close(root_fd)
    payload = (json.dumps(projected, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
    if len(payload) > MAX_ADVERTISEMENT:
        raise DiscoveryError("invalid_advertisement", "Advertisement exceeds limit")
    registry, directory_fd = _open_registry(create=True)
    name = "session-" + projected["id"] + ".json"
    path = registry / name
    temporary = ".publish-" + uuid.uuid4().hex
    fd, linked, retained = None, False, False
    try:
        fd = os.open(temporary, os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW |
                     os.O_CLOEXEC, 0o600, dir_fd=directory_fd)
        offset = 0
        while offset < len(payload):
            written = os.write(fd, payload[offset:])
            if written <= 0:
                raise OSError("Could not write discovery advertisement")
            offset += written
        os.fsync(fd)
        # link() is an atomic, no-replacement publication. Discovery ignores the
        # brief nlink=2 interval before the private temporary name is removed.
        try:
            os.link(temporary, name, src_dir_fd=directory_fd, dst_dir_fd=directory_fd,
                    follow_symlinks=False)
        except FileExistsError as error:
            raise DiscoveryError("advertisement_exists", "Session advertisement already exists; it was preserved") from error
        linked = True
        os.unlink(temporary, dir_fd=directory_fd)
        info = os.fstat(fd)
        _regular(info, MAX_ADVERTISEMENT)
        publication = _Publication(os.getpid(), directory_fd, fd,
                                   _identity(os.fstat(directory_fd)), _identity(info), payload)
        with _LOCK:
            _OWNED[str(path)] = publication
        retained = True
        return path
    finally:
        if not retained:
            try:
                if fd is not None:
                    identity = _identity(os.fstat(fd))
                    if linked:
                        _unlink_exact(directory_fd, name, identity)
                    _unlink_exact(directory_fd, temporary, identity)
            finally:
                if fd is not None:
                    os.close(fd)
                os.close(directory_fd)


def withdraw(path):
    """Remove only this process's unchanged advertisement; return success.

    Unknown, replaced, edited, linked or foreign-process entries are preserved.
    Metadata is private to one uid; this is not a boundary against hostile code
    already running as that same uid and racing directory operations.
    """
    try:
        path = _path(path)
    except (TypeError, ValueError):
        return False
    with _LOCK:
        publication = _OWNED.get(str(path))
        if publication is None or publication.pid != os.getpid():
            return False
        del _OWNED[str(path)]
        check_fd = None
        try:
            registry, check_fd = _open_registry()
            if path.parent != registry or _identity(os.fstat(check_fd)) != publication.directory_identity:
                return False
            current = os.stat(path.name, dir_fd=publication.directory_fd, follow_symlinks=False)
            _regular(current, MAX_ADVERTISEMENT)
            if _identity(current) != publication.file_identity:
                return False
            if _read_at(publication.directory_fd, path.name, MAX_ADVERTISEMENT) != publication.payload:
                return False
            return _unlink_exact(publication.directory_fd, path.name, publication.file_identity)
        except (OSError, ValueError):
            return False
        finally:
            if check_fd is not None:
                os.close(check_fd)
            os.close(publication.file_fd)
            os.close(publication.directory_fd)


def discover():
    """Read bounded, valid candidates; never connect, kill or clean stale files.

    Missing registry returns []; unsafe registry raises. Invalid advertisements
    are skipped. A removed/unverifiable session directory cannot be advertised.
    Results are capped at 64, with at most 256 directory entries examined.
    """
    try:
        registry, directory_fd = _open_registry()
    except FileNotFoundError:
        return []
    result = []
    try:
        with os.scandir(directory_fd) as entries:
            for index, entry in enumerate(entries):
                if index >= MAX_SCAN or len(result) >= MAX_RESULTS:
                    break
                match = _ENTRY.fullmatch(entry.name)
                if match is None:
                    continue
                root_fd = None
                try:
                    projected = _projection(_json(_read_at(directory_fd, entry.name, MAX_ADVERTISEMENT)),
                                            advertisement=True)
                    if projected["id"] != match.group(1):
                        continue
                    root_fd = _open_directory(Path(projected["directory"]))
                    _session_matches(root_fd, projected)
                    socket_present = _socket_present(root_fd)
                    result.append({**projected, "socket": "control.sock", "notConnected": True,
                                   "connectionState": "notConnected", "staleCandidate": True,
                                   "socketPresent": socket_present, "liveness": "unverified"})
                except (OSError, ValueError, TypeError, KeyError, RecursionError):
                    continue
                finally:
                    if root_fd is not None:
                        os.close(root_fd)
        return sorted(result, key=lambda value: value["id"])
    finally:
        os.close(directory_fd)
