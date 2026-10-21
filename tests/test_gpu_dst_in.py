"""Run the production GPU decoder's solid-path DST_IN pixel/guard vectors."""
import os
import pathlib
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class GpuDestinationInTest(unittest.TestCase):
    def test_solid_path_destination_in(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        pkgconfig = shutil.which("pkg-config")
        if not compiler or not pkgconfig:
            self.skipTest("A C compiler and GLib pkg-config are required, as for QEMU")
        flags = subprocess.check_output([pkgconfig, "--cflags", "--libs", "glib-2.0"], text=True)
        (ROOT / "build").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="gpu-dst-in-test-", dir=ROOT / "build") as work:
            binary = pathlib.Path(work) / "gpu-dst-in"
            subprocess.run([compiler, "-O1", "-g", "-fsanitize=address,undefined",
                            str(ROOT / "tests/test_gpu_dst_in.c"), *shlex.split(flags),
                            "-o", str(binary)], check=True)
            output = subprocess.check_output([str(binary)], text=True, env={**os.environ,
                "UBSAN_OPTIONS": "halt_on_error=1", "ASAN_OPTIONS": "abort_on_error=1"})
            self.assertIn("240 checks passed", output)


if __name__ == "__main__":
    unittest.main()
