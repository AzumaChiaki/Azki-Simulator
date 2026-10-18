"""Content-addressed firmware inventory/import; no executable is run here.

Catalog v1: {schema:'azki.simulator.firmware.v1', version:1, objects:{sha:
{path:'objects/<sha>', bytes:int, localAllowed:bool, publicAllowed:bool,
rightsEvidence:[str]}}, profiles:[{id,displayName,model,version,width,height,
storageKind,assets:{role:sha},recipe:{original non-path catalog fields}}],
imports:[{sha256,bytes,kind:'ap'|'ota',profileIDs:[exact-id],members:[
{asset:sha,record:{original ZIP local-header inventory}}]}]}.

The bundled catalog supplies hardware presets (product models and their screen
geometry) and the app's own device metadata. Firmware never comes from it: every
runnable entry is a complete OTA the user imported, inventoried structurally and
offered on every hardware preset. Nothing here matches a firmware version or a
digest against a list, and nothing here refuses to boot the user's own import.
"""
from __future__ import annotations

import ctypes
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import sys
import time
import uuid
import zlib

from .minative import (INCLUDED_PROFILE_IDS, MError, _canonical, _decode, _directory_fd,
                       _geometry, _json, _open_file, _path, _pin, _read, generated_profile)

SCHEMA = "azki.simulator.firmware.v1"
MAX_ASSET = 768 * 1024 * 1024
MAX_JSON_LIBRARY = 8 * 1024 * 1024
MAX_EXPANDED = 1024 * 1024 * 1024
# Library bounds only keep the index document reasonable; they are far above
# realistic use (one OTA is 0.1-0.5 GiB) and never the user's disk policy.
MAX_LIBRARY = 1024 * 1024 * 1024 * 1024
MAX_OBJECTS = 512  # Bundled catalog only.
MAX_LIBRARY_OBJECTS = 8192
MAX_LIBRARY_IMPORTS = 2048
EXECUTION_PROFILES = INCLUDED_PROFILE_IDS
# Profiles whose board/factory/resource are pre-pinned catalog objects keep "prepared" mode;
# every other profile is "generated": the sandboxed runner derives storage from the pinned AP/OTA.
PRODUCT_NAMES = {
    "n66": "小米手环 9", "n67": "小米手环 9 Pro", "o66": "小米手环 10", "p67": "小米手环 10 Pro", "q66": "小米手环 11",
    "p65": "红米手表6", "q65": "红米手表7"}
BRANCH_NAMES = {
    "cn": "标准版", "nfc": "NFC 版", "tc": "陶瓷版", "lj": "耀影金版", "glt": "GLT", "u": "U", "": ""}


def display_name(profile):
    """Readable product name for an exact profile; unknown shapes keep the exact ID."""
    model, version = profile.get("model", ""), profile.get("version", "")
    code = model.rsplit(".", 1)[-1]
    family = next((key for key in PRODUCT_NAMES if code.startswith(key)), None)
    if family is None:
        return profile["id"]
    branch = code[len(family):]
    if family == "n66" and branch == "tc":
        branch_name = "陶瓷特别版"
    elif family == "p67" and branch == "cn":
        branch_name = "金属版"
    elif family == "q66" and branch == "":
        branch_name = ""
    else:
        branch_name = BRANCH_NAMES.get(branch, branch.upper())
    return " ".join(part for part in (PRODUCT_NAMES[family], branch_name) if part) + " · " + version


def storage_mode(profile):
    return "prepared" if "board" in profile["assets"] else "generated"


def product_name(model):
    """Product name of a hardware preset, without any firmware version."""
    return display_name({"id": model, "model": model, "version": ""}).rstrip(" ·") or model


ROLES = {"ap", "ota", "resource", "board", "factory", "seed", "seedProof", "resourceProof"}
SHA = re.compile(r"[0-9a-f]{64}\Z")


class CatalogError(ValueError):
    def __init__(self, code, message, details=None):
        self.code, self.details = code, details or {}
        super().__init__(message)

    def as_json(self):
        return {"version": 1, "ok": False, "error": {
            "code": self.code, "message": str(self), "details": self.details}}


def _need(condition, code, message, **details):
    if not condition:
        raise CatalogError(code, message, details)


def _sha(value):
    return isinstance(value, str) and SHA.fullmatch(value) is not None


# Names the app itself owns in a firmware library root. Anything else is the
# user's business: an unexpected entry never blocks an import.
LIBRARY_LAYOUT = {"objects", "index.json", ".import.lock", "instances"}


def _owned_directory(path, *, create=False):
    """Open a directory this app owns, repairing a wider mode.

    The library belongs to the user: a mode changed by a backup, a Finder copy or
    a different build must not brick imports. Links and foreign owners are still
    rejected; any wider mode, including group/other-writable, is narrowed back
    to owner-only.
    """
    path = _path(path)
    try:
        fd = _directory_fd(path)
    except FileNotFoundError:
        _need(create, "library_required", "固件库目录不存在")
        # Only genuinely missing levels are created, each owner-only; the nearest
        # existing ancestor is opened without traversing any link.
        missing, ancestor = [], path
        while True:
            try:
                fd = _directory_fd(ancestor)
                break
            except FileNotFoundError:
                missing.append(ancestor)
                ancestor = ancestor.parent
        try:
            for item in reversed(missing):
                os.mkdir(item.name, 0o700, dir_fd=fd)
                os.close(fd)
                fd = _directory_fd(item)
        finally:
            os.close(fd)
        fd = _directory_fd(path)
    info = os.fstat(fd)
    if info.st_uid != os.getuid():
        os.close(fd)
        raise CatalogError("unsafe_library", "固件库目录不属于当前用户，请把它移走或修正所有者后重试：" + str(path)[:160])
    if stat.S_IMODE(info.st_mode) != 0o700:
        os.fchmod(fd, 0o700)
    return fd


def _owned_file(path, limit=MAX_ASSET):
    """Open a file in the user's own library, narrowing a wider mode back."""
    path = _path(path)
    directory = _owned_directory(path.parent)
    try:
        fd = os.open(path.name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory)
    finally:
        os.close(directory)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_size > limit or info.st_uid != os.getuid():
            raise CatalogError("unsafe_library", "固件库文件不是当前用户的普通文件：" + path.name[:80])
        if stat.S_IMODE(info.st_mode) != 0o600:
            os.fchmod(fd, 0o600)
            # chmod changes ctime; callers compare the file stamp across a read.
            info = os.fstat(fd)
    except BaseException:
        os.close(fd)
        raise
    return fd, info


def _discard_tree(directory, name):
    """Remove one abandoned staging directory; links are removed, never followed."""
    child = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=directory)
    try:
        with os.scandir(child) as entries:
            for entry in entries:
                info = entry.stat(follow_symlinks=False)
                _need(info.st_uid == os.getuid(), "unsafe_library", "未完成的导入含其他用户的文件")
                if stat.S_ISDIR(info.st_mode):
                    _discard_tree(child, entry.name)
                elif stat.S_ISREG(info.st_mode) or stat.S_ISLNK(info.st_mode):
                    os.unlink(entry.name, dir_fd=child)
                else:
                    raise CatalogError("unsafe_library", "未完成的导入含不支持的文件类型")
    finally:
        os.close(child)
    os.rmdir(name, dir_fd=directory)


