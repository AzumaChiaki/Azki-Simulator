"""Targeted Q65 mode-toggle vector; source root may select the frozen baseline.

This compiles extracted production C functions with a host clock/IRQ/QOM stub.
No QEMU guest or socket is started. Retained build directories are task-owned.
"""
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
SOURCE = Path(os.environ.get('AZKI_ADC_SOURCE_ROOT', ROOT)).resolve()
ARM = SOURCE / 'engine/qemu-overlay/hw/arm'


class Q65AdcToggleTest(unittest.TestCase):
    def test_real_q65_fixture_toggle(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        pkg = shutil.which('pkg-config')
        if not compiler or not pkg:
            self.skipTest('C compiler and GLib required, as for QEMU')
        flags = shlex.split(subprocess.check_output([pkg, '--cflags', '--libs', 'glib-2.0'], text=True))
        source = (ARM / 'azki-q65.c').read_text()
        print('Q65_SOURCE', str(ARM / 'azki-q65.c'))
        print('Q65_SOURCE_SHA256', hashlib.sha256(source.encode()).hexdigest())
        structure = re.search(r'typedef struct \{\n    MemoryRegion mmio;\n'
                              r'    uint32_t regs\[0x8c / 4\];\n'
                              r'    uint16_t analog\[4\]\[5\]\[256\];.*?\} BesIspi;', source, re.S)
        self.assertIsNotNone(structure)
        names = ['pmu_key_update_irq', 'ispi_adc_update', 'ispi_adc_trace', 'ispi_read',
                 'ispi_write', 'best1503_instance_init', 'best1503_set_adc_raw',
                 'best1503_set_power_adc_fixture']
        functions = []
        for name in names:
            match = re.search(r'^static [^;{}]*\b' + name + r'\([^;{}]*\)\n\{.*?^\}\n', source, re.M | re.S)
            self.assertIsNotNone(match, 'missing production Q65 function: ' + name)
            functions.append(match.group())
        (ROOT / 'build').mkdir(exist_ok=True)
        artifact = Path(tempfile.mkdtemp(prefix='q65-adc-toggle-', dir=ROOT / 'build'))
        print('RETAINED_BUILD', str(artifact))
        (artifact / 'q65-toggle-ispi-state.inc').write_text(structure.group())
        (artifact / 'q65-toggle-functions.inc').write_text('\n'.join(functions))
        binary = artifact / 'q65-adc-toggle'
        subprocess.run([compiler, '-O1', '-g', '-fsanitize=address,undefined',
                        '-I', str(artifact), '-I', str(ARM), str(ROOT / 'tests/test_q65_adc_toggle.c'),
                        *flags, '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], text=True, capture_output=True, env={**os.environ,
            'UBSAN_OPTIONS': 'halt_on_error=1', 'ASAN_OPTIONS': 'abort_on_error=1'})
        (artifact / 'stdout.txt').write_text(result.stdout)
        (artifact / 'stderr.txt').write_text(result.stderr)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('23 checks passed; host-only, no guest run', result.stdout)
        print(result.stdout.strip())


if __name__ == '__main__':
    unittest.main()
