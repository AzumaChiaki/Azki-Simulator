"""Bounded native commands to the existing runner's private virtual peer.

No QMP ownership, radio, credentials, or independent emulator lives here. The
manifest and instance binding jointly enable exact AP/profile pairs only.
"""
from __future__ import annotations

import os
from pathlib import Path
import re
import socket
import stat
import time
import uuid

from .minative import MError, _canonical, _decode, _inside_private, _peer_identity

PROFILE = "q66@4.100.177"
AP_SHA256 = "ff74c6f467529963b6afd40669437d8ec39a02e48c10d0893739847b3ba3dc2b"
PROFILE_ID = re.compile(r"[A-Za-z0-9._@-]{1,128}\Z")
MAX_REQUEST = 2048
MAX_RESPONSE = 65536
DEADLINE_SECONDS = 30.0
PEER_COMMANDS = ["status", "connect", "inventory", "install", "watchface-select", "operation-status"]
COMMANDS = {"guest-status": "status", "guest-connect": "connect", "guest-inventory": "inventory",
            "guest-install": "install", "guest-select": "watchface-select", "guest-operation": "operation-status"}
EXTERNAL_FIELDS = {"external-open": set(), "external-status": {"bridgeID"},
                   "external-reconnect": {"bridgeID"},
                   "external-close": {"bridgeID"}, "external-result": {"bridgeID", "operationID"},
                   "external-request": {"bridgeID", "operation", "key", "value", "offset", "enabled"}}
COMMANDS.update({name: name for name in EXTERNAL_FIELDS})
MODULE_NAMES = tuple(sorted(("__init__.py", "auth_crypto.py", "auth_protocol.py", "bind_info.py",
    "business_session.py", "central.py", "crypto.py", "external.py", "hci.py", "installer.py", "link.py",
    "MIT-miconnect.txt", "packages.py", "production_client.py", "secure_link.py", "service.py",
    "session_api.py", "smp.py", "state.py", "virtual_packages.py", "wire.py")))
RUNTIME_FILES = tuple("backend/src/virtual_peer/" + name for name in MODULE_NAMES)
SHA = re.compile(r"[0-9a-f]{64}\Z")
FILE = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\.bin\Z")


class _PeerRejection(MError):
    """A complete, correlated rejection is not an uncertain transport outcome."""


def checked_capability(capability, qemu_sha256):
    required = {"schema", "runnerProtocol", "controlProtocol", "transport", "qemuSHA256", "profiles",
                "stateSchema", "automaticConnect", "physicalRadio", "maxRequestBytes", "maxResponseBytes", "commands"}
    profiles = capability.get("profiles") if type(capability) is dict else None
    if (type(qemu_sha256) is not str or not SHA.fullmatch(qemu_sha256)
            or type(capability) is not dict or set(capability) != required
            or type(capability.get("schema")) is not int or capability["schema"] != 1
            or type(capability.get("runnerProtocol")) is not int or capability["runnerProtocol"] != 1
            or type(capability.get("controlProtocol")) is not int or capability["controlProtocol"] != 1
            or capability.get("transport") != "local-virtual-hci"
            or type(capability.get("qemuSHA256")) is not str or not SHA.fullmatch(capability["qemuSHA256"])
            or type(capability.get("stateSchema")) is not int or capability["stateSchema"] != 1
            or type(capability.get("automaticConnect")) is not bool or capability["automaticConnect"] is not False
            or type(capability.get("physicalRadio")) is not bool or capability["physicalRadio"] is not False
            or type(capability.get("maxRequestBytes")) is not int or capability["maxRequestBytes"] != MAX_REQUEST
            or type(capability.get("maxResponseBytes")) is not int or capability["maxResponseBytes"] != MAX_RESPONSE
            or capability.get("commands") != PEER_COMMANDS
            or type(profiles) is not dict or not 1 <= len(profiles) <= 64):
        raise MError("unsupported_virtual_peer", "虚拟通信声明与已核验核心不匹配")
    for profile_id, descriptor in profiles.items():
        if (type(profile_id) is not str or PROFILE_ID.fullmatch(profile_id) is None
                or type(descriptor) is not dict or set(descriptor) != {"apSHA256"}
                or type(descriptor["apSHA256"]) is not str or SHA.fullmatch(descriptor["apSHA256"]) is None):
            raise MError("unsupported_virtual_peer", "虚拟通信固件表格式无效")
    return dict(capability)


def supports_peer(plan):
    if type(plan) is not dict or type(plan.get("profileID")) is not str:
        return False
    identity = plan.get("identity", {})
    if type(identity) is not dict or type(identity.get("ap")) is not dict:
        return False
    binding = identity.get("instanceStorage", {})
    if type(binding) is not dict or binding.get("virtualPeerEnabled") is not True:
        return False
    try:
        capability = checked_capability(identity.get("runtimeCapabilities", {}).get("virtualPeer"),
                                         identity.get("qemu", {}).get("sha256"))
        descriptor = capability["profiles"].get(plan["profileID"])
        from .instance_storage import VIRTUAL_PROFILES
        expected_ap = VIRTUAL_PROFILES.get(plan["profileID"], (None, None))[0]
        if type(descriptor) is not dict or expected_ap is None:
            return False
    except (MError, AttributeError):
        return False
    return True


