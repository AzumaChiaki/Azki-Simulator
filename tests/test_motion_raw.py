"""Compile only the production sensor header; no QEMU guest or control socket."""
import os
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MotionRawContract(unittest.TestCase):
    def test_raw_sensor_bus_contract_with_sanitizers(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        pkg = shutil.which('pkg-config')
        if not compiler or not pkg:
            self.skipTest('C compiler and GLib required, as for QEMU')
        flags = shlex.split(subprocess.check_output([pkg, '--cflags', '--libs', 'glib-2.0'], text=True))
        (ROOT / 'build').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='motion-raw-', dir=ROOT / 'build') as temp:
            binary = Path(temp) / 'motion-raw'
            subprocess.run([compiler, '-O1', '-g', '-fsanitize=address,undefined',
                str(ROOT / 'tests/test_motion_raw.c'), *flags, '-o', str(binary)], check=True)
            output = subprocess.check_output([str(binary)], text=True, env={**os.environ,
                'UBSAN_OPTIONS': 'halt_on_error=1', 'ASAN_OPTIONS': 'abort_on_error=1'})
            print(output.strip())
            self.assertIn('checks passed; no guest consumption asserted', output)
            snapshots = [json.loads(line.removeprefix('OBSERVATION_JSON '))
                         for line in output.splitlines() if line.startswith('OBSERVATION_JSON ')]
            self.assertEqual(['lsm6dso', 'icm42607'], [s['imu'] for s in snapshots])
            for state in snapshots:
                self.assertEqual({'schema', 'scope', 'guestObserved', 'inputVersion', 'mode', 'imu', 'accel', 'gyro'}, set(state))
                self.assertEqual(1, state['schema'])
                self.assertEqual('sensor-bus', state['scope'])
                self.assertIs(False, state['guestObserved'])
                self.assertEqual('raw', state['mode'])
                self.assertEqual(1, state['inputVersion'])
                for component in ('accel', 'gyro'):
                    self.assertEqual({'sampleValid', 'generatedVersion', 'registerReads', 'registerVersion', 'fifoReads', 'fifoVersion'}, set(state[component]))
                    self.assertIs(True, state[component]['sampleValid'])
                    for key in ('generatedVersion', 'registerReads', 'registerVersion', 'fifoReads', 'fifoVersion'):
                        self.assertEqual(1, state[component][key])


if __name__ == '__main__':
    unittest.main()
