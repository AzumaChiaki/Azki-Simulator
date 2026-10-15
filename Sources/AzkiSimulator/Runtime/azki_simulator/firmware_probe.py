"""Version-independent static firmware diagnostics. Never executes/extracts code.

An unknown digest is inspectable. It is not evidence that an unmodelled SoC,
board or storage layout can execute. The sealed catalog remains a source of
known identities, not the parser's list of accepted firmware versions.
"""
from __future__ import annotations

import hashlib
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
    if chip != "best1503":
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
        need(0x20000000 <= stack <= 0x20160000 and stack % 8 == 0,
             "MSP 不在已建模 RAM 范围")
        result.update(headerRecognized=True, apOffset=offset, flashBytes=flash,
                      dataAlias=alias, resetEntry=entry, resetSP=stack)
    except (KeyError, ValueError, struct.error) as error:
        result["diagnostic"] = "启动头需要适配：" + str(error)[:160]
    return result


def _zip_bounds(fd, size):
    tail = os.pread(fd, min(size, 65557), max(0, size - 65557))
    end = tail.rfind(b"PK\x05\x06")
    need(end >= 0 and end + 22 <= len(tail), "完整 ZIP 目录缺失；增量包或其他容器需要对应适配器")
    magic, disk, start_disk, count_disk, count, length, offset, comment = struct.unpack_from("<4s4H2IH", tail, end)
    absolute = size - len(tail) + end
    need(end + 22 + comment == len(tail) and disk == start_disk == 0 and count_disk == count
         and 0 < count <= MAX_MEMBERS and 0 < length <= 4 * 1024 * 1024
         and offset + length == absolute, "不支持分卷、ZIP64、前缀/尾随数据或超限目录")
    return count, offset