def _recover_library(root_fd):
    """Ignore unrelated entries and clear staging left by an interrupted import.

    A crash or a forced quit can leave a .txn- staging directory behind, which
    would otherwise refuse every later import. Unknown entries are left exactly
    as they are: the library is the user's own directory.
    """
    abandoned, cleared = [], 0
    with os.scandir(root_fd) as entries:
        for entry in entries:
            info = entry.stat(follow_symlinks=False)
            # .index- temporaries are only stale once no writer can still own them.
            if (entry.name.startswith((".txn-", ".index-")) and info.st_uid == os.getuid()
                    and (stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode))
                    and (entry.name.startswith(".txn-") or time.time() - info.st_mtime > 600)):
                abandoned.append((entry.name, stat.S_ISDIR(info.st_mode)))
    for name, directory in abandoned:
        # Best effort: a leftover that cannot be cleared never blocks an import.
        try:
            if directory:
                _discard_tree(root_fd, name)
            else:
                os.unlink(name, dir_fd=root_fd)
            cleared += 1
        except (OSError, CatalogError):
            continue
    return cleared


def _text(value, maximum=128):
    return (isinstance(value, str) and 0 < len(value.encode("utf-8")) <= maximum
            and not any(ord(c) < 32 or ord(c) == 127 for c in value))


def _atomic_json(path, value, directory_fd=None):
    temp = path.with_name(".index-" + uuid.uuid4().hex)
    temporary = temp.name if directory_fd is not None else temp
    destination = path.name if directory_fd is not None else path
    fd = os.open(temporary, os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW, 0o600, dir_fd=directory_fd)
    created = os.fstat(fd)
    published = False
    try:
        with os.fdopen(fd, "wb") as out:
            out.write(_canonical(value) + b"\n")
            out.flush()
            os.fsync(out.fileno())
        os.replace(temporary, destination, src_dir_fd=directory_fd, dst_dir_fd=directory_fd)
        published = True
        directory = os.dup(directory_fd) if directory_fd is not None else _directory_fd(path.parent, private=True)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    except OSError as error:
        if published:
            raise CatalogError("publication_uncertain", "导入索引已发布，但未确认磁盘持久化；请重新读取固件库") from error
        raise
    finally:
        try:
            current = os.stat(temporary, dir_fd=directory_fd, follow_symlinks=False)
            if (current.st_dev, current.st_ino) == (created.st_dev, created.st_ino):
                os.unlink(temporary, dir_fd=directory_fd)
        except FileNotFoundError:
            pass


def _copy(source, target, *, directory_fd=None, created=None, hash_content=True):
    source_fd, before = _open_file(source, MAX_ASSET)
    target_fd = None
    digest, count = hashlib.sha256(), 0
    try:
        target_fd = os.open(target, os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW, 0o600, dir_fd=directory_fd)
        if created is not None:
            info = os.fstat(target_fd)
            created[str(target)] = (info.st_dev, info.st_ino)
        with os.fdopen(target_fd, "wb", closefd=False) as out:
            while chunk := os.read(source_fd, 1024 * 1024):
                count += len(chunk)
                _need(count <= MAX_ASSET, "input_quota", "固件文件超过大小限制")
                if hash_content:
                    digest.update(chunk)
                out.write(chunk)
            out.flush()
            os.fsync(out.fileno())
        after = os.fstat(source_fd)
        _need((before.st_size, before.st_mtime_ns, before.st_ctime_ns) ==
              (after.st_size, after.st_mtime_ns, after.st_ctime_ns) and count == before.st_size,
              "input_changed", "导入时固件文件发生变化，请重新选择")
        result = {"bytes": count}
        if hash_content:
            # The digest names the content-addressed object; it never gates a boot.
            result["sha256"] = digest.hexdigest()
        return result
    finally:
        os.close(source_fd)
        if target_fd is not None:
            os.close(target_fd)



def _publish_object(source_fd, name, target_fd):
    """Atomic no-replace rename; never overwrite even a competing new object."""
    libc = ctypes.CDLL(None, use_errno=True)
    function = "renameatx_np" if sys.platform == "darwin" else "renameat2"
    call = getattr(libc, function, None)
    _need(call is not None, "publication_unavailable", "系统不支持安全的独占资产发布")
    call.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    call.restype = ctypes.c_int
    encoded = os.fsencode(name)
    if call(source_fd, encoded, target_fd, encoded, 4 if sys.platform == "darwin" else 1):
        error = ctypes.get_errno()
        if error == errno.EEXIST:
            return False
        raise OSError(error, "Exclusive asset publication failed")
    return True


def _object_size(directory, name):
    """Existing object metadata only: no re-read, no digest gate."""
    fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory)
    try:
        info = os.fstat(fd)
        _need(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid() and info.st_size <= MAX_ASSET,
              "unsafe_library", "现有固件对象不是当前用户的普通文件，请删除该对象后重新导入")
        if stat.S_IMODE(info.st_mode) != 0o600:
            os.fchmod(fd, 0o600)
        return {"bytes": info.st_size}
    finally:
        os.close(fd)

def _member_record(value, asset):
    _need(isinstance(value, dict), "invalid_catalog", "固件目录中的包记录无效")
    required = ("header_offset", "data_offset", "compressed_size", "size", "compression_method")
    _need(all(type(value.get(k)) is int and value[k] >= 0 for k in required),
          "invalid_catalog", "固件包边界记录无效")
    name = value.get("name")
    _need(_text(name, 512) and not name.startswith(("/", "\\")) and "\\" not in name
          and not any(x in ("", ".", "..") for x in name.split("/")) and ":" not in name,
          "unsafe_archive_path", "固件包包含不安全的成员路径")
    # A member digest is optional: it is descriptive when present and never a
    # condition for reading or booting the package.
    _need((value.get("sha256") is None or (value.get("sha256") == asset and _sha(asset))) and
          value["compression_method"] in (0, 8) and 0 < value["size"] <= MAX_ASSET
          and 0 < value["compressed_size"] <= MAX_ASSET
          and value["size"] <= max(4096, value["compressed_size"] * 4096)
          and isinstance(value.get("crc32"), str) and re.fullmatch("[0-9a-f]{8}", value["crc32"]),
          "invalid_catalog", "固件包的格式或大小记录无效")
    return value


