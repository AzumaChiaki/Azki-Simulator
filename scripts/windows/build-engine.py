#!/usr/bin/env python3
"""Build the existing three Azki QEMU boards for native Windows UCRT64.

Run from the project's private MSYS2 UCRT64 shell. The cache is reusable; final
files are published only in repository-root dist/windows. No firmware is bundled.
"""
from __future__ import annotations

import argparse
import filecmp
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import time

ROOT = Path(__file__).resolve().parents[2]
QEMU_VERSION = "10.1.0"
QEMU_SHA256 = "e0517349b50ca73ebec2fa85b06050d5c463ca65c738833bd8fc1f15f180be51"


def sha(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1048576), b""):
            h.update(block)
    return h.hexdigest()


def run(args, **kwargs):
    print("+ " + subprocess.list2cmdline(list(map(str, args))), flush=True)
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def copy_changed(source, target):
    target = Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.is_file() or not filecmp.cmp(source, target, shallow=False):
        shutil.copy2(source, target)
        # Restored/Git sources can have timestamps older than existing objects.
        # Ninja must observe every changed overlay even when its source mtime
        # predates the previous build; content hashes remain in the receipt.
        os.utime(target, None)
    return target


def copy_sources(source, target):
    shutil.copytree(source, target, dirs_exist_ok=True, copy_function=copy_changed,
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc", ".DS_Store", "build", "bin", "obj", "assets", "results"))


def copy_overlay(source, target):
    # This diagnostic machine is deliberately absent from mi-emu.mak and the
    # supported device inventory. Do not redistribute unused research inputs
    # as part of the corresponding Windows board sources.
    shutil.copytree(source, target, dirs_exist_ok=True, copy_function=copy_changed,
                    ignore=shutil.ignore_patterns("azki-marconi-probe.c", "__pycache__", "*.pyc", ".DS_Store"))


def overlay_inventory(source):
    return [{"path": path.relative_to(source).as_posix(), "bytes": path.stat().st_size,
             "sha256": sha(path)} for path in sorted(Path(source).rglob("*"))
            if path.is_file() and path.name not in ("azki-marconi-probe.c", ".DS_Store")
            and "__pycache__" not in path.parts and path.suffix != ".pyc"]


def posix(path):
    return str(Path(path).absolute()).replace("\\", "/")


def extract_windows_source(archive, sources):
    # The official archive contains optional firmware/CI/Linux-only symlinks.
    # Windows does not grant symlink creation by default. Materialize their
    # in-archive targets as copies; no Developer Mode or privilege change.
    links = []
    with tarfile.open(archive, "r:xz") as incoming:
        def regular(member, destination):
            if member.issym() or member.islnk():
                links.append(member)
                return None
            return tarfile.data_filter(member, destination)
        incoming.extractall(sources, filter=regular)
    return materialize_source_links(links, sources)


def materialize_source_links(links, sources):
    source_root = sources.resolve()
    pending = []
    for member in links:
        # EDK2's macOS-only X11 development convenience link intentionally
        # points outside its source tree. It is not a Windows dependency.
        if (member.name, member.linkname) == (
                "qemu-10.1.0/roms/edk2/EmulatorPkg/Unix/Host/X11IncludeHack", "/opt/X11/include"):
            continue
        pending.append(member)
    count = len(pending)
    while pending:
        later = []
        for member in pending:
            destination = sources / member.name
            target = (destination.parent / member.linkname if member.issym()
                      else sources / member.linkname).resolve()
            if not target.is_relative_to(source_root):
                raise RuntimeError("Archive link escapes source root: " + member.name)
            if not target.exists():
                later.append(member)  # A link can target a later archive link.
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            if target.is_dir():
                shutil.copytree(target, destination, dirs_exist_ok=True, copy_function=copy_changed)
            else:
                copy_changed(target, destination)
        if len(later) == len(pending):
            raise RuntimeError("Archive contains an unresolved link: " + later[0].name)
        pending = later
    return count


SYSTEM_DLLS = {name + ".dll" for name in (
    "kernel32", "advapi32", "user32", "gdi32", "ole32", "oleaut32", "shell32",
    "ws2_32", "winmm", "secur32", "bcrypt", "ntdll", "version", "crypt32",
    "setupapi", "shlwapi", "comctl32", "psapi", "dnsapi", "iphlpapi", "winhttp",
    "dbghelp", "ucrtbase", "powrprof", "imm32", "msimg32", "comdlg32", "winspool",
    "rpcrt4", "normaliz", "userenv", "wtsapi32", "ntmarta", "dwmapi", "wintrust",
)}


def dll_closure(executable, output, prefix):
    available = {path.name.lower(): path for path in (prefix / "bin").glob("*.dll")}
    pending, seen, copied = [Path(executable)], set(), []
    while pending:
        current = pending.pop()
        identity = current.name.lower()
        if identity in seen:
            continue
        seen.add(identity)
        listing = subprocess.check_output(["objdump", "-p", str(current)], text=True, errors="replace")
        for name in re.findall(r"DLL Name:\s*(\S+)", listing):
            key = name.lower()
            if key.startswith(("api-ms-win-", "ext-ms-win-")) or key in SYSTEM_DLLS:
                continue
            if key == "msvcrt.dll":
                raise RuntimeError("UCRT64 package unexpectedly imports MSVCRT; do not mix toolchains")
            if key not in available:
                raise RuntimeError("Unresolved portable DLL dependency: " + name)
            target = output / available[key].name
            copy_changed(available[key], target)
            copied.append(target)
            pending.append(target)
    return sorted(set(copied))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-archive", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--runtime-python", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=6)
    args = parser.parse_args(argv)
    if sys.platform != "win32" or os.environ.get("MSYSTEM") != "UCRT64":
        parser.error("Use native Windows MSYS2 UCRT64, not MSYS/MINGW64 or a macOS build tree")
    if not (ROOT / "engine/windows-bluetooth/publish/AzkiBluetoothHost.exe").is_file():
        parser.error("Publish the Bluetooth host first, or use scripts/build-windows.ps1 with the private .NET 10 SDK")
    if not 1 <= args.jobs <= 32 or sha(args.source_archive) != QEMU_SHA256:
        parser.error("Exact QEMU 10.1.0 source archive and bounded job count required")
    for tool in ("bash", "gcc", "objdump", "ninja", "pkg-config"):
        if not shutil.which(tool):
            parser.error("Missing UCRT64 build dependency: " + tool)
    machine = subprocess.check_output(["gcc", "-dumpmachine"], text=True).strip()
    if machine != "x86_64-w64-mingw32":
        parser.error("A native Windows x64 UCRT64 compiler is required")
    prefix = Path(shutil.which("gcc")).resolve().parent.parent
    work = args.work_root.absolute()
    marker = work / ".azki-windows-build.json"
    if work.exists() and not marker.is_file():
        parser.error("Existing cache directory has no project ownership marker")
    work.mkdir(parents=True, exist_ok=True)
    if not marker.exists():
        marker.write_text(json.dumps({"schema": "azki.windows.build.v1", "root": str(ROOT),
                                      "QEMU_SHA256": QEMU_SHA256}, indent=2) + "\n")
    identity = json.loads(marker.read_text())
    if identity.get("schema") != "azki.windows.build.v1" or identity.get("root") != str(ROOT):
        parser.error("Cache belongs to another source root")
    sources = work / "source"
    sources.mkdir(exist_ok=True)
    qsource = sources / f"qemu-{QEMU_VERSION}"
    extracted = qsource / ".azki-original-source.json"
    if not extracted.exists():
        if qsource.exists():
            parser.error("Partial source extraction retained; select a fresh work-root")
        print("Extracting exact QEMU source with Windows link copies", flush=True)
        count = extract_windows_source(args.source_archive, sources)
        extracted.write_text(json.dumps({"QEMU_SHA256": QEMU_SHA256, "archiveLinksMaterializedAsCopies": count}) + "\n")
    copy_overlay(ROOT / "engine/qemu-overlay", qsource)
    qbuild = work / "qemu-build"
    qbuild.mkdir(exist_ok=True)
    if not (qbuild / "build.ninja").exists():
        run(["bash", posix(qsource / "configure"), "--target-list=arm-softmmu",
             "--with-devices-arm=mi-emu", "--without-default-devices", "--disable-docs",
             "--disable-tools", "--disable-guest-agent", "--disable-gtk", "--disable-sdl",
             "--disable-vnc", "--disable-opengl", "--disable-werror", "--disable-download",
             "--audio-drv-list=", "--disable-slirp", "--extra-ldflags=-ladvapi32",
             "--python=" + posix(sys.executable)], cwd=qbuild)
    run(["ninja", "-C", qbuild, "-j", args.jobs, "qemu-system-arm.exe"])
    core = qbuild / "qemu-system-arm.exe"
    if not core.is_file():
        raise RuntimeError("Native Windows core is absent after ninja success")
    output = ROOT / "dist/windows"
    output.mkdir(parents=True, exist_ok=True)
    output_marker = output / ".azki-windows-output.json"
    if not output_marker.exists() and any(output.iterdir()):
        raise RuntimeError("Refusing to merge final outputs with an unowned directory")
    output_marker.write_text(json.dumps({"schema": "azki.windows.output.v1", "source": str(ROOT)}) + "\n")
    copy_changed(core, output / core.name)
    dlls = dll_closure(core, output, prefix)
    project = output / "project"
    copy_sources(ROOT / "engine/project", project)
    copy_changed(ROOT / "engine/catalog.json", output / "catalog.json")
    copy_sources(ROOT / "engine/windows-app", output / "windows-app")
    copy_changed(ROOT / "scripts/windows/launch-portable.cmd", output / "AzkiSimulator.cmd")
    copy_changed(ROOT / "scripts/windows/bootstrap-app.py", output / "AzkiSimulator.pyw")
    for name in ("run-windows.cmd", "run-windows.ps1"):
        copy_changed(ROOT / "scripts" / name, output / "scripts" / name)
    bluetooth_publish = ROOT / "engine/windows-bluetooth/publish"
    if not (bluetooth_publish / "AzkiBluetoothHost.exe").is_file():
        raise RuntimeError("Publish the Windows Bluetooth helper before producing the complete Windows package")
    copy_sources(bluetooth_publish, output / "windows-bluetooth")
    # SDK installation is private to the builder. Ship just its compatible
    # runtime and host, never the SDK or a machine-wide installation.
    dotnet = args.runtime_python.resolve().parent.parent / "dotnet"
    if not (dotnet / "dotnet.exe").is_file():
        raise RuntimeError("The portable Bluetooth package needs the private .NET 10 runtime")
    copy_changed(dotnet / "dotnet.exe", output / "dotnet/dotnet.exe")
    for component in ("host/fxr", "shared/Microsoft.NETCore.App"):
        versions = [p for p in (dotnet / component).iterdir() if p.is_dir() and p.name.startswith("10.")]
        if not versions:
            raise RuntimeError("Missing .NET 10 runtime component: " + component)
        for version in versions:
            copy_sources(version, output / "dotnet" / component / version.name)
    for name in ("LICENSE.txt", "ThirdPartyNotices.txt"):
        if (dotnet / name).is_file():
            copy_changed(dotnet / name, output / "dotnet" / name)
    copy_changed(ROOT / "LICENSE", output / "licenses/Azki-GPL-2.0.txt")
    copy_changed(ROOT / "engine/THIRD_PARTY.md", output / "licenses/THIRD_PARTY.md")
    copy_sources(ROOT / "engine/licenses", output / "licenses/engine")
    for name in ("COPYING", "COPYING.LIB", "LICENSE"):
        copy_changed(qsource / name, output / "licenses" / ("QEMU-" + name))
    if (prefix / "share/licenses").is_dir():
        copy_sources(prefix / "share/licenses", output / "licenses/UCRT64")
    # Keep the exact corresponding QEMU source and every project overlay.
    copy_changed(args.source_archive, output / "sources" / args.source_archive.name)
    copy_overlay(ROOT / "engine/qemu-overlay", output / "sources/qemu-overlay")
    copy_sources(ROOT / "engine/third-party", output / "sources/third-party")
    copy_sources(ROOT / "engine/windows-bluetooth/AzkiBluetoothHost", output / "sources/AzkiBluetoothHost")
    copy_sources(ROOT / "scripts", output / "sources/build-scripts")
    y, u = ROOT / "engine/third-party/yaffs2", ROOT / "engine/third-party/unqlite"
    helpers = project / "backend/build"
    helpers.mkdir(parents=True, exist_ok=True)
    definitions = ("CONFIG_YAFFS_DIRECT", "CONFIG_YAFFS_YAFFS2", "CONFIG_YAFFS_DEFINES_TYPES",
                   "CONFIG_YAFFS_PROVIDE_DEFS", "CONFIG_YAFFSFS_PROVIDE_VALUES", "CONFIG_YAFFS_USE_32_BIT_TIME_T")
    yaffs = [*sorted((y / "core").glob("*.c")),
             *[y / "direct" / name for name in ("yaffsfs.c", "yaffs_attribs.c", "yaffs_error.c", "yaffs_hweight.c")]]
    flags = ["gcc", "-O2", "-include", "stdint.h", "-include", "sys/types.h",
             "-Dmain=azki_helper_main", "-municode", *["-D" + name for name in definitions], "-Dloff_t=int64_t",
             "-I" + str(y / "core"), "-I" + str(y / "direct")]
    entry = ROOT / "engine/project/backend/src/windows_helper_entry.c"
    run([*flags, ROOT / "engine/project/backend/src/yaffs_image.c", *yaffs, entry, "-ladvapi32", "-o", helpers / "yaffs-image.exe"])
    run(["gcc", "-O2", "-Dmain=azki_helper_main", "-municode", "-I" + str(u),
         ROOT / "engine/project/backend/src/legacy_ui_db.c", u / "unqlite.c", entry,
         "-ladvapi32", "-o", helpers / "legacy-ui-db.exe"])
    run([*flags, "-I" + str(u), ROOT / "engine/project/backend/src/instance_props.c",
         *yaffs, u / "unqlite.c", entry, "-ladvapi32", "-o", helpers / "instance-props.exe"])
    for helper in helpers.glob("*.exe"):
        dlls.extend(dll_closure(helper, helpers, prefix))
    source_helpers = ROOT / "engine/project/backend/build"
    source_helpers.mkdir(parents=True, exist_ok=True)
    for path in helpers.iterdir():
        if path.suffix.lower() in (".exe", ".dll"):
            copy_changed(path, source_helpers / path.name)
    for name in ("unqlite.c", "unqlite.h"):
        copy_changed(u / name, project / "references/openvela-unqlite" / name)
    python_root = args.runtime_python.resolve().parent
    if not (python_root / "Lib/tkinter").is_dir():
        raise RuntimeError("The packaged runtime Python must include Tk")
    shutil.copytree(python_root, output / "python", dirs_exist_ok=True,
                    copy_function=copy_changed, ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "Scripts", "Doc", "test"))
    copied = []
    for path in sorted(output.rglob("*")):
        if path.is_file() and path.name != "azki-windows-engine.json":
            copied.append({"path": path.relative_to(output).as_posix(), "bytes": path.stat().st_size, "sha256": sha(path)})
    overlay_inputs = overlay_inventory(ROOT / "engine/qemu-overlay")
    for row in overlay_inputs:
        if sha(qsource / row["path"]) != row["sha256"] or sha(output / "sources/qemu-overlay" / row["path"]) != row["sha256"]:
            raise RuntimeError("Build/distribution overlay differs from the recorded source: " + row["path"])
    record = {"schema": "azki.windows.engine.v1", "created": time.time(),
              "platform": "windows-x86_64-ucrt", "QEMU_VERSION": QEMU_VERSION,
              "sourceArchiveSHA256": QEMU_SHA256, "coreSHA256": sha(output / core.name),
              "overlayInputs": overlay_inputs, "unusedResearchMachineExcluded": True,
              "firmwareIncluded": False, "executionVerified": False,
              "machines": ["best1503", "azki-p65", "azki-q65"], "files": copied}
    (output / "azki-windows-engine.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({"output": str(output), "coreSHA256": record["coreSHA256"], "files": len(copied),
                      "executionVerified": False}, indent=2), flush=True)


if __name__ == "__main__":
    main()