def validate_control(value):
    if (type(value) is not dict or set(value) != {"schema", "socket", "transport", "automaticConnect"}
            or type(value["schema"]) is not int or value["schema"] != 1
            or value["transport"] != "local-virtual-hci" or value["automaticConnect"] is not False
            or type(value["socket"]) is not str):
        raise MError("unsafe_control", "虚拟通信端点声明无效")


def _uuid(value):
    try:
        return type(value) is str and str(uuid.UUID(value)) == value
    except (ValueError, AttributeError):
        return False


def validate_arguments(command, arguments):
    if command not in COMMANDS or type(arguments) is not dict:
        raise MError("invalid_guest_operation", "未知设备操作")
    if command in EXTERNAL_FIELDS:
        fields = EXTERNAL_FIELDS[command]
        if command == "external-open" and "connectionTimeoutSeconds" in arguments:
            fields = fields | {"connectionTimeoutSeconds"}
            timeout = arguments["connectionTimeoutSeconds"]
            if type(timeout) is not int or not 1 <= timeout <= 300:
                raise MError("invalid_guest_operation", "连接超时须为 1–300 秒的整数")
        if set(arguments) != fields:
            raise MError("invalid_guest_operation", "蓝牙操作参数不匹配")
        for key in ("bridgeID", "operationID"):
            if key in arguments and not _uuid(arguments[key]):
                raise MError("invalid_guest_operation", "蓝牙会话已失效")
        if command == "external-request":
            if (arguments["operation"] not in ("read", "write", "subscribe")
                    or type(arguments["key"]) is not str or not re.fullmatch(r"h[0-9]+", arguments["key"])
                    or type(arguments["value"]) is not str or len(arguments["value"]) > 1024
                    or len(arguments["value"]) % 2 or re.fullmatch(r"[0-9a-fA-F]*", arguments["value"]) is None
                    or type(arguments["offset"]) is not int or not 0 <= arguments["offset"] <= 512
                    or type(arguments["enabled"]) is not bool):
                raise MError("invalid_guest_operation", "蓝牙读写参数无效")
        return {"schema": 1, "requestID": str(uuid.uuid4()), "command": command, **arguments}
    fields = {"expected_session_id", "expected_generation"} if command != "guest-status" else set()
    fields |= {"guest-install": {"fileName", "sha256"}, "guest-operation": {"operationID"},
               "guest-select": {"identifier"}, "guest-inventory": {"kind"}}.get(command, set())
    if "connectionTimeoutSeconds" in arguments:
        fields.add("connectionTimeoutSeconds")
    if set(arguments) != fields:
        raise MError("invalid_guest_operation", "设备操作参数不匹配")
    request = {"schema": 1, "requestID": str(uuid.uuid4()), "command": COMMANDS[command]}
    if "connectionTimeoutSeconds" in arguments:
        timeout = arguments["connectionTimeoutSeconds"]
        if type(timeout) is not int or not 1 <= timeout <= 300:
            raise MError("invalid_guest_operation", "连接超时须为 1–300 秒的整数")
        request["connectionTimeoutSeconds"] = timeout
    if command != "guest-status":
        session, generation = arguments["expected_session_id"], arguments["expected_generation"]
        if (type(session) is not str or not 1 <= len(session) <= 128
                or not re.fullmatch(r"[a-zA-Z0-9_.:-]+", session)
                or type(generation) is not int or not 0 <= generation < 2**53):
            raise MError("invalid_guest_operation", "设备会话或代际无效")
        request.update(sessionID=session, generation=generation)
    if command == "guest-install":
        if (type(arguments["fileName"]) is not str or not FILE.fullmatch(arguments["fileName"])
                or type(arguments["sha256"]) is not str or not SHA.fullmatch(arguments["sha256"])):
            raise MError("invalid_guest_package", "安装文件身份无效")
    elif command == "guest-select":
        if type(arguments["identifier"]) is not str or not re.fullmatch(r"(?:[0-9]{12}|L[0-9]{11})", arguments["identifier"]):
            raise MError("invalid_guest_operation", "表盘 ID 无效")
        request["identifier"] = arguments["identifier"]
    elif command == "guest-operation":
        if not _uuid(arguments["operationID"]):
            raise MError("invalid_guest_operation", "设备操作 ID 无效")
        request["operationID"] = arguments["operationID"]
    elif command == "guest-inventory":
        if arguments["kind"] not in ("watchface", "quickapp"):
            raise MError("invalid_guest_operation", "请选择应用或表盘库存")
        request["kind"] = arguments["kind"]
    return request


def _remaining(deadline):
    seconds = deadline - time.monotonic()
    if seconds <= 0:
        raise MError("guest_timeout", "设备操作响应超时")
    return seconds


