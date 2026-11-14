"""Static firmware container inventory and boot-header diagnostics."""
from __future__ import annotations

import hashlib
import io
import json
import os
from pathlib import PurePosixPath
import re
import stat
import struct
import zipfile
import zlib

from .minative import _open_file, _path

MAX_INPUT = 768 * 1024 * 1024
MAX_AP = 64 * 1024 * 1024
MAX_EXPANDED = 1024 * 1024 * 1024
MAX_MEMBERS = 4096
BES_ENVELOPE_MAGIC = b"\x60ZZ~"


class ProbeError(ValueError):
    code = "firmware_inspection_failed"


def need(condition, message):
    if not condition:
        raise ProbeError(message)


def inspect_ap(data):
    """Recover header fields from these bytes; never reuse a previous version."""
    need(len(data) <= MAX_AP, "AP 超过静态分析大小限制")
    result = {"sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data),
              "chip": "unidentified", "headerRecognized": False}
    if len(data) < 128:
        return result
    stack, entry = struct.unpack_from("<II", data)
    if (0x60000000 <= stack <= 0x62000000 and stack % 8 == 0
            and entry & 1 and 0x605c0000 <= (entry & ~1) < 0x605c0000 + len(data)):
        result.update(chip="xring", headerRecognized=True, imageBase=0x605c0000,
                      resetEntry=entry, resetSP=stack)
        return result
    matches = re.finditer(rb"(?<=\n)CHIP=([^\r\n\x00 ]{1,32})(?=[\r\n\x00 ])", data)
    match = next(matches, None)
    if match is None or next(matches, None) is not None:
        result["diagnostic"] = "缺少唯一芯片头，不能推断 CPU 或复用其他版本的启动地址"
        return result
    chip = match[1].decode("ascii", "replace")
    result["chip"] = chip
    block = data[match.start():match.start() + 2048].split(b"\0", 1)[0]
    pairs = re.findall(rb"(?:^|\n)([A-Z_0-9]+)=([^\r\n]+)", block)
    if len({key for key, _ in pairs}) != len(pairs):
        result["diagnostic"] = "芯片头有重复字段"
        return result
    build = {key.decode("ascii"): value.decode("ascii", "replace") for key, value in pairs}
    result["build"] = build
    if chip not in ("best1503", "best1502p"):
        result["diagnostic"] = "当前核心尚未实现此芯片，保留版本身份用于适配"
        return result
    try:
        offset, flash = int(build["OTA_CODE_OFFSET"], 0), int(build["FLASH_SIZE"], 0)
        need(0 <= offset < flash <= 64 * 1024 * 1024 and offset % 4096 == 0,
             "AP 分区或 flash 边界无效")
        need(len(data) <= flash - offset, "AP 超出声明的 flash 分区")
        alias = struct.unpack_from("<I", data, 12)[0] - (match.start() - 1)
        need(0x28000000 <= alias < 0x30000000, "BES 地址别名无效")
        first = struct.unpack_from("<H", data, 16)[0]
        need(first & 0xff00 == 0x4800, "BES 启动序列尚未适配")
        literal = 0x14 + (first & 255) * 4
        need(literal + 4 <= len(data), "BES 启动常量越界")
        value = struct.unpack_from("<I", data, literal)[0]
        if data[18:20] == b"\x00\x47":
            candidates = []
            for at in range(20, 48, 2):
                ins = struct.unpack_from("<H", data, at)[0]
                if ins & 0xff00 == 0x4800 and data[at + 2:at + 6] == b"\x80\xf3\x08\x88":
                    literal = ((at + 4) & ~3) + (ins & 255) * 4
                    need(literal + 4 <= len(data), "MSP 常量越界")
                    candidates.append(struct.unpack_from("<I", data, literal)[0])
            need(len(candidates) == 1, "无法唯一恢复 MSP")
            stack, entry = candidates[0], value - 4
        else:
            need(data[18:22] == b"\x80\xf3\x08\x88", "BES MSP 启动序列尚未适配")
            stack, entry = value, alias + 17
        need(0 < entry <= 0xffffffff and entry & 1, "BES Thumb 启动地址无效")
        # Match the selected BEST1503 core's reset-sp geometry bound. A broad
        # 0x20000000..0x40000000 range also includes flash and unmapped memory.
        ram_end = 0x201a0000 if chip == "best1502p" else 0x20160000
        need(0x20000000 <= stack <= ram_end and stack % 8 == 0,
             "MSP 不在已建模 RAM 范围")
        result.update(headerRecognized=True, apOffset=offset, flashBytes=flash,
                      dataAlias=alias, resetEntry=entry, resetSP=stack)
    except (KeyError, ValueError, struct.error) as error:
        result["diagnostic"] = "启动头需要适配：" + str(error)[:160]
    return result


