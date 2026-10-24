"""Discoverable external device controls, reusing the native app's board inputs.

No guest execution, network download, or arbitrary runner command is exposed.
Live values are modeled hardware inputs, not claimed firmware display readings.
Each write changes one advertised field; it never starts/restarts a guest.
"""
from __future__ import annotations

import copy
import fcntl
import json
import math
import os
from pathlib import Path

from . import instance_settings as settings
from .bridge import request, private_root, regular_bytes, MAX_MESSAGE
from .minative import MError, battery_inputs, health_sensors, board_hardware
from .instance_storage import public_instance_identity, requested_virtual_identity
from .live_commands import validate_orientation

HOST_FILE = "external-settings.json"
LANGUAGES = ("zh_CN", "zh_TW", "zh_HK", "en_US", "en_GB", "ja_JP", "ko_KR", "de_DE", "fr_FR", "es_ES", "it_IT", "pt_BR", "ru_RU", "th_TH", "vi_VN")
LIVE = (
    ("heartRate", "脉搏输入（bpm）", "integer", 0, 255, "heartRate"),
    ("worn", "佩戴状态", "boolean", None, None, "heartRate"),
    ("cadence", "步频（步/分钟）", "integer", 40, 200, "steps"),
    ("addSteps", "追加模拟步行", "integer", 1, 100000, "steps"),
    ("batteryLevel", "电池电压目标（%）", "integer", 0, 100, "batteryLevel"),
    ("batteryRaw", "电池原始采样", "integer", 0, 65535, "batteryRaw"),
    ("charger", "充电状态", "enum", None, None, "charger"),
    ("temperatureC", "温度输入（°C）", "number", -40, 125, "temperature"),
    ("orientation", "IMU 姿态（度）", "object", None, None, "orientation"),
)


def _need(condition, message, code="invalid_parameters"):
    if not condition:
        raise MError(code, message)


def _row(id, label, kind, value, group, mode="restart", *, available=True, readonly=False, reason=None, **extra):
    row = dict(id=id, label=label, type=kind, value=value, group=group, applyMode=mode,
               available=bool(available), readOnly=bool(readonly), capability=id, description=extra.pop("description", ""))
    if reason:
        row["reason"] = reason
    row.update(extra)
    row.setdefault("requestedValue", value if str(row.get("valueSource", "")).startswith("next-start") else None)
    row.setdefault("observedValue", None)
    return row


def _options(values):
    return [{"value": value, "label": str(value)} for value in values]


def _host(root):
    path = root / HOST_FILE
    if not path.exists():
        return {"schema": 1, "skipOOBE": True}
    value = json.loads(settings._read_library(path, 65536))
    _need(type(value) is dict and value.get("schema") == 1 and
          set(value) <= {"schema", "skipOOBE", "virtualIdentity"}, "外部启动设置格式无效")
    _need(type(value.get("skipOOBE", True)) is bool, "跳过首次设置必须是布尔值")
    return value


def start_options(library, instance, model):
    host = _host(settings._instance(library, instance, create=True))
    result = {"skipOOBE": host.get("skipOOBE", True)}
    identity = host.get("virtualIdentity")
    if identity is not None:
        _need(type(identity) is dict and set(identity) == {"serial", "address"}, "虚拟身份必须同时包含 serial 和 address")
        result["virtualIdentity"] = requested_virtual_identity(identity["serial"], identity["address"], model)
    return result


def session_call(session, instance, profile):
    """Bind every request to the chosen instance/profile before contacting IPC."""
    if session is None:
        return None
    root = private_root(Path(session))
    descriptor = json.loads(regular_bytes(root / "session.json", MAX_MESSAGE))
    binding = descriptor.get("instanceStorage", {})
    _need(binding.get("instanceID") == instance and descriptor.get("profileID") == profile and
          binding.get("profileID") == profile, "会话不属于所选实例/固件", "instance_mismatch")
    def call(command, arguments=None):
        reply = request(root, command, arguments or {})
        if reply.get("ok") is False:
            error = reply.get("error", {})
            raise MError(error.get("code", "parameter_read_failed"), error.get("message", "读取设备失败"))
        return reply.get("result", reply)
    return call