def _response(data, request):
    value = _decode(data)
    if (type(value) is not dict or type(value.get("schema")) is not int or value.get("schema") != 1
            or value.get("requestID") != request["requestID"] or type(value.get("ok")) is not bool):
        raise MError("guest_protocol", "设备操作响应身份不匹配")
    if value["ok"]:
        if set(value) != {"schema", "requestID", "ok", "result"} or type(value["result"]) is not dict:
            raise MError("guest_protocol", "设备操作结果无效")
        return value["result"]
    error = value.get("error")
    if (set(value) != {"schema", "requestID", "ok", "error"} or type(error) is not dict
            or set(error) != {"code", "message"} or type(error["code"]) is not str
            or not re.fullmatch(r"[a-z_]{1,64}", error["code"])
            or type(error["message"]) is not str or len(error["message"]) > 512):
        raise MError("guest_protocol", "设备操作错误响应无效")
    raise _PeerRejection(error["code"], error["message"])


def execute(adapter, command, arguments):
    request = validate_arguments(command, arguments)
    if adapter is None:
        raise MError("not_running", "请先启动实例")
    if not supports_peer(adapter.plan):
        raise MError("unsupported_virtual_peer", "此实例尚未配置可用的虚拟通信身份")
    deadline = time.monotonic() + request.get("connectionTimeoutSeconds", DEADLINE_SECONDS)
    if not adapter._lock.acquire(timeout=_remaining(deadline)):
        raise MError("guest_timeout", "设备操作等待实例超时")
    try:
        if (adapter._stop_requested or adapter.process is None or adapter.process.poll() is not None
                or adapter._control is None):
            raise MError("not_running", "实例正在停止或尚未就绪")
        process, run, root, profile = adapter.process, adapter.run, adapter.root, adapter.plan["profileID"]
        control = adapter._read_control()
        runner_pid = adapter._runner_pid()
    finally:
        adapter._lock.release()

    def check_current():
        if not adapter._lock.acquire(timeout=_remaining(deadline)):
            raise MError("guest_timeout", "设备操作等待实例超时")
        try:
            if (adapter._stop_requested or adapter.process is not process or process.poll() is not None
                    or adapter.run != run or adapter._control is None):
                raise MError("not_running", "实例已停止或会话已变化")
        finally:
            adapter._lock.release()

    # Staging and peer IO must not hold the adapter's status/frame/lifecycle
    # lock. Bind the response to the captured process and private run instead.
    sent = False
    mutation = command in ("guest-connect", "guest-install", "guest-select", "guest-inventory")
    try:
        validate_control(control.get("virtual_peer"))
        path = _inside_private(control["virtual_peer"]["socket"], run / "tmp")
        if path.name != "peer.sock" or path.parent != Path(control["input_socket"]).parent:
            raise MError("unsafe_control", "虚拟通信端点不属于本实例")
        before = path.lstat()
        if not stat.S_ISSOCK(before.st_mode) or before.st_uid != os.getuid() or stat.S_IMODE(before.st_mode) != 0o600:
            raise MError("unsafe_control", "虚拟通信端点权限无效")
        if command == "guest-install":
            from .virtual_packages import stage_watchface
            source = root / "incoming" / arguments["fileName"]
            # The native actor supplies only this session's private incoming
            # copy. Reparse and rehash it here; UI inspection is never trusted.
            staged = stage_watchface(source, run, profile_id=profile,
                                     expected_source_sha256=arguments["sha256"])
            request.update(filename=staged["filename"], sha256=staged["sha256"])
        payload = _canonical(request) + b"\n"
        if len(payload) > MAX_REQUEST:
            raise MError("invalid_guest_operation", "设备操作请求过大")
        check_current()
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(_remaining(deadline))
            connection.connect(str(path))
            if _peer_identity(connection) != (runner_pid, os.getuid()):
                raise MError("peer_identity", "虚拟通信进程不属于本实例")
            after = path.lstat()
            if ((before.st_dev, before.st_ino) != (after.st_dev, after.st_ino)
                    or not stat.S_ISSOCK(after.st_mode) or after.st_uid != os.getuid()
                    or stat.S_IMODE(after.st_mode) != 0o600):
                raise MError("peer_identity", "虚拟通信端点连接时被替换")
            check_current()
            connection.settimeout(_remaining(deadline))
            sent = True
            connection.sendall(payload)
            received = bytearray()
            while b"\n" not in received:
                connection.settimeout(_remaining(deadline))
                chunk = connection.recv(MAX_RESPONSE + 1 - len(received))
                if not chunk:
                    raise MError("guest_protocol", "虚拟通信未返回完整响应")
                received.extend(chunk)
                if len(received) > MAX_RESPONSE:
                    raise MError("guest_protocol", "虚拟通信响应超过上限")
            if received.count(b"\n") != 1 or not received.endswith(b"\n"):
                raise MError("guest_protocol", "虚拟通信返回多条响应")
            check_current()
            return _response(bytes(received[:-1]), request)
    except _PeerRejection:
        raise
    except (OSError, MError) as error:
        if sent and mutation:
            raise MError("guest_outcome_unknown", "请求可能已受理；请查看设备状态，不要自动重试") from error
        if isinstance(error, MError):
            raise
        if isinstance(error, socket.timeout):
            raise MError("guest_timeout", "设备连接超时；实例仍保持运行") from error
        raise MError("guest_transport", "虚拟通信暂不可用") from error
