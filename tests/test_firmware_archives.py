"""Firmware Archives containers and archival-only catalog entries."""
from pathlib import Path
import io
import json
import struct
import sys
import tempfile
import unittest
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'Sources/AzkiSimulator/Runtime'))
from azki_simulator.firmware_probe import archive_recipe, inspect_firmware, ProbeError
from azki_simulator.firmware_catalog import FirmwareCatalog, CatalogError


def xring_ap():
    data = bytearray(512)
    struct.pack_into('<II', data, 0, 0x61745d00, 0x605c0041)
    return bytes(data)


def zip_bytes(members):
    output = io.BytesIO()
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as package:
        for name, content in members.items():
            package.writestr(name, content)
    return output.getvalue()


def bes_envelope():
    members = {'vela_ap.bin': xring_ap(), 'version.ini': b'version = 3.1.171'}
    base = 128
    table = bytearray(80 + 128 * len(members))
    table[:4] = b'ZZZ~'
    table[4:11] = b'3.1.171'
    struct.pack_into('<H', table, 70, len(members))
    data = bytearray()
    for i, (name, payload) in enumerate(members.items()):
        packed = zip_bytes({name: payload})
        offset = 80 + 128 * len(members) + 4 + len(data)
        struct.pack_into('<III', table, 80 + i * 128, 0x740001, offset, len(packed))
        path = ('/data/ota/' + name).encode()
        table[92 + i * 128:92 + i * 128 + len(path)] = path
        data += packed
    header = bytearray(base)
    header[:4] = b'`ZZ~'
    struct.pack_into('<I', header, 84, base)
    return header + table + struct.pack('<I', zlib.crc32(table)) + data


class FirmwareArchivesTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir='/private/tmp')
        self.root = Path(self.temp.name)
        self.catalog = FirmwareCatalog(ROOT / 'engine', self.root / 'library')

    def tearDown(self):
        self.temp.cleanup()

    def file(self, name, data):
        path = self.root / name
        path.write_bytes(data)
        return path

    def recipe(self, path):
        with path.open('rb') as stream:
            return archive_recipe(stream.fileno(), path.stat().st_size, path.name)

    def assert_archived(self, path, kind, container, model=None):
        receipt = self.catalog.import_file(path, model=model)
        self.assertEqual(receipt['inputKind'], 'archive')
        self.assertFalse(receipt['needsSelection'])
        self.assertEqual(receipt['candidateIDs'], [receipt['items'][0]['id']])
        item = receipt['items'][0]
        self.assertFalse(item['availability']['ready'])
        self.assertTrue(item['availability']['assetsReady'])
        self.assertEqual((item['packageKind'], item['container']), (kind, container))
        self.assertIn(item, self.catalog.list_items())
        with self.assertRaises(CatalogError):
            self.catalog.resolve_generic(item['id'])
        return receipt

    def test_patch_zip_imports_without_ap(self):
        path = self.file('miwear.watch.q63_v4.101.026_from_v4.101.020_incremental_d61a60cb.bin',
                         zip_bytes({'ota.sh': b'ddelta_apply /dev/ap vela_ap.patch', 'vela_ap.patch': b'patch'}))
        receipt = self.assert_archived(path, 'incremental', 'zip')
        self.assertEqual(receipt['items'][0]['baseVersion'], '4.101.020')
        self.assertEqual(receipt['items'][0]['width'], 0)
        self.catalog.delete_import(receipt['sha256'])
        self.assertEqual(self.catalog.list_items(), [])

    def test_diff_with_real_ap_is_not_runnable_even_when_bound(self):
        path = self.file('ota.bin', zip_bytes({'vela_ap.bin': xring_ap(),
                'ota.json': json.dumps({'firmware_type': 'diff', 'sw_version': '3.1.16'})}))
        item = self.assert_archived(path, 'incremental', 'zip', 'miwear.watch.o62m')['items'][0]
        self.assertEqual(item['width'], 466)

    def test_bes_incremental_filename_does_not_become_full_ap(self):
        path = self.file('miwear.watch.n67cn_v3.1.175_from_v3.1.171_incremental_cf9c4af0.bin', bes_envelope())
        item = self.assert_archived(path, 'incremental', 'bes-envelope')['items'][0]
        self.assertEqual(item['firmwareVersion'], '3.1.175')

    def test_aota_members_and_bounds(self):
        data = bytearray(1056)
        data[:4] = b'AOTA'
        struct.pack_into('<H', data, 12, 1)
        struct.pack_into('<H', data, 16, 512)
        data[512:519] = b'ota.xml'
        struct.pack_into('<II', data, 528, 1024, 32)
        path = self.file('miwear.watch.q65acn_v3.100.035_full_7743db42.bin', data)
        recipe = self.recipe(path)
        self.assertEqual(recipe['structure'], 'member-inventory')
        self.assertEqual(recipe['partitions'][0]['size'], 32)
        self.assert_archived(path, 'full', 'aota')
        struct.pack_into('<I', data, 532, 33)
        path.write_bytes(data)
        with self.assertRaises(ProbeError): self.recipe(path)

    def test_aota_model_survives_import_from_another_instance(self):
        data = bytearray(1056)
        data[:4] = b'AOTA'
        struct.pack_into('<H', data, 12, 1)
        struct.pack_into('<H', data, 16, 512)
        data[512:519] = b'ota.xml'
        struct.pack_into('<II', data, 528, 1024, 32)
        path = self.file('miwear.watch.q65acn_v3.100.035_full_7743db42.bin', data)
        receipt = self.assert_archived(path, 'full', 'aota', 'miwear.watch.o62m')
        item = receipt['items'][0]
        self.assertEqual(item['model'], 'miwear.watch.q65acn')
        self.assertEqual((item['width'], item['height']), (0, 0))
        self.assertTrue(receipt['candidateIDs'][0].startswith('miwear.watch.q65acn@3.100.035@'))
        index = json.loads((self.root / 'library/index.json').read_text())
        self.assertEqual(index['imports'][0]['profileIDs'], receipt['candidateIDs'])
        self.assertIsNone(index['imports'][0]['generic']['boundModel'])

    def test_band_update_header_and_filename(self):
        data = bytearray(160)
        struct.pack_into('<III', data, 0, 0x12345678, 6, 1)
        data[12:19] = b'1.8.124'
        path = self.file('miwear.watch.m66_v1.8.124_from_v1.7.230_incremental_a38ad151.bin', data)
        self.assert_archived(path, 'incremental', 'band-update')

    def test_midr_full_header(self):
        data = bytearray(128)
        data[:4] = b'full'
        struct.pack_into('>BBH', data, 12, 1, 2, 253)
        data[16:30] = b'midr.watch.k65'
        path = self.file('renamed.bin', data)
        item = self.assert_archived(path, 'full', 'midr-full')['items'][0]
        self.assertEqual(item['model'], 'midr.watch.k65')
        self.assertEqual(item['firmwareVersion'], '1.2.253')
        self.assertEqual(item['metadataSource'], 'firmware')

    def test_legacy_binary_keeps_provenance_without_inventing_ap(self):
        path = self.file('mj1205.motion.ecg_v0.2.9_full_340b1daa.bin', bytes(range(128)))
        item = self.assert_archived(path, 'full', 'raw-binary')['items'][0]
        self.assertEqual(item['structure'], 'opaque')
        self.assertEqual(item['metadataSource'], 'archive-filename')
        diagnostic = inspect_firmware(path)
        self.assertIsNone(diagnostic['ap'])
        self.assertEqual(len(diagnostic['sha256']), 64)

    def test_unknown_ap_chip_stays_archival(self):
        path = self.file('miwear.watch.q69cn_v4.100.011_full_7cf498df.bin',
                         zip_bytes({'vela_ap.bin': b'\nCHIP=unknown\n' + bytes(256), 'version.txt': b'4.100.011'}))
        self.assert_archived(path, 'full', 'zip')

    def test_full_xring_retains_real_boot_recipe(self):
        members = {'vela_ap.bin': xring_ap(), 'version.txt': b'3.1.115'}
        for part in ('tee', 'mmap', 'CP_boot', 'CP', 'CRF'):
            members['vela_' + part + '.bin'] = b'fixture'
        path = self.file('mijia.watch.o62m_v3.1.115_full_02222e2c.bin', zip_bytes(members))
        receipt = self.catalog.import_file(path, model='miwear.watch.o62m')
        self.assertEqual(receipt['inputKind'], 'ota')
        self.assertTrue(receipt['items'][0]['availability']['ready'])
        self.assertEqual(self.catalog.resolve_generic(receipt['items'][0]['id'])['recipe']['ap']['chip'], 'xring')
        self.assertTrue(inspect_firmware(path)['ap']['headerRecognized'])

    def test_zip_deflate_level_flags_are_supported(self):
        data = bytearray(zip_bytes({'vela_ap.patch': b'patch'}))
        struct.pack_into('<H', data, 6, 2)
        central = data.index(b'PK\x01\x02')
        struct.pack_into('<H', data, central + 8, 2)
        path = self.file('patch.bin', data)
        self.assertEqual(self.recipe(path)['packageKind'], 'incremental')

    def test_direct_ap_diagnostic_remains_available(self):
        path = self.file('ap.bin', xring_ap())
        result = inspect_firmware(path)
        self.assertEqual(result['inputKind'], 'ap-candidate')
        self.assertEqual(result['ap']['chip'], 'xring')

    def test_malformed_zip_is_not_silently_archived(self):
        path = self.file('miwear.watch.q63_v4.101.026_full_5cbe8fcf.bin', b'PK\x03\x04' + bytes(100))
        with self.assertRaises((ProbeError, zipfile.BadZipFile)): self.recipe(path)


if __name__ == '__main__':
    unittest.main()
