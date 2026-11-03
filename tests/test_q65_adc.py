"""Run Q65's actual ADC conversion, QOM input and ISPI FIFO functions."""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ARM = ROOT / "engine/qemu-overlay/hw/arm"


class Q65AdcTests(unittest.TestCase):
    def test_input_conversion_and_guest_observation(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        pkg = shutil.which("pkg-config")
        if not compiler or not pkg:
            self.skipTest("C compiler and GLib required, as for QEMU")
        flags = shlex.split(subprocess.check_output(
            [pkg, "--cflags", "--libs", "glib-2.0"], text=True))
        source = (ARM / "azki-q65.c").read_text()
        structure = re.search(r"typedef struct \{\n    MemoryRegion mmio;\n"
                              r"    uint32_t regs\[0x8c / 4\];\n"
                              r"    uint16_t analog\[4\]\[5\]\[256\];.*?\} BesIspi;",
                              source, re.S)
        self.assertIsNotNone(structure)
        names = ["pmu_key_update_irq", "ispi_adc_update", "ispi_adc_trace",
                 "ispi_read", "ispi_write", "best1503_instance_init",
                 "best1503_set_adc_raw", "best1503_get_adc_state"]
        functions = []
        for name in names:
            match = re.search(r"^static [^;{}]*\b" + name +
                              r"\([^;{}]*\)\n\{.*?^\}\n", source, re.M | re.S)
            self.assertIsNotNone(match, "missing production function: " + name)
            functions.append(match.group())
        (ROOT / "build").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="q65-adc-", dir=ROOT / "build") as temp:
            temp = Path(temp)
            (temp / "q65-ispi-state.inc").write_text(structure.group())
            (temp / "q65-adc-functions.inc").write_text("\n".join(functions))
            binary = temp / "q65-adc"
            subprocess.run([compiler, "-O1", "-g", "-fsanitize=address,undefined",
                            "-I", str(temp), "-I", str(ARM),
                            str(ROOT / "tests/test_q65_adc.c"), *flags,
                            "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], text=True, capture_output=True,
                                    env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1",
                                         "ASAN_OPTIONS": "abort_on_error=1"})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertRegex(result.stdout, r"Q65 ADC production regression: \d+ checks passed")


if __name__ == "__main__":
    unittest.main()
