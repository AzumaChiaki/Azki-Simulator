"""Content-addressed firmware inventory/import; no executable is run here.

Catalog v1: {schema:'azki.simulator.firmware.v1', version:1, objects:{sha:
{path:'objects/<sha>', bytes:int, localAllowed:bool, publicAllowed:bool,
rightsEvidence:[str]}}, profiles:[{id,displayName,model,version,width,height,
storageKind,assets:{role:sha},recipe:{original non-path catalog fields}}],
imports:[{sha256,bytes,kind:'ap'|'ota',profileIDs:[exact-id],members:[
{asset:sha,record:{original ZIP local-header inventory}}]}]}.

Bundled assets and user-imported private objects have separate distribution
rules. Public distribution never turns a user's local import into a redistributable
asset. A catalog package is recognized by its exact SHA; any other complete OTA is
statically inventoried (imports[].generic) and offered on every hardware preset,
with boot compatibility decided per launch by check_compatibility().
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
MAX_EXPANDED = 1024 * 1024 * 1024
MAX_LIBRARY = 6 * 1024 * 1024 * 1024
MAX_OBJECTS = 512
EXECUTION_PROFILES = INCLUDED_PROFILE_IDS
# Profiles whose board/factory/resource are pre-pinned catalog objects keep "prepared" mode;
# every other profile is "generated": the sandboxed runner derives storage from the pinned AP/OTA.
PRODUCT_NAMES = {
    "n66": "小米手环 9", "n67": "小米手环 9 Pro", "o66": "小米手环 10", "p67": "小米手环 10 Pro", "q66": "小米手环 11"}
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


def check_compatibility(model, recipe, ap_path):
    """Static launch gate for imported firmware on an instance hardware preset.

    Mirrors only what the runner can model: BES1503 boot header, a ROMFS
    resource package or a YAFFS layout of a supported product family, and
    the product family named by the AP itself. Everything else boots and
    reports its own runtime outcome.
    """
    name = display_name({"id": model, "model": model, "version": ""}).rstrip(" ·") or model
    ap = recipe["ap"]
    def refuse(reason):
        raise CatalogError("firmware_incompatible", "此固件无法在「" + name + "」硬件预设上启动：" + reason,
                           {"model": model, "version": recipe.get("version")})
    if ap.get("chip") != "best1503":
        refuse("芯片为 " + str(ap.get("chip")) + "，模拟核心只实现了 BES1503")
    if not all(type(ap.get(key)) is int for key in ("ap_offset", "flash_bytes", "reset_entry", "reset_sp")):
        refuse("AP 启动头无法识别（" + str(ap.get("backend_requirement", ""))[:160] + "）")
    names = [record["name"] for record in recipe["partitions"]]
    if names.count("vela_resource.bin") > 1:
        refuse("固件包含多个 ROMFS 资源分区")
    if "vela_resource.bin" not in names:
        if not model.startswith(("miwear.watch.n66", "miwear.watch.o66", "miwear.watch.n67")):
            refuse("固件使用多分区 YAFFS 资源，此硬件预设只支持 ROMFS 资源包（vela_resource.bin）")
        if not model.startswith("miwear.watch.n67") and "ota.json" not in names:
            refuse("多分区固件缺少 ota.json 资源清单")
    code = model.rsplit(".", 1)[-1]
    family = code[:3]
    declared = set()
    data = Path(ap_path).read_bytes() if ap_path else b""
    for match in re.finditer(rb"miwear\.watch\.([a-z0-9]{3,16})(?![a-z0-9])", data):
        declared.add(match[1].decode("ascii"))
    if recipe.get("family"):
        declared.add(str(recipe["family"]))
    if declared and not any(item[:3] == family for item in declared):
        families = sorted({item[:3] for item in declared})
        products = "、".join(PRODUCT_NAMES.get(item, item) for item in families)
        refuse("固件声明的设备为 " + products + "，与实例的 " + PRODUCT_NAMES.get(family, family) + " 不一致")
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


def _copy(source, target, expected=None, *, directory_fd=None, created=None, hash_content=True):
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
            result["sha256"] = digest.hexdigest()
        if expected:
            _need(result == expected, "asset_hash_mismatch", "固件内容与已识别版本不一致")
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


def _object_pin(directory, name):
    fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory)
    try:
        before = os.fstat(fd)
        _need(stat.S_ISREG(before.st_mode) and before.st_uid == os.getuid() and before.st_nlink == 1
              and stat.S_IMODE(before.st_mode) == 0o600 and before.st_size <= MAX_ASSET,
              "unsafe_library", "现有固件对象不是私有普通文件")
        digest, total = hashlib.sha256(), 0
        while chunk := os.read(fd, 1024 * 1024):
            total += len(chunk)
            _need(total <= MAX_ASSET, "input_quota", "现有固件对象超过大小限制")
            digest.update(chunk)
        after = os.fstat(fd)
        _need((before.st_size, before.st_mtime_ns, before.st_ctime_ns) ==
              (after.st_size, after.st_mtime_ns, after.st_ctime_ns) and total == before.st_size,
              "input_changed", "现有固件对象在导入期间发生变化")
        return {"sha256": digest.hexdigest(), "bytes": total}
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
    _need(value.get("sha256") == asset and _sha(asset) and
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
    # Bits 1-2 only describe the DEFLATE level; encryption/data descriptors stay rejected.
    _need(magic == b"PK\x03\x04" and flags & ~0x806 == 0 and method in (0, 8)
          and data_offset == record["data_offset"] and data_offset + packed <= source_size
          and (local_packed, local_size, method, f"{crc:08x}") ==
          (packed, size, record["compression_method"], record["crc32"]),
          "archive_format", "固件包成员与已校验清单不一致")
    name = os.pread(source_fd, names, offset + 30)
    _need(name == record["name"].encode("utf-8"), "archive_format", "固件包成员名称与清单不一致")
    # Member names are validated for provenance only, never used as host paths.
    descriptor = os.open(target, os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW, 0o600, dir_fd=directory_fd)
    if created is not None:
        info = os.fstat(descriptor)
        created[str(target)] = (info.st_dev, info.st_ino)
    digest, checksum, total = hashlib.sha256(), 0, 0
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
                    digest.update(block)
                    checksum = zlib.crc32(block, checksum)
                    out.write(block)
                    if decoder:
                        _need(not decoder.unused_data, "archive_format", "固件压缩成员含额外数据")
            _need((decoder is None or (decoder.eof and not decoder.unconsumed_tail)) and total == size
                  and checksum == crc and digest.hexdigest() == record["sha256"],
                  "asset_hash_mismatch", "展开的固件成员未通过大小、CRC 或 SHA-256 校验")
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

    def _recheck_catalog(self):
        _need(_pin(self.bundle_root / "catalog.json")["sha256"] == self.catalog_sha256,
              "catalog_changed", "固件目录已变化，请重新读取后再操作")

    def _index(self, create=False):
        empty = {"version": 1, "generation": 0, "objects": {}, "imports": []}
        if self.library_root is None:
            _need(not create, "library_required", "尚未配置私有固件库")
            return empty
        if not self.library_root.exists():
            if not create:
                return empty
            parent = _directory_fd(self.library_root.parent)
            try:
                os.mkdir(self.library_root.name, 0o700, dir_fd=parent)
            finally:
                os.close(parent)
        root = _directory_fd(self.library_root, private=True)
        try:
            if create:
                try:
                    os.mkdir("objects", 0o700, dir_fd=root)
                except FileExistsError:
                    pass
                directory = _directory_fd(self.library_root / "objects", private=True)
                os.close(directory)
        finally:
            os.close(root)
        try:
            index = _json(self.library_root / "index.json", private=True)
        except FileNotFoundError:
            return empty
        _need(index.get("version") == 1 and type(index.get("generation")) is int and 0 <= index["generation"] < 2**63
              and isinstance(index.get("objects"), dict) and len(index["objects"]) <= MAX_OBJECTS
              and isinstance(index.get("imports"), list) and len(index["imports"]) <= 128,
              "invalid_library", "私有固件库索引无效")
        for digest, record in index["objects"].items():
            _need(_sha(digest) and isinstance(record, dict) and type(record.get("bytes")) is int
                  and 0 < record["bytes"] <= MAX_ASSET, "invalid_library", "私有固件资产记录无效")
        return index

    def _asset(self, digest, index, verify=False):
        item = self.objects[digest]
        options = []
        if self.library_root and digest in index["objects"] and item["localAllowed"]:
            _need(index["objects"][digest]["bytes"] == item["bytes"], "asset_hash_mismatch", "私有资产大小与目录不一致")
            options.append((self.library_root / "objects" / digest, "imported"))
        allowed = item["localAllowed"] if self.distribution == "local" else item["publicAllowed"]
        if allowed:
            options.append((self.bundle_root / item["path"], "bundled"))
        for path, source in options:
            try:
                fd, info = _open_file(path, MAX_ASSET, private=source == "imported")
            except FileNotFoundError:
                continue
            os.close(fd)
            _need(info.st_size == item["bytes"], "asset_hash_mismatch", "固件资产大小发生变化", sha256=digest)
            if verify:
                _need(_pin(path)["sha256"] == digest, "asset_hash_mismatch", "固件资产未通过 SHA-256 校验", sha256=digest)
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
        return sorted({profile["model"] for profile in self.profiles.values()})

    def _object_path(self, digest, size, index):
        """Library (or permitted bundled) object path for an exact size, else None."""
        if digest in self.objects:
            return self._asset(digest, index)[0]
        record = index["objects"].get(digest)
        if self.library_root is None or record is None or record.get("bytes") != size:
            return None
        path = self.library_root / "objects" / digest
        try:
            fd, info = _open_file(path, MAX_ASSET, private=True)
        except FileNotFoundError:
            return None
        os.close(fd)
        _need(info.st_size == size, "asset_hash_mismatch", "导入固件对象大小发生变化", sha256=digest)
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
                                   "apBytes": generic["apBytes"], "recipe": recipe}
            elif digest in self.imports and self.imports[digest]["kind"] == "ota":
                profile = self.profiles[sorted(self.imports[digest]["profileIDs"])[0]]
                ap = profile["assets"]["ap"]
                sources[digest] = {"version": profile["version"], "ap": ap, "apBytes": self.objects[ap]["bytes"],
                                   "recipe": {"ap": profile["recipe"]["ap"],
                                              "partitions": profile["recipe"].get("partitions", []), "family": ""}}
        return sources

    def _generic_item(self, model, digest, source, index):
        profile_id = model + "@" + source["version"] + "@" + digest[:12]
        width, height = _geometry(model)
        ready = (self._object_path(source["ap"], source["apBytes"], index) is not None
                 and self._object_path(digest, index["objects"].get(digest, {}).get("bytes", -1), index) is not None)
        return {"id": profile_id, "displayName": display_name({"id": profile_id, "model": model,
                                                                "version": source["version"]}) + " · " + digest[:8],
                "device": model, "model": model, "firmwareVersion": source["version"],
                "width": width, "height": height,
                "availability": {"ready": ready, "assetsReady": ready,
                                 "reason": "" if ready else "导入的固件文件已缺失，请重新导入",
                                 "missingRoles": [] if ready else ["ota"], "verification": "checked_at_launch"},
                "source": "imported"}

    def _generic_items(self, index, only=None):
        items = []
        for digest, source in sorted(self._generic_sources(index).items()):
            if only is not None and digest != only:
                continue
            for model in self.hardware_models:
                item = self._generic_item(model, digest, source, index)
                if item["id"] not in self.profiles:
                    items.append(item)
        return items

    def list_items(self):
        self._recheck_catalog()
        index = self._index()
        return [self._item(self.profiles[key], index) for key in sorted(self.profiles)] + self._generic_items(index)

    def resolve_generic(self, profile_id):
        """Runner recipe for imported firmware on the profile's hardware preset."""
        self._recheck_catalog()
        parsed = generated_profile(profile_id)
        _need(parsed is not None and profile_id not in self.profiles, "unknown_profile", "请选择固件库中的固件")
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

    def resolve_profile(self, profile_id, *, verify=True):
        self._recheck_catalog()
        _need(profile_id in self.profiles, "unknown_profile", "请选择固件目录中的准确型号与版本")
        profile, index = self.profiles[profile_id], self._index()
        item = self._item(profile, index)
        _need(item["availability"]["assetsReady"], "missing_assets", item["availability"]["reason"],
              profileID=profile_id, missingRoles=item["availability"]["missingRoles"])
        assets = {}
        for role, digest in profile["assets"].items():
            path, source = self._asset(digest, index, verify=verify)
            if path is not None:
                assets[role] = {"path": str(path), "sha256": digest, "bytes": self.objects[digest]["bytes"], "source": source}
        return {"version": 1, "profileID": profile_id, "item": item, "assets": assets,
                "recipe": profile["recipe"], "storageKind": profile["storageKind"],
                "catalogSHA256": self.catalog_sha256,
                "executionApproved": False}

    def import_file(self, source, profile_id=None):
        self._recheck_catalog()
        source = _path(source)
        self._index(create=True)
        root_fd = _directory_fd(self.library_root, private=True)
        objects_fd = lock = None
        stage_name, stage_fd, created = None, None, {}
        def anchored():
            for path, held in ((self.library_root, root_fd), (self.library_root / "objects", objects_fd)):
                check = _directory_fd(path, private=True)
                try:
                    before, after = os.fstat(held), os.fstat(check)
                    _need((before.st_dev, before.st_ino) == (after.st_dev, after.st_ino),
                          "library_changed", "导入期间私有固件库目录发生变化")
                finally:
                    os.close(check)
        try:
            objects_fd = _directory_fd(self.library_root / "objects", private=True)
            lock = os.open(".import.lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600, dir_fd=root_fd)
            info = os.fstat(lock)
            _need(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid() and info.st_nlink == 1
                  and stat.S_IMODE(info.st_mode) == 0o600, "unsafe_library", "私有固件库锁文件不安全")
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise CatalogError("library_busy", "固件库正在导入，请稍后重试") from error
            with os.scandir(root_fd) as entries:
                for number, entry in enumerate(entries):
                    _need(number < 16 and entry.name in {"objects", "index.json", ".import.lock"},
                          "library_recovery_required", "固件库有未完成事务，请先恢复或保留后检查")
            index = self._index()
            anchored()
            existing_total = 0
            existing_names = set()
            with os.scandir(objects_fd) as entries:
                    count = 0
                    for entry in entries:
                        count += 1
                        record = entry.stat(follow_symlinks=False)
                        _need(count <= MAX_OBJECTS and _sha(entry.name) and stat.S_ISREG(record.st_mode)
                              and record.st_uid == os.getuid() and record.st_nlink == 1 and stat.S_IMODE(record.st_mode) == 0o600,
                              "unsafe_library", "私有固件库包含异常文件或超过对象数量限制")
                        existing_total += record.st_size
                        existing_names.add(entry.name)
            fd, input_info = _open_file(source, MAX_ASSET)
            os.close(fd)
            _need(existing_total + input_info.st_size <= MAX_LIBRARY, "library_quota", "私有固件库已达到容量限制")
            stage_name = ".txn-" + uuid.uuid4().hex
            os.mkdir(stage_name, 0o700, dir_fd=root_fd)
            stage_fd = os.open(stage_name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=root_fd)
            actual = _copy(source, "input", directory_fd=stage_fd, created=created)
            entry = self.imports.get(actual["sha256"])
            generic = None
            if entry is None or entry["bytes"] != actual["bytes"]:
                # Not a catalog package: any complete OTA is accepted. Whether it
                # boots is decided per launch against the instance hardware preset.
                from .firmware_probe import ProbeError, ota_recipe
                input_fd = os.open("input", os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=stage_fd)
                try:
                    _need(os.pread(input_fd, 4, 0) == b"PK\x03\x04", "unknown_firmware",
                          "请导入完整 OTA 固件包（ZIP，包含 vela_ap.bin 和资源）；单独的 AP 文件缺少启动所需的资源",
                          sha256=actual["sha256"])
                    try:
                        generic = ota_recipe(input_fd, actual["bytes"])
                    except (ProbeError, MError, ValueError, OSError, zlib.error) as error:
                        raise CatalogError("unknown_firmware", "固件包结构无法识别：" + str(error)[:240],
                                           {"sha256": actual["sha256"]}) from error
                finally:
                    os.close(input_fd)
                ap_record = next(r for r in generic["partitions"] if r["name"].rsplit("/", 1)[-1] == "vela_ap.bin")
                _need(ap_record["sha256"] == generic["ap"]["sha256"], "asset_hash_mismatch", "AP 成员与分析结果不一致")
                entry = {"kind": "ota", "profileIDs": [], "members": [{"asset": ap_record["sha256"], "record": ap_record}]}
            generic_ids = [model + "@" + (generic["version"] if generic else
                                          self.profiles[sorted(entry["profileIDs"])[0]]["version"]) + "@" + actual["sha256"][:12]
                           for model in self.hardware_models] if entry["kind"] == "ota" else []
            candidates = sorted(set(entry["profileIDs"]) | {x for x in generic_ids if x not in self.profiles})
            _need(profile_id is None or profile_id in candidates, "profile_mismatch", "此固件不属于所选配置", candidateIDs=candidates)
            _need(generic is not None or self.objects[actual["sha256"]]["localAllowed"], "local_use_unavailable", "此固件尚无本地使用许可记录")
            _need(existing_total + actual["bytes"] + sum(member["record"]["size"] for member in entry.get("members", [])) <= MAX_LIBRARY,
                  "library_quota", "固件包的临时展开将超过私有库容量限制")
            os.replace("input", actual["sha256"], src_dir_fd=stage_fd, dst_dir_fd=stage_fd)
            created[actual["sha256"]] = created.pop("input")
            staged = {actual["sha256"]: actual["bytes"]}
            if entry["kind"] == "ota":
                source_fd = os.open(actual["sha256"], os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=stage_fd)
                try:
                    info = os.fstat(source_fd)
                    _need(stat.S_ISREG(info.st_mode) and (info.st_dev, info.st_ino) == created[actual["sha256"]]
                          and info.st_size == actual["bytes"], "input_changed", "暂存固件对象发生变化")
                    for member in entry.get("members", []):
                        digest, record = member["asset"], member["record"]
                        if digest not in staged:
                            _need(generic is not None or self.objects[digest]["localAllowed"], "local_use_unavailable",
                                  "固件包成员缺少本地使用记录")
                            _extract_member(source_fd, record, digest, actual["bytes"], directory_fd=stage_fd, created=created)
                            staged[digest] = record["size"]
                finally:
                    os.close(source_fd)
            _need(len(existing_names | set(staged)) <= MAX_OBJECTS and len(index["imports"]) < 128
                  and existing_total + sum(size for digest, size in staged.items()
                                           if digest not in existing_names) <= MAX_LIBRARY,
                  "library_quota", "导入后将超过私有固件库容量限制")
            anchored()
            for digest, size in staged.items():
                if digest in existing_names or not _publish_object(stage_fd, digest, objects_fd):
                    pin = _object_pin(objects_fd, digest)
                    _need(pin["sha256"] == digest and pin["bytes"] == size, "asset_hash_mismatch", "现有固件对象损坏，已停止导入")
                index["objects"][digest] = {"bytes": size}
            os.fsync(objects_fd)
            index["generation"] += 1
            record = {"sha256": actual["sha256"], "inputKind": entry["kind"],
                      "profileIDs": sorted(entry["profileIDs"]), "importedAtUnix": int(time.time())}
            if generic is not None:
                record["generic"] = {"version": generic["version"], "family": generic["family"],
                                     "declaredModels": generic["declaredModels"],
                                     "apSHA256": generic["ap"]["sha256"], "apBytes": generic["ap"]["size"],
                                     "recipe": {"ap": generic["ap"], "partitions": generic["partitions"],
                                                "family": generic["family"]}}
            # Re-importing the same bytes replaces, never duplicates, its record.
            index["imports"] = [item for item in index["imports"]
                                if not (isinstance(item, dict) and item.get("sha256") == actual["sha256"])] + [record]
            # The sole visibility/commit point. Interrupted object publication
            # leaves unreferenced objects, never a partially visible profile.
            anchored()
            _atomic_json(self.library_root / "index.json", index, directory_fd=root_fd)
            anchored()
            chosen = [profile_id] if profile_id else candidates
            return {"version": 1, "recognized": True, "imported": True, "inputKind": entry["kind"],
                    "sha256": actual["sha256"], "needsSelection": profile_id is None and len(candidates) > 1,
                    "candidateIDs": candidates,
                    "items": [self._item(self.profiles[key], index) for key in chosen if key in self.profiles]
                             + [item for item in self._generic_items(index, only=actual["sha256"]) if item["id"] in chosen]}
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
