"""Per-instance firmware settings and clock, applied by the runner at boot.

Library file: <library>/instances/<uuid>/settings.json
  {"schema": 1, "properties": {"persist.locale": "en_US", "persist.x": null},
   "clock": {"mode": "host"} | {"mode": "offset", "offsetSeconds": int} | {"mode": "firmware"}}

properties: values written into the firmware's persist.db before boot; null
deletes the key so the firmware falls back to its own default.
clock "host" (default) boots with the Mac's current time and time zone,
"offset" with the Mac time shifted by offsetSeconds, "firmware" leaves the
saved clock alone. Firmware semantics (Band 9 3.1.32, verified): persist.rtc_save
is UTC seconds, persist.system_tz an IANA zone name, persist.system_time_format
"true" for a 12-hour clock.

The runner records the firmware's own values after each run; the host keeps
the latest copy as observed.json for display. No firmware code runs here.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
import stat
import time

from .minative import _canonical, _decode, _path, _read, battery_inputs, battery_level_sample, board_hardware

SCHEMA = 1
KEY = re.compile(r"persist\.[A-Za-z0-9._-]{1,120}\Z")
MAX_PROPERTIES = 512
MAX_VALUE = 1024
MAX_OFFSET = 100 * 366 * 86400
MAX_SAMPLE = 65535
CHARGERS = ("auto", "unplugged", "charging", "full")
GAUGES = ("auto", "hp4560", "hp4570")
MAX_STEPS = 10_000_000
MAX_HEART_RATE = 255
CADENCE_RANGE = (40, 200)
SETTINGS = "settings.json"
OBSERVED = "observed.json"
SESSION_FILE = "instance-settings.json"
ZONE = re.compile(r"[A-Za-z][A-Za-z0-9_+\-]*(/[A-Za-z0-9_+\-]+){0,2}\Z")


class SettingsError(ValueError):
    code = "invalid_settings"


def default():
    return {"schema": SCHEMA, "properties": {}, "clock": {"mode": "host"}}


def _battery(value):
    """Board battery input, still unbound to a model here."""
    if value is None:
        return {}
    if not isinstance(value, dict) or set(value) - {"sampleRaw", "charger", "gauge", "level"}:
        raise SettingsError("电池设置格式无效")
    resolved = {}
    level = value.get("level")
    if level is not None:
        if type(level) is not int or not 0 <= level <= 100:
            raise SettingsError("开机电量必须是 0…100 的整数")
        resolved["level"] = level
    sample = value.get("sampleRaw")
    if sample is not None:
        if type(sample) is not int or not 0 <= sample <= MAX_SAMPLE:
            raise SettingsError("电池采样原始值必须是 0…65535 的整数")
        resolved["sampleRaw"] = sample
    charger = value.get("charger", "auto")
    if charger not in CHARGERS:
        raise SettingsError("充电状态只能是 auto、unplugged、charging 或 full")
    if charger != "auto":
        resolved["charger"] = charger
    gauge = value.get("gauge", "auto")
    if gauge not in GAUGES:
        raise SettingsError("电量计型号只能是 auto、hp4560 或 hp4570")
    if gauge != "auto":
        resolved["gauge"] = gauge
    return resolved


def _health(value):
    """Wearer input for the preset's modeled motion and optical sensors.

    walkSteps: steps the wearer walks after each boot, counted by the firmware
    itself from the emulated IMU; cadence: steps per minute while walking;
    heartRate: pulse the emulated optical sensor sees (0 = not on a wrist).
    A legacy "steps" value is read as walkSteps.
    """
    if value is None:
        return {}
    if not isinstance(value, dict) or set(value) - {"steps", "walkSteps", "cadence", "heartRate"}:
        raise SettingsError("健康数据设置格式无效")
    resolved = {}
    steps = value.get("walkSteps", value.get("steps"))
    if steps is not None:
        if type(steps) is not int or not 0 <= steps <= MAX_STEPS:
            raise SettingsError("步行步数必须是 0…%d 的整数" % MAX_STEPS)
        resolved["walkSteps"] = steps
    cadence = value.get("cadence")
    if cadence is not None:
        if type(cadence) is not int or not CADENCE_RANGE[0] <= cadence <= CADENCE_RANGE[1]:
            raise SettingsError("步频必须是 %d…%d 步/分钟" % CADENCE_RANGE)
        resolved["cadence"] = cadence
    rate = value.get("heartRate")
    if rate is not None:
        if type(rate) is not int or not 0 <= rate <= MAX_HEART_RATE:
            raise SettingsError("心率必须是 0…%d 的整数" % MAX_HEART_RATE)
        resolved["heartRate"] = rate
    return resolved


def validate(value):
    """Return a normalized copy or raise SettingsError."""
    if (not isinstance(value, dict) or value.get("schema") != SCHEMA
            or set(value) - {"schema", "properties", "clock", "battery", "health"}):
        raise SettingsError("设置格式无效")
    properties = value.get("properties", {})
    if not isinstance(properties, dict) or len(properties) > MAX_PROPERTIES:
        raise SettingsError("属性数量超出限制")
    normalized = {}
    for key, item in properties.items():
        if not isinstance(key, str) or KEY.fullmatch(key) is None:
            raise SettingsError("属性名必须是 persist. 开头的字母、数字、点、下划线或连字符：" + str(key)[:80])
        if item is not None and (not isinstance(item, str) or len(item) > MAX_VALUE
                                 or any(c in item for c in "\0\n\r\t")):
            raise SettingsError("属性值必须是不含换行和制表符的文本（最多 1024 字）：" + key)
        normalized[key] = item
    clock = value.get("clock", {"mode": "host"})
    if not isinstance(clock, dict) or clock.get("mode") not in ("host", "offset", "firmware"):
        raise SettingsError("时钟模式无效")
    if clock["mode"] == "offset":
        offset = clock.get("offsetSeconds")
        if set(clock) != {"mode", "offsetSeconds"} or type(offset) is not int or abs(offset) > MAX_OFFSET:
            raise SettingsError("时间偏移无效")
        clock = {"mode": "offset", "offsetSeconds": offset}
    else:
        if set(clock) != {"mode"}:
            raise SettingsError("时钟模式字段无效")
        clock = {"mode": clock["mode"]}
    result = {"schema": SCHEMA, "properties": normalized, "clock": clock}
    battery, health = _battery(value.get("battery")), _health(value.get("health"))
    if battery:
        result["battery"] = battery
    if health:
        result["health"] = health
    return result


def _instance(library_root, instance_id, *, create=False):
    from .firmware_catalog import _owned_directory
    from .instance_storage import _uuid
    root = _path(library_root) / "instances" / _uuid(instance_id)
    fd = _owned_directory(root, create=create)
    os.close(fd)
    return root


def _read_library(path, limit):
    """Bounded read of a file this app owns, narrowing a wider mode back.

    Ownership and link checks stay; a mode changed by a restore or a file
    manager is repaired instead of blocking the settings panel.
    """
    from .firmware_catalog import _owned_file
    fd, before = _owned_file(path, limit)
    try:
        data = bytearray()
        while True:
            chunk = os.read(fd, min(65536, limit + 1 - len(data)))
            if not chunk:
                break
            data.extend(chunk)
            if len(data) > limit:
                raise SettingsError("设置文件超过大小限制")
        after = os.fstat(fd)
        if (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
                after.st_size, after.st_mtime_ns, after.st_ctime_ns):
            raise SettingsError("读取设置时文件发生变化")
        return bytes(data)
    finally:
        os.close(fd)


def _write(path, value):
    from .firmware_catalog import _atomic_json
    _atomic_json(path, value)


def load(library_root, instance_id):
    root = _instance(library_root, instance_id, create=True)
    path = root / SETTINGS
    if not path.exists():
        return default()
    return validate(_decode(_read_library(path, 65536)))


def save(library_root, instance_id, value):
    settings = validate(value)
    _write(_instance(library_root, instance_id, create=True) / SETTINGS, settings)
    return settings


def observed(library_root, instance_id):
    path = _instance(library_root, instance_id, create=True) / OBSERVED
    if not path.exists():
        return None
    try:
        value = _decode(_read_library(path, 4 * 1024 * 1024))
    except (OSError, ValueError):
        return None  # Informational copy only; the next run rewrites it.
    if not isinstance(value, dict) or not isinstance(value.get("properties"), dict):
        return None
    return value


def load_lenient(library_root, instance_id):
    """(settings, problem): unusable saved settings fall back to defaults.

    The file itself is left exactly as it is, so the user can fix or replace
    it from the settings panel; it never blocks a start or the panel.
    """
    try:
        return load(library_root, instance_id), None
    except SettingsError as error:
        return default(), "已保存的实例设置无法使用（" + str(error)[:200] + "），本次按默认设置运行；保存一次设置即可修复"
    except (OSError, ValueError) as error:
        return default(), "无法读取已保存的实例设置（" + str(error)[:200] + "），本次按默认设置运行；保存一次设置即可修复"


def describe(library_root, instance_id):
    settings, problem = load_lenient(library_root, instance_id)
    from .minative import BATTERY_INPUTS, HEALTH_SENSORS
    result = {"ok": True, "version": 1, "settings": settings, "observed": observed(library_root, instance_id),
              # Presets whose firmware a boot level / charger state really drives.
              "batteryInputs": [{"modelPrefix": prefix, "level": "levelKey" in inputs,
                                "charger": bool(inputs.get("charger"))}
                                for prefix, inputs in sorted(BATTERY_INPUTS.items())],
              # Presets whose own firmware counts the walked steps / measures
              # the simulated pulse from the modeled sensors.
              "healthInputs": [{"modelPrefix": prefix, "steps": bool(sensors.get("steps")),
                               "heartRate": bool(sensors.get("heartRate"))}
                               for prefix, sensors in sorted(HEALTH_SENSORS.items())]}
    if problem:
        result["settingsError"] = problem
    return result


def _remove_tree(path):
    """Depth-first removal that never follows a link and never leaves its root."""
    count = 0
    with os.scandir(path) as entries:
        for entry in entries:
            info = entry.stat(follow_symlinks=False)
            if info.st_uid != os.getuid():
                raise SettingsError("实例数据包含其他用户的文件，已停止删除：" + entry.name[:80])
            if stat.S_ISDIR(info.st_mode):
                count += _remove_tree(entry.path)
            elif stat.S_ISREG(info.st_mode) or stat.S_ISLNK(info.st_mode):
                os.unlink(entry.path)
                count += 1
            else:
                raise SettingsError("实例数据包含不支持的文件类型：" + entry.name[:80])
    os.rmdir(path)
    return count + 1


def discard(library_root, instance_id):
    """Delete one instance's private settings and storage from the library.

    The record store drops the instance first; this only removes the files that
    belonged to it. Links are unlinked, never traversed, and entries owned by
    another user abort the removal instead of being deleted.
    """
    from .instance_storage import _uuid
    identifier = _uuid(instance_id)
    container = _path(library_root) / "instances"
    root = container / identifier
    try:
        info = os.lstat(root)
    except FileNotFoundError:
        return {"ok": True, "version": 1, "instanceID": identifier, "removed": False, "entries": 0}
    if not stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode) or info.st_uid != os.getuid():
        raise SettingsError("实例数据目录不是本用户的普通目录，已停止删除")
    entries = _remove_tree(root)
    try:
        if not any(True for _ in os.scandir(container)):
            os.rmdir(container)
    except OSError:
        pass
    return {"ok": True, "version": 1, "instanceID": identifier, "removed": True, "entries": entries}


def resolve(settings, model):
    """Bind saved board options to one hardware preset for a session file.

    Returns the resolved battery block and the health block; both are None when
    the preset has no such device, in which case the core keeps its default.
    """
    hardware = board_hardware(model)
    if hardware is None:
        return None, None
    battery = dict(settings.get("battery") or {})
    if "gauge" in battery and battery["gauge"] != hardware["batteryGauge"]:
        # A gauge chosen for another preset (for example after the instance's
        # preset changed) is dropped for this board instead of failing the start.
        battery.pop("gauge")
    # A boot level becomes the firmware's own inputs: its saved state of charge
    # and the matching battery voltage sample. Explicit values win; a preset
    # without verified inputs keeps the firmware's default (no shown fake level).
    inputs = battery_inputs(model)
    level = battery.pop("level", None)
    if level is not None and inputs is not None and "levelKey" in inputs:
        battery.setdefault("sampleRaw", battery_level_sample(inputs, level))
        settings["properties"].setdefault(inputs["levelKey"], str(level * inputs["levelScale"]))
    if "charger" in battery and not (inputs or {}).get("charger"):
        battery.pop("charger")
    resolved = {"channel": hardware["batteryADCChannel"], "gauge": hardware["batteryGauge"],
                "coulomb": hardware["coulombCounter"]}
    resolved.update(battery)
    return resolved, _resolve_health(settings.get("health"), hardware)


def _resolve_health(health, hardware):
    """Session health block, or None when nothing the preset models is set.

    Only inputs verified to drive this preset's firmware are kept; the others
    are dropped instead of being passed to a board that would ignore them."""
    inputs = hardware.get("healthInputs") or {}
    if not health or not hardware.get("sensorBoard"):
        return None
    resolved = {}
    if inputs.get("steps") and "walkSteps" in health:
        resolved["walkSteps"] = health["walkSteps"]
        if "cadence" in health:
            resolved["cadence"] = health["cadence"]
    if inputs.get("heartRate") and "heartRate" in health:
        resolved["heartRate"] = health["heartRate"]
    if not resolved:
        return None
    resolved["sensorBoard"] = hardware["sensorBoard"]
    return resolved


def materialize(library_root, instance_id, directory, *, model=None):
    """Write the session's settings file; returns (path, sha256).

    model is the instance hardware preset. It binds board options to the devices
    that preset actually has and records them in the session file, so the
    sandboxed runner needs no model knowledge of its own.
    """
    import hashlib
    settings, _ = load_lenient(library_root, instance_id)
    # The native host passes its current zone; the session file is the only
    # place it is recorded, so the library settings stay host-independent.
    zone = os.environ.get("AZKI_HOST_TIMEZONE", "")
    if settings["clock"]["mode"] == "host" and len(zone) <= 64 and ZONE.fullmatch(zone):
        settings["clock"]["timeZone"] = zone
    # Health input reaches the core only bound to a preset's modeled sensors.
    library_health = settings.pop("health", None)
    if model is not None:
        settings["health"] = library_health
        battery, health = resolve(settings, model)
        settings.pop("health", None)
        if battery is not None:
            settings["battery"] = battery
        if health:
            settings["health"] = health
    path = Path(directory) / SESSION_FILE
    data = _canonical(settings) + b"\n"
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "wb") as out:
        out.write(data)
        out.flush()
        os.fsync(out.fileno())
    return str(path), hashlib.sha256(data).hexdigest()


def record_observed(library_root, instance_id, run_directory):
    """Copy the runner's post-run property dump, if any, into the instance."""
    source = Path(run_directory) / "instance-props-observed.json"
    if not source.exists():
        return False
    value = _decode(_read(source, private=True))
    if (not isinstance(value, dict) or not isinstance(value.get("properties"), dict)
            or len(value["properties"]) > 4096
            or not all(isinstance(k, str) and isinstance(v, str) and len(k) <= 128 and len(v) <= 4096
                       for k, v in value["properties"].items())):
        return False
    _write(_instance(library_root, instance_id) / OBSERVED,
           {"properties": value["properties"], "recordedAtUnix": int(time.time())})
    return True
