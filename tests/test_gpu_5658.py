"""Exercise real BGRA5658 source/target byte layouts and filtered blending."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Gpu5658Test(unittest.TestCase):
    def test_bgra5658_bytes(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        pkg = shutil.which('pkg-config')
        if not compiler or not pkg:
            self.skipTest('C compiler and GLib required, as for QEMU')
        flags = shlex.split(subprocess.check_output([pkg, '--cflags', '--libs', 'glib-2.0'], text=True))
        (ROOT / 'build').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='gpu-5658-', dir=ROOT / 'build') as temp:
            binary = Path(temp) / 'gpu-5658'
            subprocess.run([compiler, '-O1', '-g', '-fsanitize=address,undefined',
                            str(ROOT / 'tests/test_gpu_5658.c'), *flags, '-o', str(binary)], check=True)
            output = subprocess.check_output([str(binary)], text=True, env={**os.environ,
                'UBSAN_OPTIONS': 'halt_on_error=1', 'ASAN_OPTIONS': 'abort_on_error=1'})
            self.assertIn('44 checks passed', output)


if __name__ == '__main__':
    unittest.main()
