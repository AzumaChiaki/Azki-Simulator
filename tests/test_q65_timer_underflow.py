"""Production CMSDK handlers + production ptimer, with a deterministic host clock.

This test never starts a guest or opens a QEMU control socket.  The timer queue
is the boundary stub; countdown, reload, transactions and register handlers
come from the actual QEMU sources rather than a second timer implementation.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Q65TimerUnderflowTests(unittest.TestCase):
    def test_production_timer_and_ptimer(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler required")
        source = (ROOT / "engine/qemu-overlay/hw/timer/cmsdk-apb-dualtimer.c").read_text()
        header = (ROOT / "engine/qemu-overlay/include/hw/timer/cmsdk-apb-dualtimer.h").read_text()
        header = re.sub(r'^#include.*$', '', header, flags=re.M)
        ptimer_root = ROOT / "engine/qemu-overlay"
        policies = (ptimer_root / "include/hw/ptimer.h").read_text()
        policies = '\n'.join(re.findall(r'^#define PTIMER_POLICY_.*$', policies, re.M))
        ptimer = (ptimer_root / "hw/core/ptimer.c").read_text()
        ptimer = re.sub(r'^#include.*$', '', ptimer, flags=re.M)
        ptimer = re.sub(r'^const VMStateDescription vmstate_ptimer = \{.*?^\};',
                        '', ptimer, flags=re.M | re.S)
        registers = source[source.index('REG32(TIMER1LOAD'):source.index('static bool cmsdk_dualtimermod_intstatus')]
        names = ["cmsdk_dualtimermod_intstatus", "cmsdk_apb_dualtimer_update",
                 "cmsdk_dualtimermod_divisor", "cmsdk_dualtimermod_bes_underflow",
                 "cmsdk_dualtimermod_raw_count", "cmsdk_dualtimermod_decode_count",
                 "cmsdk_dualtimermod_latch_period",
                 "cmsdk_dualtimermod_set_raw_count", "cmsdk_dualtimermod_set_period",
                 "cmsdk_dualtimermod_write_control", "cmsdk_apb_dualtimer_read",
                 "cmsdk_apb_dualtimer_write", "cmsdk_dualtimermod_tick",
                 "cmsdk_dualtimermod_reset", "cmsdk_apb_dualtimer_clk_update",
                 "cmsdk_dualtimermod_policy",
                 "cmsdk_apb_dualtimer_post_load"]
        blocks = [header, registers]
        for name in names:
            match = re.search(r'^static [^;{}]*\b' + name + r'\([^;{}]*\)\n\{.*?^\}\n',
                              source, re.M | re.S)
            self.assertIsNotNone(match, name)
            blocks.append(match.group())
        (ROOT / 'build').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='q65-timer-', dir=ROOT / 'build') as directory:
            directory = Path(directory)
            (directory / 'ptimer-production.inc').write_text(policies + '\n' + ptimer)
            (directory / 'dualtimer-production.inc').write_text('\n'.join(blocks))
            binary = directory / 'timer-test'
            subprocess.run([compiler, '-std=c11', '-O1', '-g', '-fsanitize=address,undefined',
                            '-I', str(directory), str(ROOT / 'tests/test_q65_timer_underflow.c'),
                            '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], text=True, capture_output=True,
                                    env={**os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1',
                                         'ASAN_OPTIONS': 'abort_on_error=1'})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('production ptimer underflow tests passed', result.stdout)
            print(result.stdout.strip())


if __name__ == '__main__':
    unittest.main()
