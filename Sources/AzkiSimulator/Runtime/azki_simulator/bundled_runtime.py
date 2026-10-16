"""Resolve a sealed self-contained Engine, and prepare an isolated local session.

All operations in this module are static file/JSON/hash operations. There is no
fallback to system Python, an external venv, downloads, package installation or
execution of the bundled interpreter, QEMU or firmware.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat

from .firmware_catalog import (CatalogError, FirmwareCatalog, _atomic_json, _copy,
                               EXECUTION_PROFILES, _need, _sha, _text, storage_mode)
from .minative import (INCLUDED_PROFILE_IDS, _canonical, _decode, _directory_fd,
                       _open_file, _path, _pin, _read, inspect_backend)

SCHEMA = "azki.simulator.engine.v1"
MAX_ENGINE_FILES = 20000
MAX_ENGINE_BYTES = 2 * 1024**3
RUN_LIMIT_SECONDS = 0  # Interactive sessions run until explicitly stopped.


def _relative(value):
    _need(_text(value, 1024) and not value.startswith("/") and "\\" not in value and ":" not in value
          and not any(part in ("", ".", "..") for part in value.split("/")),
          "unsafe_engine_path", "内置引擎包含不安全的相对路径")
    return value



def _manifest(engine_root, verify=False):
    root = _path(engine_root)
    data = _read(root / "manifest.json")
    manifest = _decode(data)
    _need(isinstance(manifest, dict) and manifest.get("schema") == SCHEMA and manifest.get("version") == 1
          and manifest.get("profile") in ("local-assets", "public"),
          "invalid_engine", "内置引擎清单版本不受支持")
    entries, python, files = (manifest.get(key) for key in ("entrypoints", "python", "files"))
    _need(isinstance(entries, dict) and set(entries) == {"python", "qemu", "project", "bootstrap"}
          and entries == {"python": "python/bin/python3.12", "qemu": "project/backend/build/qemu-system-arm",
                          "project": "project", "bootstrap": "bootstrap.py"}
          and isinstance(python, dict) and python.get("version") == "3.12"
          and python.get("startupFlags") == ["-I", "-S", "-B"]
          and python.get("stdlib") == "python/lib/python3.12"
          and python.get("sitePackages") == "python/lib/python3.12/site-packages"
          and isinstance(files, list) and 0 < len(files) <= MAX_ENGINE_FILES,
          "invalid_engine", "内置引擎入口或 Python 布局无效")
    by_path, total = {}, 0
    for item in files:
        _need(isinstance(item, dict), "invalid_engine", "内置引擎文件记录无效")
        relative = _relative(item.get("path"))
        _need(relative not in by_path and _sha(item.get("sha256")) and type(item.get("bytes")) is int
              and 0 <= item["bytes"] <= 768 * 1024 * 1024 and type(item.get("executable")) is bool,
              "invalid_engine", "内置引擎文件重复或哈希记录无效")
        total += item["bytes"]
        _need(total <= MAX_ENGINE_BYTES, "engine_quota", "内置引擎超过容量限制")
        path = root / relative
        fd, info = _open_file(path)
        os.close(fd)
        _need(info.st_size <= 768 * 1024 * 1024 and (not item["executable"] or bool(info.st_mode & 0o111)),
              "engine_changed", "内置引擎文件大小或执行标记不一致", path=relative)
        if verify:
            _need(info.st_size == item["bytes"], "engine_changed", "内置引擎发布尺寸不一致", path=relative)
            _need(_pin(path)["sha256"] == item["sha256"], "engine_changed", "内置引擎文件未通过哈希校验", path=relative)
        by_path[relative] = item
    payload = json.dumps(files, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()
    if verify:
        _need(hashlib.sha256(payload).hexdigest() == manifest.get("payloadSha256"),
              "engine_changed", "内置引擎聚合身份无效")
    # Files beyond the manifest (caches, Finder metadata, a newer build's
    # extras) are not engine inputs and no longer make the whole engine
    # unusable; every listed file is still required, regular and unlinked.
    _need(all(entries[key] in by_path for key in ("python", "qemu", "bootstrap"))
          and by_path[entries["python"]]["executable"] and by_path[entries["qemu"]]["executable"]
          and all("project/" + value in by_path for value in (
              "multi_device.py", "device_catalog.py", "firmware.py", "firmware_resources.py",
              "legacy_ui_seed.py", "runtime_fingerprint.py", "backend/src/runner.py",
              "backend/src/control.py", "backend/src/factory.py", "backend/src/qmp.py",
              "backend/src/legacy_ui_db.c", "backend/src/yaffs_image.c")),
          "engine_incomplete", "内置引擎缺少解释器、核心或运行源码")
    _need(any(path.startswith(python["stdlib"] + "/") for path in by_path)
          and any(path.startswith(python["sitePackages"] + "/") for path in by_path),
          "engine_incomplete", "内置 Python 标准库或运行依赖不完整")
    return {"root": root, "manifest": manifest, "files": by_path,
            "sha256": hashlib.sha256(data).hexdigest()}


def runtime_readiness(engine_root):
    try:
        info = _manifest(engine_root)
        return {"ready": True, "reason": "", "version": 1, "source": "bundled",
                "profile": info["manifest"]["profile"], "manifestSHA256": info["sha256"],
                "executionVerified": False, "manifestExecutionClaim": info["manifest"].get("executionVerified") is True,
                "executionProfiles": sorted(EXECUTION_PROFILES), "verification": "startup-metadata"}
    except (OSError, ValueError, KeyError, TypeError) as error:
        return {"ready": False, "reason": "内置引擎不可用，请重新安装应用：" + str(error)[:240],
                "code": getattr(error, "code", "engine_missing"), "version": 1, "source": "bundled",
                "executionVerified": False, "executionProfiles": []}


def _write_json(path, value):
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    # The entire preparation root is newly and exclusively created. Atomic JSON
    # publication retains mode 0600 and never edits files inside the app bundle.
    _atomic_json(path, value)


def _materialize(engine, prepared):
    for relative, record in engine["files"].items():
        if not relative.startswith(("project/", "lib/")):
            continue
        # Firmware/catalog are supplied separately. A malformed Engine cannot
        # smuggle device state or an extra external runtime into this overlay.
        _need(not relative.startswith(("project/assets/", "project/profiles/", "project/.venv")),
              "invalid_engine", "内置引擎不能携带未登记的固件或开发环境")
        target = prepared / relative
        target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        _copy(engine["root"] / relative, target, hash_content=False)
        if record["executable"]:
            target.chmod(0o700)
        # Preserve an existing quarantine attribute; never clear it to launch.
        if record["executable"]:
            from .bridge import quarantine
            value = quarantine(engine["root"] / relative)
            if value is not None:
                quarantine(target, value)


def prepare_profile(engine_root, library_root, profile_id, session_root, *, reservation_nonce=None, instance_id=None,
                    virtual_key_file=None, virtual_key_sha256=None, virtual_serial=None, virtual_address=None, skip_oobe=True):
    """Create static inputs and a new local session; never start a process.

    session_root is a new private container. The returned session directory is
    <session_root>/session; <session_root>/prepared contains copied runner/core
    inputs. No sealed application file or original import is modified.
    """
    engine = _manifest(engine_root)
    catalog = FirmwareCatalog(engine["root"].parent / "Firmware", library_root,
                              distribution="public" if engine["manifest"]["profile"] == "public" else "local")
    _need(bool(catalog.hardware_models), "profile_scope_incomplete", "内置目录没有任何硬件预设，请重新安装应用")
    # The user's imported firmware runs on the instance hardware preset named by
    # the ID and takes precedence over a bundled metadata profile with the same
    # ID. Nothing is checked or refused up front: the boot reports the outcome,
    # and the user is never asked to match a cataloged version.
    imported = catalog.imported_id(profile_id)
    resolved = catalog.resolve_generic(profile_id) if imported else catalog.resolve_profile(profile_id)
    container = _path(session_root)
    from .bridge import SessionReservation
    reservation = SessionReservation(container, reservation_nonce) if reservation_nonce is not None else None
    if reservation is None:
        parent = _directory_fd(container.parent)
        try:
            os.mkdir(container.name, mode=0o700, dir_fd=parent)
        except FileExistsError as error:
            raise CatalogError("session_exists", "准备目录已存在，请使用新的独立会话目录") from error
        finally:
            os.close(parent)
    prepared = container / "prepared"
    try:
        if reservation is not None:
            reservation.check()
            os.mkdir("prepared", mode=0o700, dir_fd=reservation.fd)
        else:
            prepared.mkdir(mode=0o700)
        _materialize(engine, prepared)
        project = prepared / "project"
        generated = imported or storage_mode(catalog.profiles[profile_id]) == "generated"
        # The sandboxed runner builds storage caches here; nothing in it is pre-approved.
        (project / "assets").mkdir(mode=0o700)
        (project / "assets/devices").mkdir(mode=0o700)
        rows = []
        recipes = {key: profile["recipe"] for key, profile in catalog.profiles.items()}
        if imported:
            recipes[profile_id] = resolved["recipe"]
        for key, recipe in sorted(recipes.items()):
            row = dict(recipe)
            # Paths exist only in this newly materialized private recipe. The
            # portable bundled catalog contains no developer absolute paths.
            absent = project / "assets/unavailable"
            row.update(ap_path=str(absent / "ap.bin"), resource_path=None, ota_path=None,
                       status="unverified", desktop_verified=False)
            if key == profile_id:
                row.update(ap_path=resolved["assets"]["ap"]["path"],
                           resource_path=(resolved["assets"]["resource"]["path"]
                                          if "resource" in resolved["assets"] and not (generated and "ota" in resolved["assets"])
                                          else None),
                           ota_path=resolved["assets"].get("ota", {}).get("path"))
            rows.append(row)
        _write_json(project / "profiles/catalog.json", {"schema_version": 1, "profiles": rows})
        _write_json(project / "profiles/mi-band-scope.json", {"schema_version": 1, "scope": "xiaomi_smart_band_only",
                                                            "included_profile_ids": sorted(recipes)})
        prepared_assets = {} if generated else {
            role: data["path"] for role, data in resolved["assets"].items()
            if role in {"board", "resource", "factory", "seed", "seedProof", "resourceProof"}}
        identity_file = prepared / "bundle-identity.json"
        _write_json(identity_file, {"version": 1, "engineManifestSHA256": engine["sha256"],
                                   "catalogSHA256": resolved["catalogSHA256"], "profileID": profile_id,
                                   "assets": {role: {"sha256": item["sha256"], "bytes": item["bytes"]}
                                              for role, item in resolved["assets"].items()},
                                   "executionApproved": True, "approvalBasis": "bundled-local-runtime"})
        runtime = Path(__file__).parent
        bridge_paths = [p for p in runtime.glob("*.py") if not p.name.startswith("test_")]
        cli = runtime.parent / "azki-simulator.py"
        if cli.is_file():
            bridge_paths.append(cli)
        dependencies = [engine["root"] / path for path in engine["files"]]
        dependencies += [engine["root"] / "manifest.json", catalog.bundle_root / "catalog.json", identity_file]
        dependencies += [prepared / path for path in engine["files"] if path.startswith("lib/")]
        plan = inspect_backend(project, engine["root"] / engine["manifest"]["entrypoints"]["python"], profile_id,
                               prepared=prepared_assets, dependency_paths=dependencies, bridge_paths=bridge_paths,
                               bootstrap=engine["root"] / engine["manifest"]["entrypoints"]["bootstrap"],
                               engine_root=engine["root"], engine_info=engine)
        from .minative_bridge import create_minative_session
        if reservation is not None:
            reservation.check()
        descriptor = create_minative_session(container / "session", plan, instance_id=instance_id, library_root=library_root,
            virtual_key_file=virtual_key_file, virtual_key_sha256=virtual_key_sha256,
            virtual_serial=virtual_serial, virtual_address=virtual_address, skip_oobe=skip_oobe)
        if reservation is not None:
            reservation.check()
        return {**descriptor, "ok": True, "version": 1, "profileID": profile_id,
                "executionApproved": True, "approvalRequired": False, "approvalBasis": "bundled-local-runtime",
                "runtimeReady": True,
                "identityDigest": descriptor["approvalDigest"], "maxRuntimeSeconds": RUN_LIMIT_SECONDS,
                "identityMode": "generated" if generated else "prepared",
                "summary": resolved["item"]["displayName"] + " · 内置核心与所选固件已就绪，可直接运行",
                "summaryDetails": {"displayName": resolved["item"]["displayName"], "storageKind": resolved["storageKind"],
                            "engineSource": "bundled", "engineManifestSHA256": engine["sha256"],
                            "assetCount": len(resolved["assets"]), "firmwareModified": False,
                            "desktopVerified": False, "guestInstallSupported": True}}
    finally:
        # Keep a claimed/failed preparation for diagnosis; never clear or reuse it.
        if reservation is not None:
            reservation.close()