def describe(library, instance, item, call=None):
    model, profile = item["model"], item["profileID"]
    receipt = settings.describe(library, instance)
    saved, observed = receipt["settings"], receipt.get("observed") or {}
    host = start_options(library, instance, model)
    status = call("status") if call else {}
    running = status.get("state") == "running" and status.get("adapterState", "running") == "running"
    live, adc, notes = {}, {}, []
    if receipt.get("settingsError"):
        notes.append(receipt["settingsError"])
    for command in ("live-get", "pmu-adc-get"):
        if running and command in status.get("capabilities", []):
            try:
                value = call(command)
                if command == "live-get":
                    live = value
                else:
                    adc = value
            except (ValueError, OSError) as error:
                notes.append(command + ": " + str(error)[:250])
    live_state, supported = live.get("state", {}), live.get("supported", {})
    sensors, battery = health_sensors(model) or {}, battery_inputs(model) or {}
    rows = []
    def add(*args, **kwargs):
        rows.append(_row(*args, **kwargs))
    for id, label, value in (("instance", "实例 UUID", instance), ("model", "设备型号", model),
                             ("profile", "固件配置", profile), ("firmwareVersion", "固件版本", item.get("firmwareVersion", item.get("version")))):
        add(id, label, "string", value, "设备", readonly=True, reason="创建时确定；更换机型/固件请创建对应实例")
    for key in ("width", "height"):
        add("display." + key, "屏幕" + ("宽度" if key == "width" else "高度"), "integer", status.get(key, item.get(key)), "显示与触摸", readonly=True, reason="由真实固件/硬件预设决定，不可任意改变")
    add("input.touch", "触摸输入", "boolean", running, "显示与触摸", readonly=True,
        reason="通过实际画面的按下、拖动、抬起操作；不是固件设置")
    add("boot.skipOOBE", "跳过新设备首次设置", "boolean", host["skipOOBE"], "启动", description="仅影响首次设置；保留原厂完整系统，不删除系统应用", valueSource="next-start-config")
    identity = public_instance_identity(library, instance).get("identity")
    add("identity.current", "上次保存的虚拟身份", "string", json.dumps(identity, ensure_ascii=False) if identity else None,
        "身份", readonly=True, reason="停止保存后读取；不包含配对密钥", valueSource="last-saved")
    add("boot.virtualIdentity", "下次启动虚拟身份（JSON）", "string", json.dumps(host["virtualIdentity"]) if host.get("virtualIdentity") else None,
        "身份", nullable=True, valueSource="next-start-config", description='同时输入 {"serial":"AZKI-DEMO","address":"02:12:34:56:78:9A"}；更改已有身份会重建此设备的初始数据和配对身份；原快照保留。恢复默认保留已有身份。')
    for key, label, kind, minimum, maximum, cap in LIVE:
        available = running and supported.get(cap) is True
        value = live_state.get(key)
        write_only = key in ("addSteps", "batteryLevel")
        extra = {"valueSource": "write-only-input" if write_only else "modeled-hardware", "writeOnly": write_only}
        if minimum is not None:
            extra.update(minimum=minimum, maximum=maximum, step=1 if kind == "integer" else .1)
        if key == "charger":
            extra["options"] = _options(("unplugged", "charging", "full"))
        if key == "orientation":
            extra.update(nullable=True, units="degrees", fields={axis: {"type": "number", "minimum": -180, "maximum": 180}
                         for axis in ("rollDegrees", "pitchDegrees")})
        add("live." + key, label, kind, value, "实时传感器", "live", available=available,
            reason=None if available else ("请先启动实例并读取硬件状态" if not running else "当前精确核心/固件没有此可调输入或读取失败"),
            description=("原子设置 rollDegrees/pitchDegrees；空值恢复自动动作。只改变原始 IMU 采样，保留板级轴映射；不会注入 Lua 数据。" if key == "orientation" else
                         "只改变模拟硬件输入；固件独立计算显示值。" + ("电量通过电压改变，固件会渐变且通常只在充电时上升。" if key == "batteryLevel" else "")), **extra)
    for key, label in (("walked", "已模拟行走步数"), ("walkSteps", "剩余模拟步数")):
        add("live." + key, label, "integer", live_state.get(key), "实时传感器", "live", readonly=True,
            available=running and supported.get("steps") is True, reason="模拟器输入计数，不是固件活动页步数", valueSource="modeled-hardware")
    boot = (("health", "heartRate", "开机脉搏（bpm）", "integer", 0, 255, sensors.get("heartRate")),
            ("health", "worn", "开机佩戴状态", "boolean", None, None, sensors.get("heartRate")),
            ("health", "cadence", "开机步频", "integer", 40, 200, sensors.get("steps")),
            ("health", "walkSteps", "每次开机模拟步数", "integer", 0, settings.MAX_STEPS, sensors.get("steps")),
            ("battery", "level", "开机电量目标（%）", "integer", 0, 100, "ocvMillivolts" in battery),
            ("battery", "sampleRaw", "开机电池原始采样", "integer", 0, 65535, bool(battery)),
            ("battery", "charger", "开机充电状态", "enum", None, None, battery.get("charger")))
    for section, key, label, kind, low, high, cap in boot:
        extra = {"nullable": True, "valueSource": "next-start-config"}
        if low is not None:
            extra.update(minimum=low, maximum=high, step=1)
        if kind == "enum":
            extra["options"] = _options(settings.CHARGERS)
        add("boot." + section + "." + key, label, kind, saved.get(section, {}).get(key), "启动传感器", available=bool(cap),
            reason=None if cap else "此机型没有对应模拟硬件", description="空值使用固件/模拟器默认；只在下次启动时生效", **extra)
    add("boot.battery.gauge", "电量计型号", "string", (board_hardware(model) or {}).get("batteryGauge"), "启动传感器", readonly=True,
        reason="由硬件预设决定；不接受与机型不一致的电量计")
    clock = saved["clock"]
    add("boot.clock.mode", "开机时间模式", "enum", clock["mode"], "时间", options=_options(("host", "offset", "firmware")), valueSource="next-start-config")
    add("boot.clock.offsetSeconds", "相对主机时间偏移（秒）", "integer", clock.get("offsetSeconds", 0), "时间",
        minimum=-settings.MAX_OFFSET, maximum=settings.MAX_OFFSET, step=1, valueSource="next-start-config", requestedValue=clock.get("offsetSeconds"), description="写入后选择 offset 模式；运行中的固件 RTC 不会被改写")
    properties = saved.get("properties", {})
    observed_props = observed.get("properties", {})
    keys = set(properties) | set(observed_props) | {"persist.locale", "persist.system_time_format", "persist.system_tz"}
    for key in sorted(keys - {"persist.rtc_save"}):
        if not isinstance(key, str) or settings.KEY.fullmatch(key) is None:
            continue
        options = LANGUAGES if key == "persist.locale" else ("true", "false") if key == "persist.system_time_format" else None
        # Preserve unknown observed values: never coerce an existing language to a preset.
        value = properties.get(key, observed_props.get(key))
        if options and value is not None and value not in options:
            options = (*options, value)
        add("property." + key, key, "enum" if options else "string", value, "固件系统属性", nullable=True,
            valueSource="next-start-override" if key in properties else "last-stopped-firmware", observedAt=observed.get("recordedAtUnix"), observedValue=observed_props.get(key), requestedValue=properties.get(key),
            description="空值删除该属性，让固件恢复默认；固件实际值在停止保存后采集", **({"options": _options(options)} if options else {}))
    add("boot.propertyOverrides", "全部属性覆盖（JSON）", "string", json.dumps(properties, ensure_ascii=False, sort_keys=True), "高级固件属性",
        valueSource="next-start-config", description="完整 persist.* 覆盖字典。可添加自定义属性；移除键取消覆盖，null 删除固件值。请保留其他已有项。")
    adc_state = adc.get("state", {})
    words = adc_state.get("values", [])
    for index in range(8):
        available = running and len(words) == 8
        add("adc." + str(index), "PMU 采样寄存器 0x%02X" % (0x56 + index), "integer", words[index] if available else None,
            "PMU 原始输入", "live", available=available, minimum=0, maximum=65535, step=1, valueSource="modeled-register",
            reason=None if available else "当前运行核心未声明可用的 PMU ADC 输入或读取失败", description="原始计数，非校准物理量；重启恢复默认",
            readCount=(adc_state.get("readCount") or [None]*8)[index], lastReadVersion=(adc_state.get("lastReadVersion") or [None]*8)[index], inputVersion=adc_state.get("version"))
    for id, label, kind, value, reason in (
        ("connection.virtualPeer", "本实例虚拟连接", "boolean", status.get("guestConnectSupported"), "由精确核心和固件能力决定；配对/安装使用专用连接接口"),
        ("connection.externalBluetooth", "对外蓝牙广播", "boolean", None, "仅原生应用的 CoreBluetooth 权限宿主可管理广播；后台开发会话未提供此能力"),
        ("debugger.enabled", "GDB 调试服务", "boolean", bool(status.get("debugger")), "由原生应用启动权限管理；后台开发入口未提供启用/端口/等待设置"),
        ("connection.timeout", "原生应用连接超时", "integer", None, "原生应用宿主偏好；SDK 安装请求使用独立 timeout 参数，不读取或修改原生偏好")):
        add(id, label, kind, value, "连接与调试", readonly=True, reason=reason, valueSource="runtime" if value is not None else "not-exposed")
    return {"schema_version": 1, "instance": instance, "model": model, "running": running, "parameters": rows,
            "start_options": host, "notes": notes + list(live.get("notes", {}).values()), "guestObserved": False}


