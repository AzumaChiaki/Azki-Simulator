"""Exercise 4-bit indexed sources, as used by stock N66 notification art."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GpuIndex4Test(unittest.TestCase):
    def test_index4(self):
        flags = shlex.split(subprocess.check_output(
            ['pkg-config', '--cflags', '--libs', 'glib-2.0'], text=True))
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as directory:
            binary = Path(directory) / 'gpu-index4'
            subprocess.run(['clang', '-O1', '-g', '-fsanitize=address,undefined',
                            str(ROOT / 'tests/test_gpu_index4.c'), *flags,
                            '-o', str(binary)], check=True)
            result = subprocess.check_output([str(binary)], text=True, env={
                **os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1',
                'ASAN_OPTIONS': 'abort_on_error=1'})
            self.assertIn('4-bit indexed source passed', result)
