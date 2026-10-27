"""Self-healing for instance state and leftover session roots.

instance-reset settles one instance before it starts: an interrupted storage
transaction is committed when its stopped run was healthy, otherwise rolled
back to the last committed snapshot; crash leftovers are removed. Committed
snapshots that HEAD can still reach and settings.json are never deleted.

sessions-cleanup removes session roots this app created under /private/tmp
once nothing runs in them. Removal never follows a link, never leaves the
root it was given, and stops at the first entry owned by another user.
"""
from __future__ import annotations

import ctypes
import errno
import json
import os
from pathlib import Path
import re
import socket
import stat
import sys
import time

from .minative import _decode, _path

TMP_ROOT = Path("/private/tmp")
SESSION_PREFIXES = ("azki-b-", "azki-sim-", "azki-cmd-")
# A root with no process/socket evidence is treated as live while this young:
# it may be a session being prepared right now.
GRACE_SECONDS = 10 * 60
PID_FILES = ("state.json", "runner-process.json", "core-process.json", "control.json")
REVISION = re.compile(r"r-[0-9a-f]{32}\Z")
MAX_SCAN = 20000


class MaintenanceError(ValueError):
    def __init__(self, code, message):
        self.code = code
        super().__init__(message)


class _Foreign(OSError):
    pass


# ---------------------------------------------------------------- removal

def _remove_entry(parent_fd, name, uid, depth=0):
    if depth > 64:
        raise OSError(errno.ELOOP, "目录层级过深")
    info = os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
    if info.st_uid != uid:
        raise _Foreign(errno.EPERM, "包含其他用户的文件：" + name[:80])
    if not stat.S_ISDIR(info.st_mode):
        # Regular files, links (the link itself), sockets and FIFOs.
        os.unlink(name, dir_fd=parent_fd)
        return
    if stat.S_IMODE(info.st_mode) & 0o700 != 0o700:
        # Our own read-only directory: widen for the owner only so it can go.
        os.chmod(name, stat.S_IMODE(info.st_mode) | 0o700, dir_fd=parent_fd, follow_symlinks=False)
    fd = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=parent_fd)
    try:
        opened = os.fstat(fd)
        if (opened.st_dev, opened.st_ino) != (info.st_dev, info.st_ino):
            raise OSError(errno.EAGAIN, "删除期间目录发生变化")
        with os.scandir(fd) as entries:
            names = [entry.name for entry in entries]
        for child in names:
            _remove_entry(fd, child, uid, depth + 1)
    finally:
        os.close(fd)
    os.rmdir(name, dir_fd=parent_fd)


def remove_tree(parent, name):
    """Remove parent/name without following links or touching foreign files."""
    fd = os.open(parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        _remove_entry(fd, name, os.getuid())
    finally:
        os.close(fd)


def _remove_if_present(parent, name, removed, label):
    try:
        remove_tree(parent, name)
    except FileNotFoundError:
        return False
    removed.append(label)
    return True


# --------------------------------------------------------------- liveness

class _BSDInfo(ctypes.Structure):
    _fields_ = [("flags", ctypes.c_uint32), ("status", ctypes.c_uint32), ("xstatus", ctypes.c_uint32),
                ("pid", ctypes.c_uint32), ("ppid", ctypes.c_uint32), ("uid", ctypes.c_uint32),
                ("gid", ctypes.c_uint32), ("ruid", ctypes.c_uint32), ("rgid", ctypes.c_uint32),
                ("svuid", ctypes.c_uint32), ("svgid", ctypes.c_uint32), ("rfu", ctypes.c_uint32),
                ("comm", ctypes.c_char * 16), ("name", ctypes.c_char * 32), ("nfiles", ctypes.c_uint32),
                ("pgid", ctypes.c_uint32), ("pjobc", ctypes.c_uint32), ("tdev", ctypes.c_uint32),
                ("tpgid", ctypes.c_uint32), ("nice", ctypes.c_int32), ("start_sec", ctypes.c_uint64),
                ("start_usec", ctypes.c_uint64)]


def _process_started(pid):
    """(uid, start seconds) of a live process, None if gone, ... if unknown."""
    if sys.platform != "darwin":
        return ...
    try:
        library = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
        call = library.proc_pidinfo
        call.argtypes = (ctypes.c_int, ctypes.c_int, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_int)
        call.restype = ctypes.c_int
        info = _BSDInfo()
        ctypes.set_errno(0)
        size = call(pid, 3, 0, ctypes.byref(info), ctypes.sizeof(info))  # PROC_PIDTBSDINFO
    except OSError:
        return ...
    if size == ctypes.sizeof(info):
        return info.uid, info.start_sec
    return None if ctypes.get_errno() in (errno.ESRCH, 0) else ...


def pid_alive(pid, recorded_at=None):
    """True/False, or None when it cannot be determined."""
    if type(pid) is not int or not 2 <= pid <= 2**31 - 1:
        return False
    if pid == os.getpid():
        return True
    try:
        os.kill(pid, 0)
        signalled = True
    except ProcessLookupError:
        return False
    except PermissionError:
        signalled = None
    except OSError:
        signalled = None
    started = _process_started(pid)
    if started is None:
        return None if signalled else False
    if started is ...:
        return signalled
    uid, start = started
    if uid != os.getuid():
        return False  # Every process this app starts runs as the user.
    if recorded_at is not None and start > recorded_at + 2:
        return False  # The recorded PID was reused by a newer process.
    return True


def _socket_alive(path):
    probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    probe.settimeout(0.5)
    try:
        probe.connect(path)
        return True
    except (ConnectionRefusedError, FileNotFoundError):
        return False
    except OSError:
        return None
    finally:
        probe.close()


def _small_json(directory_fd, name):
    try:
        fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory_fd)
    except OSError:
        return None, None
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_size > 65536:
            return None, None
        value = json.loads(os.read(fd, 65537).decode("utf-8"))
        return (value if isinstance(value, dict) else None), info.st_mtime
    except (OSError, ValueError, UnicodeError):
        return None, None
    finally:
        os.close(fd)


