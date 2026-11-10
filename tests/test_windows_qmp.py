"""A timed-out control write cannot be mistaken for another hardware input."""
from pathlib import Path
from types import SimpleNamespace
import json
import sys
import threading
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "engine/project/backend"))
from src.windows_qmp import WindowsQMP


class CorrelatedControls(unittest.TestCase):
    def transport(self, replies):
        writes, closes = [], []

        def write(handle, data, size, count, overlapped):
            writes.append(json.loads(data))
            count._obj.value = size
            return True

        transport = WindowsQMP.__new__(WindowsQMP)
        transport.api = SimpleNamespace(kernel=SimpleNamespace(WriteFile=write), close=closes.append)
        transport.timeout, transport.lock = 1, threading.RLock()
        transport.closed, transport.connected, transport.next_id, transport.handle = False, True, 0, 12

        def receive(timeout=None):
            value = replies.pop(0)
            if isinstance(value, Exception):
                raise value
            return value

        transport._receive = receive
        return transport, writes, closes

    def test_timeout_closes_pipe_and_does_not_repeat_uncertain_input(self):
        control, writes, closes = self.transport([TimeoutError("no reply")])
        with self.assertRaises(TimeoutError):
            control.execute("input-send-event", {"events": []})
        with self.assertRaises(ConnectionError):
            control.execute("input-send-event", {"events": []})
        self.assertEqual(len(writes), 1)
        self.assertEqual(closes, [12])
        self.assertTrue(control.closed)

    def test_reply_from_another_command_poisoned(self):
        control, writes, closes = self.transport([{"id": 9, "return": {}}])
        with self.assertRaisesRegex(ConnectionError, "does not match"):
            control.execute("query-status")
        self.assertTrue(control.closed)
        self.assertEqual(closes, [12])

    def test_submitted_hardware_write_with_wrong_id_is_unknown_and_never_resent(self):
        from src.windows_hardware import WindowsHardware
        from src.control import WatchControl
        for reply in ({"id": 9, "return": {}}, {"id": True, "return": {}}, []):
            control, writes, closes = self.transport([reply])
            adapter = WindowsHardware.__new__(WindowsHardware)
            adapter.closed = False
            adapter.control = WatchControl(control, Path(__file__).parent)
            adapter.control.adc_raw_available = True
            with self.assertRaisesRegex(RuntimeError, "input_outcome_unknown"):
                adapter.dispatch({"command": "pmu.adcRaw.set", "values": list(range(8))})
            self.assertEqual(len(writes), 1)
            self.assertEqual(writes[0]["execute"], "qom-set")
            self.assertTrue(control.closed)
            self.assertEqual(closes, [12])
            with self.assertRaises(ConnectionError):
                control.execute("query-status")
            self.assertEqual(len(writes), 1)

    def test_correlated_qmp_error_does_not_break_transport(self):
        control, writes, closes = self.transport([
            {"id": 1, "error": {"class": "CommandNotFound", "desc": "unsupported"}},
            {"id": 2, "return": {"status": "running"}},
        ])
        with self.assertRaises(RuntimeError):
            control.execute("unsupported-command")
        self.assertEqual(control.execute("query-status"), {"status": "running"})
        self.assertEqual([item["id"] for item in writes], [1, 2])
        self.assertFalse(control.closed)
        self.assertEqual(closes, [])


if __name__ == "__main__":
    unittest.main()