def _extract_member(source_fd, record, target, source_size, *, directory_fd=None, created=None):
    offset, packed, size = record["header_offset"], record["compressed_size"], record["size"]
    _need(offset + 30 <= source_size, "archive_bounds", "固件包头超出文件边界")
    header = os.pread(source_fd, 30, offset)
    _need(len(header) == 30, "archive_bounds", "固件包头不完整")
    magic, _, flags, method, _, _, crc, local_packed, local_size, names, extras = struct.unpack("<4s5H3I2H", header)
    data_offset = offset + 30 + names + extras
    # Bits 1-2 describe the DEFLATE level. Bit 3 (signed OTAs) moves CRC and
    # sizes to a trailing descriptor, so the local fields are zero and the
    # central-directory values are the ones that bound the read below.
    descriptor = bool(flags & 8)
    local = (local_packed, local_size, f"{crc:08x}")
    _need(magic == b"PK\x03\x04" and flags & ~0x80e == 0 and method in (0, 8)
          and method == record["compression_method"]
          and data_offset == record["data_offset"] and data_offset + packed <= source_size
          and (local == (packed, size, record["crc32"]) or (descriptor and local == (0, 0, "00000000"))),
          "archive_format", "固件包成员与已校验清单不一致")
    name = os.pread(source_fd, names, offset + 30)
    _need(name == record["name"].encode("utf-8"), "archive_format", "固件包成员名称与清单不一致")
    # Member names are validated for provenance only, never used as host paths.
    descriptor = os.open(target, os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW, 0o600, dir_fd=directory_fd)
    if created is not None:
        info = os.fstat(descriptor)
        created[str(target)] = (info.st_dev, info.st_ino)
    total = 0
    decoder = zlib.decompressobj(-15) if method == 8 else None
    try:
        with os.fdopen(descriptor, "wb", closefd=False) as out:
            cursor, remaining = data_offset, packed
            while remaining:
                chunk = os.pread(source_fd, min(65536, remaining), cursor)
                _need(bool(chunk), "archive_bounds", "固件包成员被截断")
                cursor += len(chunk)
                remaining -= len(chunk)
                pending = chunk
                while pending:
                    try:
                        block = decoder.decompress(pending, 1024 * 1024) if decoder else pending
                    except zlib.error as error:
                        raise CatalogError("archive_format", "固件包解压失败") from error
                    pending = decoder.unconsumed_tail if decoder else b""
                    total += len(block)
                    _need(total <= size, "archive_quota", "固件包展开大小超出清单限制")
                    out.write(block)
                    if decoder:
                        _need(not decoder.unused_data, "archive_format", "固件压缩成员含额外数据")
            # Decoding must land exactly on the recorded size; the bytes are
            # taken as the user supplied them, not re-identified against a digest.
            _need((decoder is None or (decoder.eof and not decoder.unconsumed_tail)) and total == size,
                  "archive_format", "展开的固件成员大小与清单不一致")
            out.flush()
            os.fsync(out.fileno())
    finally:
        os.close(descriptor)


