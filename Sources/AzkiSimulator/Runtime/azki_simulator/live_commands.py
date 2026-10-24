"""Live board inputs of a running instance through its runner input socket.

The host changes what the modeled hardware senses (the wearer walks, the pulse,
the charger, the battery voltage, the thermistors); the firmware notices it
through its own drivers and shows its own result. Nothing here writes a value
the firmware displays. These are fixed requests, never a generic QMP bridge.
"""
from __future__ import annotations

import math
import time

from .adc_commands import DEADLINE_SECONDS, runner_exchange
from .minative import MError, _decode, battery_inputs, battery_level_sample, health_sensors

# Band 10 / 10 Pro "[lc ntc]" thermistor table (N67 3.1.187 0x2c5e3e5c, same in
# P67 3.101.043): sensor millivolts at -40 … 125 C in 1 C steps.
LC_NTC_MV = (
    1578.97, 1565.20, 1550.73, 1535.23, 1519.03, 1502.56, 1485.10, 1467.16, 1448.61, 1429.11,
    1409.24, 1389.71, 1367.44, 1345.79, 1323.48, 1300.45, 1277.04, 1253.21, 1228.86, 1206.07,
    1178.96, 1156.40, 1126.90, 1100.80, 1074.95, 1048.69, 1024.68, 995.50, 969.33, 942.73,
    916.32, 890.32, 865.80, 840.50, 814.31, 790.23, 762.25, 737.55, 713.02, 688.78,
    667.69, 644.12, 621.42, 599.61, 578.73, 557.93, 538.13, 517.39, 498.39, 481.47,
    462.67, 443.99, 426.97, 410.38, 394.64, 379.07, 364.45, 350.65, 336.59, 323.22,
    311.60, 298.39, 286.16, 274.99, 260.26, 250.00, 240.19, 230.95, 221.45, 212.70,
    204.35, 196.41, 188.28, 180.75, 173.65, 166.99, 160.17, 154.08, 147.69, 141.83,
    136.24, 131.04, 126.31, 121.38, 116.03, 111.47, 107.09, 102.97, 98.94, 95.12,
    91.46, 87.95, 84.55, 81.33, 78.23, 75.25, 72.39, 69.69, 67.08, 64.55,
    62.12, 59.81, 57.58, 55.51, 53.44, 51.47, 49.60, 47.82, 46.08, 44.45,
    42.82, 41.30, 39.83, 38.41, 37.08, 35.82, 34.54, 33.39, 32.19, 31.10,
    30.00, 28.96, 27.95, 27.03, 26.13, 25.23, 24.40, 23.56, 22.79, 22.05,
    21.29, 20.60, 19.94, 19.27, 18.65, 18.06, 17.48, 16.90, 16.37, 15.85,
    15.36, 14.88, 14.41, 13.94, 13.51, 13.10, 12.69, 12.30, 11.93, 11.57,
    11.21, 10.88, 10.55, 10.24, 9.93, 9.64, 9.36, 9.08, 8.82, 8.56,
    8.32, 8.08, 7.85, 7.63, 7.41, 7.21)
LC_NTC_PREFIXES = ("miwear.watch.n67", "miwear.watch.p67")
ADC_SLOPE, ADC_OFFSET = 0.051538, -23.3

SET_KEYS = {"addSteps", "cadence", "heartRate", "worn", "charger", "batteryLevel", "batteryRaw", "temperatureC", "orientation"}
NOTES = {
    "clock": "运行中修改 RTC 固件不会采用（它只在开机时读取时间），请在固件设置中修改并重启实例。",
    "settings": "系统属性、语言、时区和时间格式只在开机时读取，修改后需重启实例。",
}


def _model(adapter):
    identity = adapter.plan.get("identity") if adapter is not None and type(adapter.plan) is dict else None
    if type(identity) is not dict:
        return None
    storage = identity.get("instanceStorage")
    if type(storage) is dict and type(storage.get("model")) is str:
        return storage["model"]
    profile = identity.get("profile")
    return profile.get("model") if type(profile) is dict else None


def _ntc_raw(celsius):
    if not -40 <= celsius <= 125:
        raise MError("invalid_live", "temperatureC must be within -40…125")
    low = min(164, int(celsius + 40))
    fraction = celsius + 40 - low
    millivolts = LC_NTC_MV[low] + fraction * (LC_NTC_MV[low + 1] - LC_NTC_MV[low])
    return round((millivolts - ADC_OFFSET) / ADC_SLOPE)


def _ntc_celsius(raw):
    millivolts = ADC_SLOPE * raw + ADC_OFFSET
    if millivolts >= LC_NTC_MV[0]:
        return -40.0
    for index in range(1, len(LC_NTC_MV)):
        if millivolts >= LC_NTC_MV[index]:
            upper, lower = LC_NTC_MV[index - 1], LC_NTC_MV[index]
            return round(index - 40 - (millivolts - lower) / (upper - lower), 1)
    return 125.0


