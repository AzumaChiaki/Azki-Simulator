"""Exercise the original hardware validators through the Windows adapter."""
from pathlib import Path
import json
import sys
import tempfile
import unittest
import runpy

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "engine/project/backend"))
from src.windows_hardware import WindowsHardware, model_profile


class QMP:
    def __init__(self):
        self.closed = False
        self.writes = []
        self.adc = {"schema": 1, "fixtureEnabled": True, "version": 0,
                    "values": [5303] * 8, "readCount": [0] * 8,
                    "lastReadVersion": [0] * 8}
        self.properties = {"health-fixture": "walk=0;cadence=110;heart-rate=70;worn=1;walked=0",
                           "motion-orientation": "auto", "battery-charger": "unplugged"}
        self.fail_orientation_readback = False

    def execute(self, command, args):
        self.assert_property(args)
        name = args["property"]
        if command == "qom-get":
            if name == "pmu-adc-state":
                return json.dumps(self.adc)
            if name == "motion-orientation" and self.fail_orientation_readback and self.writes:
                raise RuntimeError("correlated readback error")
            if name not in self.properties:
                raise RuntimeError("property unavailable")
            return self.properties[name]
        if command != "qom-set":
            raise AssertionError("Unexpected QMP command")
        self.writes.append(dict(args))
        if name == "pmu-adc-raw":
            self.adc["values"] = list(map(int, args["value"].split(",")))
            self.adc["version"] += 1
        else:
            self.properties[name] = args["value"]
        return {}

    @staticmethod
    def assert_property(args):
        if args.get("path") != "/machine":
            raise AssertionError("Unexpected QOM path")


class HardwareTests(unittest.TestCase):
    def test_model_export_matches_authoritative_literals_and_keeps_unknown_unmapped(self):
        export = runpy.run_path(str(ROOT / "scripts/windows/export-hardware-profiles.py"))["exported"]
        source = ROOT / "Sources/AzkiSimulator/Runtime/azki_simulator/minative.py"
        metadata = json.loads((ROOT / "engine/project/hardware-input-profiles.json").read_text())
        self.assertEqual(metadata, json.loads(json.dumps(export(source))))
        self.assertEqual(model_profile(ROOT / "engine/project", "miwear.watch.p65")["batteryChannel"], 6)
        self.assertIsNone(model_profile(ROOT / "engine/project", "q66")["sensors"])
        self.assertIsNone(model_profile(ROOT / "engine/project", "unknown")["batteryChannel"])

    def test_export_cannot_inject_machine_options_or_duplicate_keys(self):
        data = json.loads((ROOT / "engine/project/hardware-input-profiles.json").read_text())
        with tempfile.TemporaryDirectory() as temporary:
            project = Path(temporary)
            path = project / "hardware-input-profiles.json"
            data["healthSensors"]["miwear.watch.q66"]["board"] += ",untrusted=on"
            path.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError, "sensor wiring"):
                model_profile(project, "miwear.watch.q66tc")
            path.write_text('{"schema":1,"schema":1}')
            with self.assertRaisesRegex(ValueError, "Duplicate"):
                model_profile(project, "miwear.watch.q66tc")

    def adapter(self):
        qmp = QMP()
        return qmp, WindowsHardware(qmp, ROOT, {"geometry": {"pmu-adc-ntc": 36}})

    def test_real_validator_getters_are_model_evidence_only(self):
        qmp, adapter = self.adapter()
        caps = adapter.dispatch({"command": "capabilities"})
        self.assertTrue(all(caps[key] for key in ("adcRaw", "health", "orientation", "charger")))
        self.assertFalse(caps["guestObserved"])
        live = adapter.dispatch({"command": "live.get"})
        self.assertIsNone(live["batteryChannel"])
        self.assertEqual(live["ntcChannels"], 36)
        self.assertEqual(qmp.writes, [])

    def test_no_arbitrary_qmp_touch_button_or_extra_fields_route(self):
        qmp, adapter = self.adapter()
        for request in ({"command": "qom-set", "property": "pc", "value": 1},
                        {"x": 1, "y": 1, "down": True},
                        {"command": "q65.button", "button": "lower", "clicks": 1},
                        {"command": "health.get", "unexpected": 1}):
            with self.assertRaises(ValueError):
                adapter.dispatch(request)
        self.assertEqual(qmp.writes, [])

    def test_adc_words_and_selected_channels_keep_existing_bounds(self):
        qmp, adapter = self.adapter()
        result = adapter.dispatch({"command": "pmu.adcRaw.set", "values": list(range(8))})
        self.assertTrue(result["accepted"])
        self.assertFalse(result["guestObserved"])
        adapter.dispatch({"command": "pmu.adcChannels.set", "channels": {"3": 65535}})
        self.assertEqual(qmp.adc["values"], [0, 1, 2, 65535, 4, 5, 6, 7])
        count = len(qmp.writes)
        for request in ({"command": "pmu.adcRaw.set", "values": [True] * 8},
                        {"command": "pmu.adcRaw.set", "values": [0] * 9},
                        {"command": "pmu.adcChannels.set", "channels": {"8": 0}}):
            with self.assertRaises(ValueError):
                adapter.dispatch(request)
        self.assertEqual(len(qmp.writes), count)

    def test_health_and_orientation_reject_invalid_values_without_writes(self):
        qmp, adapter = self.adapter()
        for request in ({"command": "health.set", "value": {"worn": 1}},
                        {"command": "health.walk", "steps": True},
                        {"command": "health.walk", "steps": 100001},
                        {"command": "motion.orientation.set", "value": {"rollDegrees": float("nan"), "pitchDegrees": 0}},
                        {"command": "charger.set", "value": "invented"}):
            with self.assertRaises(ValueError):
                adapter.dispatch(request)
        self.assertEqual(qmp.writes, [])

    def test_acknowledged_write_is_preserved_when_readback_fails(self):
        qmp, adapter = self.adapter()
        qmp.fail_orientation_readback = True
        result = adapter.dispatch({"command": "motion.orientation.set", "value": None})
        self.assertEqual(result, {"accepted": True, "guestObserved": False, "stateUnavailable": True})
        self.assertEqual(len(qmp.writes), 1)

    def test_unsupported_models_do_not_gain_capabilities(self):
        qmp = QMP()
        qmp.adc["fixtureEnabled"] = False
        qmp.properties = {}
        adapter = WindowsHardware(qmp, ROOT, {"geometry": {}})
        caps = adapter.dispatch({"command": "capabilities"})
        self.assertTrue(all(caps[key] is False for key in ("adcRaw", "health", "orientation", "charger")))
        with self.assertRaisesRegex(RuntimeError, "unsupported_hardware_input"):
            adapter.dispatch({"command": "pmu.adcRaw.get"})

    def test_transport_failure_is_not_reported_as_unsupported_sensor(self):
        class Broken(QMP):
            def execute(self, command, args):
                self.closed = True
                raise ConnectionError("owned pipe closed")
        with self.assertRaises(ConnectionError):
            WindowsHardware(Broken(), ROOT, {"geometry": {}})

    def test_stopped_adapter_is_terminal(self):
        _, adapter = self.adapter()
        adapter.close()
        with self.assertRaisesRegex(RuntimeError, "closed"):
            adapter.dispatch({"command": "health.get"})


if __name__ == "__main__":
    unittest.main()
