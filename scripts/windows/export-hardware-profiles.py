"""Export literal model wiring without importing the macOS runtime."""
import argparse
import ast
import hashlib
import json
from pathlib import Path

TABLES = ("BATTERY_ADC_CHANNEL", "BATTERY_ADC_CHANNELS", "BATTERY_GAUGES", "HEALTH_SENSORS")


def exported(source):
    raw = Path(source).read_bytes()
    values = {}
    for node in ast.parse(raw).body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1:
            target = node.targets[0]
            if isinstance(target, ast.Name) and target.id in TABLES:
                if target.id in values:
                    raise ValueError("Duplicate hardware table")
                values[target.id] = ast.literal_eval(node.value)
    if set(values) != set(TABLES):
        raise ValueError("Missing literal hardware tables")
    return {"schema": 1, "source": {
        "path": "Sources/AzkiSimulator/Runtime/azki_simulator/minative.py",
        "sha256": hashlib.sha256(raw).hexdigest(), "tables": list(TABLES)},
        "healthSensors": values["HEALTH_SENSORS"],
        "batteryADCChannel": values["BATTERY_ADC_CHANNEL"],
        "batteryADCChannels": values["BATTERY_ADC_CHANNELS"],
        "batteryGauges": values["BATTERY_GAUGES"]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.write_text(json.dumps(exported(args.source), indent=2) + "\n", encoding="utf-8")
