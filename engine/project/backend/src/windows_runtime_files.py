"""Win32 ownership, private directories, file identity and owned process jobs.

This is a Windows implementation, not a no-op substitute for POSIX checks.
Only current-user-owned, single-link local files with private DACLs are accepted.
"""
from __future__ import annotations

import ctypes
from ctypes import wintypes as w
import os
from pathlib import Path
import shutil
import uuid


class WindowsHostUnavailable(RuntimeError):
    pass


class SECURITY_ATTRIBUTES(ctypes.Structure):
    _fields_ = [("nLength", w.DWORD), ("lpSecurityDescriptor", w.LPVOID),
                ("bInheritHandle", w.BOOL)]


class FILETIME(ctypes.Structure):
    _fields_ = [("low", w.DWORD), ("high", w.DWORD)]


class FILE_INFORMATION(ctypes.Structure):
    _fields_ = [("attributes", w.DWORD), ("created", FILETIME),
                ("accessed", FILETIME), ("written", FILETIME),
                ("volume", w.DWORD), ("sizeHigh", w.DWORD),
                ("sizeLow", w.DWORD), ("links", w.DWORD),
                ("indexHigh", w.DWORD), ("indexLow", w.DWORD)]


class ACL_SIZE_INFORMATION(ctypes.Structure):
    _fields_ = [("count", w.DWORD), ("used", w.DWORD), ("free", w.DWORD)]


