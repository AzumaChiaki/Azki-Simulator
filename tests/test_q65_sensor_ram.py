"""Check the production sensor RAMX mapping with ASan and UBSan."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Q65SensorRamTests(unittest.TestCase):
    def test_original_firmware_ram_execution_alias(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler required")
        source = (ROOT / "engine/qemu-overlay/hw/arm/azki-q65.c").read_text()
        function = re.search(
            r"^static void sensor_map_exec_alias\([^;{}]*\)\n"
            r"\{.*?^\}\n", source, re.M | re.S)
        self.assertIsNotNone(function)
        (ROOT / "build").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="q65-ram-", dir=ROOT / "build") as temp:
            temp = Path(temp)
            (temp / "q65-sensor-ram.inc").write_text(function.group())
            binary = temp / "q65-ram"
            subprocess.run([compiler, "-O1", "-g", "-fsanitize=address,undefined",
                            "-I", str(temp), str(ROOT / "tests/test_q65_sensor_ram.c"),
                            "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], text=True, capture_output=True,
                                    env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1",
                                         "ASAN_OPTIONS": "abort_on_error=1"})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("code/data coherence, bounds and AP alias passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