def session_state(root):
    """Return ("running" | "stopped" | "unknown", newest mtime) for a session root.

    "stopped" requires recorded processes that are gone and sockets nobody
    listens on; a root with no such evidence is "unknown".
    """
    evidence, alive, unsure, newest, count = False, False, False, 0.0, 0
    root = str(root)

    def visit(fd, path, depth):
        nonlocal evidence, alive, unsure, newest, count
        with os.scandir(fd) as entries:
            items = [(entry.name, entry.stat(follow_symlinks=False)) for entry in entries]
        for name, info in items:
            count += 1
            if count > MAX_SCAN:
                unsure = True
                return
            if depth <= 2:
                newest = max(newest, info.st_mtime)
            if stat.S_ISSOCK(info.st_mode):
                evidence = True
                state = _socket_alive(path + "/" + name)
                alive |= state is True
                unsure |= state is None
            elif stat.S_ISREG(info.st_mode) and name in PID_FILES:
                value, recorded = _small_json(fd, name)
                if value is None:
                    continue
                reaped = name == "state.json" and value.get("cleanup") == "reaped" and value.get("state") in ("exited", "failed")
                for key in ("pid", "supervisorPID"):
                    if type(value.get(key)) is int:
                        evidence = True
                        if reaped:
                            continue
                        state = pid_alive(value[key], recorded)
                        alive |= state is True
                        unsure |= state is None
            elif stat.S_ISDIR(info.st_mode) and depth < 4 and name not in ("prepared", "instance-data", "host-tmp"):
                try:
                    child = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
                except OSError:
                    unsure = True
                    continue
                try:
                    visit(child, path + "/" + name, depth + 1)
                finally:
                    os.close(child)

    try:
        fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    except FileNotFoundError:
        return "stopped", 0.0
    except OSError:
        return "unknown", time.time()
    try:
        newest = os.fstat(fd).st_mtime
        visit(fd, root, 0)
    except OSError:
        unsure = True
    finally:
        os.close(fd)
    if alive:
        return "running", newest
    if unsure or not evidence:
        return "unknown", newest
    return "stopped", newest


# ---------------------------------------------------------- instance reset

def _instance_dir(library_root, instance_id):
    from .instance_storage import _uuid
    try:
        identifier = _uuid(instance_id)
    except (ValueError, TypeError, AttributeError) as error:
        raise MaintenanceError("invalid_instance", "实例 ID 无效，应为小写 UUID") from error
    return identifier, _path(library_root) / "instances" / identifier


def _owned_dir(path):
    """Narrow an owned directory's mode; None when absent."""
    try:
        info = os.lstat(path)
    except FileNotFoundError:
        return None
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
        raise MaintenanceError("unsafe_instance", "实例目录不是本用户的普通文件夹，未做任何修改：" + str(path)[:160])
    if stat.S_IMODE(info.st_mode) != 0o700:
        os.chmod(path, 0o700, follow_symlinks=False)
    return info


