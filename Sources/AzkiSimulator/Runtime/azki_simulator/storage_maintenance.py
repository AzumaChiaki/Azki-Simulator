"""Storage accounting and safe reclamation for an instance library.

The simulator stores each accepted NAND snapshot as an exact-size image pair.
Before APFS clones, every run also wrote a second complete pair for the guest
session and every new revision wrote a third. A library could therefore hold
several 384 MiB copies of the same mostly-erased flash. This module reports
that cost and reclaims the parts that are provably identical:

* byte-identical revision images are replaced by APFS clones of one file;
* a completely erased image is replaced by a clone of one shared 0xFF template;
* only snapshots outside the requested HEAD-chain retention are pruned;
* stopped temporary sessions are removed by the existing bounded cleaner.

Everything is opt-in except the launch-time "auto" pass, which never prunes a
committed revision: it only deduplicates identical bytes, shares erased images
and removes already-finished temporary sessions. A live instance (its storage
lock is held) is always skipped.
"""
from __future__ import annotations

import fcntl
import hashlib
import os
from pathlib import Path
import re
import shutil
import stat
import uuid

from .instance_storage import (InstanceStorage, MAX_REVISIONS, _clone_file, _erased_sample_ok,
                               _sync_dir, erased_sha256, erased_template, clonefile_supported)
from .maintenance import (REVISION, _committed, _head_name, _read_json, cleanup_sessions,
                          remove_tree, reset_instance)

DEFAULT_KEEP = 4
SESSION_PREFIXES = ("azki-b-", "azki-sim-")
IMAGE_KEYS = {"nand-data.bin": ("dataSHA256", "data_bytes"),
              "nand-spare.bin": ("spareSHA256", None),
              "nor-config.bin": ("norSHA256", "nor_bytes")}
MAX_TREE_FILES = 200_000
HEX64 = re.compile(r"[0-9a-f]{64}\Z")


class StorageError(ValueError):
    code = "storage_error"


def _library(library_root):
    root = Path(library_root)
    if not root.is_absolute():
        root = root.absolute()
    info = root.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
        raise StorageError("固件库目录不是当前用户的普通目录，未做任何修改")
    return root


def _instances(library_root):
    folder = library_root / "instances"
    try:
        entries = sorted(os.listdir(folder))
    except FileNotFoundError:
        return []
    result = []
    for name in entries:
        try:
            identifier = str(uuid.UUID(name))
        except (ValueError, AttributeError):
            continue
        if identifier != name:
            continue
        path = folder / name
        try:
            info = path.lstat()
        except OSError:
            continue
        if stat.S_ISDIR(info.st_mode) and not stat.S_ISLNK(info.st_mode) and info.st_uid == os.getuid():
            result.append((name, path))
    return result


def _tree_size(path, *, max_files=MAX_TREE_FILES):
    """(logical bytes, files, newest mtime, unsafe) without following links."""
    total = files = 0
    newest = 0.0
    unsafe = False
    stack = [Path(path)]
    while stack:
        current = stack.pop()
        try:
            info = current.lstat()
            if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
                unsafe = True
                continue
            newest = max(newest, info.st_mtime)
            with os.scandir(current) as entries:
                for entry in entries:
                    if files >= max_files:
                        return total, files, newest, True
                    try:
                        item = entry.stat(follow_symlinks=False)
                    except OSError:
                        unsafe = True
                        continue
                    files += 1
                    newest = max(newest, item.st_mtime)
                    if stat.S_ISDIR(item.st_mode):
                        stack.append(Path(entry.path))
                    elif stat.S_ISREG(item.st_mode):
                        total += item.st_size
                    elif stat.S_ISLNK(item.st_mode):
                        # Links are not data and are never traversed.
                        unsafe = True
        except OSError:
            unsafe = True
    return total, files, newest, unsafe


def _lock_state(storage, *, readonly=False):
    path = storage / ".lock"
    try:
        fd = os.open(path, (os.O_RDONLY if readonly else os.O_RDWR) | os.O_NOFOLLOW | os.O_CLOEXEC)
    except OSError:
        return "missing"
    try:
        try:
            fcntl.flock(fd, (fcntl.LOCK_SH if readonly else fcntl.LOCK_EX) | fcntl.LOCK_NB)
        except BlockingIOError:
            return "busy"
        except OSError:
            return "unknown"
        fcntl.flock(fd, fcntl.LOCK_UN)
        return "idle"
    finally:
        os.close(fd)


