"""Compile the production optional LSM6DSO model with ASan and UBSan."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Q65ImuTest(unittest.TestCase):
    def test_register_fifo_and_raw_input_contract(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler required")
        (ROOT / "build").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="q65-imu-", dir=ROOT / "build") as d:
            exe = Path(d) / "test"
            subprocess.run([compiler, "-O1", "-g", "-fsanitize=address,undefined",
                            "-I", str(ROOT / "engine/qemu-overlay/hw/arm"),
                            str(ROOT / "tests/test_q65_imu.c"), "-o", str(exe)], check=True)
            result = subprocess.run([str(exe)], text=True, capture_output=True,
                                    env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1",
                                         "ASAN_OPTIONS": "abort_on_error=1"})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertRegex(result.stdout, r"Q65 raw IMU: \d+ checks passed")


if __name__ == "__main__":
    unittest.main()
