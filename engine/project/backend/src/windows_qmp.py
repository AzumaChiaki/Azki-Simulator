"""Private Win32 named-pipe QMP transport for an owned QEMU process.

QEMU creates its ACL-restricted pipe using the Windows pipe backend. The
host verifies the actual server PID; QEMU verifies its expected host client PID.
No unauthenticated TCP listener or POSIX-check bypass is used.
"""
from __future__ import annotations

import ctypes
from ctypes import wintypes as w
import json
import threading
import time
import uuid

from .windows_runtime_files import WindowsFiles


class WindowsQMP:
    def __init__(self, *, timeout=3.0, api=None):
        self.api = api or WindowsFiles()
        self.timeout = timeout
        self.lock = threading.RLock()
        self.closed = False
        self.connected = False
        self.next_id = 0
        self.buffer = bytearray()
        self.name = "AzkiSimulator-QMP-" + uuid.uuid4().hex
        self.path = "\\\\.\\pipe\\" + self.name
        self.handle = None
        k = self.api.kernel
        k.GetNamedPipeServerProcessId.argtypes = [w.HANDLE, ctypes.POINTER(w.ULONG)]
        k.GetNamedPipeServerProcessId.restype = w.BOOL
        k.SetNamedPipeHandleState.argtypes = [w.HANDLE, ctypes.POINTER(w.DWORD), w.LPVOID, w.LPVOID]
        k.SetNamedPipeHandleState.restype = w.BOOL
        k.ReadFile.argtypes = [w.HANDLE, w.LPVOID, w.DWORD, ctypes.POINTER(w.DWORD), w.LPVOID]
        k.ReadFile.restype = w.BOOL
        k.WriteFile.argtypes = [w.HANDLE, w.LPCVOID, w.DWORD, ctypes.POINTER(w.DWORD), w.LPVOID]
        k.WriteFile.restype = w.BOOL

    @property
    def qemu_argument(self):
        return "pipe:" + self.name

    def accept(self, process, *, startup_timeout=30.0):
        # QEMU 10.1's Windows pipe chardev is the server. Its patched backend
        # creates a protected owner/SYSTEM DACL and verifies our parent PID.
        deadline = time.monotonic() + startup_timeout
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("QEMU exited before its private QMP pipe started")
            handle = self.api.kernel.CreateFileW(self.path, 0xc0020000, 0, None, 3, 0, None)
            if handle != self.api.INVALID:
                self.handle = handle
                try:
                    pid = w.ULONG()
                    if not self.api.kernel.GetNamedPipeServerProcessId(handle, ctypes.byref(pid)):
                        raise self.api.error("GetNamedPipeServerProcessId")
                    if pid.value != process.pid:
                        raise PermissionError("QMP server is not this runtime's owned QEMU process")
                    self.api.check_private(handle)
                    mode = w.DWORD(1)  # PIPE_READMODE_BYTE | PIPE_NOWAIT
                    if not self.api.kernel.SetNamedPipeHandleState(handle, ctypes.byref(mode), None, None):
                        raise self.api.error("Set nonblocking QMP pipe mode")
                    self.connected = True
                    greeting = self._receive(timeout=max(.001, deadline - time.monotonic()))
                    if not isinstance(greeting, dict) or "QMP" not in greeting:
                        raise RuntimeError("QMP greeting is missing")
                    # QEMU sends its greeting before board/resource setup is
                    # complete. The first command shares the startup deadline;
                    # normal hardware inputs retain the short command timeout.
                    self.execute("qmp_capabilities", timeout=max(.001, deadline - time.monotonic()))
                    return
                except Exception:
                    self.close()
                    raise
            code = ctypes.get_last_error()
            if code not in (2, 3, 231):
                raise ctypes.WinError(code, "Open QEMU QMP pipe")
            time.sleep(.01)
        raise TimeoutError("QEMU private QMP pipe did not start")

    def _readline(self, timeout):
        deadline = time.monotonic() + timeout
        while True:
            end = self.buffer.find(b"\n")
            if end >= 0:
                if end >= 8 * 1024 * 1024:
                    raise RuntimeError("QMP reply exceeds bounded limit")
                line = bytes(self.buffer[:end + 1])
                del self.buffer[:end + 1]
                return line
            if self.closed:
                raise ConnectionError("QMP pipe is closed")
            if len(self.buffer) > 8 * 1024 * 1024:
                raise RuntimeError("QMP reply exceeds bounded limit")
            data = ctypes.create_string_buffer(65536)
            count = w.DWORD()
            if self.api.kernel.ReadFile(self.handle, data, len(data), ctypes.byref(count), None):
                if count.value:
                    self.buffer.extend(data.raw[:count.value])
                    continue
            else:
                code = ctypes.get_last_error()
                if code not in (232, 234):
                    raise ctypes.WinError(code, "Read QMP pipe")
            if time.monotonic() >= deadline:
                raise TimeoutError("QMP reply timed out; do not automatically resend a write")
            time.sleep(.002)

    def _receive(self, timeout=None):
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        while True:
            response = json.loads(self._readline(max(.001, deadline - time.monotonic())))
            if not isinstance(response, dict):
                raise RuntimeError("Invalid QMP reply")
            if "event" not in response:
                return response
            if time.monotonic() >= deadline:
                raise TimeoutError("QMP events exhausted the reply deadline")

    def execute(self, command, arguments=None, *, timeout=None):
        with self.lock:
            if self.closed or not self.connected:
                raise ConnectionError("QMP pipe is not connected")
            self.next_id += 1
            request = {"execute": command, "id": self.next_id}
            if arguments is not None:
                request["arguments"] = arguments
            data = json.dumps(request, separators=(",", ":")).encode() + b"\n"
            offset = 0
            deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
            try:
                while offset < len(data):
                    count = w.DWORD()
                    # A nonblocking Windows byte pipe reports zero progress
                    # when a whole write cannot fit its receive buffer. QEMU's
                    # legacy pipe buffer is 2048 bytes; bound each transfer.
                    part = data[offset:offset + 2048]
                    if not self.api.kernel.WriteFile(self.handle, part, len(part), ctypes.byref(count), None):
                        raise self.api.error("Write QMP pipe; outcome may be unknown")
                    if count.value:
                        offset += count.value
                    elif time.monotonic() >= deadline:
                        raise TimeoutError("QMP write timed out; do not automatically resend")
                    else:
                        time.sleep(.002)
                response = self._receive(timeout=max(.001, deadline - time.monotonic()))
                if type(response) is not dict:
                    raise RuntimeError("Invalid QMP reply")
                if type(response.get("id")) is not int or response["id"] != request["id"]:
                    raise RuntimeError("QMP response does not match the pending command")
                if ("return" in response) == ("error" in response):
                    raise RuntimeError("QMP response must contain one return or error")
            except Exception as error:
                # A late reply can never satisfy a later input command. In
                # particular, an uncertain touch/write is never sent twice.
                self.close()
                if isinstance(error, RuntimeError):
                    raise ConnectionError("QMP protocol failed; outcome may be unknown: " + str(error)) from error
                raise
            if "error" in response:
                raise RuntimeError(str(response["error"]))
            return response["return"]

    def close(self):
        with self.lock:
            if not self.closed:
                self.closed = True
                self.connected = False
                self.api.close(self.handle)
                self.handle = None