def _chain(storage):
    names = _revision_names(storage)
    head = _head_name(storage)
    chain, cursor = [], head
    while isinstance(cursor, str) and cursor in names and cursor not in chain:
        chain.append(cursor)
        manifest = _read_json(storage / "revisions" / cursor / "revision.json") or {}
        cursor = manifest.get("previous")
    return chain


def _revision_names(storage):
    folder = storage / "revisions"
    try:
        names = sorted(os.listdir(folder))
    except FileNotFoundError:
        return []
    return [name for name in names if REVISION.fullmatch(name)]


def _revision_images(revision, manifest):
    """(name, path, size, digest) for every exact image this revision owns."""
    firmware = manifest.get("firmware") if isinstance(manifest, dict) else None
    if not isinstance(firmware, dict):
        return []
    result = []
    data_bytes = firmware.get("data_bytes")
    if type(data_bytes) is not int or not 0 < data_bytes <= 512 * 1024 * 1024:
        return []
    for name, (digest_key, size_key) in IMAGE_KEYS.items():
        if name == "nor-config.bin" and not firmware.get("nor_bytes"):
            continue
        size = firmware.get("nor_bytes") if size_key == "nor_bytes" else (
            data_bytes if name == "nand-data.bin" else data_bytes // 2048 * 64)
        if type(size) is not int or size <= 0:
            continue
        path = revision / name
        try:
            info = path.lstat()
        except OSError:
            continue
        if not stat.S_ISREG(info.st_mode) or info.st_size != size:
            continue
        digest = manifest.get(digest_key)
        if not isinstance(digest, str) or not HEX64.fullmatch(digest):
            digest = None
        result.append((name, path, size, digest))
    return result


def _duplicate_groups(records):
    """records: (name, path, size, digest) -> groups with more than one file."""
    groups = {}
    for record in records:
        name, path, size, digest = record
        if digest is None:
            continue
        groups.setdefault((name, size, digest), []).append(record)
    return {key: value for key, value in groups.items() if len(value) > 1}


def _sha256_file(path):
    digest = hashlib.sha256()
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        while chunk := os.read(fd, 4 * 1024 * 1024):
            digest.update(chunk)
    finally:
        os.close(fd)
    return digest.hexdigest()


def _trusted_digest(path, manifest_path, expected, size):
    """Use the recorded digest while the image is older than its manifest."""
    if not isinstance(expected, str) or not HEX64.fullmatch(expected):
        return None
    try:
        if path.stat().st_size != size:
            return None
        if path.stat().st_mtime_ns <= manifest_path.stat().st_mtime_ns:
            return expected
    except OSError:
        return None
    actual = _sha256_file(path)
    return actual if actual == expected else None


def _sessions(tmp_root, owned_ids):
    """Map instance id -> [{path, bytes, state, instanceID}] for stopped copies."""
    result = {identifier: [] for identifier in owned_ids}
    try:
        names = sorted(os.listdir(tmp_root))
    except OSError:
        return result
    from .maintenance import session_state
    for name in names:
        if not name.startswith(SESSION_PREFIXES) or "/" in name:
            continue
        path = Path(tmp_root) / name
        try:
            info = path.lstat()
        except OSError:
            continue
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
            continue
        session = path / "session"
        binding = _read_json(session / "instance-storage.json")
        if not isinstance(binding, dict):
            continue
        identifier = binding.get("instanceID")
        if identifier not in result:
            continue
        size, files, newest, unsafe = _tree_size(session / "instance-data")
        state, _ = session_state(path)
        result[identifier].append({"path": str(path), "bytes": size, "files": files,
                                   "state": state, "unsafe": unsafe})
    return result


def _instance_report(library_root, identifier, folder, sessions_for_id):
    storage = folder / "storage"
    model = None
    entry = {"id": identifier, "model": None, "bytes": 0, "revisionBytes": 0,
             "revisionCount": 0, "committedCount": 0, "head": None,
             "prunable2Bytes": 0, "prunable4Bytes": 0, "duplicateBytes": 0,
             "erasedBytes": 0, "sessionsBytes": 0, "sessions": sessions_for_id,
             "busy": False, "pending": False, "unsafe": False}
    total, _, newest, unsafe = _tree_size(folder)
    entry["bytes"] = total
    info = _read_json(storage / "instance.json")
    if isinstance(info, dict) and isinstance(info.get("model"), str):
        model = info["model"]
        entry["model"] = model
    if not storage.is_dir():
        return entry
    entry["busy"] = (_lock_state(storage, readonly=True) == "busy"
                     or any(item["state"] == "running" for item in sessions_for_id))
    entry["pending"] = (storage / "pending.json").exists()
    head = _head_name(storage)
    entry["head"] = head
    chain = _chain(storage)
    records = []
    names = _revision_names(storage)
    revision_bytes = 0
    for name in names:
        folder_path = storage / "revisions" / name
        size, _, _, bad = _tree_size(folder_path)
        revision_bytes += size
        entry["unsafe"] |= bad
        manifest = _read_json(folder_path / "revision.json") or {}
        if _committed(folder_path):
            entry["committedCount"] += 1
        records.extend(_revision_images(folder_path, manifest))
    entry["revisionBytes"] = revision_bytes
    entry["revisionCount"] = len(names)
    for keep, key in ((2, "prunable2Bytes"), (4, "prunable4Bytes")):
        keep_set = set(chain[:keep])
        entry[key] = sum(_tree_size(storage / "revisions" / name)[0]
                         for name in names if name not in keep_set)
    duplicate = 0
    erased = 0
    for (_, size, digest), group in _duplicate_groups(records).items():
        duplicate += (len(group) - 1) * size
    for name, path, size, digest in records:
        if digest == erased_sha256(size):
            erased += size
    entry["duplicateBytes"] = duplicate
    entry["erasedBytes"] = erased
    entry["sessionsBytes"] = sum(item["bytes"] for item in sessions_for_id)
    entry["unsafe"] |= unsafe
    entry["lastUsed"] = newest
    return entry


def report(library_root, *, tmp_root=Path("/private/tmp")):
    root = _library(library_root)
    instances = _instances(root)
    session_map = _sessions(Path(tmp_root), {identifier for identifier, _ in instances})
    entries = [_instance_report(root, identifier, folder, session_map.get(identifier, []))
               for identifier, folder in instances]
    total, _, _, unsafe = _tree_size(root)
    instances_bytes = sum(entry["bytes"] for entry in entries)
    firmware_bytes = _tree_size(root / "objects")[0] + (
        (root / "index.json").stat().st_size if (root / "index.json").is_file() else 0)
    cache_bytes = _tree_size(root / ".storage")[0]
    sessions_bytes = sum(item["bytes"] for entry in entries for item in entry["sessions"])
    other = max(0, total - instances_bytes - firmware_bytes - cache_bytes)
    try:
        usage = shutil.disk_usage(root)
    except OSError:
        # A deny-default helper may not be granted statfs for the volume; the
        # library sizes remain valid even when free space is unavailable.
        usage = shutil._ntuple_diskusage(0, 0, 0)
    prunable = sum(entry["prunable4Bytes"] for entry in entries)
    duplicate = sum(entry["duplicateBytes"] for entry in entries)
    stopped_sessions = sum(item["bytes"] for entry in entries for item in entry["sessions"]
                           if item["state"] != "running")
    erased = sum(entry["erasedBytes"] for entry in entries)
    return {
        "ok": True, "version": 1, "root": str(root),
        "totalBytes": total + sessions_bytes,
        "libraryBytes": total,
        "sessionsBytes": sessions_bytes,
        "freeBytes": usage.free, "volumeBytes": usage.total,
        "unsafe": unsafe or any(entry["unsafe"] for entry in entries),
        "categories": [
            {"id": "instances", "name": "实例与快照", "bytes": instances_bytes},
            {"id": "firmware", "name": "固件库", "bytes": firmware_bytes},
            {"id": "sessions", "name": "临时会话", "bytes": sessions_bytes},
            {"id": "cache", "name": "共享空白镜像", "bytes": cache_bytes},
            {"id": "other", "name": "其他", "bytes": other},
        ],
        "instances": entries,
        "reclaimableBytes": prunable + duplicate + stopped_sessions,
        "prunableBytes": prunable,
        "duplicateBytes": duplicate,
        "stoppedSessionBytes": stopped_sessions,
        "erasedBytes": erased,
        "retention": {"suggested": DEFAULT_KEEP, "maximum": MAX_REVISIONS, "minimum": 1},
    }


def _replace_with_clone(source, target, expected_size):
    """Atomically replace target with an APFS clone of source, keeping its time."""
    info = target.stat()
    if info.st_size != expected_size:
        raise StorageError("待替换镜像大小与记录不一致")
    temporary = target.parent / (".dedupe-" + uuid.uuid4().hex)
    try:
        if not _clone_file(source, temporary):
            raise StorageError("此卷不支持 APFS 克隆")
        os.chmod(temporary, 0o600)
        os.utime(temporary, ns=(info.st_atime_ns, info.st_mtime_ns))
        os.replace(temporary, target)
        _sync_dir(target.parent)
    finally:
        try: temporary.unlink()
        except OSError: pass


def _maintain_instance(root, identifier, storage, *, keep, prune, dedupe, dry_run, stats):
    root = Path(root)
    if (storage / "pending.json").exists():
        # A run is either still live (its session images are outside this
        # lock) or was not settled by reset_instance. Never rewrite/prune the
        # committed history that its rollback may still name.
        stats["skipped"].append({"instanceID": identifier, "reason": "unsettled-run"})
        return
    if _lock_state(storage, readonly=dry_run) == "busy":
        stats["skipped"].append({"instanceID": identifier, "reason": "running"})
        return
    store = None
    try:
        if dry_run:
            head_name = _head_name(storage)
            if head_name is None:
                stats["skipped"].append({"instanceID": identifier, "reason": "no-committed-head"})
                return
        else:
            store = InstanceStorage(storage)
            head = store.head()
            if head is None:
                stats["skipped"].append({"instanceID": identifier, "reason": "no-committed-head"})
                return
            head_name = head[0].name
        names = _revision_names(storage)
        chain = _chain(storage)
        pending = _read_json(storage / "pending.json") or {}
        protected = {pending.get("revision")} if isinstance(pending.get("revision"), str) else set()
        keep_set = set(chain[:max(1, keep)]) | {name for name in protected if name in names}
        removable = [name for name in names if name not in keep_set]
        removed = []
        if prune:
            for name in removable:
                size = _tree_size(storage / "revisions" / name)[0]
                if dry_run:
                    removed.append(name)
                    stats["prunedBytes"] += size
                    continue
                try:
                    remove_tree(str(storage / "revisions"), name)
                    removed.append(name)
                    stats["prunedBytes"] += size
                except OSError as error:
                    stats["errors"].append({"instanceID": identifier, "revision": name,
                                            "error": type(error).__name__ + ": " + str(error)[:160]})
            if removed:
                stats["removedRevisions"].extend(
                    {"instanceID": identifier, "revision": name} for name in removed)
                names = [name for name in names if name not in set(removed)]
        if not dedupe:
            return
        records = []
        for name in names:
            folder = storage / "revisions" / name
            manifest = _read_json(folder / "revision.json") or {}
            records.extend(_revision_images(folder, manifest))
        groups = _duplicate_groups(records)
        for (image_name, size, digest), group in groups.items():
            # Prefer HEAD, then the newest file in the group.
            group.sort(key=lambda item: (item[1].parent.name != head_name,
                                         -item[1].stat().st_mtime))
            canonical = group[0]
            if _trusted_digest(canonical[1], canonical[1].parent / "revision.json",
                               digest, size) is None:
                stats["errors"].append({"instanceID": identifier, "image": image_name,
                                        "error": "canonical digest differs from its revision record"})
                continue
            source = canonical[1]
            erased = digest == erased_sha256(size)
            if erased and not dry_run and clonefile_supported(root):
                try:
                    template = erased_template(root, size)
                    if _erased_sample_ok(template, size):
                        source = template
                except (OSError, ValueError):
                    source = canonical[1]
            for _, path, _, _ in group[1:]:
                if _trusted_digest(path, path.parent / "revision.json", digest, size) is None:
                    stats["errors"].append({"instanceID": identifier, "image": image_name,
                                            "error": "duplicate digest differs from its revision record"})
                    continue
                stats["duplicateFiles"] += 1
                stats["duplicateBytes"] += size
                if dry_run:
                    continue
                try:
                    _replace_with_clone(source, path, size)
                    stats["clonedFiles"] += 1
                except (OSError, ValueError, StorageError) as error:
                    stats["errors"].append({"instanceID": identifier, "image": image_name,
                                            "error": type(error).__name__ + ": " + str(error)[:160]})
    except BlockingIOError:
        stats["skipped"].append({"instanceID": identifier, "reason": "running"})
    except (OSError, ValueError, KeyError, TypeError) as error:
        stats["errors"].append({"instanceID": identifier,
                                "error": type(error).__name__ + ": " + str(error)[:160]})
    finally:
        if store is not None:
            store.close()


def maintain(library_root, *, keep=DEFAULT_KEEP, prune=True, dedupe=True,
             clean_sessions=True, dry_run=False, instance_id=None, tmp_root=Path("/private/tmp")):
    root = _library(library_root)
    if type(keep) is not int or not 1 <= keep <= MAX_REVISIONS:
        raise StorageError("保留快照数必须在 1…%d 之间" % MAX_REVISIONS)
    if instance_id is not None:
        try:
            identifier = str(uuid.UUID(instance_id))
        except (ValueError, AttributeError) as error:
            raise StorageError("实例 ID 无效") from error
        candidates = [(identifier, root / "instances" / identifier)]
        if not candidates[0][1].is_dir():
            raise StorageError("实例数据目录不存在")
    else:
        candidates = _instances(root)
    try:
        usage = shutil.disk_usage(root)
    except OSError:
        usage = shutil._ntuple_diskusage(0, 0, 0)
    session_map = _sessions(Path(tmp_root), {identifier for identifier, _ in candidates})
    stopped_session_bytes = sum(item["bytes"] for items in session_map.values()
                                for item in items if item["state"] != "running")
    stats = {"ok": True, "version": 1, "dryRun": dry_run, "keep": keep,
             "removedRevisions": [], "prunedBytes": 0, "clonedFiles": 0,
             "duplicateFiles": 0, "duplicateBytes": 0, "skipped": [], "errors": [],
             "sessionsRemoved": [], "beforeFreeBytes": usage.free,
             "stoppedSessionBytes": stopped_session_bytes}
    for identifier, folder in candidates:
        storage = folder / "storage"
        if not storage.is_dir():
            continue
        if _lock_state(storage) == "busy":
            stats["skipped"].append({"instanceID": identifier, "reason": "running"})
            continue
        if not dry_run:
            try:
                settled = reset_instance(root, identifier, finish_sessions=True)
                if settled.get("active"):
                    stats["skipped"].append({"instanceID": identifier, "reason": "running"})
                    continue
            except Exception as error:
                stats["errors"].append({"instanceID": identifier,
                                        "error": type(error).__name__ + ": " + str(error)[:160]})
                continue
        _maintain_instance(root, identifier, storage, keep=keep, prune=prune,
                           dedupe=dedupe, dry_run=dry_run, stats=stats)
    if clean_sessions and instance_id is None and not dry_run:
        try:
            cleaned = cleanup_sessions(root, tmp_root)
            stats["sessionsRemoved"] = cleaned.get("removed", [])
        except Exception as error:
            stats["errors"].append({"sessions": type(error).__name__ + ": " + str(error)[:160]})
    if dry_run and clean_sessions and instance_id is None:
        # Report only: do not ask the cleaner to remove anything.
        stats["sessionsRemoved"] = [item["path"] for items in session_map.values()
                                    for item in items if item["state"] != "running"]
    try:
        after = shutil.disk_usage(root)
    except OSError:
        after = shutil._ntuple_diskusage(0, 0, 0)
    stats["afterFreeBytes"] = after.free
    stats["reclaimedBytes"] = (stats["prunedBytes"] + stats["duplicateBytes"]
                               + (stopped_session_bytes if clean_sessions else 0))
    stats["freedBytes"] = after.free - usage.free
    return stats