def _zip(fd, size):
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
            need(kind in (0, stat.S_IFREG, stat.S_IFDIR) and not entry.flag_bits & ~0x808
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
        need(len(ap_entries) == 1, "完整固件必须包含唯一 vela_ap.bin；不能猜测差分包或多设备包")
        need(ap_entries[0].file_size <= MAX_AP, "AP 超过静态分析大小限制")
        ap_data = bytearray()
        for entry in entries:
            read = 0
            with archive.open(entry) as member:
                while chunk := member.read(65536):
                    read += len(chunk)
                    need(read <= entry.file_size, "ZIP 解压超出声明边界")
                    if entry is ap_entries[0]:
                        ap_data.extend(chunk)
            need(read == entry.file_size, "ZIP 成员被截断")
        return bytes(ap_data), {"members": count, "expandedBytes": expanded,
            "resourceLayout": "romfs" if any(PurePosixPath(e.filename).name == "vela_resource.bin" for e in entries) else "multi-partition",
            "allMemberCRCChecked": True}


VERSION_TEXT = re.compile(r"[0-9]+(?:\.[0-9]+){1,7}")
DECLARED_MODEL = re.compile(rb"miwear\.watch\.([a-z0-9]{3,16})(?![a-z0-9])")


def _version(text):
    match = VERSION_TEXT.search(text or "")
    return match[0] if match and len(match[0]) <= 32 else None


def engine_ap(ap_data):
    """The runner's catalog AP record, derived only from these bytes."""
    probe = inspect_ap(ap_data)
    ap = {"sha256": probe["sha256"], "size": probe["bytes"], "chip": probe["chip"]}
    if "build" in probe:
        ap["build"] = probe["build"]
    if probe["headerRecognized"]:
        ap.update(image_format="bes-header", ap_offset=probe["apOffset"], flash_bytes=probe["flashBytes"],
                  data_alias=probe["dataAlias"], reset_entry=probe["resetEntry"], reset_sp=probe["resetSP"],
                  backend_requirement="BES1503 peripheral and product storage/panel profile")
    else:
        ap.update(image_format="unrecognized", backend_requirement=probe.get("diagnostic", "启动头尚未适配"))
    return ap


def ota_recipe(fd, size):
    """Static runner recipe for any complete OTA ZIP: member boundaries, AP
    header, package version and the product models the AP itself declares.
    Nothing is executed and no member name becomes a host path."""
    ap_data, archive = _zip(fd, size)
    records, small = [], {}
    with os.fdopen(os.dup(fd), "rb") as stream, zipfile.ZipFile(stream) as zipped:
        for index, info in enumerate(zipped.infolist()):
            if info.is_dir():
                continue
            need(not info.flag_bits & 9, "固件包成员使用了加密或数据描述符，无法按偏移读取")
            head = os.pread(fd, 30, info.header_offset)
            names, extras = struct.unpack_from("<HH", head, 26)
            digest = hashlib.sha256()
            with zipped.open(info) as member:
                while chunk := member.read(1024 * 1024):
                    digest.update(chunk)
            records.append({"index": index, "name": info.filename, "header_offset": info.header_offset,
                            "data_offset": info.header_offset + 30 + names + extras,
                            "compressed_size": info.compress_size, "size": info.file_size,
                            "compression_method": info.compress_type, "crc32": f"{info.CRC:08x}",
                            "sha256": digest.hexdigest()})
            base = PurePosixPath(info.filename).name
            if base in ("ota.json", "version", "version.ini", "ota.sh") and info.file_size <= 65536:
                small.setdefault(base, zipped.read(info).decode("utf-8", "replace"))
    family, version = "", None
    if "ota.json" in small:
        try:
            spec = json.loads(small["ota.json"])
            family = str(spec.get("magic_string", ""))[:16] if isinstance(spec, dict) else ""
            version = _version(str(spec.get("sw_version", ""))) if isinstance(spec, dict) else None
        except ValueError:
            pass
    for name in ("version", "version.ini", "ota.sh"):
        version = version or _version(small.get(name))
    ap = engine_ap(ap_data)
    version = version or _version(ap.get("build", {}).get("VERSION")) or "0"
    declared = sorted({m.decode("ascii") for m in DECLARED_MODEL.findall(ap_data)})[:64]
    return {"version": version, "family": family, "declaredModels": declared, "ap": ap,
            "partitions": records, "resourceLayout": archive["resourceLayout"]}


def inspect_firmware(source):
    """All versions enter analysis; execution capability remains explicit."""
    fd, before = _open_file(_path(source), MAX_INPUT)
    try:
        need(before.st_size > 0, "固件文件为空")
        digest = hashlib.sha256()
        read = 0
        while chunk := os.read(fd, min(1024 * 1024, before.st_size + 1 - read)):
            read += len(chunk)
            need(read <= before.st_size, "分析期间固件发生变化")
            digest.update(chunk)
        need(read == before.st_size, "分析期间固件发生变化")
        result = {"version": 1, "sha256": digest.hexdigest(), "bytes": before.st_size,
                  "executed": False, "modified": False, "compatibility": "unverified",
                  "guestDebuggingVerified": False, "versionWhitelistUsed": False}
        if os.pread(fd, 4, 0) == b"PK\x03\x04":
            data, result["archive"] = _zip(fd, before.st_size)
            result["inputKind"] = "full-ota-zip"
        else:
            need(before.st_size <= MAX_AP, "非 ZIP 文件超过 AP 上限，需要识别其固件容器")
            data = os.pread(fd, before.st_size, 0)
            need(len(data) == before.st_size, "分析期间固件发生变化")
            result["inputKind"] = "ap-candidate"
        result["ap"] = inspect_ap(data)
        recognized = result["ap"]["headerRecognized"]
        result["nextStep"] = ("核对具体型号、此版本分区和面板/触摸模型，再建立独立运行身份；尚未启动验证"
            if recognized else "识别并实现此芯片或启动布局，保留该文件用于调试适配")
        result["runtimeReady"] = False
        after = os.fstat(fd)
        need((before.st_size, before.st_mtime_ns, before.st_ctime_ns) ==
             (after.st_size, after.st_mtime_ns, after.st_ctime_ns), "分析期间固件发生变化")
        return result
    except (zipfile.BadZipFile, UnicodeError, struct.error, NotImplementedError, zlib.error, EOFError) as error:
        raise ProbeError("固件结构无效：" + str(error)[:160]) from error
    finally:
        os.close(fd)
