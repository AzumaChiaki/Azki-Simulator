"""Exercise NONE blits against stale pixels and a dependent offscreen draw."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GpuCopyTest(unittest.TestCase):
    def test_copy(self):
        flags = shlex.split(subprocess.check_output(
            ['pkg-config', '--cflags', '--libs', 'glib-2.0'], text=True))
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as directory:
            binary = Path(directory) / 'gpu-copy'
            subprocess.run(['clang', '-O1', '-g', '-fsanitize=address,undefined',
                            str(ROOT / 'tests/test_gpu_copy.c'), *flags,
                            '-o', str(binary)], check=True)
            result = subprocess.check_output([str(binary)], text=True, env={
                **os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1',
                'ASAN_OPTIONS': 'abort_on_error=1'})
            self.assertIn('intermediate copy passed', result)
