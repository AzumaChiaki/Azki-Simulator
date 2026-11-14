"""XRing OTA routing and the native eMMC launch contract."""
from pathlib import Path
import json
import os
import runpy
import struct
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'Sources/AzkiSimulator/Runtime'))
from azki_simulator import bridge, firmware_probe, minative


class XRingHostTests(unittest.TestCase):
    def test_ota_selects_xring_without_bes_header(self):
        ap = bytearray(512)
        struct.pack_into('<II', ap, 0, 0x61745d00, 0x605c0041)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'ota.zip'
            with zipfile.ZipFile(path, 'w') as package:
                package.writestr('vela_ap.bin', ap)
                for name in ('vela_tee.bin', 'vela_mmap.bin', 'vela_CP_boot.bin', 'vela_CP.bin', 'vela_CRF.bin'):
                    package.writestr(name, b'fixture')
                package.writestr('ota.json', json.dumps({'sw_version': '3.1.115'}))
            with path.open('rb') as stream:
                recipe = firmware_probe.ota_recipe(stream.fileno(), path.stat().st_size)
        self.assertEqual(recipe['ap']['chip'], 'xring')
        self.assertEqual(recipe['declaredModels'], ['o62m'])
        self.assertEqual(recipe['version'], '3.1.115')
        ap[4:8] = struct.pack('<I', 0x605d0001)
        self.assertEqual(firmware_probe.inspect_ap(ap)['chip'], 'unidentified')

    def test_real_frame_contract_accepts_466_square(self):
        self.assertEqual(minative._geometry('miwear.watch.o62m'), (466, 466))
        header = struct.pack('<8s6I', b'Q66FRAME', 1, 2, 466, 466, 466 * 3, 2)
        bridge.validate_frame(header, bytes(466 * 466 * 3), (466, 466))

    def test_native_emmc_is_bound_to_session(self):
        invoke = runpy.run_path(str(ROOT / 'Sources/AzkiSimulator/Runtime/azki_simulator/minative_launch.py'))['native_invocation']
        run = '/private/tmp/xring-test/session/m-0123456789ab'
        args = ['--native', '0', run + '/runner-process.json', str(os.getpid()),
                '/private/tmp/Engine', '/private/tmp/xring-test/prepared/project',
                'miwear.watch.o62m@3.1.115@0123456789ab', run,
                '--instance-emmc-image', '/private/tmp/xring-test/session/instance-data/emmc.img',
                '--instance-emmc-initialize', '--skip-oobe']
        command = invoke(args)[3]
        self.assertIn('--instance-emmc-initialize', command)
        self.assertEqual(command[-1], '--skip-oobe')
        args[9] = '/private/tmp/other.img'
        with self.assertRaises(ValueError): invoke(args)


if __name__ == '__main__': unittest.main()