def _zip_bounds(fd, size):
    """Central directory of a complete OTA; a signed trailer is expected.

    Official signed packages append a signature block after the directory, so
    the directory record is located by position instead of by requiring the
    ZIP to end exactly at the last byte.
    """
    tail = os.pread(fd, min(size, 65557), max(0, size - 65557))
    end = tail.rfind(b"PK\x05\x06")
    need(end >= 0 and end + 22 <= len(tail), "完整 ZIP 目录缺失；增量包或其他容器需要对应适配器")
    magic, disk, start_disk, count_disk, count, length, offset, comment = struct.unpack_from("<4s4H2IH", tail, end)
    absolute = size - len(tail) + end
    need(end + 22 + comment <= len(tail) and disk == start_disk == 0 and count_disk == count
         and 0 < count <= MAX_MEMBERS and 0 < length <= 4 * 1024 * 1024
         and offset + length <= absolute, "不支持分卷、ZIP64 或超限目录")
    need(os.pread(fd, 4, offset) == b"PK\x01\x02", "ZIP 中央目录位置与记录不一致")
    return count, offset


def inventory(fd, size):
    """Structural OTA inventory; only the AP image is ever decompressed.

    Returns the AP bytes together with the member count, the expanded size the
    package declares and its resource layout. Member payloads are never read,
    so an import costs what the file costs, not what it expands to.
    """
    count, central = _zip_bounds(fd, size)
    with os.fdopen(os.dup(fd), "rb") as stream, zipfile.ZipFile(stream) as archive:
        entries = archive.infolist()
        need(len(entries) == count, "ZIP 项目数不一致")
        names, ranges, expanded, ap_entries = set(), [], 0, []
        for entry in entries:
            name = entry.filename
            parts = name.rstrip("/").split("/")
            need(name and len(name.encode("utf-8")) <= 1024 and not name.startswith(("/", "\\"))
                 and "\\" not in name and ":" not in name and "\0" not in name
                 and all(part not in ("", ".", "..") for part in parts), "固件包路径不安全")
            need(name.casefold() not in names, "固件包有重复或大小写冲突的路径")
            names.add(name.casefold())
            kind = stat.S_IFMT(entry.external_attr >> 16)
            need(kind in (0, stat.S_IFREG, stat.S_IFDIR) and not entry.flag_bits & ~0x80e
                 and entry.compress_type in (0, 8), "固件包含链接、加密或不支持的压缩方式")
            need(0 <= entry.file_size <= MAX_INPUT and 0 <= entry.compress_size <= MAX_INPUT
                 and entry.file_size <= max(4096, entry.compress_size * 4096), "固件成员超过大小或压缩比限制")
            expanded += entry.file_size
            need(expanded <= MAX_EXPANDED, "固件包展开量超过限制")
            head = os.pread(fd, 30, entry.header_offset)
            need(len(head) == 30 and head[:4] == b"PK\x03\x04", "固件成员头缺失")
            fields = struct.unpack("<4s5H3I2H", head)
            start = entry.header_offset + 30 + fields[-2] + fields[-1]
            need(0 <= entry.header_offset < start <= start + entry.compress_size <= central,
                 "固件成员数据越界")
            need(fields[2] == entry.flag_bits and fields[3] == entry.compress_type, "ZIP 本地头与目录不一致")
            if not entry.flag_bits & 8:
                need(fields[6:9] == (entry.CRC, entry.compress_size, entry.file_size), "ZIP 大小或 CRC 不一致")
            local_name = os.pread(fd, fields[-2], entry.header_offset + 30)
            need(local_name.decode("utf-8" if entry.flag_bits & 0x800 else "cp437") == name,
                 "ZIP 成员名称不一致")
            ranges.append((entry.header_offset, start + entry.compress_size))
            if PurePosixPath(name).name == "vela_ap.bin":
                ap_entries.append(entry)
        ranges.sort()
        need(all(a[1] <= b[0] for a, b in zip(ranges, ranges[1:])), "固件成员互相重叠")
        need(len(ap_entries) <= 1, "固件包含多个 AP，无法确定启动镜像")
        if not ap_entries:
            return b"", {"members": count, "expandedBytes": expanded, "resourceLayout": "multi-partition"}
        need(ap_entries[0].file_size <= MAX_AP, "AP 超过静态分析大小限制")
        ap_data = bytearray()
        with archive.open(ap_entries[0]) as member:
            while chunk := member.read(65536):
                need(len(ap_data) + len(chunk) <= ap_entries[0].file_size, "ZIP 解压超出声明边界")
                ap_data.extend(chunk)
        need(len(ap_data) == ap_entries[0].file_size, "ZIP 成员被截断")
    return bytes(ap_data), {"members": count, "expandedBytes": expanded,
        "resourceLayout": "romfs" if any(PurePosixPath(name).name == "vela_resource.bin" for name in names)
        else "multi-partition"}