class WindowsFiles:
    INVALID = ctypes.c_void_p(-1).value

    def __init__(self):
        if os.name != "nt":
            raise WindowsHostUnavailable("This host interface requires Windows")
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.security = ctypes.WinDLL("advapi32", use_last_error=True)
        k, a = self.kernel, self.security
        k.GetCurrentProcess.restype = w.HANDLE
        k.CloseHandle.argtypes = [w.HANDLE]
        k.LocalFree.argtypes = [w.HLOCAL]
        k.LocalFree.restype = w.HLOCAL
        k.CreateDirectoryW.argtypes = [w.LPCWSTR, ctypes.POINTER(SECURITY_ATTRIBUTES)]
        k.CreateDirectoryW.restype = w.BOOL
        k.GetFileAttributesW.argtypes = [w.LPCWSTR]
        k.GetFileAttributesW.restype = w.DWORD
        k.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD,
                                 ctypes.POINTER(SECURITY_ATTRIBUTES), w.DWORD,
                                 w.DWORD, w.HANDLE]
        k.CreateFileW.restype = w.HANDLE
        k.GetFileInformationByHandle.argtypes = [w.HANDLE, ctypes.POINTER(FILE_INFORMATION)]
        k.GetFileInformationByHandle.restype = w.BOOL
        k.GetFileType.argtypes = [w.HANDLE]
        k.GetFileType.restype = w.DWORD
        k.GetFinalPathNameByHandleW.argtypes = [w.HANDLE, w.LPWSTR, w.DWORD, w.DWORD]
        k.GetFinalPathNameByHandleW.restype = w.DWORD
        a.OpenProcessToken.argtypes = [w.HANDLE, w.DWORD, ctypes.POINTER(w.HANDLE)]
        a.OpenProcessToken.restype = w.BOOL
        a.GetTokenInformation.argtypes = [w.HANDLE, w.DWORD, w.LPVOID, w.DWORD,
                                          ctypes.POINTER(w.DWORD)]
        a.GetTokenInformation.restype = w.BOOL
        a.SetTokenInformation.argtypes = [w.HANDLE, ctypes.c_int, w.LPVOID, w.DWORD]
        a.SetTokenInformation.restype = w.BOOL
        a.ConvertSidToStringSidW.argtypes = [w.LPVOID, ctypes.POINTER(w.LPWSTR)]
        a.ConvertSidToStringSidW.restype = w.BOOL
        a.ConvertStringSecurityDescriptorToSecurityDescriptorW.argtypes = [
            w.LPCWSTR, w.DWORD, ctypes.POINTER(w.LPVOID), ctypes.POINTER(w.DWORD)]
        a.ConvertStringSecurityDescriptorToSecurityDescriptorW.restype = w.BOOL
        a.GetSecurityInfo.argtypes = [w.HANDLE, w.DWORD, w.DWORD,
                                     ctypes.POINTER(w.LPVOID), ctypes.POINTER(w.LPVOID),
                                     ctypes.POINTER(w.LPVOID), ctypes.POINTER(w.LPVOID),
                                     ctypes.POINTER(w.LPVOID)]
        a.GetSecurityInfo.restype = w.DWORD
        a.GetAclInformation.argtypes = [w.LPVOID, w.LPVOID, w.DWORD, w.DWORD]
        a.GetAclInformation.restype = w.BOOL
        a.GetAce.argtypes = [w.LPVOID, w.DWORD, ctypes.POINTER(w.LPVOID)]
        a.GetAce.restype = w.BOOL
        self.sid = self._current_sid()

    def initialize_private_creator(self):
        """Make new files belong to this user within this application process.

        Elevated Windows/OpenSSH tokens can default to an Administrators owner.
        File owners do not inherit from directory owners. Select TokenUser as
        this process token's default owner without changing any privileges,
        access grants, external process, user account or machine policy.
        """
        token = w.HANDLE()
        if not self.security.OpenProcessToken(self.kernel.GetCurrentProcess(), 0x88, ctypes.byref(token)):
            raise self.error("Open current process creator token")
        try:
            user_size = w.DWORD()
            self.security.GetTokenInformation(token, 1, None, 0, ctypes.byref(user_size))
            user = ctypes.create_string_buffer(user_size.value)
            if not self.security.GetTokenInformation(token, 1, user, user_size, ctypes.byref(user_size)):
                raise self.error("Read creator TokenUser")
            sid_pointer = ctypes.cast(user, ctypes.POINTER(w.LPVOID))[0]
            owner = w.LPVOID(sid_pointer)
            if not self.security.SetTokenInformation(token, 4, ctypes.byref(owner), ctypes.sizeof(owner)):
                raise self.error("Select current user as process default file owner")
            owner_size = w.DWORD()
            self.security.GetTokenInformation(token, 4, None, 0, ctypes.byref(owner_size))
            measured = ctypes.create_string_buffer(owner_size.value)
            if not self.security.GetTokenInformation(token, 4, measured, owner_size, ctypes.byref(owner_size)):
                raise self.error("Verify creator TokenOwner")
            actual = ctypes.cast(measured, ctypes.POINTER(w.LPVOID))[0]
            if self._sid_string(actual) != self.sid:
                raise PermissionError("Process default file owner is not the current user")
        finally:
            self.close(token)

    @staticmethod
    def error(label):
        return ctypes.WinError(ctypes.get_last_error(), label)

    def close(self, handle):
        if handle not in (None, 0, self.INVALID):
            self.kernel.CloseHandle(handle)

    def _sid_string(self, sid):
        output = w.LPWSTR()
        if not self.security.ConvertSidToStringSidW(sid, ctypes.byref(output)):
            raise self.error("ConvertSidToStringSidW")
        try:
            return output.value
        finally:
            self.kernel.LocalFree(ctypes.cast(output, w.HLOCAL))

    def _current_sid(self):
        token = w.HANDLE()
        if not self.security.OpenProcessToken(self.kernel.GetCurrentProcess(), 8, ctypes.byref(token)):
            raise self.error("OpenProcessToken")
        try:
            size = w.DWORD()
            self.security.GetTokenInformation(token, 1, None, 0, ctypes.byref(size))
            memory = ctypes.create_string_buffer(size.value)
            if not self.security.GetTokenInformation(token, 1, memory, size, ctypes.byref(size)):
                raise self.error("GetTokenInformation")
            return self._sid_string(ctypes.cast(memory, ctypes.POINTER(w.LPVOID))[0])
        finally:
            self.close(token)

    def security_attributes(self, *, directory=False):
        descriptor = w.LPVOID()
        # Protected DACL: current user and LocalSystem only; no inherited grants.
        inheritance = "OICI" if directory else ""
        sddl = f"O:{self.sid}D:P(A;{inheritance};FA;;;{self.sid})(A;{inheritance};FA;;;SY)"
        if not self.security.ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl, 1, ctypes.byref(descriptor), None):
            raise self.error("Build private DACL")
        attributes = SECURITY_ATTRIBUTES(ctypes.sizeof(SECURITY_ATTRIBUTES), descriptor, False)
        return attributes, descriptor

    def check_private(self, handle):
        owner, dacl, descriptor = w.LPVOID(), w.LPVOID(), w.LPVOID()
        code = self.security.GetSecurityInfo(handle, 1, 5, ctypes.byref(owner), None,
                                             ctypes.byref(dacl), None, ctypes.byref(descriptor))
        if code:
            raise ctypes.WinError(code, "GetSecurityInfo")
        try:
            if not owner or self._sid_string(owner) != self.sid or not dacl:
                raise PermissionError("Windows file must have current-user owner and private DACL")
            info = ACL_SIZE_INFORMATION()
            if not self.security.GetAclInformation(dacl, ctypes.byref(info), ctypes.sizeof(info), 2):
                raise self.error("GetAclInformation")
            allowed = {self.sid, "S-1-5-18", "S-1-5-32-544"}
            for index in range(info.count):
                ace = w.LPVOID()
                if not self.security.GetAce(dacl, index, ctypes.byref(ace)):
                    raise self.error("GetAce")
                kind = ctypes.c_ubyte.from_address(ace.value).value
                flags = ctypes.c_ubyte.from_address(ace.value + 1).value
                if flags & 8:  # INHERIT_ONLY_ACE does not grant this object access.
                    continue
                if kind == 1:  # deny ACE cannot broaden access
                    continue
                if kind != 0:
                    raise PermissionError("Unsupported Windows file ACL entry")
                sid = self._sid_string(ace.value + 8)
                if sid not in allowed:
                    raise PermissionError("Windows file grants access outside the private owner set")
        finally:
            self.kernel.LocalFree(descriptor)

    def local_path(self, value):
        path = Path(value)
        text = str(path)
        if not path.is_absolute() or not path.drive or text.startswith("\\\\") or ".." in path.parts:
            raise ValueError("Expected absolute local-drive Windows path")
        reserved = {"CON", "PRN", "AUX", "NUL", *[f"COM{i}" for i in range(1, 10)], *[f"LPT{i}" for i in range(1, 10)]}
        for part in path.parts[1:]:
            if (any(ord(char) < 32 or char in ':*?"<>|' for char in part) or
                    part.endswith((" ", ".")) or part.split(".")[0].upper() in reserved):
                raise ValueError("Alternate streams, device names and ambiguous paths are not accepted")
        # No reparse point is accepted anywhere, including parent junctions.
        current = Path(path.anchor)
        for part in path.parts[1:]:
            current /= part
            attrs = self.kernel.GetFileAttributesW(str(current))
            if attrs == 0xffffffff:
                code = ctypes.get_last_error()
                if code in (2, 3):
                    break
                raise ctypes.WinError(code, "GetFileAttributesW")
            if attrs & 0x400:
                raise PermissionError("Reparse points are not accepted in runtime paths")
        return path

    def private_directory(self, path, *, exist_ok=False):
        path = self.local_path(path)
        if path.exists():
            if not exist_ok:
                raise FileExistsError(path)
            handle = self.open_read(path, directory=True)
            self.close(handle)
            return path
        if not path.parent.exists():
            self.private_directory(path.parent, exist_ok=True)
        attributes, descriptor = self.security_attributes(directory=True)
        try:
            if not self.kernel.CreateDirectoryW(str(path), ctypes.byref(attributes)):
                raise self.error("Create private runtime directory")
        finally:
            self.kernel.LocalFree(descriptor)
        return path

    def open_read(self, path, *, directory=False, require_private=True, share_write=True):
        path = self.local_path(path)
        flags = 0x00200000 | (0x02000000 if directory else 0)  # OPEN_REPARSE_POINT/BACKUP_SEMANTICS
        handle = self.kernel.CreateFileW(str(path), 0x80000000 | (0x00020000 if require_private else 0),
                                         3 if share_write else 1, None, 3, flags, None)
        if handle == self.INVALID:
            raise self.error("Open private runtime file")
        try:
            info = FILE_INFORMATION()
            if not self.kernel.GetFileInformationByHandle(handle, ctypes.byref(info)):
                raise self.error("GetFileInformationByHandle")
            if info.attributes & 0x400 or bool(info.attributes & 0x10) != directory:
                raise PermissionError("Unexpected file type or reparse point")
            if not directory and (info.links != 1 or self.kernel.GetFileType(handle) != 1):
                raise PermissionError("Runtime file must be a single-link regular disk file")
            if require_private:
                self.check_private(handle)
            final = ctypes.create_unicode_buffer(32768)
            count = self.kernel.GetFinalPathNameByHandleW(handle, final, len(final), 0)
            if not count or count >= len(final):
                raise self.error("GetFinalPathNameByHandleW")
            resolved = final.value.removeprefix("\\\\?\\")
            if os.path.normcase(os.path.normpath(resolved)) != os.path.normcase(os.path.normpath(str(path))):
                raise PermissionError("Runtime path resolved to another file")
            return handle
        except BaseException:
            self.close(handle)
            raise

    def information(self, handle):
        info = FILE_INFORMATION()
        if not self.kernel.GetFileInformationByHandle(handle, ctypes.byref(info)):
            raise self.error("GetFileInformationByHandle")
        return (info.volume, info.indexHigh << 32 | info.indexLow,
                info.sizeHigh << 32 | info.sizeLow)


