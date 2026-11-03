"""ASan/UBSan test of production Q65 IMU SPI and GPIO glue."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class Q65ImuBusTest(unittest.TestCase):
    def test_spi_dequeue_and_sensor_irq_domain(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler required")
        source = (ROOT / "engine/qemu-overlay/hw/arm/azki-q65.c").read_text()
        state = re.search(r"typedef struct \{\n    MemoryRegion mmio;\n"
                          r"    uint32_t regs\[0x8c / 4\];\n"
                          r"    Best1503State \*machine;.*?\} BesSpiConfig;", source, re.S)
        self.assertIsNotNone(state)
        names = ["panel_te_update_irq", "sensor_gpio_update_irq", "sensor_imu_schedule",
                 "sensor_imu_pins", "sensor_imu_transfer", "sensor_imu_observe",
                 "sensor_gpio_summary_read", "sensor_gpio_summary_write", "gpio_summary_read",
                 "spi0_unconnected_transfer", "spi0_read", "spi0_write"]
        blocks = []
        for name in names:
            block = re.search(r"^static [^;{}]*\b" + name + r"\([^;{}]*\)\n\{.*?^\}\n", source, re.M | re.S)
            self.assertIsNotNone(block, name)
            blocks.append(block.group())
        (ROOT / "build").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="q65-imu-bus-", dir=ROOT / "build") as directory:
            d = Path(directory)
            (d / "q65-spi-state.inc").write_text(state.group())
            (d / "q65-imu-glue.inc").write_text("\n".join(blocks))
            exe = d / "test"
            subprocess.run([compiler,"-O1","-g","-fsanitize=address,undefined",
                            "-I",str(d),"-I",str(ROOT / "engine/qemu-overlay/hw/arm"),
                            str(ROOT / "tests/test_q65_imu_bus.c"),"-o",str(exe)],check=True)
            result = subprocess.run([str(exe)],text=True,capture_output=True,
                                    env={**os.environ,"UBSAN_OPTIONS":"halt_on_error=1",
                                         "ASAN_OPTIONS":"abort_on_error=1"})
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn("independent sensor GPIO IRQ passed",result.stdout)
