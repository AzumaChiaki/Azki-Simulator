"""Host control reads both packed LCD formats without changing channel values."""
import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('lcd_control', ROOT / 'engine/project/backend/src/control.py')
control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(control)


class ControlFrameTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name).resolve()
        self.path = self.root / 'lcd-frame.raw'

    def tearDown(self):
        self.temp.cleanup()

    def frame(self, pixel_format=2, sequence=2, **changes):
        fmt = pixel_format
        values = dict(magic=b'Q66FRAME', version=1, sequence=sequence,
                      width=4, height=1, stride=4 * (2 if fmt == 1 else 3), fmt=fmt)
        values.update(changes)
        header = struct.pack('<8s6I', *values.values())
        pixels = (struct.pack('<4H', 0xf800, 0x07e0, 0x001f, 0x8e71) if fmt == 1 else
                  bytes((7, 11, 19, 23, 69, 254, 255, 127, 1, 8, 4, 8)))
        self.path.write_bytes(header + pixels)
        self.path.chmod(0o600)
        return header, pixels

    def reader(self):
        return control.WatchControl(None, self.root, frame_file=self.path)

    def test_rgb888_and_565_colors_and_unchanged_cache(self):
        for fmt in (1, 2):
            with self.subTest(fmt=fmt):
                _, pixels = self.frame(fmt)
                reader = self.reader()
                expected = (bytes((248, 0, 0, 0, 252, 0, 0, 0, 248, 136, 204, 136)) if fmt == 1 else pixels)
                self.assertEqual((4, 1), reader.frame_geometry())
                self.assertEqual(expected, reader.frame_image().tobytes())
                with mock.patch.object(control.os, 'pread', wraps=os.pread) as reads:
                    self.assertEqual(expected, reader.frame_image().tobytes())
                self.assertEqual([(32, 0), (32, 0)], [(call.args[1], call.args[2]) for call in reads.call_args_list])

    def test_stride_format_length_and_odd_sequence_rejected(self):
        for fmt in (1, 2):
            for change in ({'stride': 4}, {'stride': 16}, {'fmt': 3}, {'sequence': 1}, {'sequence': 0}, {'version': 2}):
                self.frame(fmt, **change)
                for method in ('frame_geometry', 'frame_image'):
                    with self.subTest(fmt=fmt, change=change, method=method), self.assertRaises(RuntimeError):
                        getattr(self.reader(), method)()
            for delta in (-1, 1):
                self.frame(fmt)
                with self.path.open('r+b') as stream:
                    stream.truncate(self.path.stat().st_size + delta)
                for method in ('frame_geometry', 'frame_image'):
                    with self.assertRaises(RuntimeError): getattr(self.reader(), method)()

    def test_fixed_core_mapping_capacity_is_compatible(self):
        for fmt in (1, 2):
            _, pixels = self.frame(fmt)
            with self.path.open('r+b') as stream:
                stream.truncate(32 + 1024 * 1024 * (2 if fmt == 1 else 3))
            reader = self.reader()
            self.assertEqual((4, 1), reader.frame_geometry())
            picture = reader.frame_image()
            self.assertEqual((4, 1), picture.size)
            if fmt == 2:
                self.assertEqual(pixels, picture.tobytes())

    def test_rejects_unsafe_file_and_replacement(self):
        self.frame()
        reader = self.reader()
        reader.frame_image()
        original = self.root / 'original'
        self.path.rename(original)
        self.frame()
        for method in ('frame_geometry', 'frame_image'):
            with self.assertRaises(RuntimeError): getattr(reader, method)()
        self.path.unlink()
        self.path.symlink_to(original)
        for method in ('frame_geometry', 'frame_image'):
            with self.assertRaises(OSError): getattr(self.reader(), method)()
        self.path.unlink()
        os.link(original, self.path)
        for method in ('frame_geometry', 'frame_image'):
            with self.assertRaises(RuntimeError): getattr(self.reader(), method)()

    def test_double_header_mutation_never_returns_uncached_pixels(self):
        header, pixels = self.frame()
        changed = struct.pack('<8s6I', b'Q66FRAME', 1, 4, 4, 1, 12, 2)
        reader = self.reader()
        with mock.patch.object(control.os, 'pread', side_effect=[header, pixels, changed] * 3):
            with self.assertRaises(RuntimeError): reader.frame_image()
        with mock.patch.object(control.os, 'pread', side_effect=[header, pixels, changed, changed, pixels, changed]):
            self.assertEqual(pixels, reader.frame_image().tobytes())
        self.assertEqual(4, reader.frame_sequence)


if __name__ == '__main__':
    unittest.main()
