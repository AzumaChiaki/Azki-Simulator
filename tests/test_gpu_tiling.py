"""Production GPU pixel/addressing contracts; never execute guest firmware.

Run with: python3 -m unittest discover -s tests -p test_gpu_tiling.py -v
Needs the existing C compiler and GLib used by QEMU. Each test compiles the
actual backend with ASan/UBSan; source data and expected pixels come from
independent host fixtures. This does not certify per-device firmware effects.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADERS = ROOT / "engine/qemu-overlay/hw/arm"
FAMILIES = {
    "generic": ("best1503-gpu.h", [0, 1, 2, 3]),
    "p65": ("azki-p65-gpu.h", [0, 1, 2, 3, 4]),
    "q65": ("azki-q65-gpu.h", [0, 1, 2, 3, 4, 5]),
}


class GpuTilingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = shutil.which("clang") or shutil.which("cc")
        pkg = shutil.which("pkg-config")
        if not cls.compiler or not pkg:
            raise unittest.SkipTest("C compiler and GLib required, as for QEMU")
        cls.flags = shlex.split(subprocess.check_output(
            [pkg, "--cflags", "--libs", "glib-2.0"], text=True))
        (ROOT / "build").mkdir(exist_ok=True)
        cls.work = tempfile.TemporaryDirectory(prefix="gpu-tiling-", dir=ROOT / "build")
        cls.addClassCleanup(cls.work.cleanup)

    def run_fixture(self, family, kind=None, controls=False):
        header = HEADERS / FAMILIES[family][0]
        source = ROOT / "tests" / ("test_gpu_gaussian_rejection.c" if controls else
                                    "test_gpu_tiling_formats.c" if kind else
                                    "test_gpu_tiling.c")
        binary = Path(self.work.name) / f"{family}-{kind}-{'controls' if controls else 'tile'}"
        command = [self.compiler, "-O1", "-g", "-fsanitize=address,undefined",
                   f'-DBES_GPU_HEADER="{header}"']
        if family == "generic":
            command.append("-DGENERIC_COORDS=1")
        if kind:
            command.append(f"-DSOURCE_KIND={kind}")
        command += [str(source), *self.flags, "-o", str(binary)]
        build = subprocess.run(command, text=True, capture_output=True, timeout=60)
        self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
        result = subprocess.run([str(binary)], text=True, capture_output=True,
                                timeout=30, env={**os.environ,
            "ASAN_OPTIONS": "abort_on_error=1", "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        if controls:
            expected = "6 vectors, 18 checks" if family == "generic" else "10 vectors, 34 checks"
        else:
            expected = "228 vectors, 1368 checks" if kind == 2 else (
                "108 vectors, 648 checks" if kind == 5 else "342 vectors, 2052 checks")
        self.assertIn(expected + ", 0 failures; guest_executed=false", result.stdout)

    def test_generic_formats_and_transaction_controls(self):
        for kind in FAMILIES["generic"][1]:
            with self.subTest(format=kind):
                self.run_fixture("generic", kind)
        self.run_fixture("generic", controls=True)

    def test_p65_formats_and_transaction_controls(self):
        for kind in FAMILIES["p65"][1]:
            with self.subTest(format=kind):
                self.run_fixture("p65", kind)
        self.run_fixture("p65", controls=True)

    def test_q65_formats_legacy_encoding_and_transaction_controls(self):
        for kind in FAMILIES["q65"][1]:
            with self.subTest(format=kind):
                self.run_fixture("q65", kind)
        self.run_fixture("q65", controls=True)


if __name__ == "__main__":
    unittest.main()
