"""Production sensor watchdog handlers; QEMU ptimer is the clock boundary."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Q65SensorWatchdogTests(unittest.TestCase):
    def test_sensor_domain_watchdog(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler required")
        source = (ROOT / "engine/qemu-overlay/hw/arm/azki-q65.c").read_text()
        state = re.search(r"typedef struct \{\n    MemoryRegion mmio;\n"
                          r"    ptimer_state \*timer;.*?\} BesSensorWatchdog;",
                          source, re.S)
        self.assertIsNotNone(state)
        blocks = [state.group()]
        for name in ["sensor_watchdog_outputs", "sensor_watchdog_tick",
                     "sensor_watchdog_reset", "sensor_watchdog_read",
                     "sensor_watchdog_write"]:
            block = re.search(r"^static [^;{}]*\b" + name +
                              r"\([^;{}]*\)\n\{.*?^\}\n", source, re.M | re.S)
            self.assertIsNotNone(block, name)
            blocks.append(block.group())
        (ROOT / "build").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="q65-wdt-", dir=ROOT / "build") as temp:
            temp = Path(temp)
            (temp / "q65-sensor-watchdog.inc").write_text("\n".join(blocks))
            binary = temp / "q65-watchdog"
            subprocess.run([compiler, "-O1", "-g", "-fsanitize=address,undefined",
                            "-I", str(temp), str(ROOT / "tests/test_q65_sensor_watchdog.c"),
                            "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], text=True, capture_output=True,
                                    env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1",
                                         "ASAN_OPTIONS": "abort_on_error=1"})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("expiry and domain isolation passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