class FirmwareCatalog:
    def __init__(self, bundle_root, library_root=None, *, distribution=None):
        self.bundle_root = _path(bundle_root)
        self.library_root = _path(library_root) if library_root is not None else None
        catalog_bytes = _read(self.bundle_root / "catalog.json")
        self.data = _decode(catalog_bytes)
        self.catalog_sha256 = hashlib.sha256(catalog_bytes).hexdigest()
        _need(isinstance(self.data, dict) and self.data.get("schema") == SCHEMA and self.data.get("version") == 1,
              "invalid_catalog", "内置固件目录版本不受支持")
        distribution = distribution or self.data.get("distribution", "local")
        _need(distribution in ("local", "public"), "invalid_distribution", "未知资源分发模式")
        self.distribution = distribution
        objects, profiles, imports = (self.data.get(k) for k in ("objects", "profiles", "imports"))
        _need(isinstance(objects, dict) and len(objects) <= MAX_OBJECTS and isinstance(profiles, list)
              and len(profiles) <= 39 and isinstance(imports, list) and len(imports) <= 128,
              "invalid_catalog", "内置固件目录规模或结构无效")
        self.objects, self.profiles, self.imports = objects, {}, {}
        for digest, item in objects.items():
            _need(_sha(digest) and isinstance(item, dict) and item.get("path") == "objects/" + digest
                  and type(item.get("bytes")) is int and 0 < item["bytes"] <= MAX_ASSET
                  and type(item.get("localAllowed")) is bool and type(item.get("publicAllowed")) is bool
                  and isinstance(item.get("rightsEvidence"), list) and len(item["rightsEvidence"]) <= 16
                  and all(_text(x, 1024) for x in item["rightsEvidence"])
                  and (not item["publicAllowed"] or bool(item["rightsEvidence"])),
                  "invalid_catalog", "内置资产路径、大小或权利记录无效")
        for profile in profiles:
            _need(isinstance(profile, dict) and profile.get("id") in INCLUDED_PROFILE_IDS
                  and profile["id"] not in self.profiles and _text(profile.get("displayName"))
                  and "@".join(profile["id"].split("@")[:2]) == str(profile.get("model")) + "@" + str(profile.get("version"))
                  and (profile.get("width"), profile.get("height")) == _geometry(profile["model"])
                  and profile.get("storageKind") in ("romfs", "yaffs"),
                  "invalid_catalog", "固件配置标识或屏幕尺寸无效")
            assets = profile.get("assets")
            _need(isinstance(assets, dict) and not set(assets) - ROLES and all(_sha(value) and value in objects for value in assets.values()),
                  "invalid_catalog", "固件配置引用了未知资产")
            # Roles the sandboxed runner derives from the bundled OTA at run time; never ap/ota.
            generated_roles = profile.get("generatedAssets", [])
            _need(isinstance(generated_roles, list) and len(generated_roles) <= 8
                  and all(isinstance(role, str) and role in ROLES - {"ap", "ota"} for role in generated_roles)
                  and (not generated_roles or "ota" in assets) and not set(generated_roles) & set(assets),
                  "invalid_catalog", "运行时生成的资产声明无效")
            recipe = profile.get("recipe")
            _need(isinstance(recipe, dict) and recipe.get("id") == profile["id"]
                  and recipe.get("model") == profile["model"] and recipe.get("version") == profile["version"]
                  and not {"ap_path", "resource_path", "ota_path"} & recipe.keys()
                  and isinstance(recipe.get("ap"), dict) and recipe["ap"].get("chip") == "best1503",
                  "invalid_catalog", "固件运行配方无效或含外部文件路径")
            if "ap" in assets:
                _need(recipe["ap"].get("sha256") == assets["ap"] and recipe["ap"].get("size") == objects[assets["ap"]]["bytes"],
                      "invalid_catalog", "AP 身份与配置不一致")
            self.profiles[profile["id"]] = profile
        for item in imports:
            _need(isinstance(item, dict) and _sha(item.get("sha256")) and item["sha256"] not in self.imports
                  and item.get("kind") in ("ap", "ota") and type(item.get("bytes")) is int
                  and 0 < item["bytes"] <= MAX_ASSET and item["sha256"] in objects
                  and item["bytes"] == objects[item["sha256"]]["bytes"],
                  "invalid_catalog", "固件导入标识无效")
            ids, members = item.get("profileIDs"), item.get("members", [])
            _need(isinstance(ids, list) and 0 < len(ids) <= 39 and all(isinstance(x, str) for x in ids) and len(ids) == len(set(ids))
                  and all(x in self.profiles for x in ids) and isinstance(members, list) and len(members) <= 4096,
                  "invalid_catalog", "固件导入候选配置无效")
            _need(all(self.profiles[x]["assets"].get(item["kind"]) == item["sha256"] for x in ids),
                  "invalid_catalog", "导入文件与候选固件配置不一致")
            ranges, expanded, seen = [], 0, set()
            for member in members:
                _need(isinstance(member, dict) and _sha(member.get("asset")) and member["asset"] in objects and member["asset"] not in seen,
                      "invalid_catalog", "固件包引用了重复或未知对象")
                seen.add(member["asset"])
                record = _member_record(member.get("record"), member["asset"])
                _need(record["size"] == objects[member["asset"]]["bytes"] and
                      record["data_offset"] >= record["header_offset"] + 30 and
                      record["data_offset"] + record["compressed_size"] <= item["bytes"],
                      "invalid_catalog", "固件包成员边界无效")
                ranges.append((record["header_offset"], record["data_offset"] + record["compressed_size"]))
                expanded += record["size"]
            ranges.sort()
            _need(expanded <= MAX_EXPANDED and all(a[1] <= b[0] for a, b in zip(ranges, ranges[1:])),
                  "archive_quota", "固件包成员重叠或展开大小过大")
            self.imports[item["sha256"]] = item

    def _index(self, create=False):
        empty = {"version": 1, "generation": 0, "objects": {}, "imports": []}
        if self.library_root is None:
            _need(not create, "library_required", "尚未配置私有固件库")
            return empty
        if not self.library_root.exists() and not create:
            return empty
        root = _owned_directory(self.library_root, create=create)
        try:
            try:
                objects = _owned_directory(self.library_root / "objects", create=create)
            except CatalogError:
                # No object store yet: nothing has been imported into this library.
                return empty
            else:
                os.close(objects)
            try:
                fd, info = _owned_file(self.library_root / "index.json", MAX_JSON_LIBRARY)
            except FileNotFoundError:
                return empty
            try:
                raw = b""
                while True:
                    chunk = os.read(fd, min(1024 * 1024, MAX_JSON_LIBRARY + 1 - len(raw)))
                    if not chunk:
                        break
                    raw += chunk
                    _need(len(raw) <= MAX_JSON_LIBRARY, "invalid_library", "私有固件库索引过大")
                after = os.fstat(fd)
                _need((info.st_size, info.st_mtime_ns, info.st_ctime_ns) ==
                      (after.st_size, after.st_mtime_ns, after.st_ctime_ns),
                      "input_changed", "读取索引时固件库发生变化")
            finally:
                os.close(fd)
            try:
                index = _decode(raw)
                self._check_index(index)
                return index
            except (ValueError, KeyError, TypeError) as error:
                # A damaged index is recoverable: the objects are the source of
                # truth. Keep the unreadable document beside it, never overwrite it.
                return self._rebuild_index(root, raw, error)
        finally:
            os.close(root)

    @staticmethod
    def _check_index(index):
        _need(isinstance(index, dict) and index.get("version") == 1
              and type(index.get("generation")) is int and 0 <= index["generation"] < 2**63
              and isinstance(index.get("objects"), dict) and len(index["objects"]) <= MAX_LIBRARY_OBJECTS
              and isinstance(index.get("imports"), list) and len(index["imports"]) <= MAX_LIBRARY_IMPORTS,
              "invalid_library", "私有固件库索引无效")
        for digest, record in index["objects"].items():
            _need(_sha(digest) and isinstance(record, dict) and type(record.get("bytes")) is int
                  and 0 < record["bytes"] <= MAX_ASSET, "invalid_library", "私有固件资产记录无效")

    def _rebuild_index(self, root, damaged, error):
        """Rebuild the library index from the objects that are actually present.

        Every full OTA object is inventoried again, so imported firmware stays
        runnable after a lost or damaged index; extracted members are kept as
        unnamed objects and re-derived on the next import of their package.
        """
        from .firmware_probe import ProbeError, ota_recipe
        objects_directory = _owned_directory(self.library_root / "objects", create=True)
        index = {"version": 1, "generation": 0, "objects": {}, "imports": []}
        names = []
        try:
            with os.scandir(objects_directory) as entries:
                for entry in entries:
                    info = entry.stat(follow_symlinks=False)
                    if not _sha(entry.name) or not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid():
                        continue
                    names.append((entry.name, info.st_size))
            for name, size in sorted(names):
                if not 0 < size <= MAX_ASSET or len(index["objects"]) >= MAX_LIBRARY_OBJECTS:
                    continue
                index["objects"][name] = {"bytes": size}
                if len(index["imports"]) >= MAX_LIBRARY_IMPORTS:
                    continue
                fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=objects_directory)
                try:
                    if os.pread(fd, 4, 0) != b"PK\x03\x04":
                        continue
                    try:
                        generic = ota_recipe(fd, size)
                    except (ProbeError, MError, ValueError, OSError, zlib.error):
                        continue
                    ap = next((record for record in generic["partitions"]
                               if record["name"].rsplit("/", 1)[-1] == "vela_ap.bin"), None)
                    if ap is None or "sha256" not in ap:
                        continue
                    index["objects"].setdefault(ap["sha256"], {"bytes": ap["size"]})
                    index["imports"].append({"sha256": name, "inputKind": "ota", "profileIDs": [],
                        "importedAtUnix": int(time.time()),
                        "generic": {"version": generic["version"], "family": generic["family"],
                                    "declaredModels": generic["declaredModels"],
                                    "apSHA256": ap["sha256"], "apBytes": ap["size"],
                                    "recipe": {"ap": generic["ap"], "partitions": generic["partitions"],
                                               "family": generic["family"]}}})
                finally:
                    os.close(fd)
        finally:
            os.close(objects_directory)
        if damaged is not None:
            stamp = time.strftime("%Y%m%d-%H%M%S")
            try:
                os.rename("index.json", "index.json.recovered-" + stamp, src_dir_fd=root, dst_dir_fd=root)
            except OSError:
                pass
        _atomic_json(self.library_root / "index.json", index, directory_fd=root)
        return index

    def _asset(self, digest, index):
        """Path of an existing asset; the catalog digest only names the file."""
        item = self.objects[digest]
        options = []
        if self.library_root and digest in index["objects"] and item["localAllowed"]:
            options.append((self.library_root / "objects" / digest, "imported"))
        allowed = item["localAllowed"] if self.distribution == "local" else item["publicAllowed"]
        if allowed:
            options.append((self.bundle_root / item["path"], "bundled"))
        for path, source in options:
            try:
                if source == "imported":
                    fd, _ = _owned_file(path, MAX_ASSET)
                else:
                    fd, _ = _open_file(path, MAX_ASSET)
            except (OSError, ValueError):
                continue  # One unusable object only marks its own entry unavailable.
            os.close(fd)
            return path, source
        return None, None

    @staticmethod
    def _required(profile):
        if storage_mode(profile) == "generated":
            return ["ap", "ota"] if "ota" in profile["assets"] else ["ap", "resource"]
        return (["ap", "resource", "board", "factory"] if profile["storageKind"] == "romfs" else
                ["ap", "ota", "resource", "board", "seed", "seedProof", "resourceProof"])

    def _item(self, profile, index):
        missing, sources = [], []
        for role in self._required(profile):
            digest = profile["assets"].get(role)
            path, source = self._asset(digest, index) if digest else (None, None)
            if path is None:
                missing.append(role)
            else:
                sources.append(source)
        return {"id": profile["id"], "displayName": display_name(profile), "device": profile["model"],
                "model": profile["model"], "firmwareVersion": profile["version"],
                "width": profile["width"], "height": profile["height"],
                "availability": {"ready": not missing and profile["id"] in EXECUTION_PROFILES, "assetsReady": not missing,
                                 "reason": ("缺少同版本资产：" + "、".join(missing) if missing else
                                            "" if profile["id"] in EXECUTION_PROFILES else "已识别；此配置的隔离运行尚未接通"),
                                 "missingRoles": missing, "verification": "required_before_run"},
                "source": "imported" if "imported" in sources else "bundled"}

    @property
    def hardware_models(self):
        """Instance hardware presets: every product model the catalog describes."""
        # q65 is metadata-only: its firmware is supplied by the user and is
        # intentionally not represented as a bundled profile.
        return sorted({profile["model"] for profile in self.profiles.values()} |
                      {"miwear.watch.p65", "miwear.watch.q65"})

    def hardware_presets(self):
        """Instance hardware presets for the UI: model metadata, no firmware version."""
        rows = {}
        for profile in self.profiles.values():
            rows.setdefault(profile["model"], {"model": profile["model"],
                                               "displayName": product_name(profile["model"]),
                                               "width": profile["width"], "height": profile["height"]})
        rows.setdefault("miwear.watch.p65", {"model": "miwear.watch.p65", "displayName": "红米手表6", "width": 432, "height": 514})
        rows.setdefault("miwear.watch.q65", {"model": "miwear.watch.q65", "displayName": "红米手表7", "width": 432, "height": 514})
        tail = {"miwear.watch.p65": 0, "miwear.watch.q65": 1}
        return [rows[model] for model in sorted(rows, key=lambda value: (value in tail, tail.get(value, 0), value))]

    def _object_path(self, digest, size, index):
        """Library (or permitted bundled) object path, else None."""
        if digest in self.objects:
            return self._asset(digest, index)[0]
        record = index["objects"].get(digest)
        if self.library_root is None or record is None:
            return None
        path = self.library_root / "objects" / digest
        try:
            fd, _ = _owned_file(path, MAX_ASSET)
        except (OSError, ValueError):
            return None  # Reported as missing on its own entry; the list still loads.
        os.close(fd)
        return path

    def _generic_sources(self, index):
        """Every imported full OTA, keyed by SHA-256; each may run on any preset."""
        sources = {}
        for entry in index["imports"]:
            if not isinstance(entry, dict) or not _sha(entry.get("sha256")) or entry.get("inputKind") != "ota":
                continue
            digest = entry["sha256"]
            generic = entry.get("generic")
            if isinstance(generic, dict):
                recipe = generic.get("recipe")
                if not (isinstance(recipe, dict) and isinstance(recipe.get("ap"), dict)
                        and isinstance(recipe.get("partitions"), list) and _sha(generic.get("apSHA256"))
                        and type(generic.get("apBytes")) is int and isinstance(generic.get("version"), str)
                        and re.fullmatch(r"[0-9][0-9.]{0,31}", generic["version"])):
                    continue
                sources[digest] = {"version": generic["version"], "ap": generic["apSHA256"],
                                   "apBytes": generic["apBytes"], "declaredModels": generic.get("declaredModels", []),
                                   "boundModel": generic.get("boundModel"),
                                   "recipe": recipe}
            elif digest in self.imports and self.imports[digest]["kind"] == "ota":
                profile = self.profiles[sorted(self.imports[digest]["profileIDs"])[0]]
                ap = profile["assets"]["ap"]
                sources[digest] = {"version": profile["version"], "ap": ap, "apBytes": self.objects[ap]["bytes"],
                                   "declaredModels": [],
                                   "recipe": {"ap": profile["recipe"]["ap"],
                                              "partitions": profile["recipe"].get("partitions", []), "family": ""}}
        return sources

    def _generic_item(self, model, digest, source, index):
        profile_id = model + "@" + source["version"] + "@" + digest[:12]
        width, height = _geometry(model)
        ready = (self._object_path(source["ap"], source["apBytes"], index) is not None
                 and self._object_path(digest, index["objects"].get(digest, {}).get("bytes", -1), index) is not None)
        isolated = model in ("miwear.watch.p65", "miwear.watch.q65")
        return {"id": profile_id, "displayName": display_name({"id": profile_id, "model": model,
                                                                "version": source["version"]}) + " · " + digest[:8],
                "device": model, "model": model, "firmwareVersion": source["version"],
                "otaSHA256": digest,
                "width": width, "height": height,
                "availability": {"ready": ready, "assetsReady": ready,
                                 "reason": ("" if ready else "导入的固件文件已缺失，请重新导入"),
                                 "missingRoles": [] if ready else ["ota"], "verification": "checked_at_launch"},
                "source": "imported"}

    def imported_id(self, profile_id):
        """True when this ID names firmware the user imported.

        An imported OTA may share its ID with a bundled metadata profile when the
        bytes are the same release; the user's own import always wins.
        """
        parsed = generated_profile(profile_id)
        if parsed is None:
            return False
        model, version, prefix = parsed
        return any(digest.startswith(prefix) and source["version"] == version
                   for digest, source in self._generic_sources(self._index()).items())

    def _candidate_models(self, generic, sha256):
        """Profile IDs an unbound import offers: AP-declared models first."""
        models = []
        for code in sorted(generic.get("declaredModels") or []):
            full = "miwear.watch." + code
            if full in self.hardware_models:
                models.append(full)
            elif code in self.hardware_models:
                models.append(code)
        if not models:
            models = self.hardware_models
        return sorted(model + "@" + generic["version"] + "@" + sha256[:12] for model in models)

    def _generic_items(self, index, only=None):
        items = []
        for digest, source in sorted(self._generic_sources(index).items()):
            if only is not None and digest != only:
                continue
            # Model isolation: an imported OTA appears only under the model the
            # user bound it to at import time (boundModel). Older records fall
            # back to the AP-declared models, then to the legacy expansion.
            bound = source.get("boundModel")
            if bound in self.hardware_models:
                models = [bound]
            else:
                models = []
                for code in sorted(source.get("declaredModels") or []):
                    full = "miwear.watch." + code
                    if full in self.hardware_models:
                        models.append(full)
                    elif code in self.hardware_models:
                        models.append(code)
                if not source.get("declaredModels") and bound is None:
                    models = self.hardware_models
            for model in models:
                items.append(self._generic_item(model, digest, source, index))
        return items

    def list_items(self):
        """Every runnable entry is firmware the user imported.

        The bundled catalog is device metadata (models, screen geometry, storage
        kind); it never contributes a locked firmware version to this list.
        """
        return self._generic_items(self._index())

    def resolve_generic(self, profile_id):
        """Runner recipe for imported firmware on the profile's hardware preset."""
        parsed = generated_profile(profile_id)
        _need(parsed is not None, "unknown_profile", "请选择固件库中的固件")
        model, version, prefix = parsed
        _need(model in self.hardware_models, "unknown_profile", "未知的实例硬件预设", model=model)
        index = self._index()
        sources = self._generic_sources(index)
        matches = [digest for digest, source in sources.items()
                   if digest.startswith(prefix) and source["version"] == version]
        _need(len(matches) == 1, "unknown_profile", "所选导入固件已不在固件库中，请重新导入", profileID=profile_id)
        digest = matches[0]
        source = sources[digest]
        ota_bytes = index["objects"].get(digest, {}).get("bytes", -1)
        ap_path = self._object_path(source["ap"], source["apBytes"], index)
        ota_path = self._object_path(digest, ota_bytes, index)
        _need(ap_path is not None and ota_path is not None, "missing_assets", "导入的固件文件已缺失，请重新导入",
              profileID=profile_id, missingRoles=["ota"])
        item = self._generic_item(model, digest, source, index)
        recipe = {"id": profile_id, "model": model, "version": version, "ap": source["recipe"]["ap"],
                  "ota_sha256": digest, "partitions": source["recipe"]["partitions"],
                  "family": source["recipe"].get("family", "")}
        storage = "romfs" if any(p.get("name") == "vela_resource.bin" for p in recipe["partitions"]) else "yaffs"
        return {"version": 1, "profileID": profile_id, "item": item, "recipe": recipe, "storageKind": storage,
                "assets": {"ap": {"path": str(ap_path), "sha256": source["ap"], "bytes": source["apBytes"], "source": "imported"},
                           "ota": {"path": str(ota_path), "sha256": digest, "bytes": ota_bytes, "source": "imported"}},
                "catalogSHA256": self.catalog_sha256, "executionApproved": False}

    def resolve_profile(self, profile_id):
        """Prepared-catalog resolution; kept for developer-local bundled assets."""
        _need(profile_id in self.profiles, "unknown_profile", "请选择固件目录中的准确型号与版本")
        profile, index = self.profiles[profile_id], self._index()
        item = self._item(profile, index)
        _need(item["availability"]["assetsReady"], "missing_assets", item["availability"]["reason"],
              profileID=profile_id, missingRoles=item["availability"]["missingRoles"])
        assets = {}
        for role, digest in profile["assets"].items():
            path, source = self._asset(digest, index)
            if path is not None:
                assets[role] = {"path": str(path), "sha256": digest, "bytes": self.objects[digest]["bytes"], "source": source}
        return {"version": 1, "profileID": profile_id, "item": item, "assets": assets,
                "recipe": profile["recipe"], "storageKind": profile["storageKind"],
                "catalogSHA256": self.catalog_sha256,
                "executionApproved": False}

    def import_file(self, source, model=None):
        source = _path(source)
        self._index(create=True)
        root_fd = _owned_directory(self.library_root, create=True)
        objects_fd = lock = None
        stage_name, stage_fd, created = None, None, {}
        def anchored():
            for path, held in ((self.library_root, root_fd), (self.library_root / "objects", objects_fd)):
                check = _owned_directory(path)
                try:
                    before, after = os.fstat(held), os.fstat(check)
                    _need((before.st_dev, before.st_ino) == (after.st_dev, after.st_ino),
                          "library_changed", "导入期间私有固件库目录发生变化")
                finally:
                    os.close(check)
        try:
            objects_fd = _owned_directory(self.library_root / "objects", create=True)
            lock = os.open(".import.lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600, dir_fd=root_fd)
            info = os.fstat(lock)
            _need(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid(),
                  "unsafe_library", "私有固件库锁文件不安全")
            if stat.S_IMODE(info.st_mode) != 0o600:
                os.fchmod(lock, 0o600)
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise CatalogError("library_busy", "固件库正在导入，请稍后重试") from error
            # Abandoned staging from an interrupted import is cleared here; any
            # other entry is the user's and never blocks an import.
            _recover_library(root_fd)
            index = self._index()
            anchored()
            existing_total = 0
            existing_names = set()
            with os.scandir(objects_fd) as entries:
                    for entry in entries:
                        record = entry.stat(follow_symlinks=False)
                        # Only content-addressed regular files are firmware objects;
                        # anything else the user put here is ignored, never touched.
                        if not _sha(entry.name) or not stat.S_ISREG(record.st_mode) or record.st_uid != os.getuid():
                            continue
                        if stat.S_IMODE(record.st_mode) != 0o600:
                            fd = os.open(entry.name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=objects_fd)
                            try:
                                os.fchmod(fd, 0o600)
                            finally:
                                os.close(fd)
                        existing_total += record.st_size
                        existing_names.add(entry.name)
            fd, input_info = _open_file(source, MAX_ASSET)
            os.close(fd)
            _need(existing_total + input_info.st_size <= MAX_LIBRARY, "library_quota", "私有固件库已达到容量限制")
            stage_name = ".txn-" + uuid.uuid4().hex
            os.mkdir(stage_name, 0o700, dir_fd=root_fd)
            stage_fd = os.open(stage_name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=root_fd)
            actual = _copy(source, "input", directory_fd=stage_fd, created=created)
            # The user's own import is the only firmware source. No catalog lookup,
            # version list or license record decides what may be imported: any
            # complete OTA is inventoried structurally, and the run itself is what
            # reports whether the firmware works on the instance hardware preset.
            from .firmware_probe import ProbeError, ota_recipe
            input_fd = os.open("input", os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=stage_fd)
            try:
                # A complete OTA is either a plain ZIP or the BES full-package
                # envelope (0x60ZZ~ header, embedded per-member ZIP chain).
                _need(os.pread(input_fd, 4, 0) in (b"PK\x03\x04", b"\x60ZZ~"), "unknown_firmware",
                      "请导入完整 OTA 固件包（ZIP 或 BES 整包 bin，包含 vela_ap.bin 和资源）；单独的 AP 文件缺少启动所需的资源",
                      sha256=actual["sha256"])
                try:
                    generic = ota_recipe(input_fd, actual["bytes"])
                except (ProbeError, MError, ValueError, OSError, zlib.error) as error:
                    raise CatalogError("unknown_firmware", "固件包结构无法识别：" + str(error)[:240],
                                       {"sha256": actual["sha256"]}) from error
            finally:
                os.close(input_fd)
            ap_record = next(r for r in generic["partitions"] if r["name"].rsplit("/", 1)[-1] == "vela_ap.bin")
            entry = {"kind": "ota", "profileIDs": [], "members": [{"asset": generic["ap"]["sha256"], "record": ap_record}]}
            # Model binding only. The user's selected preset owns visibility:
            # the import is bound to that model and appears nowhere else. No
            # firmware capability is validated here — the structural inventory
            # above is all the import requires; the run itself reports whether
            # the firmware works on the instance hardware.
            bound_model = None
            if model:
                _need(model in self.hardware_models, "unknown_profile",
                      "此机型预设不存在，无法绑定导入的固件", model=model)
                bound_model = model
                entry["profileIDs"] = [model + "@" + generic["version"] + "@" + actual["sha256"][:12]]
            candidates = ([model + "@" + generic["version"] + "@" + actual["sha256"][:12]] if bound_model else
                          self._candidate_models(generic, actual["sha256"]))
            _need(existing_total + actual["bytes"] + sum(member["record"]["size"] for member in entry.get("members", [])) <= MAX_LIBRARY,
                  "library_quota", "固件包的临时展开将超过私有库容量限制")
            os.replace("input", actual["sha256"], src_dir_fd=stage_fd, dst_dir_fd=stage_fd)
            created[actual["sha256"]] = created.pop("input")
            staged = {actual["sha256"]: actual["bytes"]}
            source_fd = os.open(actual["sha256"], os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=stage_fd)
            try:
                info = os.fstat(source_fd)
                _need(stat.S_ISREG(info.st_mode) and (info.st_dev, info.st_ino) == created[actual["sha256"]]
                      and info.st_size == actual["bytes"], "input_changed", "暂存固件对象发生变化")
                for member in entry["members"]:
                    digest, record = member["asset"], member["record"]
                    if digest not in staged:
                        _extract_member(source_fd, record, digest, actual["bytes"], directory_fd=stage_fd, created=created)
                        staged[digest] = record["size"]
            finally:
                os.close(source_fd)
            _need(len(existing_names | set(staged)) <= MAX_LIBRARY_OBJECTS and len(index["imports"]) < MAX_LIBRARY_IMPORTS
                  and existing_total + sum(size for digest, size in staged.items()
                                           if digest not in existing_names) <= MAX_LIBRARY,
                  "library_quota", "导入后将超过私有固件库容量限制")
            anchored()
            for digest, size in staged.items():
                if digest in existing_names or not _publish_object(stage_fd, digest, objects_fd):
                    pin = _object_size(objects_fd, digest)
                    _need(pin["bytes"] == size, "library_changed", "现有固件对象大小与本次导入不一致")
                index["objects"][digest] = {"bytes": size}
            os.fsync(objects_fd)
            index["generation"] += 1
            record = {"sha256": actual["sha256"], "inputKind": entry["kind"],
                      "profileIDs": sorted(entry["profileIDs"]), "importedAtUnix": int(time.time()),
                      "generic": {"version": generic["version"], "family": generic["family"],
                                  "declaredModels": generic["declaredModels"],
                                  "boundModel": bound_model,
                                  "apSHA256": generic["ap"]["sha256"], "apBytes": generic["ap"]["size"],
                                  "recipe": {"ap": generic["ap"], "partitions": generic["partitions"],
                                             "family": generic["family"]}}}
            # Re-importing the same bytes replaces, never duplicates, its record.
            index["imports"] = [item for item in index["imports"]
                                if not (isinstance(item, dict) and item.get("sha256") == actual["sha256"])] + [record]
            # The sole visibility/commit point. Interrupted object publication
            # leaves unreferenced objects, never a partially visible profile.
            anchored()
            _atomic_json(self.library_root / "index.json", index, directory_fd=root_fd)
            anchored()
            # The receipt always carries the item matching every candidate ID:
            # the Swift decoder rejects a receipt whose items do not cover its
            # candidates, so build the bound item directly instead of relying
            # on the catalog listing's filtering.
            source_view = {"version": generic["version"], "ap": generic["ap"]["sha256"],
                           "apBytes": generic["ap"]["size"], "declaredModels": generic["declaredModels"],
                           "boundModel": bound_model,
                           "recipe": {"ap": generic["ap"], "partitions": generic["partitions"],
                                      "family": generic["family"]}}
            if bound_model:
                items = [self._generic_item(bound_model, actual["sha256"], source_view, index)]
            else:
                items = [item for item in self._generic_items(index, only=actual["sha256"])
                         if item["id"] in candidates]
            return {"version": 1, "recognized": True, "imported": True, "inputKind": entry["kind"],
                    "sha256": actual["sha256"],
                    "needsSelection": bound_model is None and len(candidates) > 1,
                    "candidateIDs": candidates, "items": items}
        finally:
            if stage_fd is not None:
                for name, identity in created.items():
                    try:
                        info = os.stat(name, dir_fd=stage_fd, follow_symlinks=False)
                        if (info.st_dev, info.st_ino) == identity:
                            os.unlink(name, dir_fd=stage_fd)
                    except FileNotFoundError:
                        pass
                try:
                    named, held = os.stat(stage_name, dir_fd=root_fd, follow_symlinks=False), os.fstat(stage_fd)
                    if (named.st_dev, named.st_ino) == (held.st_dev, held.st_ino):
                        os.rmdir(stage_name, dir_fd=root_fd)
                except OSError:
                    pass  # preserve unexpected entries; never enumerate/delete them
                os.close(stage_fd)
            if lock is not None:
                os.close(lock)
            if objects_fd is not None:
                os.close(objects_fd)
            os.close(root_fd)

    def delete_import(self, sha256, force=False):
        """Remove one imported OTA and the object files only it references.

        The OTA object and any extracted member objects that no other import
        shares are unlinked; objects still referenced by another import stay.
        When the catalog has drifted or the import record is missing, delete is
        still attempted (force): the object file named by the SHA-256 is reclaimed
        if no other import depends on it, so a stale index never blocks removal.
        The commit point is a single atomic index write.
        """
        _need(_sha(sha256), "invalid_sha", "固件标识无效")
        if self.library_root is None:
            raise CatalogError("library_required", "尚未配置私有固件库")
        root_fd = _owned_directory(self.library_root)
        lock = objects_fd = None
        try:
            lock = os.open(".import.lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600, dir_fd=root_fd)
            info = os.fstat(lock)
            _need(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid(),
                  "unsafe_library", "私有固件库锁文件不安全")
            if stat.S_IMODE(info.st_mode) != 0o600:
                os.fchmod(lock, 0o600)
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise CatalogError("library_busy", "固件库正在导入或清理，请稍后重试") from error
            # Abandoned staging from an interrupted import is cleared here; any
            # other entry is the user's and never blocks a delete.
            _recover_library(root_fd)
            index = self._index()
            import_records = [it for it in index["imports"]
                              if isinstance(it, dict) and it.get("sha256") == sha256]
            if not import_records:
                if not force:
                    _need(False, "not_found", "固件库中找不到该固件")
                # Force: the catalog lost the record (or it was never indexed).
                # Reclaim the object file only if nothing else still needs it.
                referenced = set()
                for it in index["imports"]:
                    if not isinstance(it, dict):
                        continue
                    referenced.add(it.get("sha256"))
                    for member in it.get("members", []):
                        referenced.add(member.get("asset") if isinstance(member, dict) else None)
                remove = [sha256] if (sha256 not in referenced and sha256 in index["objects"]) else []
                objects_fd = _owned_directory(self.library_root / "objects", create=True)
                try:
                    for digest in remove:
                        try:
                            os.unlink(digest, dir_fd=objects_fd)
                        except FileNotFoundError:
                            pass
                        index["objects"].pop(digest, None)
                finally:
                    os.close(objects_fd)
                    objects_fd = None
                index["imports"] = [it for it in index["imports"]
                                    if not (isinstance(it, dict) and it.get("sha256") == sha256)]
                index["generation"] += 1
                _atomic_json(self.library_root / "index.json", index, directory_fd=root_fd)
                return {"ok": True, "version": 1, "removed": remove, "forced": not remove}
            # Only the objects owned exclusively by these records are reclaimed;
            # any object another import also references stays intact. Generic
            # OTA imports own their AP object too (generic.apSHA256); without
            # it every delete would orphan a 10-15 MiB object file.
            owned = {sha256}
            for rec in import_records:
                generic = rec.get("generic") if isinstance(rec, dict) else None
                if isinstance(generic, dict) and _sha(generic.get("apSHA256")):
                    owned.add(generic["apSHA256"])
                for member in rec.get("members", []):
                    if isinstance(member, dict) and _sha(member.get("asset")):
                        owned.add(member["asset"])
            protected = set()
            for it in index["imports"]:
                if not isinstance(it, dict) or it.get("sha256") == sha256:
                    continue
                protected.add(it.get("sha256"))
                generic = it.get("generic") if isinstance(it, dict) else None
                if isinstance(generic, dict) and _sha(generic.get("apSHA256")):
                    protected.add(generic["apSHA256"])
                for member in it.get("members", []):
                    if isinstance(member, dict):
                        protected.add(member.get("asset"))
            remove = sorted(digest for digest in owned if digest in index["objects"] and digest not in protected)
            objects_fd = _owned_directory(self.library_root / "objects", create=True)
            try:
                for digest in remove:
                    try:
                        os.unlink(digest, dir_fd=objects_fd)
                    except FileNotFoundError:
                        pass
                    index["objects"].pop(digest, None)
            finally:
                os.close(objects_fd)
                objects_fd = None
            index["imports"] = [it for it in index["imports"]
                                if not (isinstance(it, dict) and it.get("sha256") == sha256)]
            index["generation"] += 1
            _atomic_json(self.library_root / "index.json", index, directory_fd=root_fd)
            return {"ok": True, "version": 1, "removed": remove}
        finally:
            if objects_fd is not None:
                os.close(objects_fd)
            if lock is not None:
                os.close(lock)
            os.close(root_fd)

    def delete_all_imports(self):
        """Remove every imported firmware and reclaim all library object files.

        The bundled catalog lives in the Engine, not the library, so every object
        here belongs to an import; clearing the index and the objects directory
        leaves the library empty and ready for fresh imports. The commit point is
        a single atomic index write.
        """
        if self.library_root is None:
            raise CatalogError("library_required", "尚未配置私有固件库")
        root_fd = _owned_directory(self.library_root)
        lock = objects_fd = None
        try:
            lock = os.open(".import.lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600, dir_fd=root_fd)
            info = os.fstat(lock)
            _need(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid(),
                  "unsafe_library", "私有固件库锁文件不安全")
            if stat.S_IMODE(info.st_mode) != 0o600:
                os.fchmod(lock, 0o600)
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise CatalogError("library_busy", "固件库正在导入或清理，请稍后重试") from error
            _recover_library(root_fd)
            index = self._index()
            removed = []
            objects_fd = _owned_directory(self.library_root / "objects", create=True)
            try:
                for digest in list(index.get("objects", {})):
                    if not _sha(digest):
                        continue
                    try:
                        os.unlink(digest, dir_fd=objects_fd)
                    except FileNotFoundError:
                        pass
                    removed.append(digest)
            finally:
                os.close(objects_fd)
                objects_fd = None
            index["objects"] = {}
            index["imports"] = []
            index["generation"] += 1
            _atomic_json(self.library_root / "index.json", index, directory_fd=root_fd)
            return {"ok": True, "version": 1, "removed": removed}
        finally:
            if objects_fd is not None:
                os.close(objects_fd)
            if lock is not None:
                os.close(lock)
            os.close(root_fd)


def from_handoff_catalog(project, prepared_by_profile):
    """Static packager input: return portable catalog and explicit source map.

    No files are copied and no preparation/build code is imported. The packager
    supplies reviewed existing board/resource/factory/seed evidence per profile.
    Missing preparation remains missing in the returned catalog.
    """
    from .minative import _catalog
    project = _path(project)
    source = _catalog(project)
    objects, sources, profiles, imports, pin_cache = {}, {}, [], {}, {}
    def register(path):
        path = _path(path)
        if path not in pin_cache:
            pin_cache[path] = _pin(path)
        pin = pin_cache[path]
        digest = pin["sha256"]
        objects[digest] = {"path": "objects/" + digest, "bytes": pin["bytes"],
                           "localAllowed": True, "publicAllowed": False, "rightsEvidence": []}
        sources[digest] = pin["path"]
        return digest
    def metadata(digest, size):
        _need(_sha(digest) and type(size) is int and 0 < size <= MAX_ASSET, "invalid_catalog", "源固件元数据无效")
        objects.setdefault(digest, {"path": "objects/" + digest, "bytes": size,
                                   "localAllowed": True, "publicAllowed": False, "rightsEvidence": []})
    for key in sorted(INCLUDED_PROFILE_IDS):
        row = source[key]
        assets = {}
        metadata(row["ap"]["sha256"], row["ap"]["size"])
        assets["ap"] = row["ap"]["sha256"]
        if Path(row["ap_path"]).is_file():
            _need(register(row["ap_path"]) == assets["ap"], "asset_hash_mismatch", "原始 AP 哈希不符")
        imports.setdefault(assets["ap"], {"sha256": assets["ap"], "bytes": row["ap"]["size"],
                                          "kind": "ap", "profileIDs": [], "members": []})["profileIDs"].append(key)
        if row.get("ota_path"):
            ota = register(row["ota_path"])
            _need(ota == row["ota_sha256"], "asset_hash_mismatch", "原始完整固件包哈希不符")
            assets["ota"] = ota
            item = imports.setdefault(ota, {"sha256": ota, "bytes": objects[ota]["bytes"],
                                            "kind": "ota", "profileIDs": [], "members": []})
            item["profileIDs"].append(key)
            for record in row.get("partitions", []):
                if record.get("name") in ("vela_ap.bin", "vela_resource.bin") and "header_offset" in record:
                    digest = record["sha256"]
                    metadata(digest, record["size"])
                    if not any(m["asset"] == digest for m in item["members"]):
                        item["members"].append({"asset": digest, "record": record})
        storage = "romfs" if any(p.get("name") == "vela_resource.bin" for p in row["partitions"]) else "yaffs"
        prepared = prepared_by_profile.get(key, {})
        for role, path in prepared.items():
            _need(role in ROLES - {"ap", "ota"}, "invalid_prepared", "未知预备固件资产角色")
            assets[role] = register(path)
        if "resource" not in assets and row.get("resource_path"):
            assets["resource"] = register(row["resource_path"])
        if "resource" not in assets and storage == "romfs":
            record = next(p for p in row["partitions"] if p.get("name") == "vela_resource.bin")
            if record.get("sha256"):
                metadata(record["sha256"], record["size"])
                assets["resource"] = record["sha256"]
        width, height = _geometry(row["model"])
        recipe = {name: row[name] for name in ("id", "model", "version", "ap", "ota_sha256", "partitions") if name in row}
        profiles.append({"id": key, "displayName": "小米手环 11 · 4.100.177" if key == "q66@4.100.177" else key,
                         "model": row["model"], "version": row["version"], "width": width, "height": height,
                         "storageKind": storage, "assets": assets, "recipe": recipe})
    for entry in profiles:
        entry["displayName"] = display_name(entry)
    return {"catalog": {"schema": SCHEMA, "version": 1, "objects": objects,
                        "profiles": profiles, "imports": list(imports.values())},
            "sourcePaths": sources}