VERSION_TEXT = re.compile(r"[0-9]+(?:\.[0-9]+){1,7}")
DECLARED_MODEL = re.compile(rb"miwear\.watch\.([a-z0-9]{3,16})(?![a-z0-9])")


def _version(text):
    match = VERSION_TEXT.search(text or "")
    return match[0] if match and len(match[0]) <= 32 else None


def engine_ap(ap_data):
    """The runner's AP record, derived only from these bytes."""
    probe = inspect_ap(ap_data)
    ap = {"sha256": probe["sha256"], "size": probe["bytes"], "chip": probe["chip"]}
    if "build" in probe:
        ap["build"] = probe["build"]
    if probe["chip"] == "xring":
        ap.update(image_format="xring-vector", image_base=probe["imageBase"],
                  reset_entry=probe["resetEntry"], reset_sp=probe["resetSP"],
                  backend_requirement="xring O62M original TEE/AP boot")
    elif probe["headerRecognized"]:
        ap.update(image_format="bes-header", ap_offset=probe["apOffset"], flash_bytes=probe["flashBytes"],
                  data_alias=probe["dataAlias"], reset_entry=probe["resetEntry"], reset_sp=probe["resetSP"],
                  backend_requirement=probe["chip"] + " peripheral and product storage/panel profile")
    else:
        ap.update(image_format="unrecognized", backend_requirement=probe.get("diagnostic", "启动头尚未适配"))
    return ap


