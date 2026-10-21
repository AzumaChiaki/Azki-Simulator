"""Raw-frame polling keeps the existing reader's security and seqlock checks."""
import os
from pathlib import Path
import stat
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'Sources/AzkiSimulator/Runtime'))
from azki_simulator import bridge


class RawFrameTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.path = self.root / 'frame.raw'

    def tearDown(self):
        self.temp.cleanup()

    def frame(self, geometry=(212, 520), sequence=2, **changes):
        width, height = geometry
        values = dict(magic=b'Q66FRAME', version=1, sequence=sequence,
                      width=width, height=height, stride=width * 2, fmt=1)
        values.update(changes)
        header = struct.pack('<8s6I', *values.values())
        pixels = struct.pack('<4H', 0xf800, 0x07e0, 0x001f, 0xffff) * (width * height // 4)
        self.path.write_bytes(header + pixels)
        return header, pixels

    def test_raw_all_geometries_and_rgb_compatibility(self):
        for geometry in bridge.DISPLAY_GEOMETRIES:
            with self.subTest(geometry=geometry):
                _, pixels = self.frame(geometry)
                self.assertEqual((2, *geometry, pixels), bridge.read_frame_raw(self.root, geometry))
                seq, width, height, rgb = bridge.read_frame(self.root, geometry)
                self.assertEqual((2, *geometry), (seq, width, height))
                self.assertEqual(width * height * 3, len(rgb))
                self.assertEqual(bytes((248, 0, 0, 0, 252, 0, 0, 0, 248, 248, 252, 248)), rgb[:12])

    def test_unchanged_validates_two_headers_without_reading_pixels(self):
        self.frame()
        with mock.patch.object(bridge.os, 'pread', wraps=os.pread) as read:
            self.assertIsNone(bridge.read_frame_raw(self.root, previous_sequence=2))
        self.assertEqual([(32, 0), (32, 0)], [(x.args[1], x.args[2]) for x in read.call_args_list])

    def test_different_sequence_returns_full_snapshot(self):
        _, pixels = self.frame(sequence=4)
        self.assertEqual((4, 212, 520, pixels), bridge.read_frame_raw(self.root, previous_sequence=2))

    def test_invalid_sequence_arguments(self):
        self.frame()
        for value in (True, False, -1, 2**32, 2.0, '2'):
            with self.subTest(value=value), self.assertRaises(bridge.BridgeError):
                bridge.read_frame_raw(self.root, previous_sequence=value)

    def test_unchanged_does_not_bypass_format_or_geometry(self):
        for change in ({'magic': b'BADFRAME'}, {'version': 2}, {'fmt': 2},
                       {'width': 192}, {'height': 490}, {'stride': 426}):
            with self.subTest(change=change):
                self.frame(**change)
                with self.assertRaises(bridge.BridgeError):
                    bridge.read_frame_raw(self.root, previous_sequence=2)
        self.frame()
        with self.assertRaises(bridge.BridgeError):
            bridge.read_frame_raw(self.root, (1, 1), previous_sequence=2)

    def test_incomplete_or_odd_header_never_reports_unchanged(self):
        for sequence in (0, 1, 3):
            with self.subTest(sequence=sequence):
                self.frame(sequence=sequence)
                with self.assertRaises(bridge.BridgeError) as caught:
                    bridge.read_frame_raw(self.root, previous_sequence=sequence)
                self.assertEqual('frame_not_ready', caught.exception.code)
        self.path.write_bytes(b'Q66FRAME')
        with self.assertRaises(bridge.BridgeError):
            bridge.read_frame_raw(self.root, previous_sequence=2)

    def test_unchanged_rejects_short_long_and_oversize_files(self):
        for change in (-1, 1, bridge.MAX_FRAME):
            self.frame()
            with self.path.open('r+b') as stream:
                stream.truncate(self.path.stat().st_size + change)
            with self.subTest(change=change), self.assertRaises(bridge.BridgeError):
                bridge.read_frame_raw(self.root, previous_sequence=2)

    def test_rejects_symlink_hardlink_and_nonregular_file(self):
        self.frame()
        original = self.root / 'original'
        self.path.rename(original)
        self.path.symlink_to(original)
        with self.assertRaises(OSError):
            bridge.read_frame_raw(self.root, previous_sequence=2)
        self.path.unlink()
        os.link(original, self.path)
        with self.assertRaises(bridge.BridgeError):
            bridge.read_frame_raw(self.root, previous_sequence=2)
        self.path.unlink()
        os.mkfifo(self.path)
        with self.assertRaises(bridge.BridgeError):
            bridge.read_frame_raw(self.root, previous_sequence=2)

    def test_rejects_foreign_owner_even_when_sequence_matches(self):
        self.frame()
        info = self.path.stat()
        other = SimpleNamespace(st_mode=stat.S_IFREG | 0o600, st_uid=os.getuid() + 1,
                                st_nlink=1, st_size=info.st_size)
        with mock.patch.object(bridge.os, 'fstat', return_value=other), self.assertRaises(bridge.BridgeError):
            bridge.read_frame_raw(self.root, previous_sequence=2)

    def test_unchanged_double_header_race_retries_then_fails(self):
        header, _ = self.frame()
        changed = struct.pack('<8s6I', b'Q66FRAME', 1, 4, 212, 520, 424, 1)
        with mock.patch.object(bridge.os, 'pread', side_effect=[header, changed] * 3) as read:
            with self.assertRaises(bridge.BridgeError) as caught:
                bridge.read_frame_raw(self.root, previous_sequence=2)
        self.assertEqual('frame_not_ready', caught.exception.code)
        self.assertEqual(6, read.call_count)

    def test_full_copy_race_retries_using_new_stable_sequence(self):
        header, pixels = self.frame()
        changed = struct.pack('<8s6I', b'Q66FRAME', 1, 4, 212, 520, 424, 1)
        with mock.patch.object(bridge.os, 'pread', side_effect=[header, pixels, changed, changed, pixels, changed]):
            self.assertEqual((4, 212, 520, pixels), bridge.read_frame_raw(self.root))

    def test_short_pixel_copy_retries_and_fails(self):
        header, pixels = self.frame()
        with mock.patch.object(bridge.os, 'pread', side_effect=[header, pixels[:-2], header] * 3):
            with self.assertRaises(bridge.BridgeError) as caught:
                bridge.read_frame_raw(self.root)
        self.assertEqual('frame_not_ready', caught.exception.code)


if __name__ == '__main__':
    unittest.main()