class OwnedWindowsJob:
    """Kill-on-close applies only to the process explicitly attached by its owner."""

    def __init__(self, api=None):
        self.api = api or WindowsFiles()
        k = self.api.kernel
        k.CreateJobObjectW.argtypes = [w.LPVOID, w.LPCWSTR]
        k.CreateJobObjectW.restype = w.HANDLE
        k.SetInformationJobObject.argtypes = [w.HANDLE, ctypes.c_int, w.LPVOID, w.DWORD]
        k.SetInformationJobObject.restype = w.BOOL
        k.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
        k.AssignProcessToJobObject.restype = w.BOOL

        class BASIC(ctypes.Structure):
            _fields_ = [("processTime", ctypes.c_longlong), ("jobTime", ctypes.c_longlong),
                        ("flags", w.DWORD), ("minWork", ctypes.c_size_t),
                        ("maxWork", ctypes.c_size_t), ("active", w.DWORD),
                        ("affinity", ctypes.c_size_t), ("priority", w.DWORD),
                        ("scheduling", w.DWORD)]

        class EXTENDED(ctypes.Structure):
            _fields_ = [("basic", BASIC), ("io", ctypes.c_ulonglong * 6),
                        ("processMemory", ctypes.c_size_t), ("jobMemory", ctypes.c_size_t),
                        ("peakProcessMemory", ctypes.c_size_t), ("peakJobMemory", ctypes.c_size_t)]

        self.handle = k.CreateJobObjectW(None, None)
        if not self.handle:
            raise self.api.error("CreateJobObjectW")
        limits = EXTENDED()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not k.SetInformationJobObject(self.handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            self.close()
            raise self.api.error("SetInformationJobObject")

    def attach(self, process):
        if not self.api.kernel.AssignProcessToJobObject(self.handle, int(process._handle)):
            raise self.api.error("AssignProcessToJobObject")

    def close(self):
        if self.handle:
            self.api.close(self.handle)
            self.handle = None


class WindowsCacheLock:
    """Real exclusive Win32 byte-range lock for the existing seed build cache."""

    def __init__(self, path):
        self.path = path
        self.api = WindowsFiles()
        self.handle = None

    def __enter__(self):
        path = self.api.local_path(self.path)
        attributes, descriptor = self.api.security_attributes()
        try:
            handle = self.api.kernel.CreateFileW(str(path), 0xc0020000, 3,
                                                  ctypes.byref(attributes), 4,
                                                  0x00200000, None)
        finally:
            self.api.kernel.LocalFree(descriptor)
        if handle == self.api.INVALID:
            raise self.api.error("Open private cache lock")
        self.handle = handle
        try:
            info = FILE_INFORMATION()
            if not self.api.kernel.GetFileInformationByHandle(handle, ctypes.byref(info)):
                raise self.api.error("Inspect cache lock")
            if info.attributes & (0x10 | 0x400) or info.links != 1 or self.api.kernel.GetFileType(handle) != 1:
                raise PermissionError("Cache lock must be a private single-link regular file")
            self.api.check_private(handle)

            class OVERLAPPED(ctypes.Structure):
                _fields_ = [("internal", ctypes.c_size_t), ("high", ctypes.c_size_t),
                            ("offset", w.DWORD), ("offsetHigh", w.DWORD), ("event", w.HANDLE)]

            self.overlapped = OVERLAPPED()
            self.api.kernel.LockFileEx.argtypes = [w.HANDLE, w.DWORD, w.DWORD,
                                                    w.DWORD, w.DWORD, ctypes.POINTER(OVERLAPPED)]
            self.api.kernel.LockFileEx.restype = w.BOOL
            if not self.api.kernel.LockFileEx(handle, 3, 0, 0xffffffff, 0xffffffff,
                                               ctypes.byref(self.overlapped)):
                raise self.api.error("Cache is locked by another preparation; retry explicitly later")
            return self
        except BaseException:
            self.api.close(self.handle)
            self.handle = None
            raise

    def __exit__(self, *_):
        self.api.close(self.handle)  # Closing the owning handle releases its lock.
        self.handle = None


class WindowsTemporaryDirectory:
    """Explicit user/SYSTEM staging without ambiguous OWNER RIGHTS grants."""

    def __init__(self, *, prefix, dir):
        self.api = WindowsFiles()
        self.directory = Path(dir) / (prefix + uuid.uuid4().hex)
        self.identity = None

    def __enter__(self):
        self.api.initialize_private_creator()
        self.api.private_directory(self.directory)
        handle = self.api.open_read(self.directory, directory=True)
        try:
            self.identity = self.api.information(handle)[:2]
        finally:
            self.api.close(handle)
        return str(self.directory)

    def __exit__(self, *_):
        handle = self.api.open_read(self.directory, directory=True)
        try:
            if self.api.information(handle)[:2] != self.identity:
                raise PermissionError("Owned helper staging directory was replaced")
        finally:
            self.api.close(handle)
        shutil.rmtree(self.directory)