def ota_recipe(fd, size):
    """Runner recipe for any complete OTA: member boundaries, AP header,
    package version and the product models the AP itself declares. Only the AP
    member carries a digest, because that digest names the extracted AP object;
    no other member is read or hashed. Nothing is executed and no member name
    becomes a host path. Accepts both a plain OTA ZIP and the BES full-package
    envelope (0x60ZZ~ header with an embedded per-member ZIP chain)."""
    if os.pread(fd, 4, 0) == BES_ENVELOPE_MAGIC:
        need(size <= MAX_INPUT, "固件包超过大小限制")
        data = os.pread(fd, size, 0)
        need(len(data) == size, "分析期间固件发生变化")
        return _envelope_recipe(data)
    ap_data, archive = inventory(fd, size)
    ap = engine_ap(ap_data) if ap_data else {}
    records, small = [], {}
    with os.fdopen(os.dup(fd), "rb") as stream, zipfile.ZipFile(stream) as zipped:
        for index, info in enumerate(zipped.infolist()):
            if info.is_dir():
                continue
            need(not info.flag_bits & 1, "固件包成员已加密，无法读取")
            head = os.pread(fd, 30, info.header_offset)
            names, extras = struct.unpack_from("<HH", head, 26)
            record = {"index": index, "name": info.filename, "header_offset": info.header_offset,
                      "data_offset": info.header_offset + 30 + names + extras,
                      "compressed_size": info.compress_size, "size": info.file_size,
                      "compression_method": info.compress_type, "crc32": f"{info.CRC:08x}"}
            base = PurePosixPath(info.filename).name
            if base == "vela_ap.bin":
                record["sha256"] = ap["sha256"]
            records.append(record)
            if base in ("ota.json", "version", "version.ini", "version.txt", "ota.sh", "build.prop") and info.file_size <= 65536:
                small.setdefault(base, zipped.read(info).decode("utf-8", "replace"))
    family, version, package_kind = "", None, "full"
    if "ota.json" in small:
        try:
            spec = json.loads(small["ota.json"])
            family = str(spec.get("magic_string", ""))[:16] if isinstance(spec, dict) else ""
            if isinstance(spec, dict) and spec.get("firmware_type") in ("diff", "incremental"):
                package_kind = "incremental"
            version = _version(str(spec.get("sw_version", ""))) if isinstance(spec, dict) else None
        except ValueError:
            pass
    for name in ("version", "version.ini", "version.txt", "ota.sh"):
        version = version or _version(small.get(name))
    version = version or _version(ap.get("build", {}).get("VERSION"))
    if version is None:
        # Families differ in where the release lives and which build.prop key
        # carries it (ro.build.id, ro.build.version.incremental, ro.build.display.id,
        # ...), so accept any ro.build.* assignment whose value looks like a
        # release: first a packaged build.prop member, then the AP ROMFS bytes.
        for raw in (small.get("build.prop", "").encode("utf-8", "replace"), ap_data):
            # No trailing boundary: releases like "5.0.3-release" count; dotted
            # requirement already excludes dates and raw unix timestamps.
            match = re.search(rb"ro\.build\.[a-zA-Z_.]{1,40}=[ \t]*([0-9]+(?:\.[0-9]+){1,7})", raw)
            version = _version(match.group(1).decode("ascii", "ignore")) if match else None
            if version:
                break
    version = version or "0"
    declared = sorted({m.decode("ascii") for m in DECLARED_MODEL.findall(ap_data)})[:64]
    if ap.get("chip") == "xring" and {"vela_tee.bin", "vela_mmap.bin", "vela_CP_boot.bin", "vela_CP.bin", "vela_CRF.bin"} <= {PurePosixPath(r["name"]).name for r in records}:
        declared = ["o62m"]
    return {"version": version, "family": family, "declaredModels": declared, "ap": ap,
            "partitions": records, "resourceLayout": archive["resourceLayout"],
            "packageKind": "incremental" if any(PurePosixPath(r["name"]).name.startswith("vela_") and r["name"].endswith(".patch") for r in records) else package_kind,
            "container": "zip"}


def _inflate_member(data, record):
    """Decompress one inventoried member from the envelope bytes."""
    begin, packed = record["data_offset"], record["compressed_size"]
    need(0 <= begin < begin + packed <= len(data), "成员数据越界")
    if record["compression_method"] == 0:
        output = data[begin:begin + packed]
    elif record["compression_method"] == 8:
        decoder = zlib.decompressobj(-15)
        output = decoder.decompress(data[begin:begin + packed], record["size"] + 1)
        need(decoder.eof and not decoder.unused_data and not decoder.unconsumed_tail,
             "无效的成员压缩边界")
    else:
        raise ProbeError("不支持的成员压缩方式")
    need(len(output) == record["size"] and zlib.crc32(output) == int(record["crc32"], 16),
         "成员大小或 CRC 不匹配")
    return output


