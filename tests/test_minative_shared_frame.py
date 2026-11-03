"""Real file reads through the adapter; no guest or launcher is executed."""
import os
from pathlib import Path
import struct
import sys
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'Sources/AzkiSimulator/Runtime'))
from azki_simulator import bridge, minative, minative_bridge


class AdapterFrameTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name).resolve()
        self.path = self.root / 'lcd-frame.raw'
        self.adapter = object.__new__(minative.MinativeAdapter)
        self.adapter._lock = threading.RLock()
        self.adapter.process = SimpleNamespace(poll=lambda: None)
        self.adapter._control = {'fixture': True}
        self.adapter._read_control = lambda: {'framebuffer': self.path}
        self.adapter.plan = {'width': 432, 'height': 514}

    def tearDown(self):
        self.temp.cleanup()

    def frame(self, pixel_format=2, fixed=False, **changes):
        width, height = 432, 514
        bpp = 2 if pixel_format == 1 else 3
        values = dict(magic=b'Q66FRAME', version=1, sequence=2,
                      width=width, height=height, stride=width * bpp, fmt=pixel_format)
        values.update(changes)
        header = struct.pack('<8s6I', *values.values())
        pixel = bytes((0x71, 0x8e)) if pixel_format == 1 else bytes((7, 11, 19))
        pixels = pixel * (width * height)
        self.path.write_bytes(header + pixels)
        self.path.chmod(0o600)
        if fixed:
            with self.path.open('r+b') as stream:
                stream.truncate(32 + 1024 * 1024 * bpp)
        return header, pixels

    def test_formats_exact_and_fixed_capacity_preserve_pixels(self):
        for fmt in (1, 2):
            for fixed in (False, True):
                with self.subTest(fmt=fmt, fixed=fixed):
                    header, pixels = self.frame(fmt, fixed)
                    self.assertEqual((header, pixels), self.adapter.read_frame())
                    with mock.patch.object(minative.os, 'pread', wraps=os.pread) as calls:
                        self.assertIsNone(self.adapter.read_frame(previous_header=header))
                    self.assertEqual([(32, 0), (32, 0)], [(call.args[1], call.args[2]) for call in calls.call_args_list])

    def test_unchanged_does_not_skip_format_geometry_or_length_validation(self):
        for fmt in (1, 2):
            for change in ({'fmt': 3}, {'stride': 432}, {'width': 212}, {'height': 520}, {'version': 2}):
                header, _ = self.frame(fmt, **change)
                with self.subTest(fmt=fmt, change=change), self.assertRaises(minative.MError):
                    self.adapter.read_frame(previous_header=header)
            for delta in (-1, 1):
                header, _ = self.frame(fmt)
                with self.path.open('r+b') as stream:
                    stream.truncate(self.path.stat().st_size + delta)
                with self.assertRaises(minative.MError): self.adapter.read_frame(previous_header=header)

    def test_both_header_and_pixel_copy_races_fail_closed(self):
        for fmt in (1, 2):
            header, pixels = self.frame(fmt)
            changed = bytearray(header)
            struct.pack_into('<I', changed, 12, 4)
            with mock.patch.object(minative.os, 'pread', side_effect=[header, bytes(changed)]):
                with self.assertRaises(minative.MError): self.adapter.read_frame(previous_header=header)
            with mock.patch.object(minative.os, 'pread', side_effect=[header, pixels, bytes(changed)]):
                with self.assertRaises(minative.MError): self.adapter.read_frame()
            with mock.patch.object(minative.os, 'pread', side_effect=[header, pixels[:-1], header]):
                with self.assertRaises(minative.MError): self.adapter.read_frame()

    def test_odd_sequence_never_counts_as_unchanged(self):
        for fmt in (1, 2):
            header, _ = self.frame(fmt, sequence=3)
            with self.assertRaises(minative.MError): self.adapter.read_frame(previous_header=header)

    def test_production_mirror_publishes_native_rgb888_unchanged(self):
        header, pixels = self.frame()
        output = self.root / 'published'
        output.mkdir(mode=0o700)
        publisher = object.__new__(minative_bridge.MinativeSessionBridge)
        publisher.root, publisher.width, publisher.height = output, 432, 514
        publisher.last_error = None
        self.adapter.status = lambda: {'state': 'running' if publisher._frame_metrics['published'] == 0 else 'stopped'}
        self.adapter.stop = lambda: self.fail('Mirror rejected a valid native RGB888 frame')
        publisher._mirror_frames(self.adapter, threading.Event())
        self.assertEqual(1, publisher._frame_metrics['published'])
        self.assertEqual(header + pixels, (output / 'frame.raw').read_bytes())
        self.assertEqual((2, 432, 514, pixels), bridge.read_frame(output, (432, 514)))

    def test_private_file_and_link_guards_preserved(self):
        self.frame()
        self.path.chmod(0o644)
        with self.assertRaises(minative.MError): self.adapter.read_frame()
        self.path.chmod(0o600)
        target = self.root / 'target'
        self.path.rename(target)
        self.path.symlink_to(target)
        with self.assertRaises(OSError): self.adapter.read_frame()
        self.path.unlink()
        os.link(target, self.path)
        with self.assertRaises(minative.MError): self.adapter.read_frame()


if __name__ == '__main__':
    unittest.main()
