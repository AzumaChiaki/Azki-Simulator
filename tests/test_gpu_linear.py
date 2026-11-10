"""Horizontal LINEAR row selection through the actual generic GPU decoder.

The fixture constructs normalized SDK inverse-translation coefficients and
uses independent constant-row expectations. No SDK, guest firmware, QEMU,
network, socket or radio is executed. Needs the C compiler/GLib used by QEMU.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GpuLinearTest(unittest.TestCase):
    def test_horizontal_linear_and_unfiltered_vertical_axis(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        pkg = shutil.which("pkg-config")
        if not compiler or not pkg:
            self.skipTest("C compiler and GLib required, as for QEMU")
        flags = shlex.split(subprocess.check_output(
            [pkg, "--cflags", "--libs", "glib-2.0"], text=True))
        (ROOT / "build").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="gpu-linear-", dir=ROOT / "build") as work:
            binary = Path(work) / "generic-linear"
            header = ROOT / "engine/qemu-overlay/hw/arm/best1503-gpu.h"
            command = [compiler, "-O1", "-g", "-fsanitize=address,undefined",
                       f'-DBES_GPU_HEADER="{header}"',
                       str(ROOT / "tests/test_gpu_linear.c"), *flags, "-o", str(binary)]
            compiled = subprocess.run(command, text=True, capture_output=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(binary)], text=True, capture_output=True,
                                    timeout=30, env={**os.environ,
                "ASAN_OPTIONS": "abort_on_error=1", "UBSAN_OPTIONS": "halt_on_error=1"})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("TOTAL: 104 vectors, 624 checks, 0 failures; guest_executed=false", result.stdout)


if __name__ == "__main__":
    unittest.main()