def _envelope_members(data):
    """Members of a BES full-package envelope: a 0x60ZZ~ header with version,
    a ZZZ~ resource table (count at +70, 128-byte records, CRC32 trailer) and
    one absolute-offset single-member ZIP per entry. Records keep absolute
    offsets so checked_member and the N67 YAFFS builder read the original bytes."""
    need(len(data) >= 0x58 and data[:4] == BES_ENVELOPE_MAGIC, "不是 BES 整包信封")
    base = struct.unpack_from("<I", data, 0x54)[0]
    need(0 < base < len(data) and data[base:base + 4] == b"ZZZ~", "资源表缺失")
    count = struct.unpack_from("<H", data, base + 70)[0]
    end = base + 80 + count * 128
    need(0 < count <= MAX_MEMBERS and end + 4 <= len(data), "资源表数量或范围无效")
    need(zlib.crc32(data[base:end]) == struct.unpack_from("<I", data, end)[0], "资源表 CRC 不匹配")
    records = []
    for index in range(count):
        at = base + 80 + index * 128
        _flags, relative, length = struct.unpack_from("<III", data, at)
        raw = data[at + 12:at + 128]
        raw_path, separator, padding = raw.partition(b"\0")
        need(separator and not any(padding), "资源路径不是零填充")
        path = raw_path.decode("utf-8")
        parts = path.split("/")
        need(path.startswith("/") and all(part not in ("", ".", "..") for part in parts[1:]),
             "资源路径不安全")
        start, zip_end = base + relative, base + relative + length
        need(0 < start < zip_end <= len(data), "资源 ZIP 越界")
        head = data[start:start + 30]
        need(len(head) == 30 and head[:4] == b"PK\x03\x04", "资源 ZIP 本地头缺失")
        fields = struct.unpack("<4s5H3I2H", head)
        nlen, elen = fields[-2], fields[-1]
        name = data[start + 30:start + 30 + nlen].decode("utf-8", "replace")
        need(PurePosixPath(path).name == name, "路径文件名与 ZIP 成员不一致")
        with zipfile.ZipFile(io.BytesIO(data[start:zip_end])) as archive:
            infos = archive.infolist()
            need(len(infos) == 1 and infos[0].filename == name and infos[0].header_offset == 0,
                 "资源 ZIP 必须恰好包含一个成员")
            info = infos[0]
        need(info.compress_type in (0, 8) and not info.flag_bits & ~0x80e,
             "固件成员包含链接、加密或不支持的压缩方式")
        records.append({"index": index, "name": name, "target_path": path, "header_offset": start,
                        "data_offset": start + 30 + nlen + elen,
                        "compressed_size": info.compress_size, "size": info.file_size,
                        "compression_method": info.compress_type, "crc32": f"{info.CRC:08x}"})
    return records


def _envelope_recipe(data):
    records = _envelope_members(data)
    ap_entries = [record for record in records if record["name"] == "vela_ap.bin"]
    need(len(ap_entries) <= 1, "固件包含多个 AP，无法确定启动镜像")
    ap_data = _inflate_member(data, ap_entries[0]) if ap_entries else b""
    ap = engine_ap(ap_data) if ap_data else {}
    if ap_entries:
        ap_entries[0]["sha256"] = ap["sha256"]
    version = None
    for record in records:
        if record["name"] == "version.ini":
            version = _version(_inflate_member(data, record).decode("utf-8", "replace"))
            break
    if version is None:
        base = struct.unpack_from("<I", data, 0x54)[0]
        version = _version(data[base + 4:base + 68].split(b"\0", 1)[0].decode("ascii", "replace"))
    if version is None:
        match = re.search(rb"ro\.build\.[a-zA-Z_.]{1,40}=[ \t]*([0-9]+(?:\.[0-9]+){1,7})", ap_data)
        version = _version(match.group(1).decode("ascii", "ignore")) if match else None
    declared = sorted({m.decode("ascii") for m in DECLARED_MODEL.findall(ap_data)})[:64]
    if ap.get("chip") == "xring" and {"vela_tee.bin", "vela_mmap.bin", "vela_CP_boot.bin", "vela_CP.bin", "vela_CRF.bin"} <= {PurePosixPath(r["name"]).name for r in records}:
        declared = ["o62m"]
    return {"version": version or "0", "family": "", "declaredModels": declared, "ap": ap,
            "partitions": records, "resourceLayout": "multi-partition", "container": "bes-envelope",
            "packageKind": "full" if ap and any(r["target_path"].startswith("/system/")
                and not r["target_path"].startswith("/system/version/") for r in records) else "unknown"}