def _read_json(path):
    try:
        info = os.lstat(path)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_size > 65536:
            return None
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        try:
            value = _decode(os.read(fd, 65537))
        finally:
            os.close(fd)
        return value if isinstance(value, dict) else None
    except (OSError, ValueError):
        return None


def _committed(folder):
    manifest = _read_json(folder / "revision.json")
    return manifest is not None and isinstance(manifest.get("dataSHA256"), str)


def _head_name(storage):
    head = _read_json(storage / "head.json")
    name = head.get("revision") if head else None
    return name if isinstance(name, str) and REVISION.fullmatch(name) else None


def _sync(path):
    try:
        fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    except OSError:
        pass


def _settle_pending(storage, removed):
    """Roll an interrupted transaction back to the last committed snapshot."""
    pending = _read_json(storage / "pending.json")
    revision = pending.get("revision") if pending else None
    head = _head_name(storage)
    if isinstance(revision, str) and REVISION.fullmatch(revision) and revision != head:
        # HEAD never points at it, so it holds no committed data. If it cannot
        # be removed it stays as an orphan; the start must not be blocked.
        try:
            _remove_if_present(storage / "revisions", revision, removed, "storage/revisions/" + revision)
        except OSError:
            pass
    _remove_if_present(storage, "pending.json", removed, "storage/pending.json")
    _sync(storage)


def _quiet_remove(parent, name, removed, label):
    """Best-effort removal of a leftover; a failure never blocks a start."""
    try:
        return _remove_if_present(parent, name, removed, label)
    except OSError:
        return False


def _stale(path, seconds=120):
    """A temporary file no writer can still be publishing."""
    try:
        return time.time() - os.lstat(path).st_mtime > seconds
    except OSError:
        return False


def _sweep(instance, storage, removed):
    """Remove temporary files and uncommitted revisions left by a crash."""
    for folder, prefix, label in ((instance, (".index-", ".json-"), ""),
                                  (storage, (".json-",), "storage/")):
        try:
            names = os.listdir(folder)
        except FileNotFoundError:
            continue
        for name in names:
            if name.startswith(prefix) and _stale(folder / name):
                _quiet_remove(folder, name, removed, label + name)
    revisions = storage / "revisions"
    try:
        names = sorted(os.listdir(revisions))
    except FileNotFoundError:
        return
    head = _head_name(storage)
    for name in names:
        folder = revisions / name
        if not REVISION.fullmatch(name):
            continue
        try:
            inner = os.listdir(folder)
        except OSError:
            continue
        for item in inner:
            if item.startswith((".accepted-", ".json-")):
                _quiet_remove(folder, item, removed, "storage/revisions/" + name + "/" + item)
        # Only a revision HEAD does not name and that never finished a commit.
        # Without a readable HEAD nothing is classified as abandoned.
        if head is not None and name != head and not _committed(folder):
            _quiet_remove(revisions, name, removed, "storage/revisions/" + name)


def _repair_head(store, storage):
    """Point a broken HEAD at the newest intact committed snapshot."""
    from .instance_storage import Firmware, _private
    try:
        store.head()
        return None
    except (OSError, ValueError, KeyError, TypeError):
        pass
    best = None
    for name in os.listdir(storage / "revisions"):
        folder = storage / "revisions" / name
        if not REVISION.fullmatch(name) or not _committed(folder):
            continue
        try:
            manifest = _read_json(folder / "revision.json")
            firmware = Firmware(**manifest["firmware"])
            firmware.validate()
            if firmware.model != store.info["model"]:
                continue
            for image, size, _, _ in firmware.images:
                _private(folder / image, size=size)
            stamp = os.lstat(folder / "revision.json").st_mtime
        except (OSError, ValueError, KeyError, TypeError):
            continue
        if best is None or stamp > best[0]:
            best = (stamp, name)
    if best is None:
        return None
    from .instance_storage import _atomic_json
    if (storage / "head.json").exists():
        try:
            os.rename(storage / "head.json", storage / ("head.json.broken-" + time.strftime("%Y%m%d-%H%M%S")))
        except OSError:
            pass
    _atomic_json(storage / "head.json", {"revision": best[1]})
    return best[1]


