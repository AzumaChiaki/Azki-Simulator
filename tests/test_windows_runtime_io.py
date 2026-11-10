"""Native Windows API acceptance; synthetic owned files, never a guest boot claim.

Run: python -m unittest discover -s tests -p test_windows_runtime_io.py -v
Optional AZKI_WINDOWS_IO_TEST_ROOT is a parent for retained private fixtures.
The suite never opens/terminates another application's process.
"""
from __future__ import annotations
import base64
import ctypes as c
from ctypes import wintypes as w
import gc
import json
import mmap
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'engine/project/backend'))
from src.windows_frame import WindowsFrameReader
from src.windows_runtime_files import WindowsFiles, OwnedWindowsJob, ACL_SIZE_INFORMATION, WindowsTemporaryDirectory


@unittest.skipUnless(os.name == 'nt', 'Requires real Windows Win32 APIs')
class WindowsRuntimeIONativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.api = WindowsFiles()
        cls.api.initialize_private_creator()
        parent = Path(os.environ.get('AZKI_WINDOWS_IO_TEST_ROOT', tempfile.gettempdir()))
        cls.root = cls.api.private_directory(parent / ('runtime-io-fixtures-' + uuid.uuid4().hex))
        cls.api.kernel.WriteFile.argtypes = [w.HANDLE, w.LPVOID, w.DWORD, c.POINTER(w.DWORD), w.LPVOID]
        cls.api.kernel.WriteFile.restype = w.BOOL
        cls.api.kernel.GetProcessHandleCount.argtypes = [w.HANDLE, c.POINTER(w.DWORD)]
        cls.api.kernel.GetProcessHandleCount.restype = w.BOOL
        cls.api.close(cls.write_private(cls.root / 'fixture-owner.json', json.dumps({'schema': 1, 'owner': 'Azki WindowsRuntimeIONativeTests'}).encode()))
        cls.checks = []
        cls.handle_counts = []
        cls.temporary_directory_metadata = []
        cls.children = []

    @classmethod
    def tearDownClass(cls):
        # Keep fixtures and receipt for review. No recursive cleanup of paths.
        for process in cls.children:
            if process.returncode is None:
                process.terminate()
                process.wait(3)
            process._handle.Close()
        cls.children.clear()
        print(json.dumps({'schema': 1, 'suite': 'WindowsRuntimeIONativeTests',
                          'checks': cls.checks, 'passedChecks': len(cls.checks),
                          'handleCounts': cls.handle_counts, 'temporaryDirectoryMetadata': cls.temporary_directory_metadata, 'privateDirectory': str(cls.root),
                          'realWin32APIs': True, 'actualFirmware': False}), flush=True)

    def good(self, label, condition=True):
        self.assertTrue(condition, label)
        self.checks.append(label)

    @classmethod
    def write_private(cls, path, data, *, share=0):
        attrs, descriptor = cls.api.security_attributes()
        try:
            handle = cls.api.kernel.CreateFileW(str(path), 0xc0020000, share, c.byref(attrs), 1, 0x00200000, None)
        finally:
            cls.api.kernel.LocalFree(descriptor)
        if handle == cls.api.INVALID:
            raise cls.api.error('Create owned native I/O fixture')
        try:
            cls.api.check_private(handle)
            memory = c.create_string_buffer(data)
            count = w.DWORD()
            if not cls.api.kernel.WriteFile(handle, memory, len(data), c.byref(count), None) or count.value != len(data):
                raise cls.api.error('Write owned native I/O fixture')
        except BaseException:
            cls.api.close(handle)
            raise
        return handle

    @classmethod
    def file(cls, path, data):
        cls.api.close(cls.write_private(path, data))
        return path

    def area(self):
        return self.api.private_directory(self.root / ('case-' + uuid.uuid4().hex))

    def handles(self):
        count = w.DWORD()
        self.assertTrue(self.api.kernel.GetProcessHandleCount(self.api.kernel.GetCurrentProcess(), c.byref(count)))
        return count.value

    def test_private_unicode_file_and_sharing(self):
        area = self.area()
        path = self.file(area / '私有数据🌙.bin', b'\x00\r\n\xffnative')
        handle = self.api.open_read(path, share_write=False)
        try:
            identity = self.api.information(handle)
            self.good('private-unicode-file-identity', identity[1] > 0 and identity[2] == 10)
            writer = self.api.kernel.CreateFileW(str(path), 0xc0000000, 3, None, 3, 0x00200000, None)
            if writer != self.api.INVALID:
                self.api.close(writer)
                self.fail('share_write=False accepted a writer')
            self.good('share-write-false-denies-writer', c.get_last_error() == 32)
        finally:
            self.api.close(handle)
        handle = self.api.open_read(path, share_write=True)
        try:
            writer = self.api.kernel.CreateFileW(str(path), 0xc0000000, 3, None, 3, 0x00200000, None)
            self.good('share-write-true-allows-writer', writer != self.api.INVALID)
            self.api.close(writer)
        finally:
            self.api.close(handle)
        directory = self.api.open_read(area, directory=True)
        self.api.close(directory)
        with self.assertRaises(PermissionError):
            self.api.open_read(area)
        self.good('directory-file-type-mismatch-rejected')
        inherited = area / 'inherited-child.bin'
        inherited.write_bytes(b'child')  # Ordinary CRT file creation must inherit private ACLs.
        handle = self.api.open_read(inherited)
        descriptor, dacl = w.LPVOID(), w.LPVOID()
        try:
            code = self.api.security.GetSecurityInfo(handle, 1, 4, None, None, c.byref(dacl), None, c.byref(descriptor))
            self.assertEqual(code, 0)
            info = ACL_SIZE_INFORMATION()
            self.assertTrue(self.api.security.GetAclInformation(dacl, c.byref(info), c.sizeof(info), 2))
            inherited_user = False
            for index in range(info.count):
                ace = w.LPVOID()
                self.assertTrue(self.api.security.GetAce(dacl, index, c.byref(ace)))
                if (c.c_ubyte.from_address(ace.value).value == 0 and
                        c.c_ubyte.from_address(ace.value + 1).value & 16 and
                        self.api._sid_string(ace.value + 8) == self.api.sid):
                    inherited_user = True
            self.good('directory-ACL-inherits-current-user-to-ordinary-child', inherited_user)
        finally:
            if descriptor:
                self.api.kernel.LocalFree(descriptor)
            self.api.close(handle)

    def test_private_file_negative_paths_and_acl(self):
        area = self.area()
        path = self.file(area / 'private.bin', b'fixture')
        os.link(path, area / 'alias.bin')
        with self.assertRaises(PermissionError):
            self.api.open_read(path)
        self.good('hardlink-refused-by-native-file-information')
        tree = self.api.private_directory(area / 'target')
        self.file(tree / 'payload.bin', b'fixture')
        junction = area / 'junction'
        result = subprocess.run(['cmd.exe', '/d', '/c', 'mklink', '/J', str(junction), str(tree)], capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0, 'Cannot create owned junction negative fixture')
        with self.assertRaises(PermissionError):
            self.api.open_read(junction / 'payload.bin')
        self.good('ancestor-junction-refused')
        broad = self.file(area / 'broad.bin', b'fixture')
        result = subprocess.run(['icacls', str(broad), '/grant', '*S-1-1-0:(R)'], capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0, 'Cannot create owned broad ACL negative fixture')
        with self.assertRaises(PermissionError):
            self.api.open_read(broad)
        self.good('broad-acl-refused')
        handle = self.api.open_read(broad, require_private=False)
        self.api.close(handle)
        self.good('explicit-public-source-still-real-file')
        for bad in ['relative.bin', r'C:\fixture.bin:stream', r'\\server\share\file', r'C:\NUL', r'C:\bad.', r'C:\bad ', r'C:\x\..\y']:
            with self.subTest(path=bad), self.assertRaises((PermissionError, ValueError)):
                self.api.local_path(bad)
        self.good('relative-ads-unc-device-ambiguous-parent-paths-refused')

    def writer(self, area, pixel_format, capacity=None):
        import msvcrt
        path = area / ('映射-' + uuid.uuid4().hex + '.bin')
        size = capacity if capacity is not None else 32 + 3 * 2 * (2 if pixel_format == 1 else 3)
        handle = self.write_private(path, bytes(size), share=1)
        fd = msvcrt.open_osfhandle(handle, os.O_RDWR | os.O_BINARY)
        try:
            mapping = mmap.mmap(fd, 0, access=mmap.ACCESS_WRITE)
        except BaseException:
            os.close(fd)
            raise
        return path, fd, mapping

    @staticmethod
    def publish(mapping, pixel_format, sequence, pixels):
        width, height = 3, 2
        stride = width * (2 if pixel_format == 1 else 3)
        mapping[0:32] = struct.pack('<8s6I', b'Q66FRAME', 1, sequence | 1, width, height, stride, pixel_format)
        mapping[32:32 + len(pixels)] = pixels
        mapping[0:32] = struct.pack('<8s6I', b'Q66FRAME', 1, sequence, width, height, stride, pixel_format)

    def test_rgb565_rgb888_real_dual_mappings_and_sequence(self):
        area = self.area()
        for pixel_format in (1, 2):
            for maximum in (False, True):
                size = 32 + 1024 * 1024 * (2 if pixel_format == 1 else 3) if maximum else None
                path, fd, writer = self.writer(area, pixel_format, size)
                one = WindowsFrameReader(path, expected=(3, 2), api=self.api)
                two = WindowsFrameReader(path, expected=(3, 2), api=self.api)
                try:
                    self.assertEqual(one.snapshot(), {'ready': False})
                    self.good(f'uninitialized-mapping-{pixel_format}-{maximum}')
                    pixels = bytes.fromhex('00f8e0071f00' * 2) if pixel_format == 1 else bytes.fromhex('ff000000ff000000ff' * 2)
                    self.publish(writer, pixel_format, 2, pixels)
                    first, second = one.snapshot(), two.snapshot()
                    self.assertEqual(first, second)
                    self.assertEqual(base64.b64decode(first['pixelsBase64']), pixels)
                    self.good(f'actual-two-view-coherence-format-{pixel_format}-{maximum}', first['ready'] and first['sequence'] == 2 and first['pixelFormat'] == ('rgb565le' if pixel_format == 1 else 'rgb888'))
                    self.publish(writer, pixel_format, 3, bytes(len(pixels)))
                    self.assertEqual(one.snapshot(), {'ready': False})
                    self.good(f'odd-sequence-frame-refused-{pixel_format}-{maximum}')
                    self.publish(writer, pixel_format, 4, pixels[::-1])
                    self.good(f'next-even-frame-update-{pixel_format}-{maximum}', base64.b64decode(two.snapshot()['pixelsBase64']) == pixels[::-1])
                    self.publish(writer, pixel_format, 0, pixels)
                    self.assertEqual(one.snapshot(), {'ready': False})
                    self.good(f'zero-sequence-frame-refused-{pixel_format}-{maximum}')
                finally:
                    one.close(); two.close(); writer.close(); os.close(fd)

    def test_identity_header_and_handle_lifetime(self):
        area = self.area()
        path, fd, writer = self.writer(area, 1)
        pixels = bytes.fromhex('00f8e0071f00' * 2)
        self.publish(writer, 1, 2, pixels)
        reader = WindowsFrameReader(path, expected=(3, 2), api=self.api)
        self.assertTrue(reader.snapshot()['ready'])
        writer.close(); os.close(fd)
        replacement = self.file(area / 'replacement.bin', path.read_bytes())
        try:
            with self.assertRaises(OSError):
                os.replace(replacement, path)
            self.good('native-reader-handle-prevents-path-replacement')
            self.assertTrue(reader.snapshot()['ready'])
            reader.path = replacement  # Exercise actual file-ID comparison, not a fake API.
            with self.assertRaises(PermissionError):
                reader.snapshot()
            self.good('different-native-file-id-rejected')
            reader.path = path
        finally:
            reader.close(); reader.close()
        bad = WindowsFrameReader(path, expected=(4, 2), api=self.api)
        try:
            with self.assertRaises(ValueError):
                bad.snapshot()
            self.good('hardware-geometry-mismatch-refused')
        finally:
            bad.close()
        baseline = self.handles()
        for _ in range(80):
            current = WindowsFrameReader(path, expected=(3, 2), api=self.api)
            self.assertTrue(current.snapshot()['ready'])
            current.close()
        gc.collect()
        after = self.handles()
        self.handle_counts.append({'operation': '80 native frame open/snapshot/close cycles', 'before': baseline, 'after': after})
        self.good('frame-handles-return-to-baseline', after == baseline)

    def test_temporary_directory_exact_acl_and_mutating_cleanup(self):
        area = self.area()
        outside = self.api.private_directory(area / 'outside-owned-control')
        keep = self.file(outside / 'keep.bin', b'owned sibling must remain')
        with WindowsTemporaryDirectory(prefix='native-阶段🌙-', dir=area) as name:
            staging = Path(name)
            handle = self.api.open_read(staging, directory=True)
            descriptor, dacl = w.LPVOID(), w.LPVOID()
            try:
                before = self.api.information(handle)
                code = self.api.security.GetSecurityInfo(handle, 1, 4, None, None, c.byref(dacl), None, c.byref(descriptor))
                self.assertEqual(code, 0)
                info = ACL_SIZE_INFORMATION()
                self.assertTrue(self.api.security.GetAclInformation(dacl, c.byref(info), c.sizeof(info), 2))
                allowed = set()
                for index in range(info.count):
                    ace = w.LPVOID()
                    self.assertTrue(self.api.security.GetAce(dacl, index, c.byref(ace)))
                    if c.c_ubyte.from_address(ace.value).value == 0:
                        allowed.add(self.api._sid_string(ace.value + 8))
                self.good('temporary-directory-exact-user-SYSTEM-without-admin-or-OW', allowed == {self.api.sid, 'S-1-5-18'})
            finally:
                if descriptor:
                    self.api.kernel.LocalFree(descriptor)
                self.api.close(handle)
            nested = staging / 'nested'
            nested.mkdir()
            for index in range(80):
                child = (nested if index % 3 == 0 else staging) / ('child-' + str(index) + '.bin')
                child.write_bytes(b'private inherited child')
                handle = self.api.open_read(child)
                self.api.close(handle)
            (staging / 'delete-before-exit.bin').write_bytes(b'temporary')
            (staging / 'delete-before-exit.bin').unlink()
            after_handle = self.api.open_read(staging, directory=True)
            try:
                after = self.api.information(after_handle)
                self.assertEqual(before[:2], after[:2])
                self.temporary_directory_metadata.append({'before': list(before), 'afterAddingDeletingChildren': list(after)})
            finally:
                self.api.close(after_handle)
            result = subprocess.run(['cmd.exe', '/d', '/c', 'mklink', '/J', str(staging / 'sibling-junction'), str(outside)], capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0)
        self.good('temporary-directory-cleanup-allows-real-child-mutations', not staging.exists())
        self.good('temporary-cleanup-does-not-follow-child-junction', keep.read_bytes() == b'owned sibling must remain')
        baseline = self.handles()
        for _ in range(24):
            with WindowsTemporaryDirectory(prefix='cycle-', dir=area) as name:
                (Path(name) / 'ordinary.bin').write_bytes(b'fixture')
        gc.collect()
        after = self.handles()
        self.handle_counts.append({'operation': '24 private temporary directory create/mutate/cleanup cycles', 'before': baseline, 'after': after})
        self.good('temporary-directory-handles-return-to-baseline', baseline == after)

    def test_temporary_directory_replaced_file_id_and_exception(self):
        area = self.area()
        temporary = WindowsTemporaryDirectory(prefix='swap-', dir=area)
        original = Path(temporary.__enter__())
        (original / 'original.bin').write_bytes(b'original owned staging')
        retained = area / ('retained-' + uuid.uuid4().hex)
        original.rename(retained)
        self.api.private_directory(original)
        (original / 'replacement.bin').write_bytes(b'replacement must remain')
        with self.assertRaises(PermissionError):
            temporary.__exit__(None, None, None)
        self.good('temporary-directory-file-ID-swap-refused', (retained / 'original.bin').read_bytes() == b'original owned staging')
        self.good('temporary-directory-refusal-preserves-replacement', (original / 'replacement.bin').read_bytes() == b'replacement must remain')
        failed_name = None
        with self.assertRaises(LookupError):
            with WindowsTemporaryDirectory(prefix='exception-', dir=area) as name:
                failed_name = Path(name)
                (failed_name / 'child.bin').write_bytes(b'owned exception fixture')
                raise LookupError('deliberate owned staging exception')
        self.good('temporary-directory-cleans-owned-exception-scope', failed_name is not None and not failed_name.exists())

    def test_unpublished_metadata_and_transient_geometry(self):
        area = self.area()
        path, fd, writer = self.writer(area, 1)
        reader = WindowsFrameReader(path, expected=(3, 2), api=self.api)
        try:
            writer[:32] = struct.pack('<8s6I', b'Q66FRAME', 1, 0, 0, 0, 0, 1)
            self.assertEqual(reader.snapshot(), {'ready': False})
            self.good('protocol-metadata-only-before-first-geometry-is-not-ready')
            writer[:32] = struct.pack('<8s6I', b'Q66FRAME', 1, 1, 0xffffffff, 0, 0, 99)
            self.assertEqual(reader.snapshot(), {'ready': False})
            self.good('odd-sequence-transient-invalid-geometry-is-not-ready')
            writer[:32] = struct.pack('<8s6I', b'Q66FRAME', 1, 2, 0, 0, 0, 1)
            with self.assertRaises(ValueError):
                reader.snapshot()
            self.good('invalid-published-even-geometry-still-rejected')
            pixels = bytes.fromhex('00f8e0071f00' * 2)
            self.publish(writer, 1, 4, pixels)
            frame = reader.snapshot()
            self.good('valid-even-frame-after-initialization', frame['ready'] and base64.b64decode(frame['pixelsBase64']) == pixels)
        finally:
            reader.close(); writer.close(); os.close(fd)

    def test_owned_job_closes_exact_child(self):
        def child():
            result = subprocess.Popen([sys.executable, '-I', '-B', '-c', 'import time;time.sleep(30)'], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
            self.children.append(result)
            return result
        cold = self.handles()
        # Native control probe shows the first subprocess creation initializes
        # two persistent CPython/Win32 handles once, with or without a Job.
        warm = child()
        warm.terminate(); warm.wait(3); warm._handle.Close()
        self.children.remove(warm)
        del warm
        gc.collect()
        initial = self.handles()
        self.handle_counts.append({'operation': 'first pure-Popen initialization control', 'before': cold, 'after': initial})
        control = child()
        control.terminate(); control.wait(3); control._handle.Close()
        self.children.remove(control)
        del control
        gc.collect()
        self.good('subsequent-pure-popen-control-no-handle-growth', self.handles() == initial)
        owned, sentinel = child(), child()
        before = self.handles()
        job = OwnedWindowsJob(self.api)
        try:
            job.attach(owned)
            self.good('native-job-attaches-live-owned-child', owned.poll() is None)
            job.close(); job.close()
            owned.wait(5)
            self.good('kill-on-job-close-terminates-owned-child', owned.returncode is not None)
            self.good('unattached-owned-sentinel-remains-running', sentinel.poll() is None)
        finally:
            job.close()
            for process in (owned, sentinel):
                if process.poll() is None:
                    process.terminate()
                process.wait(3)
                process._handle.Close()
                self.children.remove(process)
        before_object_release = self.handles()
        owned_exit, sentinel_exit = owned.returncode, sentinel.returncode
        # Measure after releasing the complete owned process-object lifetime.
        del process, owned, sentinel
        gc.collect()
        after = self.handles()
        self.handle_counts.append({'operation': '2 own children plus owned JobObject close', 'initial': initial, 'beforeWithChildren': before, 'afterNativeCloseBeforePopenRelease': before_object_release, 'after': after, 'ownedExit': owned_exit, 'sentinelExit': sentinel_exit})
        self.good('job-and-child-handles-return-to-baseline', after == initial)


if __name__ == '__main__':
    unittest.main(verbosity=2)