ARCHIVE_FILENAME = re.compile(
    r"(?P<model>[a-z0-9]+(?:\.[a-z0-9]+){2,})_v(?P<version>[0-9]+(?:\.[0-9]+){1,7})"
    r"(?:_from_v(?P<base>[0-9]+(?:\.[0-9]+){1,7}))?_(?P<kind>full|incremental)_[0-9a-f]{8,64}\.bin$",
    re.IGNORECASE)


def archive_recipe(fd, size, filename=""):
    """Parse containers; keep unimplemented binary families as described archives.

    Archive filenames supply release metadata, never CPU identity or execution
    approval. ZIP/BES members retain their original boundaries and AP bytes.
    """
    need(0 < size <= MAX_INPUT, "固件文件为空或超过大小限制")
    head = os.pread(fd, min(size, 4096), 0)
    match = ARCHIVE_FILENAME.fullmatch(PurePosixPath(filename).name)
    named = match.groupdict() if match else {}
    if head[:4] in (b"PK\x03\x04", BES_ENVELOPE_MAGIC):
        result = ota_recipe(fd, size)
        names = [PurePosixPath(r["name"]).name for r in result["partitions"]]
        need(named or any(n.startswith("vela_") or n in ("ota.json", "ota.sh", "build.prop") for n in names),
             "ZIP 不包含可识别的固件成员")
        result["structure"] = "member-inventory"
    else:
        need(len(head) >= 64, "固件头不完整")
        result = {"version": "0", "family": "", "declaredModels": [], "ap": {},
                  "partitions": [], "resourceLayout": "binary", "packageKind": "unknown",
                  "structure": "header-only"}
        if head[:4] == b"AOTA":
            need(size >= 512, "AOTA 固件头不完整")
            result.update(container="aota", headerVersion=head[64:96].split(b"\0", 1)[0].decode("ascii", "replace"),
                          boardName=head[96:128].split(b"\0", 1)[0].decode("ascii", "replace"))
            count, table = struct.unpack_from("<H", head, 12)[0], struct.unpack_from("<H", head, 16)[0]
            need(0 < count <= MAX_MEMBERS and table >= 128 and table + count * 32 <= size,
                 "AOTA 成员表越界")
            records = []
            for index in range(count):
                row = os.pread(fd, 32, table + index * 32)
                need(len(row) == 32, "AOTA 成员表不完整")
                name = row[:16].split(b"\0", 1)[0].decode("ascii")
                offset, length = struct.unpack_from("<II", row, 16)
                need(name and table + count * 32 <= offset <= offset + length <= size,
                     "AOTA 成员数据越界")
                records.append({"index": index, "name": name, "data_offset": offset, "size": length})
            result.update(partitions=records, structure="member-inventory")
        elif head[:4] == b"\x78\x56\x34\x12":
            mode = struct.unpack_from("<I", head, 8)[0]
            need(mode in (1, 2), "手环升级容器类型无效")
            result.update(container="band-update", packageKind="incremental" if mode == 1 else "full",
                          version=_version(head[12:76].split(b"\0", 1)[0].decode("ascii", "replace")) or "0")
        elif head[:8] == b"full\0\0\0\0":
            major, minor, patch = struct.unpack_from(">BBH", head, 12)
            result.update(container="midr-full", packageKind="full", version=f"{major}.{minor}.{patch}")
            device = head[16:48].split(b"\0", 1)[0].decode("ascii", "replace")
            if re.fullmatch(r"midr\.watch\.[a-z0-9]+", device):
                result["declaredModels"] = [device]
        else:
            # Older archive releases have no shared container signature. Keep
            # their bytes intact and label the release metadata's provenance.
            need(named, "无法识别固件容器；请选择原始固件包")
            result.update(container="raw-binary", structure="opaque")
    if named:
        result["archiveModel"] = named["model"].lower()
        result["archiveVersion"] = named["version"]
        result["metadataSource"] = "archive-filename"
        result["version"] = named["version"]
        if named["kind"] == "incremental":
            result["packageKind"] = "incremental"
        elif result["packageKind"] == "unknown":
            result["packageKind"] = "full"
        if named["base"]:
            result["baseVersion"] = named["base"]
        if not result["declaredModels"]:
            result["declaredModels"] = [named["model"].lower()]
    else:
        result["metadataSource"] = "firmware"
    ap = result["ap"]
    result["bootablePackage"] = (result["packageKind"] == "full" and
        ap.get("image_format") in ("bes-header", "xring-vector"))
    return result