def reset_instance(library_root, instance_id, *, finish_sessions=True):
    """Settle one instance so it can start; never touches committed data."""
    identifier, instance = _instance_dir(library_root, instance_id)
    result = {"ok": True, "version": 1, "instanceID": identifier, "recovered": False, "removed": []}
    removed = result["removed"]
    if _owned_dir(instance) is None:
        return result
    storage = instance / "storage"
    if _owned_dir(storage) is None:
        _sweep(instance, storage, removed)
        return result
    pending = _read_json(storage / "pending.json")
    if (storage / "pending.json").exists() or os.path.islink(storage / "pending.json"):
        session = pending.get("sessionRoot") if pending else None
        if isinstance(session, str) and session.startswith("/") and finish_sessions:
            state, _ = session_state(os.path.dirname(session))
            if state == "running":
                result.update(active=True, message="此实例仍在运行，未改动它的数据")
                return result
            try:
                from .instance_storage import finish_session_storage
                receipt = finish_session_storage(library_root, Path(session), identifier)
                result.update(recovered=True, committed=bool(receipt.get("committed")))
            except Exception:
                pass  # Fall back to rolling back below.
        if (storage / "pending.json").exists() or os.path.islink(storage / "pending.json"):
            store = _open_store(storage)
            try:
                _settle_pending(storage, removed)
                result.update(recovered=True, committed=False)
            finally:
                if store is not None:
                    store.close()
            if isinstance(session, str) and session.startswith("/"):
                _leave_receipt(Path(session), identifier, pending)
    store = _open_store(storage)
    try:
        _sweep(instance, storage, removed)
        if store is not None:
            repaired = _repair_head(store, storage)
            if repaired:
                result.update(recovered=True, headRepaired=repaired)
    finally:
        if store is not None:
            store.close()
    return result


def _leave_receipt(session, instance_id, pending):
    """Best effort: mark a rolled-back session finished so cleanup may remove it."""
    try:
        if not os.path.isdir(session) or os.path.islink(session) or os.path.lexists(session / "instance-storage-result.json"):
            return
        from .instance_storage import _atomic_json
        _atomic_json(session / "instance-storage-result.json", {
            "ok": True, "version": 1, "instanceID": instance_id,
            "transactionID": (pending or {}).get("revision"), "committed": False,
            "previousRevision": (pending or {}).get("previous"), "revision": (pending or {}).get("previous"),
            "consistency": "previous-snapshot-preserved", "guestFilesystemClean": False,
            "reason": "rolled-back-by-instance-reset"})
    except (OSError, ValueError):
        pass


def _open_store(storage):
    """Hold the instance lock; None when the store has no metadata yet."""
    from .instance_storage import InstanceStorage
    if not (storage / "instance.json").exists():
        return None
    try:
        return InstanceStorage(storage)
    except BlockingIOError as error:
        raise MaintenanceError("instance_busy", "此实例正被另一个操作使用，请稍后重试") from error
    except (OSError, ValueError, KeyError, TypeError):
        return None


# -------------------------------------------------------- sessions cleanup

def _protected_paths():
    protected = set()
    for value in (os.getcwd(), os.environ.get("TMPDIR", ""), os.environ.get("AZKI_SESSION_ROOT", "")):
        if value:
            protected.add(os.path.realpath(value).rstrip("/"))
    return protected


def _settle_session(root, library_root):
    """None when the session may go; otherwise why it must stay for now.

    A stopped run whose instance transaction is still open keeps its working
    disks until instance-reset commits or rolls it back (that reset leaves a
    receipt here, so the next cleanup removes the folder).
    """
    session = Path(root) / "session"
    binding = _read_json(session / "instance-storage.json")
    if binding is None or (session / "instance-storage-result.json").exists():
        return None
    instance_id = binding.get("instanceID")
    if not isinstance(instance_id, str):
        return None
    if library_root is None:
        return "awaiting-instance-reset"
    try:
        _, instance = _instance_dir(library_root, instance_id)
        pending_path = instance / "storage" / "pending.json"
        if not os.path.lexists(pending_path):
            return None
        pending = _read_json(pending_path)
    except (MaintenanceError, OSError):
        return "awaiting-instance-reset"
    if pending is None:
        return "awaiting-instance-reset"
    if pending.get("sessionRoot") != str(session):
        return None
    try:
        reset_instance(library_root, instance_id)
    except Exception:
        return "awaiting-instance-reset"
    return None if (session / "instance-storage-result.json").exists() or not os.path.lexists(pending_path) \
        else "awaiting-instance-reset"


