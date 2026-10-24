"""Run production touch commands and host reports through sleep/wake sequences."""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def run_touch_regression(source):
    compiler = shutil.which('clang') or shutil.which('cc')
    pkg = shutil.which('pkg-config')
    if not compiler or not pkg:
        raise unittest.SkipTest('C compiler and GLib required, as for QEMU')
    flags = shlex.split(subprocess.check_output([pkg, '--cflags', '--libs', 'glib-2.0'], text=True))
    structure = re.search(r'typedef struct \{\n    bool enabled, bootloader, slave_reading;.*?\} BesTouch;', source, re.S)
    assert structure, 'production touch state not found'
    names = ['touch_gesture_mode', 'touch_reporting', 'touch_reset', 'touch_queue_remove',
             'touch_gesture', 'touch_mouse', 'touch_publish', 'touch_write_packet']
    functions = ['static void touch_publish(Best1503State *s, const uint8_t *report);']
    for name in names:
        match = re.search(r'^static [^;{}]*\b' + name + r'\([^;{}]*\)\n\{.*?^\}\n', source, re.M | re.S)
        assert match, 'production function not found: ' + name
        functions.append(match.group())
    (ROOT / 'build').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='touch-sleep-', dir=ROOT / 'build') as temp:
        temp = Path(temp)
        (temp / 'touch-state.inc').write_text(structure.group())
        (temp / 'touch-functions.inc').write_text('\n'.join(functions))
        binary = temp / 'touch-sleep'
        subprocess.run([compiler, '-O1', '-g', '-fsanitize=address,undefined',
                        '-I', str(temp), str(ROOT / 'tests/test_touch_sleep.c'), *flags,
                        '-o', str(binary)], check=True)
        return subprocess.check_output([str(binary)], text=True, stderr=subprocess.STDOUT,
            env={**os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1', 'ASAN_OPTIONS': 'abort_on_error=1'})


class TouchSleepTests(unittest.TestCase):
    def test_sleep_survives_normal_mode_until_explicit_wake(self):
        source = (ROOT / 'engine/qemu-overlay/hw/arm/best1503.c').read_text()
        self.assertIn('touch sleep production regression passed', run_touch_regression(source))


if __name__ == '__main__':
    unittest.main()
