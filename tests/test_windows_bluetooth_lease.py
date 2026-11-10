"""Controller-identity leases and conservative release; no radio/USB access."""
import ctypes as C
from pathlib import Path
import sys
import threading
from types import SimpleNamespace
import unittest

SRC = Path(__file__).resolve().parents[1] / "engine/project/backend/src"
sys.path.insert(0, str(SRC))
import windows_bluetooth_lease as lease
from windows_runtime_files import SECURITY_ATTRIBUTES

INSTANCE = r"USB\VID_0A12&PID_0001\REVIEWED-OWNED"


class FakeFiles:
    INVALID = -1

    def __init__(self):
        self.handles, self.paths = {}, {}
        self.next_handle = 100
        self.releases = []
        self.share_modes = []
        self.attributes, self.links, self.private = 0, 1, True
        self.fail_close = False
        self.kernel = SimpleNamespace(CreateFileW=self.create, LocalFree=lambda _: None,
            GetFileInformationByHandle=self.information, GetFileType=lambda handle: 1,
            GetFinalPathNameByHandleW=self.final_path, CloseHandle=self.close)

    def initialize_private_creator(self):
        pass

    def local_path(self, path):
        return Path(path)

    def private_directory(self, path, *, exist_ok):
        pass

    def security_attributes(self):
        return SECURITY_ATTRIBUTES(), 0

    def check_private(self, handle):
        if not self.private:
            raise PermissionError("Not a private owner/DACL")

    def create(self, path, access, share, security, disposition, flags, template):
        self.share_modes.append(share)
        if path in self.paths:
            return self.INVALID
        self.next_handle += 1
        self.paths[path] = self.next_handle
        self.handles[self.next_handle] = path
        return self.next_handle

    def information(self, handle, output):
        output._obj.attributes = self.attributes
        output._obj.links = self.links
        return 1

    def final_path(self, handle, output, capacity, flags):
        output.value = self.handles[handle]
        return len(output.value)

    def close(self, handle):
        self.releases.append(handle)
        if self.fail_close:
            return 0
        self.paths.pop(self.handles.pop(handle))
        return 1


class ControllerLeaseTests(unittest.TestCase):
    def test_pnp_identity_not_interface_alias_is_the_resource_key(self):
        self.assertEqual(lease.controller_key(INSTANCE), lease.controller_key(INSTANCE.lower()))
        self.assertNotEqual(lease.controller_key(INSTANCE), lease.controller_key(INSTANCE + "OTHER"))
        for identity in ("", "COM3", r"PCI\x\y", "USB\\x\0\\y", None):
            with self.subTest(identity=identity), self.assertRaises(ValueError):
                lease.controller_key(identity)

    def test_exclusive_lease_can_be_released_from_an_io_thread(self):
        files = FakeFiles()
        first = lease.ControllerInstanceLease.acquire(INSTANCE, directory=SRC / "test-leases", files=files)
        with self.assertRaises(lease.ControllerLeaseError):
            lease.ControllerInstanceLease.acquire(INSTANCE.lower(), directory=SRC / "test-leases", files=files)
        self.assertEqual(files.share_modes, [0, 0])
        self.assertTrue(first.descriptor["exclusiveLease"])
        self.assertIsNone(first.descriptor["controllerAddress"])
        worker = threading.Thread(target=first.close)
        worker.start(); worker.join(timeout=1)
        self.assertFalse(worker.is_alive())
        self.assertTrue(first.closed)
        second = lease.ControllerInstanceLease.acquire(INSTANCE.lower(), directory=SRC / "test-leases", files=files)
        second.close()
        self.assertEqual(files.handles, {})

    def test_reparse_hardlink_and_non_private_lease_are_rejected_and_closed(self):
        for setting, value in (("attributes", 0x400), ("links", 2), ("private", False)):
            with self.subTest(setting=setting):
                files = FakeFiles()
                setattr(files, setting, value)
                with self.assertRaises(PermissionError):
                    lease.ControllerInstanceLease.acquire(INSTANCE, directory=SRC / "test-leases", files=files)
                self.assertEqual(files.handles, {})

    def test_failed_release_keeps_handle_quarantined_and_does_not_retry(self):
        files = FakeFiles()
        owned = lease.ControllerInstanceLease.acquire(INSTANCE, directory=SRC / "test-leases", files=files)
        start = len(lease._QUARANTINED_LEASES)
        files.fail_close = True
        try:
            with self.assertRaises(lease.ControllerLeaseError):
                owned.close()
            self.assertIs(lease._QUARANTINED_LEASES[-1], owned)
            self.assertFalse(owned.closed)
            with self.assertRaises(lease.ControllerLeaseError):
                owned.close()
            self.assertEqual(len(files.releases), 1)
            with self.assertRaises(lease.ControllerLeaseError):
                lease.ControllerInstanceLease.acquire(INSTANCE, directory=SRC / "test-leases", files=files)
        finally:
            del lease._QUARANTINED_LEASES[start:]  # These are our fake handles only.


if __name__ == "__main__":
    unittest.main()