# Files a finished helper command may leave in its azki-cmd- folder.
CMD_LEFTOVERS = {"native-p256.bin"}


def _cmd_leftovers_only(path):
    try:
        with os.scandir(path) as entries:
            return all(entry.name in CMD_LEFTOVERS and entry.is_file(follow_symlinks=False) for entry in entries)
    except OSError:
        return False


def cleanup_sessions(library_root=None, tmp_root=TMP_ROOT, *, now=None):
    tmp_root = _path(tmp_root)
    now = time.time() if now is None else now
    result = {"ok": True, "version": 1, "removed": [], "kept": [], "reasons": {}}
    protected = _protected_paths()
    try:
        entries = sorted(os.listdir(tmp_root))
    except OSError as error:
        raise MaintenanceError("tmp_unavailable", "无法读取临时目录 " + str(tmp_root) + "：" + error.strerror) from error
    for name in entries:
        if not name.startswith(SESSION_PREFIXES) or "/" in name:
            continue
        path = str(tmp_root / name)
        try:
            info = os.lstat(path)
        except FileNotFoundError:
            continue
        # Links and other users' entries are never ours to judge or remove.
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
            continue
        def keep(reason):
            result["kept"].append(path)
            result["reasons"][path] = reason
        real = os.path.realpath(path)
        if any(item == real or item.startswith(real + "/") for item in protected):
            keep("in-use-by-this-command")
            continue
        state, newest = session_state(path)
        if state == "running":
            keep("running")
            continue
        if state == "unknown" and now - newest < GRACE_SECONDS:
            keep("recent")
            continue
        if name.startswith("azki-cmd-") and not _cmd_leftovers_only(path):
            # Unknown interpreter-created files: possibly an in-use helper.
            keep("unknown-helper-files")
            continue
        try:
            waiting = _settle_session(path, library_root)
        except Exception as error:  # Never lose an uncommitted run silently.
            waiting = "instance-settle-failed: " + str(error)[:160]
        if waiting:
            keep(waiting)
            continue
        try:
            remove_tree(str(tmp_root), name)
        except FileNotFoundError:
            pass
        except OSError as error:
            keep("remove-failed: " + (error.strerror or str(error))[:160])
            continue
        result["removed"].append(path)
    return result


def remove_instance_sessions(instance_id, tmp_root=TMP_ROOT):
    """Remove this app's session roots that still hold one deleted instance.

    The instance record and its library folder are already gone when this runs.
    A running session is always kept. A recent unknown session is kept because
    it may still be preparing; an older one cannot be reset and would otherwise
    leak its working images forever. Removal remains bounded to this user's own
    azki-b-/azki-sim- roots.
    """
    try:
        from .instance_storage import _uuid
        identifier = _uuid(instance_id)
    except (ValueError, TypeError, AttributeError) as error:
        raise MaintenanceError("invalid_instance", "实例 ID 无效，应为小写 UUID") from error
    tmp_root = _path(tmp_root)
    result = {"ok": True, "version": 1, "removed": [], "kept": [], "reasons": {}}
    try:
        names = sorted(os.listdir(tmp_root))
    except OSError as error:
        raise MaintenanceError("tmp_unavailable", "无法读取临时目录 " + str(tmp_root) + "：" + error.strerror) from error
    for name in names:
        if not name.startswith(("azki-b-", "azki-sim-")) or "/" in name:
            continue
        path = tmp_root / name
        try:
            info = os.lstat(path)
        except OSError:
            continue
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
            continue
        session = path / "session"
        binding = _read_json(session / "instance-storage.json")
        if not isinstance(binding, dict) or binding.get("instanceID") != identifier:
            continue
        state, newest = session_state(path)
        if state == "running":
            result["kept"].append(str(path)); result["reasons"][str(path)] = "running"
            continue
        if (state == "unknown" and not (session / "instance-storage-result.json").exists()
                and time.time() - newest < GRACE_SECONDS):
            # A just-prepared session may not have written its first process
            # record yet. Older unknown roots cannot be reset: the instance is
            # gone, so keeping them would leak their working images forever.
            result["kept"].append(str(path)); result["reasons"][str(path)] = "recent-unknown-session"
            continue
        try:
            remove_tree(str(tmp_root), name)
        except FileNotFoundError:
            pass
        except OSError as error:
            result["kept"].append(str(path))
            result["reasons"][str(path)] = "remove-failed: " + (error.strerror or str(error))[:160]
            continue
        result["removed"].append(str(path))
    return result
