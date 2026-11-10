"""Current-user cross-process lease for one resolved USB PnP controller.

Regular disk-file share denial has no thread ownership: prepare can acquire the
lease and an I/O worker can release it. No PID discovery, stale-handle takeover,
driver operation or Bluetooth I/O occurs here. A failed release is quarantined.
"""
from __future__ import annotations

import ctypes as C
import hashlib
import os
from pathlib import Path
import threading
import uuid


_QUARANTINED_LEASES = []


class ControllerLeaseError(OSError):
    pass


def controller_key(instance_id):
    if (not isinstance(instance_id, str) or not 1 <= len(instance_id) <= 4096 or
            not instance_id.casefold().startswith("usb\\") or
            len(instance_id.split("\\")) < 3 or any(ord(c) < 32 for c in instance_id)):
        raise ValueError("A resolved USB PnP instance ID is required for a controller lease")
    return hashlib.sha256(("AzkiRawControllerLease-v1\0" + instance_id.casefold()).encode("utf-8")).hexdigest()


def _known_local_appdata():
    if os.name != "nt":
        raise OSError("Windows KnownFolder is required")
    shell = C.WinDLL("shell32", use_last_error=True)
    ole = C.WinDLL("ole32", use_last_error=True)
    shell.SHGetKnownFolderPath.argtypes = [C.c_void_p, C.c_uint32, C.c_void_p, C.POINTER(C.c_wchar_p)]
    shell.SHGetKnownFolderPath.restype = C.c_int32
    ole.CoTaskMemFree.argtypes = [C.c_void_p]
    ole.CoTaskMemFree.restype = None
    # FOLDERID_LocalAppData, resolved by Windows rather than an environment path.
    guid = C.create_string_buffer(uuid.UUID("f1b32785-6fba-4fcf-9d55-7b8e7f157091").bytes_le, 16)
    path = C.c_wchar_p()
    code = shell.SHGetKnownFolderPath(guid, 0, None, C.byref(path))
    if code < 0:
        raise OSError("SHGetKnownFolderPath failed: 0x%08x" % (code & 0xffffffff))
    try:
        if not path.value:
            raise OSError("Windows returned no LocalAppData path")
        return Path(path.value)
    finally:
        ole.CoTaskMemFree(C.cast(path, C.c_void_p))


class ControllerInstanceLease:
    """One current-user PnP identity, across interface aliases/processes/sessions."""
    @classmethod
    def acquire(cls, instance_id, *, directory=None, files=None):
        key = controller_key(instance_id)
        if files is None:
            if __package__:
                from .windows_runtime_files import WindowsFiles
            else:
                from windows_runtime_files import WindowsFiles
            files = WindowsFiles()
        files.initialize_private_creator()
        root = files.local_path(Path(directory) if directory is not None else
                                _known_local_appdata() / "AzkiSimulatorRawControllers" / "v1")
        files.private_directory(root, exist_ok=True)
        path = files.local_path(root / (key + ".lease"))
        attributes, security = files.security_attributes()
        try:
            # OPEN_ALWAYS, no inheritance or sharing, OPEN_REPARSE_POINT. The
            # empty marker remains after close; only the owned handle is a lock.
            handle = files.kernel.CreateFileW(str(path), 0xc0020000, 0, C.byref(attributes),
                                               4, 0x00200000, None)
        finally:
            files.kernel.LocalFree(security)
        if handle in (None, 0, files.INVALID):
            raise ControllerLeaseError("Controller instance is busy or its private lease is unavailable; no owner was interrupted")
        lease = cls(files, handle, instance_id, path)
        try:
            if __package__:
                from .windows_runtime_files import FILE_INFORMATION
            else:
                from windows_runtime_files import FILE_INFORMATION
            info = FILE_INFORMATION()
            if not files.kernel.GetFileInformationByHandle(handle, C.byref(info)):
                raise files.error("Inspect controller lease")
            if (info.attributes & (0x10 | 0x400) or info.links != 1 or
                    info.sizeHigh or info.sizeLow or files.kernel.GetFileType(handle) != 1):
                raise PermissionError("Controller lease must be an empty single-link regular disk file")
            files.check_private(handle)
            final = C.create_unicode_buffer(32768)
            count = files.kernel.GetFinalPathNameByHandleW(handle, final, len(final), 0)
            if not count or count >= len(final):
                raise files.error("Resolve controller lease path")
            actual = final.value.removeprefix("\\\\?\\")
            if os.path.normcase(os.path.normpath(actual)) != os.path.normcase(os.path.normpath(str(path))):
                raise PermissionError("Controller lease resolved outside its private path")
            return lease
        except BaseException:
            lease.close()
            raise

    def __init__(self, files, handle, instance_id, path):
        self.files, self.handle = files, handle
        self.instance_id, self.path = instance_id, path
        self.closed = False
        self._release_failed = False
        self._close_lock = threading.Lock()

    @property
    def descriptor(self):
        return {"controllerInstanceID": self.instance_id, "exclusiveLease": not self.closed,
                "leaseScope": "current-user-across-processes-and-sessions",
                "controllerAddress": None, "addressSource": "not-queried"}

    def close(self):
        with self._close_lock:
            if self.closed:
                return
            if self._release_failed:
                raise ControllerLeaseError("Controller lease release previously failed; no retry or takeover")
            try:
                if not self.files.kernel.CloseHandle(self.handle):
                    raise ControllerLeaseError("Owned controller lease handle did not close; retained until owner process exit")
            except BaseException as original:
                self._release_failed = True
                _QUARANTINED_LEASES.append(self)
                error = ControllerLeaseError(str(original))
                error.raw_cleanup = {"controllerIdentity": {"instanceID": self.instance_id},
                                     "exclusiveLease": self.descriptor,
                                     "resourcesQuarantined": True, "controllerClosed": False,
                                     "cleanupScope": "controller-lease-handle",
                                     "error": str(error)[:1024]}
                raise error from original
            self.handle = None
            self.closed = True