def inspect_firmware(source):
    """Describe a firmware package without executing or changing it."""
    fd, before = _open_file(_path(source), MAX_INPUT)
    try:
        head = os.pread(fd, 8, 0)
        if (head[:4] not in (b"PK\x03\x04", BES_ENVELOPE_MAGIC, b"AOTA", b"\x78\x56\x34\x12", b"full")
                and not ARCHIVE_FILENAME.fullmatch(PurePosixPath(os.fspath(source)).name)):
            need(0 < before.st_size <= MAX_AP, "非容器文件超过 AP 静态分析上限")
            data = os.pread(fd, before.st_size, 0)
            need(len(data) == before.st_size, "分析期间固件发生变化")
            ap = inspect_ap(data)
            return {"version": 1, "sha256": ap["sha256"], "bytes": before.st_size,
                    "inputKind": "ap-candidate", "executed": False, "modified": False,
                    "compatibility": "unverified", "guestDebuggingVerified": False,
                    "versionWhitelistUsed": False, "runtimeReady": False, "ap": ap,
                    "nextStep": "单独 AP 缺少启动所需资源；请选择完整固件包进行导入"}
        recipe = archive_recipe(fd, before.st_size, os.fspath(source))
        result = {"version": 1, "bytes": before.st_size, "executed": False, "modified": False,
                  "compatibility": "unverified", "guestDebuggingVerified": False, "versionWhitelistUsed": False,
                  "inputKind": recipe["packageKind"], "container": recipe["container"],
                  "firmwareVersion": recipe["version"], "declaredModels": recipe["declaredModels"],
                  "metadataSource": recipe["metadataSource"], "structure": recipe["structure"],
                  "archive": {"members": len(recipe["partitions"]), "resourceLayout": recipe["resourceLayout"],
                              "expandedBytes": sum(r["size"] for r in recipe["partitions"])},
                  "ap": None, "runtimeReady": False}
        digest = hashlib.sha256()
        for offset in range(0, before.st_size, 1024 * 1024):
            digest.update(os.pread(fd, min(1024 * 1024, before.st_size - offset), offset))
        result["sha256"] = digest.hexdigest()
        ap = recipe["ap"]
        if ap:
            result["ap"] = {"sha256": ap["sha256"], "bytes": ap["size"], "chip": ap["chip"],
                            "headerRecognized": ap["image_format"] in ("bes-header", "xring-vector")}
            for source_key, target_key in (("build", "build"), ("ap_offset", "apOffset"),
                    ("flash_bytes", "flashBytes"), ("data_alias", "dataAlias"), ("image_base", "imageBase"),
                    ("reset_entry", "resetEntry"), ("reset_sp", "resetSP")):
                if source_key in ap:
                    result["ap"][target_key] = ap[source_key]
        if "baseVersion" in recipe:
            result["baseVersion"] = recipe["baseVersion"]
        result["nextStep"] = ("完整包已解析，可选择对应的硬件实例" if recipe["bootablePackage"] else
                              "可保存到固件库；此文件不能独立启动")
        after = os.fstat(fd)
        need((before.st_size, before.st_mtime_ns, before.st_ctime_ns) ==
             (after.st_size, after.st_mtime_ns, after.st_ctime_ns), "分析期间固件发生变化")
        return result
    except (zipfile.BadZipFile, UnicodeError, struct.error, NotImplementedError, zlib.error, EOFError) as error:
        raise ProbeError("固件结构无效：" + str(error)[:160]) from error
    finally:
        os.close(fd)
