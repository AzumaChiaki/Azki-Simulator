"""Restricted hardware inputs using the existing cross-platform validators.

No HTTP, Unix socket, frame reader, guest memory access or arbitrary QMP route
is exposed. Readable model properties never imply firmware sensor consumption.
"""
from __future__ import annotations

import json
from pathlib import Path
import re

from .control import WatchControl


INPUT_COMMANDS = frozenset((
    "pmu.adcRaw.get", "pmu.adcRaw.set", "pmu.adcChannels.set",
    "health.get", "health.set", "health.walk", "motion.orientation.set", "motion.raw.set",
    "live.get", "charger.set",
))


def model_profile(project, model):
    """Read the exported, original per-model wiring; never guess an alias."""
    path = Path(project) / "hardware-input-profiles.json"
    raw = path.read_bytes()
    if len(raw) > 32768:
        raise ValueError("Hardware profile metadata exceeds its bound")
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("Duplicate hardware profile key")
            result[key] = value
        return result
    def invalid_constant(value):
        raise ValueError("Nonfinite hardware profile value")
    data = json.loads(raw, object_pairs_hook=unique, parse_constant=invalid_constant)
    fields = {"schema", "source", "healthSensors", "batteryADCChannel", "batteryADCChannels", "batteryGauges"}
    if type(data) is not dict or set(data) != fields or type(data.get("schema")) is not int or data["schema"] != 1:
        raise ValueError("Unsupported hardware profile metadata")
    from .instance_data import SENSOR_BOARD
    prefix_pattern = re.compile(r"(?:miwear|mijia)\.watch\.[a-z0-9]+")
    def prefix(value):
        return type(value) is str and prefix_pattern.fullmatch(value) is not None
    def channel(value):
        return type(value) is int and 0 <= value <= 7
    source = data["source"]
    if (type(source) is not dict or set(source) != {"path", "sha256", "tables"} or
            source["path"] != "Sources/AzkiSimulator/Runtime/azki_simulator/minative.py" or
            type(source["sha256"]) is not str or not re.fullmatch(r"[a-f0-9]{64}", source["sha256"]) or
            source["tables"] != ["BATTERY_ADC_CHANNEL", "BATTERY_ADC_CHANNELS", "BATTERY_GAUGES", "HEALTH_SENSORS"]):
        raise ValueError("Invalid hardware profile source")
    if (not channel(data["batteryADCChannel"]) or type(data["batteryADCChannels"]) is not dict or
            any(not prefix(key) or not channel(value) for key, value in data["batteryADCChannels"].items())):
        raise ValueError("Invalid battery ADC wiring")
    if type(data["healthSensors"]) is not dict or len(data["healthSensors"]) > 32:
        raise ValueError("Invalid health wiring")
    for key, item in data["healthSensors"].items():
        if (not prefix(key) or type(item) is not dict or
                set(item) != {"board", "motion", "optical", "steps", "heartRate"} or
                type(item["board"]) is not str or len(item["board"]) > 512 or not SENSOR_BOARD.fullmatch(item["board"]) or
                any(type(item[field]) is not str or not re.fullmatch(r"[a-z0-9]{1,32}", item[field]) for field in ("motion", "optical")) or
                type(item["steps"]) is not bool or type(item["heartRate"]) is not bool):
            raise ValueError("Invalid health sensor wiring")
    if (type(data["batteryGauges"]) is not dict or len(data["batteryGauges"]) > 16 or
            any(type(key) is not str or not re.fullmatch(r"[a-z0-9]{1,32}", key) or
                type(items) is not list or len(items) > 32 or any(not prefix(value) for value in items)
                for key, items in data["batteryGauges"].items())):
        raise ValueError("Invalid battery gauge wiring")
    sensors = next((item for prefix, item in data["healthSensors"].items()
                    if model.startswith(prefix)), None)
    channel = next((value for prefix, value in data["batteryADCChannels"].items()
                    if model.startswith(prefix)), data["batteryADCChannel"])
    known_battery = any(model == "q66" or model.startswith(tuple(prefixes))
                        for prefixes in data["batteryGauges"].values())
    return {"sensors": sensors, "batteryChannel": channel if known_battery else None,
            "sourceSHA256": data["source"]["sha256"]}


class WindowsHardware:
    def __init__(self, qmp, directory, board, profile=None):
        self.closed = False
        self.control = WatchControl(qmp, directory)
        geometry = board.get("geometry", {})
        self.control.ntc_channels = geometry.get("pmu-adc-ntc", 0)
        # The export carries the same original per-model channel evidence as
        # the existing instance-settings path. Unknown models stay unmapped.
        self.control.battery_channel = (profile or {}).get("batteryChannel")
        self.control.probe_adc_raw()
        if getattr(qmp, "closed", False):
            raise ConnectionError("QMP transport closed during hardware discovery")

    def capabilities(self):
        if self.closed:
            raise RuntimeError("Hardware input session is closed")
        result = {"adcRaw": self.control.adc_raw_available,
                  "health": False, "orientation": False, "charger": False,
                  "guestObserved": False, "evidence": "model-property-only"}
        for name, operation in (("health", self.control.health),
                                ("orientation", self.control.orientation)):
            try:
                operation()
                result[name] = True
            except RuntimeError as error:
                # A correlated unsupported-property response is not a sensor
                # failure. Transport errors still propagate and poison QMP.
                if getattr(self.control.qmp, "closed", False):
                    raise
                result[name + "Reason"] = str(error)[:256]
        try:
            charger = self.control.qmp.execute("qom-get", {
                "path": "/machine", "property": "battery-charger"})
            result["charger"] = charger in ("unplugged", "charging", "full")
        except RuntimeError as error:
            if getattr(self.control.qmp, "closed", False):
                raise
            result["chargerReason"] = str(error)[:256]
        return result

    def dispatch(self, args):
        if self.closed:
            raise RuntimeError("Hardware input session is closed")
        if type(args) is not dict:
            raise ValueError("Expected a hardware input object")
        if args == {"command": "capabilities"}:
            return self.capabilities()
        if args.get("command") not in INPUT_COMMANDS:
            raise ValueError("Unsupported hardware input command")
        return self.control.dispatch_input(args)

    def close(self):
        # No server was started. This also makes a stopped adapter terminal.
        self.closed = True
        self.control.close()
