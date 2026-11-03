"""Exercise Q65's production decoder, independently of the generic GPU header."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Q65GpuSamplerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang") or shutil.which("cc")
        pkg = shutil.which("pkg-config")
        if not compiler or not pkg:
            raise unittest.SkipTest("C compiler and GLib required, as for QEMU")
        flags = shlex.split(subprocess.check_output(
            [pkg, "--cflags", "--libs", "glib-2.0"], text=True))
        (ROOT / "build").mkdir(exist_ok=True)
        cls.work = tempfile.TemporaryDirectory(prefix="gpu-q65-", dir=ROOT / "build")
        cls.addClassCleanup(cls.work.cleanup)
        cls.binary = Path(cls.work.name) / "q65-sampler"
        subprocess.run([compiler, "-O1", "-g", "-fsanitize=address,undefined",
                        str(ROOT / "tests/test_gpu_q65_sampler.c"),
                        *flags, "-o", str(cls.binary)], check=True)

    def run_group(self, group, checks):
        result = subprocess.run([str(self.binary), group], text=True,
                                capture_output=True, env={**os.environ,
            "UBSAN_OPTIONS": "halt_on_error=1", "ASAN_OPTIONS": "abort_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"Q65 {group}: {checks} checks, 0 failures", result.stdout)

    def test_transparent_none_replaces_destination(self):
        self.run_group("none", 8)

    def test_point_and_bilinear_pad(self):
        self.run_group("pad", 104)

    def test_source_over(self):
        self.run_group("over", 32)


if __name__ == "__main__":
    unittest.main()
