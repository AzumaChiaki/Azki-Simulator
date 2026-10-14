"""Bounded, offline package inspection and staging. Never installs into a guest.

Only standard-library code runs. Archive members are never extracted, imported,
or executed. Formats follow AzkiDeck's RpkManifest, WatchFaceMWZ and MiWearMass
readers; a recognized BIN header is expressly not a full BIN validation.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import stat
import struct
import unicodedata
import uuid
import xml.etree.ElementTree as ET
import zlib


MAX_ARCHIVE = 128 * 1024 * 1024
MAX_EXPANDED = 256 * 1024 * 1024
MAX_ENTRY = 32 * 1024 * 1024
MAX_ENTRIES = 8192
MAX_RATIO = 200
MAX_STAGED_BYTES = 512 * 1024 * 1024
MAX_STAGES = 32
MARKERS = {"azkideck.edit-state.json", "azkideck.authoring-state.json",
           "azkideck.engineering-manifest.json"}


class PackageError(ValueError):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


def _require(condition, code, message):
    if not condition:
        raise PackageError(code, message)


def _read_source(path):
    """Open once, reject special files/symlinks, bound reads and detect mutation."""
    try:
        fd = os.open(os.fspath(path), os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        with os.fdopen(fd, "rb") as stream:
            before = os.fstat(stream.fileno())
            _require(stat.S_ISREG(before.st_mode), "source_type", "Source must be a regular file")
            _require(0 < before.st_size <= MAX_ARCHIVE, "quota", "Source exceeds package size limit or is empty")
            data = stream.read(MAX_ARCHIVE + 1)
            after = os.fstat(stream.fileno())
            _require(len(data) == before.st_size <= MAX_ARCHIVE and
                     (before.st_size, before.st_mtime_ns, before.st_ctime_ns) ==
                     (after.st_size, after.st_mtime_ns, after.st_ctime_ns),
                     "source_changed", "Source changed while being read")
            return data
    except OSError as error:
        raise PackageError("source_io", "Cannot safely read source package") from error


def _safe_name(raw):
    try:
        name = raw.decode("utf-8")
    except UnicodeDecodeError as error:
        raise PackageError("unsafe_path", "Entry name must be UTF-8") from error
    _require(0 < len(raw) <= 1024 and not name.startswith("/") and
             not any(c in name for c in "\\:") and
             not any(ord(c) < 32 or ord(c) == 127 for c in name),
             "unsafe_path", "Unsafe archive path")
    parts = name.rstrip("/").split("/")
    _require(all(p not in ("", ".", "..") for p in parts) and not name.endswith("//"),
             "unsafe_path", "Unsafe archive path component")
    return name


def _extra(extra):
    cursor = 0
    while cursor < len(extra):
        _require(cursor + 4 <= len(extra), "archive", "Truncated ZIP extra field")
        kind, size = struct.unpack_from("<HH", extra, cursor)
        _require(kind != 1, "unsupported_zip", "ZIP64 is unsupported")
        cursor += 4 + size
        _require(cursor <= len(extra), "archive", "Truncated ZIP extra payload")


def _entries(data):
    """Strict ZIP32 central/local validation, CRC checks, bounded inflate."""
    end = None
    for cursor in range(len(data) - 22, max(-1, len(data) - 65558), -1):
        if data[cursor:cursor + 4] == b"PK\x05\x06" and cursor + 22 + int.from_bytes(data[cursor + 20:cursor + 22], "little") == len(data):
            end = cursor
            break
    _require(end is not None, "archive", "ZIP end directory is missing")
    _, disk, central_disk, count_disk, count, size, directory, _ = struct.unpack_from("<4s4H2IH", data, end)
    _require(disk == central_disk == 0 and count_disk == count and count < 65535 and
             size < 0xffffffff and directory < 0xffffffff, "unsupported_zip", "Split ZIP and ZIP64 are unsupported")
    _require(0 < count <= MAX_ENTRIES and directory + size == end, "quota", "Invalid or excessive ZIP directory")
    cursor, total = directory, 0
    names, ranges, result = set(), [], []
    for _ in range(count):
        _require(cursor + 46 <= end and data[cursor:cursor + 4] == b"PK\x01\x02", "archive", "Invalid central entry")
        fields = struct.unpack_from("<4s6H3I5H2I", data, cursor)
        (_, made, needed, flags, method, _, _, crc, compressed, expanded,
         name_len, extra_len, comment_len, entry_disk, _, attributes, local) = fields
        finish = cursor + 46 + name_len + extra_len + comment_len
        _require(finish <= end and entry_disk == 0, "archive", "Invalid central entry bounds")
        raw_name = data[cursor + 46:cursor + 46 + name_len]
        name = _safe_name(raw_name)
        canonical = unicodedata.normalize("NFC", name.rstrip("/")).casefold()
        _require(canonical not in names, "duplicate_path", "Duplicate or ambiguous archive path")
        names.add(canonical)
        mode = stat.S_IFMT(attributes >> 16)
        _require(mode in (0, stat.S_IFREG, stat.S_IFDIR) and not (mode == stat.S_IFDIR and not name.endswith("/")) and
                 not (mode == stat.S_IFREG and name.endswith("/")), "unsafe_path", "Archive special files are forbidden")
        _require(not flags & ~0x080e and method in (0, 8) and needed <= 20,
                 "unsupported_zip", "Encrypted or unsupported ZIP entry")
        _require(compressed != 0xffffffff and expanded != 0xffffffff and local != 0xffffffff,
                 "unsupported_zip", "ZIP64 entry is unsupported")
        _extra(data[cursor + 46 + name_len:cursor + 46 + name_len + extra_len])
        _require(expanded <= MAX_ENTRY and compressed <= MAX_ARCHIVE and
                 expanded <= max(4096, compressed * MAX_RATIO), "quota", "Entry expansion exceeds quota")
        total += expanded
        _require(total <= MAX_EXPANDED, "quota", "Archive expansion exceeds quota")
        _require(local + 30 <= directory and data[local:local + 4] == b"PK\x03\x04", "archive", "Invalid local entry")
        (_, local_needed, local_flags, local_method, _, _, local_crc, local_compressed,
         local_expanded, local_name_len, local_extra_len) = struct.unpack_from("<4s5H3I2H", data, local)
        payload = local + 30 + local_name_len + local_extra_len
        _require((local_needed, local_flags, local_method) == (needed, flags, method) and
                 local_name_len == name_len and payload + compressed <= directory and
                 data[local + 30:local + 30 + local_name_len] == raw_name,
                 "archive", "Central and local entry disagree")
        _extra(data[local + 30 + local_name_len:payload])
        record_end = payload + compressed
        if flags & 8:
            descriptor = record_end
            if data[descriptor:descriptor + 4] == b"PK\x07\x08":
                descriptor += 4
            _require(descriptor + 12 <= directory and struct.unpack_from("<3I", data, descriptor) == (crc, compressed, expanded),
                     "archive", "Invalid ZIP data descriptor")
            record_end = descriptor + 12
        else:
            _require((local_crc, local_compressed, local_expanded) == (crc, compressed, expanded),
                     "archive", "Central and local size/checksum disagree")
        ranges.append((local, record_end))
        packed = data[payload:payload + compressed]
        if method == 0:
            output = packed
        else:
            try:
                decoder = zlib.decompressobj(-15)
                output = decoder.decompress(packed, expanded + 1)
                _require(decoder.eof and not decoder.unused_data and not decoder.unconsumed_tail,
                         "archive", "Invalid or oversized DEFLATE stream")
            except zlib.error as error:
                raise PackageError("archive", "Invalid DEFLATE stream") from error
        _require(len(output) == expanded and zlib.crc32(output) & 0xffffffff == crc,
                 "checksum", "Entry size or CRC mismatch")
        _require(not name.endswith("/") or not output, "archive", "Directory has content")
        result.append((name, output))
        cursor = finish
    _require(cursor == end, "archive", "Central directory length disagrees")
    ranges.sort()
    _require(all(right <= next_left for (_, right), (next_left, _) in zip(ranges, ranges[1:])),
             "archive", "Overlapping ZIP records")
    # File/directory collisions include a regular `a` beside `a/b`.
    regular = {unicodedata.normalize("NFC", n).casefold() for n, _ in result if not n.endswith("/")}
    for name, _ in result:
        parts = unicodedata.normalize("NFC", name.rstrip("/")).casefold().split("/")
        _require(not any("/".join(parts[:i]) in regular for i in range(1, len(parts))),
                 "duplicate_path", "File shadows an archive directory")
    return {name: payload for name, payload in result if not name.endswith("/")}


def _json(data, maximum=1024 * 1024):
    _require(len(data) <= maximum, "quota", "JSON exceeds metadata quota")
    def pairs(items):
        result = {}
        for key, value in items:
            _require(key not in result, "metadata", "Duplicate JSON key")
            result[key] = value
        return result
    def constant(_):
        raise PackageError("metadata", "Non-finite JSON number")
    try:
        result = json.loads(data.decode("utf-8"), object_pairs_hook=pairs, parse_constant=constant)
    except (ValueError, UnicodeError, RecursionError) as error:
        if isinstance(error, PackageError):
            raise
        raise PackageError("metadata", "Invalid UTF-8 JSON metadata") from error
    pending, count = [(result, 0)], 0
    while pending:
        item, depth = pending.pop()
        count += 1
        _require(depth <= 64 and count <= 100000, "quota", "JSON complexity exceeds quota")
        _require(not isinstance(item, float) or math.isfinite(item), "metadata", "Non-finite JSON number")
        if isinstance(item, (list, dict)):
            pending.extend((child, depth + 1) for child in (item.values() if isinstance(item, dict) else item))
    return result


def _manifest(data):
    item = _json(data)
    _require(isinstance(item, dict), "manifest", "Manifest must be an object")
    package, version = item.get("package"), item.get("versionCode")
    _require(isinstance(package, str) and 0 < len(package.encode()) <= 255 and
             re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)+", package),
             "manifest", "Invalid quick-app package identifier")
    _require(type(version) is int and 0 <= version <= 0x7fffffffffffffff,
             "manifest", "versionCode must be a nonnegative int64")
    for key in ("name", "versionName"):
        value = item.get(key, "")
        _require(isinstance(value, str) and len(value.encode()) <= 1024 and not any(ord(c) < 32 for c in value),
                 "manifest", "Invalid manifest text field")
    platform = item.get("minPlatformVersion", 0)
    _require(type(platform) is int and 0 <= platform <= 0x7fffffffffffffff,
             "manifest", "Invalid minimum platform version")
    devices = item.get("deviceTypeList", [])
    _require(isinstance(devices, list) and len(devices) <= 64 and
             all(isinstance(d, str) and 0 < len(d.encode()) <= 128 and not any(ord(c) < 32 for c in d) for d in devices),
             "manifest", "Invalid device type list")
    return {"packageName": package, "versionCode": version, "name": item.get("name", ""),
            "versionName": item.get("versionName", ""), "minPlatformVersion": platform,
            "deviceTypes": devices}


def _bin_header(data):
    _require(52 <= len(data) <= MAX_ENTRY and data[:4] == b"\x5a\xa5\x34\x12" and
             re.fullmatch(rb"(?:[0-9]{12}|L[0-9]{11})", data[40:52]),
             "watchface_header", "Unrecognized local watchface BIN header")
    return {"faceID": data[40:52].decode("ascii"),
            "protocolWord": int.from_bytes(data[16:20], "little"),
            "validation": "header_only", "q66Compatibility": "unverified"}


def _inspect(data):
    report = {"schemaVersion": 1, "status": "inspected", "bytes": len(data),
              "sha256": hashlib.sha256(data).hexdigest(), "guestInstallSupported": False,
              "guestInstalled": False, "signatureVerified": False, "stagingSupported": True,
              "runtimeCompatibility": "unverified",
              "warnings": ["Inspection and staging do not install or execute a package in Q66."]}
    if data[:4] == b"\x5a\xa5\x34\x12":
        report.update(format="watchface_bin_candidate", metadata=_bin_header(data), entries=[],
                      validation="header_only")
        return report
    _require(data.startswith(b"PK\x03\x04"), "unsupported_format", "Expected RPK/MWZ/packaged watchface ZIP or recognized BIN header")
    files = _entries(data)
    report["entries"] = [{"path": name, "bytes": len(payload), "sha256": hashlib.sha256(payload).hexdigest()}
                         for name, payload in sorted(files.items())]
    projects = [name for name in files if name == "editor.config.json" or name.endswith("/editor.config.json")]
    engineering = any(name.rsplit("/", 1)[-1].lower() in MARKERS for name in files)
    if "manifest.json" in files:
        _require(not projects and not engineering, "ambiguous_format", "Engineering or mixed archive cannot be classified as RPK")
        report.update(format="rpk", metadata=_manifest(files["manifest.json"]), validation="metadata_only")
    elif projects:
        _require(len(projects) == 1, "ambiguous_format", "MWZ must contain one project root")
        root = projects[0][:-len("editor.config.json")]
        config = _json(files[projects[0]], 8 * 1024 * 1024)
        _require(isinstance(config, (dict, list)), "metadata", "MWZ configuration must be an object or array")
        description = files.get(root + "description.xml", b"")
        _require(0 < len(description) <= 1024 * 1024, "metadata", "Missing or excessive MWZ description")
        try:
            xml = description.decode("utf-8")
            _require("<!DOCTYPE" not in xml.upper() and "<!ENTITY" not in xml.upper(),
                     "metadata", "DTD/entities are forbidden")
            document = ET.fromstring(xml)
            pending, nodes = [(document, 0)], 0
            while pending:
                element, depth = pending.pop()
                nodes += 1
                _require(depth <= 64 and nodes <= 100000, "quota", "XML complexity exceeds quota")
                pending.extend((child, depth + 1) for child in element)
        except (UnicodeError, ET.ParseError) as error:
            raise PackageError("metadata", "Invalid MWZ description XML") from error
        report.update(format="mwz_engineering_project", validation="container_and_metadata",
                      metadata={"root": root, "requiresCompile": True, "engineeringMarker": engineering})
    else:
        _require(not engineering, "engineering_archive", "Engineering archive lacks a valid MWZ project")
        bins = [name for name in files if name.lower().endswith(".bin")]
        _require(len(bins) == 1, "unsupported_format", "Expected exactly one packaged watchface BIN")
        report.update(format="packaged_watchface_candidate", validation="container_and_header",
                      metadata={"payloadEntry": bins[0], **_bin_header(files[bins[0]])})
    return report


def inspect_package(path) -> dict:
    """Return a verified inspection record; invalid/unsupported input raises PackageError."""
    return _inspect(_read_source(path))


@contextmanager
def _private_root(path):
    """Open the destination itself without following links; require private ownership."""
    root = Path(os.path.abspath(os.fspath(path)))
    # Existing parents are caller-selected, not taken from the package. Resolve them
    # once for normal macOS /tmp aliases; the final session root may never be a link.
    root = root.parent.resolve(strict=True) / root.name
    try:
        root.mkdir(mode=0o700)
    except FileExistsError:
        pass
    fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        info = os.fstat(fd)
        _require(info.st_uid == os.getuid() and info.st_mode & 0o077 == 0,
                 "session_permissions", "Session root must be owned by the current user and mode 0700")
        yield root, fd
    finally:
        os.close(fd)


def _write_at(directory_fd, name, data):
    fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=directory_fd)
    with os.fdopen(fd, "wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


@contextmanager
def _stage_lock(root_fd):
    """Fail fast on concurrent writers; the lock is local to this private session."""
    fd = os.open(".packages.lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600, dir_fd=root_fd)
    try:
        info = os.fstat(fd)
        _require(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid() and info.st_mode & 0o077 == 0,
                 "session_permissions", "Unsafe session lock file")
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise PackageError("session_busy", "Another package transaction is in progress; retry later") from error
        try:
            yield
        finally:
            fcntl.flock(fd, fcntl.LOCK_UN)
    finally:
        os.close(fd)


def _stage_usage(root_fd):
    """Include abandoned pending transactions so crashes cannot evade quotas."""
    count, size = 0, 0
    for name in os.listdir(root_fd):
        if not name.startswith(("package-", ".pending-")):
            continue
        count += 1
        _require(count <= MAX_STAGES, "session_quota", "Session package count exceeds quota")
        fd = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=root_fd)
        try:
            for child in os.listdir(fd):
                _require(child in ("package.bin", "receipt.json"), "session_state", "Unexpected content in a package transaction")
                info = os.stat(child, dir_fd=fd, follow_symlinks=False)
                _require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1,
                         "session_state", "Unsafe content in a package transaction")
                size += info.st_size
                _require(size <= MAX_STAGED_BYTES, "session_quota", "Session package bytes exceed quota")
        finally:
            os.close(fd)
    return count, size


def stage_package(path, session_root) -> dict:
    """Atomically stage the exact inspected bytes and receipt in a unique directory.

    No archive paths enter filesystem calls. A failed call removes only its own
    transaction. Previously committed stages and the source remain unchanged.
    The caller creates the parent of session_root; this function creates only
    the private root and one transaction beneath it. It returns status=staged.
    """
    data = _read_source(path)
    report = _inspect(data)
    stage_id = report["sha256"] + "-" + uuid.uuid4().hex
    temporary, committed = ".pending-" + stage_id, "package-" + stage_id
    try:
        with _private_root(session_root) as (root, root_fd), _stage_lock(root_fd):
            receipt = {**report, "status": "staged", "stageID": stage_id,
                       "stagedPath": str(root / committed / "package.bin"),
                       "receiptPath": str(root / committed / "receipt.json")}
            receipt_bytes = json.dumps(receipt, ensure_ascii=False, sort_keys=True, indent=2).encode("utf-8")
            count, size = _stage_usage(root_fd)
            _require(count < MAX_STAGES and size + len(data) + len(receipt_bytes) <= MAX_STAGED_BYTES,
                     "session_quota", "Session permits at most 32 stages and 512 MiB including receipts")
            os.mkdir(temporary, 0o700, dir_fd=root_fd)
            transaction_fd = None
            try:
                transaction_fd = os.open(temporary, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=root_fd)
                _write_at(transaction_fd, "package.bin", data)
                _write_at(transaction_fd, "receipt.json", receipt_bytes)
                os.fsync(transaction_fd)
                _stage_usage(root_fd)
                os.rename(temporary, committed, src_dir_fd=root_fd, dst_dir_fd=root_fd)
                # The rename is the commit point. Do not report rollback after it.
                return receipt
            except BaseException:
                # Cleanup relative to pinned descriptors, never via package paths.
                if transaction_fd is not None:
                    for child in ("package.bin", "receipt.json"):
                        try:
                            os.unlink(child, dir_fd=transaction_fd)
                        except FileNotFoundError:
                            pass
                os.rmdir(temporary, dir_fd=root_fd)
                raise
            finally:
                if transaction_fd is not None:
                    os.close(transaction_fd)
    except OSError as error:
        raise PackageError("staging_io", "Package staging failed; no previous stage was replaced") from error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("inspect", "stage"))
    parser.add_argument("package", type=Path)
    parser.add_argument("--session-root", type=Path)
    args = parser.parse_args()
    if args.action == "stage" and args.session_root is None:
        parser.error("stage requires --session-root")
    try:
        result = inspect_package(args.package) if args.action == "inspect" else stage_package(args.package, args.session_root)
    except PackageError as error:
        print(json.dumps({"status": "rejected", "code": error.code, "error": str(error)}))
        return 2
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
