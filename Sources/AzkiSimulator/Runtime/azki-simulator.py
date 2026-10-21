#!/usr/bin/env python3
"""Private runtime bridge for Azki Simulator. No global service."""
import argparse
import json
from pathlib import Path
import signal
import sys

# The bundled interpreter runs with -I -S -B. Import only this app's adjacent
# authored modules, never a cwd/PYTHONPATH/user-site package or .pth hook.
sys.path.insert(0, str(Path(__file__).resolve().parent))

from azki_simulator.bridge import SessionBridge, create_session, request, private_root, regular_bytes, MAX_MESSAGE


BUNDLED_ACTIONS = ("library-catalog", "library-import", "library-delete", "library-delete-all", "library-probe", "prepare-bundled", "instance-storage-finish",
                   "instance-identity", "instance-settings", "instance-parameters", "instance-delete", "instance-reset", "sessions-cleanup")
ACTION_FAILURES = {
    "library-catalog": "读取固件库失败，请稍后重试",
    "library-import": "导入固件失败，请确认所选文件是完整的 OTA 固件包后重试",
    "library-delete": "删除固件失败，请确认该固件已停止相关实例后重试",
    "library-delete-all": "清空固件库失败，请确认没有正在运行的实例后重试",
    "library-probe": "无法识别所选固件文件",
    "prepare-bundled": "准备启动实例失败；可在实例管理中重置该实例后重试，已保存的数据不受影响",
    "instance-storage-finish": "保存本次运行的数据失败；已保存的快照保持不变，下次启动会自动整理",
    "instance-identity": "读取实例身份失败",
    "instance-settings": "读取或保存实例设置失败",
    "instance-parameters": "读取或应用设备参数失败",
    "instance-delete": "删除实例数据失败",
    "instance-reset": "整理实例数据失败；已保存的数据未被删除",
    "sessions-cleanup": "清理临时会话失败",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    for action in BUNDLED_ACTIONS:
        library = commands.add_parser(action, help="Firmware library and private instance preparation")
        library.add_argument("--engine-root", type=Path, required=True)
        library.add_argument("--library-root", type=Path, required=True)
        if action in ("library-import", "library-probe"):
            library.add_argument("--input", type=Path, required=True)
        if action == "library-delete":
            library.add_argument("--sha256", required=True, help="要删除的导入 OTA 对象的完整 SHA-256")
        if action == "library-import":
            library.add_argument("--model",
                                 help="绑定此导入到用户选择的实例机型（完整模型名，如 miwear.watch.p65）")
            library.add_argument("--profile-id")
        if action == "prepare-bundled":
            library.add_argument("--profile-id", required=True)
            library.add_argument("--session-root", type=Path, required=True)
            library.add_argument("--reservation-nonce", help="One-use nonce for an already host-reserved private root")
            library.add_argument("--instance-id", help="Stable native instance UUID; enables only exact-core-declared storage")
            library.add_argument("--virtual-key-file", type=Path, help="Private native-generated ephemeral P-256 handoff")
            library.add_argument("--virtual-key-sha256", help="Exact native-generated handoff digest")
            library.add_argument("--virtual-serial", help="Requested synthetic serial for the next boot")
            library.add_argument("--virtual-address", help="Requested six-byte virtual Bluetooth address")
            library.add_argument("--skip-oobe", action=argparse.BooleanOptionalAction, default=True,
                                 help="Complete initial setup for new device storage")
        if action in ("instance-identity", "instance-delete", "instance-reset"):
            library.add_argument("--instance-id", required=True)
        if action == "sessions-cleanup":
            # Tests point this at a scratch folder; the app always uses /private/tmp.
            library.add_argument("--tmp-root", type=Path, default=Path("/private/tmp"), help=argparse.SUPPRESS)
        if action == "instance-settings":
            library.add_argument("--instance-id", required=True)
            library.add_argument("--set", help="Replace the instance settings with this JSON document")
        if action == "instance-parameters":
            library.add_argument("--instance-id", required=True)
            library.add_argument("--profile-id", required=True)
            library.add_argument("--session", type=Path, help="Exact selected instance session; omit for stopped settings")
            library.add_argument("--set", help='One typed field JSON, e.g. {"live.heartRate":90}; never auto-restart')
        if action == "instance-storage-finish":
            library.add_argument("--session", type=Path, required=True)
            library.add_argument("--instance-id", required=True)
    commands.add_parser("discover", help="List unconnected same-user session candidates; never auto-connect")
    power = commands.add_parser("power", help="Bounded core shutdown/cold restart/switch; never live reload")
    power.add_argument("session", type=Path)
    power.add_argument("operation", choices=("shutdown", "restart", "switch"))
    power.add_argument("--target", type=Path, help="Distinct already prepared and approved target session for switch")
    profiles = commands.add_parser("minative-profiles", help="Statically list the exact included Mi Band profiles")
    profiles.add_argument("project", type=Path)
    multi = commands.add_parser("prepare-minative", help="Statically pin a multi-device session; does not execute the supplied code")
    multi.add_argument("session", type=Path)
    multi.add_argument("--project", type=Path, required=True)
    multi.add_argument("--python", type=Path, required=True)
    multi.add_argument("--profile", required=True)
    for role in ("board", "resource", "factory", "seed", "seed-proof", "resource-proof"):
        multi.add_argument("--" + role, type=Path, required=role in ("board", "resource"))
    multi.add_argument("--dependency", type=Path, action="append", required=True,
                       help="Explicit non-system interpreter/library files or directories to pin; repeat as needed")
    multi.add_argument("--dependency-link", type=Path, action="append", default=[],
                       help="Explicit dynamic-library loading path with a pinned symbolic-link chain")
    initialize = commands.add_parser("prepare", help="Read-only fingerprint source; create a new private session")
    initialize.add_argument("session", type=Path)
    initialize.add_argument("--source", type=Path)
    initialize.add_argument("--reservation-nonce", help="One-use nonce for an already host-reserved private root")
    for role in ("qemu", "firmware", "resource"):
        initialize.add_argument("--" + role, type=Path)
    serve = commands.add_parser("serve", help="Serve this session over its private Unix socket")
    serve.add_argument("session", type=Path)
    serve.add_argument("--no-discovery", action="store_true", help="Use only this session socket; do not access the global registry")
    serve.add_argument("--native-broker", action="store_true",
                       help="Use the native host's fixed sibling-sandbox broker; this flag itself grants no isolation or execution authority")
    serve.add_argument("--max-runtime", type=int, default=0, help="0 runs until stopped (default); 1…28800 bounds an automated test")
    serve.add_argument("--allow-private-memfd", action="store_true",
                       help="Use only with explicit approval: permit run-local memfd-XXXXXX temporary files")
    for role in ("qemu", "firmware", "resource"):
        serve.add_argument("--approved-" + role + "-sha256")
    serve.add_argument("--approved-identity-sha256", help="Explicitly approved complete multi-device plan identity")
    call = commands.add_parser("call", help="One bounded JSON request over this session's Unix socket")
    call.add_argument("session", type=Path)
    call.add_argument("command", choices=["status", "start", "stop", "input", "screenshot", "logs",
                                          "inspect-package", "stage-package", "install", "uninstall"])
    call.add_argument("--arguments", default="{}", help="JSON object")
    args = parser.parse_args()
    if args.action in BUNDLED_ACTIONS:
        try:
            result = bundled_action(args)
        except Exception as error:  # Every failure is one JSON answer, never a trace.
            result = failure(args.action, error)
    elif args.action == "power":
        from azki_simulator.power import power_transaction
        result = power_transaction(args.session, args.operation, args.target)
    elif args.action == "minative-profiles":
        from azki_simulator.minative import list_profiles
        result = {"ok": True, "profiles": list_profiles(args.project)}
    elif args.action == "prepare-minative":
        from azki_simulator.minative import inspect_backend
        from azki_simulator.minative_bridge import create_minative_session
        package = Path(__file__).resolve().parent / "azki_simulator"
        bridge_paths = [Path(__file__).resolve(), *sorted(path for path in package.glob("*.py")
                                                        if not path.name.startswith("test_"))]
        prepared = {"board": args.board, "resource": args.resource,
                    "factory": args.factory, "seed": args.seed,
                    "seedProof": args.seed_proof, "resourceProof": args.resource_proof}
        plan = inspect_backend(args.project, args.python, args.profile,
                               prepared=prepared, dependency_paths=args.dependency, bridge_paths=bridge_paths,
                               dependency_links=args.dependency_link)
        result = create_minative_session(args.session, plan)
    elif args.action == "discover":
        from azki_simulator.discovery import discover
        result = {"ok": True, "sessions": discover()}
    elif args.action == "prepare":
        paths = {role: getattr(args, role) for role in ("qemu", "firmware", "resource")}
        if any(paths.values()) and not all(paths.values()):
            parser.error("Choose all three explicit input files")
        if args.source and any(paths.values()):
            parser.error("Choose --source or three input files")
        result = create_session(args.session, args.source, paths if all(paths.values()) else None,
                                reservation_nonce=getattr(args, "reservation_nonce", None))
    elif args.action == "call":
        result = request(args.session, args.command, json.loads(args.arguments))
    else:
        if not 0 <= args.max_runtime <= 28800:
            parser.error("Runtime must be 0 (unlimited) or 1…28800 seconds")
        if args.native_broker and not args.no_discovery:
            parser.error("Native broker sessions require --no-discovery")
        hashes = {role: getattr(args, "approved_" + role + "_sha256") for role in ("qemu", "firmware", "resource")}
        if any(hashes.values()) and not all(hashes.values()):
            parser.error("Execution needs all three explicitly approved SHA256 values")
        descriptor = json.loads(regular_bytes(private_root(args.session) / "session.json", MAX_MESSAGE))
        if descriptor.get("backend") == "minative":
            if any(hashes.values()) or args.allow_private_memfd:
                parser.error("Multi-device sessions use one complete identity approval and their dedicated isolation policy")
            from azki_simulator.minative_bridge import MinativeSessionBridge
            bridge = MinativeSessionBridge(args.session, args.approved_identity_sha256, args.max_runtime,
                                          native_broker=args.native_broker)
        else:
            if args.native_broker:
                parser.error("Native broker launch is available only for the exact prepared multi-device backend")
            if args.approved_identity_sha256:
                parser.error("Legacy sessions require the three exact input hashes")
            bridge = SessionBridge(args.session, hashes if all(hashes.values()) else None, args.max_runtime, args.allow_private_memfd)
        for sig in (signal.SIGINT, signal.SIGTERM):
            signal.signal(sig, lambda *_: bridge.shutdown.set())
        bridge.serve(discovery=not args.no_discovery)
        return 0
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result.get("ok", True) else 1


def failure(action, error):
    """Actionable Chinese error JSON; English internals become the detail."""
    message = str(error).strip()
    if isinstance(error, OSError) and error.strerror:
        message = error.strerror + ((": " + str(error.filename)) if error.filename else "")
    if not any("\u4e00" <= char <= "\u9fff" for char in message):
        summary = ACTION_FAILURES.get(action, "操作失败")
        detail = (type(error).__name__ + ": " + message) if message else type(error).__name__
        message = summary + "（详细信息：" + detail[:600] + "）"
    return {"ok": False, "version": 1, "error": {"code": getattr(error, "code", None) or "bundled_input_error",
                                                 "message": message[:1024]}}


def bundled_action(args):
    if args.action == "instance-reset":
        from azki_simulator.maintenance import reset_instance
        return reset_instance(args.library_root, args.instance_id)
    if args.action == "sessions-cleanup":
        from azki_simulator.maintenance import cleanup_sessions
        return cleanup_sessions(args.library_root, args.tmp_root)
    if args.action == "instance-identity":
        from azki_simulator.instance_storage import public_instance_identity
        return public_instance_identity(args.library_root, args.instance_id)
    if args.action == "instance-settings":
        from azki_simulator import instance_settings
        if args.set is not None:
            if len(args.set.encode("utf-8")) > 262144:
                raise instance_settings.SettingsError("设置内容过大")
            instance_settings.save(args.library_root, args.instance_id, json.loads(args.set))
        return instance_settings.describe(args.library_root, args.instance_id)
    if args.action == "instance-parameters":
        from azki_simulator.device_parameters import execute
        if args.set is not None and len(args.set.encode("utf-8")) > 65536:
            raise ValueError("参数内容过大")
        values = json.loads(args.set) if args.set is not None else None
        if args.set is not None and not isinstance(values, dict):
            raise ValueError("参数写入必须是 JSON 对象")
        return {"ok": True, **execute(args.engine_root, args.library_root, args.instance_id,
            args.profile_id, args.session, values)}
    if args.action == "instance-delete":
        from azki_simulator import instance_settings
        return instance_settings.discard(args.library_root, args.instance_id)
    if args.action == "instance-storage-finish":
        from azki_simulator.instance_storage import finish_session_storage
        return finish_session_storage(args.library_root, args.session, args.instance_id)
    # Static inspection is deliberately independent of the exact-hash catalog:
    # no catalog writes, prepared identity, emulator or guest execution.
    if args.action == "library-probe":
        from azki_simulator.firmware_probe import inspect_firmware
        return {"ok": True, **inspect_firmware(args.input)}
    from azki_simulator.firmware_catalog import FirmwareCatalog
    from azki_simulator.bundled_runtime import runtime_readiness, prepare_profile
    engine = args.engine_root.absolute()
    runtime = runtime_readiness(engine)
    catalog = FirmwareCatalog(engine.parent / "Firmware", args.library_root,
                              distribution="public" if runtime.get("profile") == "public" else None)
    if args.action == "library-delete":
        # Remove one imported OTA and reclaim only its exclusively-owned objects.
        # Force: a stale or missing index record never blocks removal, so a
        # firmware the UI still lists can always be deleted.
        return catalog.delete_import(args.sha256, force=True)
    if args.action == "library-delete-all":
        # Remove every imported firmware and reclaim all library object files.
        return catalog.delete_all_imports()
    if args.action == "library-catalog":
        # items: firmware the user imported. presets: instance hardware presets,
        # which are device metadata and never a locked firmware version list.
        return {"ok": True, "version": 1, "items": catalog.list_items(),
                "presets": catalog.hardware_presets(), "runtime": runtime}
    if args.action == "library-import":
        return {"ok": True, "version": 1, **catalog.import_file(args.input, model=args.model)}
    return prepare_profile(engine, args.library_root, args.profile_id, args.session_root,
                           reservation_nonce=getattr(args, "reservation_nonce", None),
                           instance_id=getattr(args, "instance_id", None),
                           virtual_key_file=getattr(args, "virtual_key_file", None),
                           virtual_key_sha256=getattr(args, "virtual_key_sha256", None),
                           virtual_serial=getattr(args, "virtual_serial", None),
                           virtual_address=getattr(args, "virtual_address", None),
                           skip_oobe=getattr(args, "skip_oobe", True))


if __name__ == "__main__":
    raise SystemExit(main())
