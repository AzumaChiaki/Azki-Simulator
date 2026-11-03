"""GPU MMIO/END lifecycle with actual renderer; no guest, socket or NVIC model."""
import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ARM = ROOT / 'engine/qemu-overlay/hw/arm'


class GPULifecycleTests(unittest.TestCase):
    def test_production_mmio_irq_reset_and_pixel_commit(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        pkg = shutil.which('pkg-config')
        if not compiler or not pkg:
            self.skipTest('C compiler and GLib required as for QEMU')
        flags = shlex.split(subprocess.check_output([pkg, '--cflags', '--libs', 'glib-2.0'], text=True))
        source = (ARM / 'best1503.c').read_text()
        fields = re.search(r'^    uint32_t gpu_probe_control, gpu_probe_mask;.*?^    qemu_irq gpu_irq;\n', source, re.M | re.S)
        self.assertIsNotNone(fields)
        names = ('gpu_update_irq', 'gpu_ram_read', 'gpu_complete', 'gpu_decode_end',
                 'gpu_probe_read', 'gpu_probe_write')
        blocks = []
        for name in names:
            match = re.search(r'^static [^;{}]*\b' + name + r'\([^;{}]*\)\n\{.*?^\}\n', source, re.M | re.S)
            self.assertIsNotNone(match, name)
            blocks.append(match.group())
        reset = re.search(r'^static void best1503_peripheral_reset\(void \*opaque\)\n\{.*?^\}', source, re.M | re.S)
        self.assertIsNotNone(reset)
        cancel = re.search(r'^    if \(s->gpu_work\) \{.*?^    memset\(s->gpu_sw_regs, 0, sizeof\(s->gpu_sw_regs\)\);\n', reset.group(), re.M | re.S)
        state = re.search(r'^    s->gpu_probe_control = s->gpu_probe_mask = 0;.*?^    gpu_update_irq\(s\);\n', reset.group(), re.M | re.S)
        timer = re.search(r'^    timer_del\(s->gpu_timer\);\n', reset.group(), re.M)
        self.assertTrue(cancel and state and timer)
        # GPU-only statements from real board reset; touch state is unrelated.
        state = state.group().replace('    s->touch.irq_rearm_pending = false;\n', '')
        blocks.append('static void production_gpu_reset(Best1503State *s)\n{\n' + timer.group() + cancel.group() + state + '}\n')
        (ROOT / 'build').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='gpu-lifecycle-', dir=ROOT / 'build') as directory:
            directory = Path(directory)
            (directory / 'gpu-fields.inc').write_text(fields.group())
            (directory / 'gpu-lifecycle-functions.inc').write_text('\n'.join(blocks))
            binary = directory / 'gpu-lifecycle'
            subprocess.run([compiler, '-O1', '-g', '-fsanitize=address,undefined',
                            '-I', str(directory), '-I', str(ARM),
                            str(ROOT / 'tests/test_gpu_lifecycle.c'), *flags, '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                env={**os.environ, 'ASAN_OPTIONS': 'abort_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1'})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertRegex(result.stdout, r'GPU lifecycle production contract: \d+ checks passed')
            print('best1503.c SHA256', hashlib.sha256(source.encode()).hexdigest())
            print(result.stdout.strip())


if __name__ == '__main__':
    unittest.main()
