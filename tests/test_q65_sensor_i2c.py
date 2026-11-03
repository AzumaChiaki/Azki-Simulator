"""Exercise the production DesignWare controller with no sensor slaves."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Q65SensorI2cTests(unittest.TestCase):
    def test_unconnected_sensor_bus(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler required")
        source = (ROOT / "engine/qemu-overlay/hw/arm/azki-q65.c").read_text()
        state = re.search(r"typedef struct \{\n    MemoryRegion mmio;\n"
                          r"    qemu_irq irq;\n    hwaddr base;.*?\} BesI2c;",
                          source, re.S)
        self.assertIsNotNone(state)
        blocks = [state.group()]
        for name in ["i2c_raw", "i2c_update", "i2c_valid", "i2c_read", "i2c_write"]:
            block = re.search(r"^static [^;{}]*\b" + name +
                              r"\([^;{}]*\)\n\{.*?^\}\n", source, re.M | re.S)
            self.assertIsNotNone(block, name)
            blocks.append(block.group())
        (ROOT / "build").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="q65-i2c-", dir=ROOT / "build") as temp:
            temp = Path(temp)
            (temp / "q65-sensor-i2c.inc").write_text("\n".join(blocks))
            binary = temp / "q65-i2c"
            subprocess.run([compiler, "-O1", "-g", "-fsanitize=address,undefined",
                            "-I", str(temp), str(ROOT / "tests/test_q65_sensor_i2c.c"),
                            "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], text=True, capture_output=True,
                                    env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1",
                                         "ASAN_OPTIONS": "abort_on_error=1"})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("controller isolation passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