def _exchange(adapter, request, *, write):
    deadline = time.monotonic() + DEADLINE_SECONDS
    try:
        payload = runner_exchange(adapter, request, write=write, deadline=deadline)
    except MError as error:
        if error.code == "runner_outcome_unknown":
            raise MError("live_outcome_unknown", "实时输入可能已生效但未确认，不要自动重复提交") from error
        raise
    try:
        result = _decode(payload)
    except (ValueError, TypeError) as error:
        if write and request["command"] == "motion.orientation.set":
            raise MError("live_outcome_unknown", "姿态写入回执无效，可能已生效，不要自动重复提交") from error
        raise
    if type(result) is not dict or set(result) not in ({"return"}, {"error"}):
        if write and request["command"] == "motion.orientation.set":
            raise MError("live_outcome_unknown", "姿态写入回执无效，可能已生效，不要自动重复提交")
        raise MError("live_protocol", "Runner returned an invalid live response")
    if "error" in result:
        message = str(result["error"])[:300]
        if message.startswith("orientation_outcome_unknown:"):
            raise MError("live_outcome_unknown", "姿态输入可能已生效但未确认，不要自动重复提交")
        if message.startswith("unsupported_hardware_input"):
            raise MError("unsupported_hardware", "此会话没有接入该硬件输入")
        raise MError("invalid_live", message)
    return result["return"]


def _report(adapter, snapshot):
    required = {"health", "charger", "adc", "batteryChannel", "ntcChannels"}
    if type(snapshot) is not dict or not required <= set(snapshot) <= required | {"orientation"}:
        raise MError("live_protocol", "Runner live snapshot has an invalid schema")
    model = _model(adapter)
    sensors = health_sensors(model) or {}
    battery = battery_inputs(model) or {}
    health, adc = snapshot["health"], snapshot["adc"]
    channel, ntc = snapshot["batteryChannel"], snapshot["ntcChannels"]
    battery_raw = adc[channel] if type(adc) is list and type(channel) is int else None
    ntc_list = [index for index in range(8) if type(ntc) is int and ntc >> index & 1]
    lc_ntc = type(model) is str and model.startswith(LC_NTC_PREFIXES) and type(adc) is list and bool(ntc_list)
    orientation = snapshot.get("orientation")
    if orientation is not None:
        if (type(orientation) is not dict or set(orientation) != {"mode", "rollDegrees", "pitchDegrees"} or
                orientation["mode"] not in ("auto", "held")):
            raise MError("live_protocol", "Runner returned invalid orientation state")
        angles = {k: orientation[k] for k in ("rollDegrees", "pitchDegrees")}
        if orientation["mode"] == "auto":
            if any(v is not None for v in angles.values()):
                raise MError("live_protocol", "Auto orientation must not claim a held pose")
        else:
            validate_orientation(angles)
    supported = {
        "steps": bool(health is not None and sensors.get("steps")),
        "heartRate": bool(health is not None and sensors.get("heartRate")),
        "charger": bool(snapshot["charger"] is not None and battery.get("charger")),
        "batteryLevel": battery_raw is not None and "ocvMillivolts" in battery,
        "batteryRaw": battery_raw is not None,
        "temperature": lc_ntc,
        "clock": False,
        "orientation": bool(orientation is not None and sensors.get("motion")),
    }
    notes = dict(NOTES)
    if not supported["temperature"]:
        notes["temperature"] = "此型号的热敏电阻换算尚未验证，暂不提供实时温度。"
    if supported["batteryLevel"]:
        notes["batteryLevel"] = ("只改变电池电压，固件电量计按自己的节奏追随：放电时逐步下降，"
                                 "只有在充电时才会回升。")
    state = {
        "walkSteps": health["walkSteps"] if supported["steps"] else None,
        "walked": health["walked"] if supported["steps"] else None,
        "cadence": health["cadence"] if supported["steps"] else None,
        "heartRate": health["heartRate"] if supported["heartRate"] else None,
        "worn": health.get("worn", True) if supported["heartRate"] else None,
        "charger": snapshot["charger"] if supported["charger"] else None,
        "batteryRaw": battery_raw,
        "temperatureC": _ntc_celsius(adc[ntc_list[0]]) if lc_ntc else None,
        "clock": None,
        "orientation": ({k: orientation[k] for k in ("rollDegrees", "pitchDegrees")}
                        if supported["orientation"] and orientation["mode"] == "held" else None),
    }
    return {"supported": supported, "notes": notes, "state": state}


