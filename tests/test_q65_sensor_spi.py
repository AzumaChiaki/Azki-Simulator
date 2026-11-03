"""Exercise the production bounded unconnected SPI fixture on sensor buses."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Q65SensorSpiTests(unittest.TestCase):
    def test_unconnected_sensor_spi(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler required")
        source = (ROOT / "engine/qemu-overlay/hw/arm/azki-q65.c").read_text()
        state = re.search(r"typedef struct \{\n    MemoryRegion mmio;\n"
                          r"    uint32_t regs\[0x8c / 4\];\n"
                          r"    Best1503State \*machine;.*?\} BesSpiConfig;",
                          source, re.S)
        self.assertIsNotNone(state)
        blocks = []
        for name in ["spi0_unconnected_transfer", "spi0_read", "spi0_write"]:
            block = re.search(r"^static [^;{}]*\b" + name +
                              r"\([^;{}]*\)\n\{.*?^\}\n", source, re.M | re.S)
            self.assertIsNotNone(block, name)
            blocks.append(block.group())
        (ROOT / "build").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="q65-spi-", dir=ROOT / "build") as temp:
            temp = Path(temp)
            (temp / "q65-spi-state.inc").write_text(state.group())
            (temp / "q65-spi-functions.inc").write_text("\n".join(blocks))
            binary = temp / "q65-spi"
            subprocess.run([compiler, "-O1", "-g", "-fsanitize=address,undefined",
                            "-I", str(temp), "-I", str(ROOT / "engine/qemu-overlay/hw/arm"),
                            str(ROOT / "tests/test_q65_sensor_spi.c"),
                            "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], text=True, capture_output=True,
                                    env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1",
                                         "ASAN_OPTIONS": "abort_on_error=1"})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("unsupported modes passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