def validate_value(row, value):
    _need(row["available"] and not row["readOnly"], row.get("reason", "该参数不可写"))
    if value is None:
        _need(row.get("nullable") is True, "该参数不允许空值")
        return
    kind = row["type"]
    if row["id"] == "live.orientation":
        validate_orientation(value)
        return
    valid = (type(value) is bool if kind == "boolean" else type(value) is int if kind == "integer" else
             type(value) in (int, float) and math.isfinite(value) if kind == "number" else type(value) is str)
    _need(valid, "参数类型错误：" + row["id"])
    if kind in ("integer", "number"):
        _need(row.get("minimum", -math.inf) <= value <= row.get("maximum", math.inf), "参数超出范围")
    if kind == "enum":
        _need(value in [option["value"] for option in row["options"]], "不支持的参数选项")
    if kind == "string":
        _need(len(value.encode()) <= 65536, "参数内容过大")


def apply(library, instance, item, values, call=None):
    _need(type(values) is dict and len(values) == 1, "每次只提交一个参数，避免部分执行和重复动作")
    schema = describe(library, instance, item, call)
    key, value = next(iter(values.items()))
    row = next((r for r in schema["parameters"] if r["id"] == key), None)
    _need(row is not None, "未声明的参数")
    validate_value(row, value)
    receipt = None
    if key.startswith("live."):
        receipt = call("live-set", {key[5:]: value})
    elif key.startswith("adc."):
        # Refetch immediately before this bounded change; preserve other channels.
        current = call("pmu-adc-get")["state"]["values"]
        words = list(current)
        words[int(key[4:])] = value
        receipt = call("pmu-adc-set", {"values": words})
    elif key in ("boot.skipOOBE", "boot.virtualIdentity"):
        root = settings._instance(library, instance, create=True)
        host = _host(root)
        if key == "boot.skipOOBE":
            host["skipOOBE"] = value
        elif value is None:
            host.pop("virtualIdentity", None)
        else:
            identity = json.loads(value)
            _need(type(identity) is dict and set(identity) == {"serial", "address"}, "虚拟身份必须同时包含 serial 和 address")
            host["virtualIdentity"] = requested_virtual_identity(identity["serial"], identity["address"], item["model"])
        settings._write(root / HOST_FILE, host)
    else:
        saved = copy.deepcopy(settings.load(library, instance))  # Never overwrite an unreadable file.
        if key.startswith("property."):
            saved["properties"][key[9:]] = value
        elif key == "boot.propertyOverrides":
            saved["properties"] = json.loads(value)
        elif key == "boot.clock.mode":
            saved["clock"] = {"mode": value, **({"offsetSeconds": saved["clock"].get("offsetSeconds", 0)} if value == "offset" else {})}
        elif key == "boot.clock.offsetSeconds":
            saved["clock"] = {"mode": "offset", "offsetSeconds": value}
        else:
            _, section, field = key.split(".")
            group = saved.setdefault(section, {})
            if value is None:
                group.pop(field, None)
            else:
                group[field] = value
            if section == "battery" and field in ("level", "sampleRaw") and value is not None:
                group.pop("sampleRaw" if field == "level" else "level", None)
        settings.save(library, instance, saved)
    result = {"schema_version": 1, "instance": instance, "applied": [key], "restart_required": row["applyMode"] == "restart", "guestObserved": False}
    if receipt is not None:
        result["receipt"] = receipt
    # A failed refresh after a successful write must not become a retryable write failure.
    try:
        result["parameters"] = describe(library, instance, item, call)
    except (ValueError, OSError) as error:
        result["refresh_error"] = str(error)[:300]
    return result


def execute(engine, library, instance, profile, session=None, values=None):
    from .firmware_catalog import FirmwareCatalog
    from .bundled_runtime import runtime_readiness
    runtime = runtime_readiness(engine)
    catalog = FirmwareCatalog(Path(engine).parent / "Firmware", library,
                              distribution="public" if runtime.get("profile") == "public" else None)
    resolved = catalog.resolve_profile(profile) if profile in catalog.profiles else catalog.resolve_generic(profile)
    item = dict(resolved["item"], profileID=profile)
    call = session_call(session, instance, profile)
    root = settings._instance(library, instance, create=True)
    fd = os.open(root / ".external-parameters.lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "r+") as stream:
        fcntl.flock(stream, fcntl.LOCK_EX if values is not None else fcntl.LOCK_SH)
        return apply(library, instance, item, values, call) if values is not None else describe(library, instance, item, call)