def validate_orientation(value):
    """An atomic pose object in degrees; null releases the host-held pose."""
    if value is not None and (type(value) is not dict or set(value) != {"rollDegrees", "pitchDegrees"} or
            any(type(v) not in (int, float) or not -180 <= v <= 180 or not math.isfinite(v)
                for v in value.values())):
        raise MError("invalid_live", "orientation requires finite rollDegrees and pitchDegrees within -180…180, or null")


def _orientation_receipt(result):
    """Retain board acknowledgement; never turn a refresh into a second write."""
    invalid = MError("live_outcome_unknown", "姿态写入回执无效，可能已生效，不要自动重复提交")
    if type(result) is not dict or result.get("accepted") is not True or result.get("guestObserved") is not False:
        raise invalid
    if set(result) == {"accepted", "guestObserved", "stateUnavailable"} and result["stateUnavailable"] is True:
        return result
    if set(result) != {"accepted", "guestObserved", "state"}:
        raise invalid
    state = result["state"]
    if type(state) is not dict or set(state) != {"mode", "rollDegrees", "pitchDegrees"}:
        raise invalid
    angles = {k: state[k] for k in ("rollDegrees", "pitchDegrees")}
    if state["mode"] == "auto":
        if any(v is not None for v in angles.values()):
            raise invalid
    elif state["mode"] == "held":
        try:
            validate_orientation(angles)
        except MError as error:
            raise invalid from error
    else:
        raise invalid
    return result


def live_get(adapter, arguments):
    if arguments:
        raise MError("invalid_live", "live-get takes no arguments")
    return _report(adapter, _exchange(adapter, {"command": "live.get"}, write=False))


def live_set(adapter, arguments):
    if type(arguments) is not dict or len(arguments) != 1 or not set(arguments) <= SET_KEYS:
        raise MError("invalid_live", "live-set takes exactly one of " + ", ".join(sorted(SET_KEYS)))
    (key, value), = arguments.items()
    if key == "orientation":
        validate_orientation(value)
    report = live_get(adapter, {})
    supported = report["supported"]
    need = {"addSteps": "steps", "cadence": "steps", "heartRate": "heartRate", "worn": "heartRate",
            "charger": "charger",
            "batteryLevel": "batteryLevel", "batteryRaw": "batteryRaw", "temperatureC": "temperature",
            "orientation": "orientation"}[key]
    if not supported[need]:
        raise MError("unsupported_hardware", "此会话不支持实时修改该项")
    if key in ("addSteps", "cadence", "heartRate", "batteryLevel", "batteryRaw") and type(value) is not int:
        raise MError("invalid_live", key + " must be an integer")
    if key == "orientation":
        request = {"command": "motion.orientation.set", "value": value}
    elif key == "addSteps":
        if not 1 <= value <= 100000:
            raise MError("invalid_live", "addSteps must be within 1…100000")
        request = {"command": "health.walk", "steps": value}
    elif key == "cadence":
        if not 40 <= value <= 200:
            raise MError("invalid_live", "cadence must be within 40…200")
        request = {"command": "health.set", "value": {"cadence": value}}
    elif key == "heartRate":
        if not 0 <= value <= 255:
            raise MError("invalid_live", "heartRate must be within 0…255")
        request = {"command": "health.set", "value": {"heartRate": value}}
    elif key == "worn":
        if type(value) is not bool:
            raise MError("invalid_live", "worn must be true or false")
        request = {"command": "health.set", "value": {"worn": value}}
    elif key == "charger":
        if value not in ("unplugged", "charging", "full"):
            raise MError("invalid_live", "charger must be unplugged, charging or full")
        request = {"command": "charger.set", "value": value}
    else:
        snapshot = _exchange(adapter, {"command": "live.get"}, write=False)
        if key == "batteryLevel":
            if not 0 <= value <= 100:
                raise MError("invalid_live", "batteryLevel must be within 0…100")
            raw = battery_level_sample(battery_inputs(_model(adapter)), value)
            channels = {str(snapshot["batteryChannel"]): raw}
        elif key == "batteryRaw":
            if not 0 <= value <= 65535:
                raise MError("invalid_live", "batteryRaw must be within 0…65535")
            channels = {str(snapshot["batteryChannel"]): value}
        else:
            if type(value) not in (int, float) or value != value:
                raise MError("invalid_live", "temperatureC must be a number")
            raw = _ntc_raw(float(value))
            channels = {str(index): raw for index in range(8) if snapshot["ntcChannels"] >> index & 1}
        request = {"command": "pmu.adcChannels.set", "channels": channels}
    result = _exchange(adapter, request, write=True)
    if key == "orientation":
        # The board already read back the exact pose (or explicitly reported
        # an acknowledged write without state). A second live-get could fail
        # after success and falsely make the caller believe the write failed.
        return _orientation_receipt(result)
    if request["command"] in ("charger.set", "pmu.adcChannels.set"):
        return _report(adapter, result)
    return live_get(adapter, {})
