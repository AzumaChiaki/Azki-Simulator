#!/usr/bin/env python3
"""Audit and reclaim simulator NAND images in research libraries and evidence.

The app's Storage settings covers only the app's own library. Azki Music
workspaces also contain many independently-created library roots and copied
evidence. This tool applies the same APFS copy-on-write replacement to exact
duplicates, without touching a live guest's working session.

Examples (dry-run by default):

    python3.12 scripts/simulator-storage.py report \
        --root "/Volumes/StudioData/Azki Music/workspaces" --hash
    python3.12 scripts/simulator-storage.py maintain \
        --root "/Volumes/StudioData/Azki Music/workspaces" --keep 2
    python3.12 scripts/simulator-storage.py dedupe \
        --root "/Volumes/StudioData/Azki Music/workspaces" --apply

A duplicate is only replaced after the full SHA-256 matches. Files in a
session/instance-data/prepared path and files modified in the last 30 minutes
are never replaced. `maintain` skips an instance whose storage lock is held.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "Sources/AzkiSimulator/Runtime"))

from azki_simulator.storage_maintenance import maintain as maintain_library  # noqa: E402

try:
    _libc = ctypes.CDLL(None, use_errno=True)
    _clonefile = _libc.clonefile
    _clonefile.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int)
    _clonefile.restype = ctypes.c_int
except (AttributeError, OSError):
    _clonefile = None

MASK = ("/session/", "/instance-data/", "/prepared/", "/host-tmp/")
SKIP_DIRS = {".git", "node_modules", "__pycache__", ".build", "dist", "build", ".Trash"}
IMAGE_NAMES = {"nand-data.bin", "nand-spare.bin", "nor-config.bin"}
UUID = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\Z")


def now():
    return time.time()


def digest(path, limit=1024 * 1024 * 1024):
    value = hashlib.sha256()
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        total = 0
        while chunk := os.read(fd, 4 * 1024 * 1024):
            total += len(chunk)
            if total > limit:
                raise ValueError("file exceeds the image size limit")
            value.update(chunk)
    finally:
        os.close(fd)
    return value.hexdigest()


def clone(source, target):
    if _clonefile is None:
        return False
    if _clonefile(os.fsencode(source), os.fsencode(target), 0) != 0:
        return False
    return True


def replace_with_clone(source, target):
    info = target.stat()
    temporary = target.parent / (".dedupe-" + uuid.uuid4().hex)
    try:
        if not clone(source, temporary):
            raise OSError("此卷不支持 APFS 克隆（或源/目标不在同一卷）")
        os.chmod(temporary, stat.S_IMODE(info.st_mode))
        os.utime(temporary, ns=(info.st_atime_ns, info.st_mtime_ns))
        os.replace(temporary, target)
        directory = os.open(target.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try: os.fsync(directory)
        finally: os.close(directory)
    finally:
        try: temporary.unlink()
        except OSError: pass


def classify(path):
    text = "/" + str(path).replace(os.sep, "/") + "/"
    if "/storage/revisions/" in text:
        return "revision"
    for marker in MASK:
        if marker in text:
            return "session"
    return "evidence"


def scan(root, *, min_age_minutes=0):
    root = Path(root).resolve()
    result = []
    cutoff = now() - min_age_minutes * 60
    for directory, names, files in os.walk(root, followlinks=False):
        names[:] = [name for name in names if name not in SKIP_DIRS]
        current = Path(directory)
        if any(marker.strip("/") in current.parts for marker in MASK):
            names[:] = []
            continue
        for name in files:
            if name not in IMAGE_NAMES:
                continue
            path = current / name
            try:
                info = path.lstat()
            except OSError:
                continue
            if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid()
                    or info.st_nlink != 1 or info.st_size <= 0):
                continue
            if info.st_mtime > cutoff:
                continue
            result.append({"path": str(path), "bytes": info.st_size,
                           "kind": classify(path), "mtime": info.st_mtime})
    return result


def find_libraries(root):
    found = set()
    root = Path(root).resolve()
    for directory, names, files in os.walk(root, followlinks=False):
        names[:] = [name for name in names if name not in SKIP_DIRS]
        if Path(directory).name != "instances":
            continue
        try:
            entries = os.listdir(directory)
        except OSError:
            continue
        for name in entries:
            if not UUID.fullmatch(name):
                continue
            storage = Path(directory) / name / "storage"
            if (storage / "instance.json").is_file():
                found.add(Path(directory).parent)
                break
    return sorted(found)


def grouped(images, min_age_minutes):
    """Exact duplicate groups keyed by (size, sha256)."""
    by_size = {}
    cutoff = now() - min_age_minutes * 60
    for item in images:
        if item["kind"] == "session":
            continue
        if item["mtime"] > cutoff:
            continue
        by_size.setdefault(item["bytes"], []).append(item)
    groups = {}
    for size, items in by_size.items():
        if len(items) < 2:
            continue
        for item in items:
            try:
                item["sha256"] = digest(Path(item["path"]))
            except (OSError, ValueError):
                continue
        buckets = {}
        for item in items:
            key = item.get("sha256")
            if key:
                buckets.setdefault((size, key), []).append(item)
        groups.update({key: value for key, value in buckets.items() if len(value) > 1})
    return groups


def report_command(args):
    images = []
    for root in args.root:
        images.extend(scan(root, min_age_minutes=args.min_age_minutes))
    kinds = {}
    for item in images:
        bucket = kinds.setdefault(item["kind"], {"files": 0, "bytes": 0})
        bucket["files"] += 1
        bucket["bytes"] += item["bytes"]
    value = {"ok": True, "version": 1, "roots": [str(Path(root).resolve()) for root in args.root],
             "images": len(images), "bytes": sum(item["bytes"] for item in images),
             "kinds": kinds, "libraries": [str(path) for root in args.root for path in find_libraries(root)]}
    if args.hash:
        groups = grouped(images, args.min_age_minutes)
        duplicate = sum((len(group) - 1) * key[0] for key, group in groups.items())
        value.update(duplicateGroups=len(groups), duplicateBytes=duplicate,
                     duplicateFiles=sum(len(group) - 1 for group in groups.values()),
                     groups=[{"size": key[0], "sha256": key[1], "count": len(group),
                              "bytes": (len(group) - 1) * key[0],
                              "paths": [item["path"] for item in group]}
                             for key, group in list(groups.items())[:200]])
    print(json.dumps(value, ensure_ascii=False, indent=2))


def maintain_command(args):
    libraries = set()
    for root in args.root:
        root = Path(root).resolve()
        libraries.update(find_libraries(root))
    results = []
    for library in sorted(libraries):
        try:
            result = maintain_library(library, keep=args.keep, prune=not args.no_prune,
                                      dedupe=True, clean_sessions=False,
                                      dry_run=not args.apply)
            results.append({"library": str(library), "result": result})
        except Exception as error:
            results.append({"library": str(library), "error": type(error).__name__ + ": " + str(error)[:400]})
    print(json.dumps({"ok": True, "version": 1, "apply": args.apply, "keep": args.keep,
                      "libraries": results}, ensure_ascii=False, indent=2))


def dedupe_command(args):
    try:
        usage = shutil.disk_usage(Path(args.root[0]).resolve())
    except OSError:
        usage = shutil._ntuple_diskusage(0, 0, 0)
    images = []
    for root in args.root:
        images.extend(scan(root, min_age_minutes=args.min_age_minutes))
    groups = grouped(images, args.min_age_minutes)
    stats = {"ok": True, "version": 1, "apply": args.apply, "groups": len(groups),
             "duplicateFiles": sum(len(group) - 1 for group in groups.values()),
             "duplicateBytes": sum((len(group) - 1) * key[0] for key, group in groups.items()),
             "cloned": 0, "errors": [], "beforeFreeBytes": usage.free}
    log = open(args.log, "a", encoding="utf-8") if args.log else None
    try:
        for key, group in sorted(groups.items(), key=lambda item: item[1][0]["path"]):
            # Keep the newest copy; replacing an older identical copy preserves
            # its own mode and mtime, so evidence tools see the same file.
            group.sort(key=lambda item: (-item["mtime"], item["path"]))
            canonical = group[0]
            for item in group[1:]:
                if not args.apply:
                    continue
                try:
                    replace_with_clone(canonical["path"], Path(item["path"]))
                    stats["cloned"] += 1
                    if log:
                        log.write(json.dumps({"kept": canonical["path"], "replaced": item["path"],
                                              "bytes": item["bytes"], "sha256": key[1]}) + "\n")
                except OSError as error:
                    stats["errors"].append({"path": item["path"], "error": str(error)[:300]})
    finally:
        if log:
            log.close()
    try:
        after = shutil.disk_usage(Path(args.root[0]).resolve())
    except OSError:
        after = shutil._ntuple_diskusage(0, 0, 0)
    stats["afterFreeBytes"] = after.free
    stats["freedBytes"] = after.free - usage.free
    print(json.dumps(stats, ensure_ascii=False, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="action", required=True)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--root", action="append", required=True,
                        help="Directory to scan; repeat for more roots")
    common.add_argument("--min-age-minutes", type=int, default=30,
                        help="Never touch files modified more recently than this (default 30)")
    rep = sub.add_parser("report", parents=[common])
    rep.add_argument("--hash", action="store_true", help="Read all candidate images to find exact duplicates")
    rep.set_defaults(func=report_command)
    man = sub.add_parser("maintain", parents=[common])
    man.add_argument("--keep", type=int, required=True, help="Committed snapshots to keep per instance")
    man.add_argument("--no-prune", action="store_true")
    man.add_argument("--apply", action="store_true", help="Apply; default is a read-only preview")
    man.set_defaults(func=maintain_command)
    ded = sub.add_parser("dedupe", parents=[common])
    ded.add_argument("--apply", action="store_true", help="Replace duplicates with APFS clones")
    ded.add_argument("--log", help="Append one JSON line per replaced file")
    ded.set_defaults(func=dedupe_command)
    args = parser.parse_args()
    if getattr(args, "min_age_minutes", 0) < 0:
        parser.error("--min-age-minutes must be non-negative")
    if args.action == "maintain" and not 1 <= args.keep <= 8:
        parser.error("--keep must be 1..8")
    args.func(args)


if __name__ == "__main__":
    main()
