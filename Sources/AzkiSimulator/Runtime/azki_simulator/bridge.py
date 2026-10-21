"""Bounded Unix-session bridge for the audited Q66FRAME/QMP interfaces.

This module does not import the download's Python code. A core is never launched
unless the operator supplied the exact audited hashes at serve time. The socket
is a per-session, same-user capability, not a network service or persistent auth.
"""
from __future__ import annotations

import hashlib
import ctypes
import json
import os
from pathlib import Path
import re
import signal
import socket
import stat
import struct
import subprocess
import sys
import threading
import time
import uuid
import zlib

MAX_MESSAGE = 65536
DISPLAY_GEOMETRIES = frozenset(((192, 490), (212, 520), (336, 480), (432, 514)))
MAX_FRAME = 32 + 432 * 514 * 2
MAX_LOG = 16 * 1024 * 1024
CAPABILITIES = ["status", "start", "stop", "input", "screenshot", "logs",
                "inspect-package", "stage-package"]


def terminate_supervisor(process):
    """Terminate the owned session group after allowing child cleanup.

    launch.py checks every 50 ms and allows its child a full second to exit.
    Never kill only the supervisor while its child may still be alive.
    Every real supervisor was created with start_new_session=True.
    """
    if process.poll() is not None:
        return
    try:
        process.terminate()
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        if process.poll() is None:
            if os.getpgid(process.pid) != process.pid:
                raise RuntimeError("Owned supervisor is no longer its session leader")
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=2)
    except ProcessLookupError:
        process.wait(timeout=2)


class BridgeError(ValueError):
    def __init__(self, code, message):
        self.code = code
        super().__init__(message)


def quarantine(path: Path, value=None):
    """Darwin xattr API with NOFOLLOW; no authorization-bypass options.

    Apple's system Python 3.9 does not expose os.getxattr/listxattr. Use the
    system libc declaration from sys/xattr.h; an unreadable attribute fails shut.
    """
    if sys.platform != "darwin":
        return None
    library = ctypes.CDLL(None, use_errno=True)
    name = b"com.apple.quarantine"
    raw_path = os.fsencode(path)
    if value is not None:
        setter = library.setxattr
        setter.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_int]
        setter.restype = ctypes.c_int
        buffer = ctypes.create_string_buffer(value)
        if setter(raw_path, name, buffer, len(value), 0, 1) != 0:
            raise OSError(ctypes.get_errno(), "Cannot preserve quarantine")
    getter = library.getxattr
    getter.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_int]
    getter.restype = ctypes.c_ssize_t
    size = getter(raw_path, name, None, 0, 0, 1)
    if size < 0:
        if ctypes.get_errno() == 93:  # ENOATTR on Darwin; missing is distinct from denied.
            return None
        raise OSError(ctypes.get_errno(), "Cannot read quarantine")
    if size > 65536:
        raise BridgeError("invalid_file", "Quarantine attribute exceeds limit")
    buffer = ctypes.create_string_buffer(size)
    actual = getter(raw_path, name, buffer, size, 0, 1)
    if actual != size:
        raise OSError(ctypes.get_errno(), "Quarantine changed while reading")
    result = buffer.raw[:size]
    if value is not None and result != value:
        raise BridgeError("invalid_file", "Quarantine preservation failed")
    return result


def regular_bytes(path: Path, limit: int) -> bytes:
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_size > limit:
            raise BridgeError("invalid_file", "Expected a bounded regular file")
        with os.fdopen(fd, "rb", closefd=False) as stream:
            data = stream.read(limit + 1)
        if len(data) > limit:
            raise BridgeError("invalid_file", "File grew beyond limit")
        return data
    finally:
        os.close(fd)


def fingerprint(path: Path, limit: int) -> dict:
    data = regular_bytes(path, limit)
    return {"path": str(path.resolve()), "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest()}


def private_root(root: Path) -> Path:
    from .minative import _directory_fd, _path
    try:
        root = _path(root.absolute())
        descriptor = _directory_fd(root, private=True)
        os.close(descriptor)
    except (OSError, ValueError) as error:
        raise BridgeError("unsafe_session", "Session must be a real, private 0700 directory owned by this user") from error
    if "," in str(root) or len(os.fsencode(root / ("run-" + "0" * 12) / "qmp.sock")) > 103:
        raise BridgeError("unsafe_session", "Session path is too long for a Unix socket")
    return root


class SessionReservation:
    """One-use host-created root; no parent-directory read/write or cleanup.

    The nonce prevents accidental reuse and races between preparers. It is not
    an execution grant, persistent credential, or proof of active OS isolation.
    A claimed or failed preparation remains for diagnosis and cannot be reused.
    """
    NAME = ".reservation.json"
    CLAIM = ".reservation.claim"

    def __init__(self, root, nonce):
        from .minative import _canonical, _decode, _directory_fd
        self.root = private_root(Path(root))
        self.fd = _directory_fd(self.root, private=True)
        self.marker = self.claim = None
        try:
            if not isinstance(nonce, str) or re.fullmatch("[0-9a-f]{32}", nonce) is None:
                raise BridgeError("invalid_reservation", "Reservation nonce must contain 32 lowercase hexadecimal characters")
            with os.scandir(self.fd) as entries:
                names = []
                for entry in entries:
                    names.append(entry.name)
                    if len(names) > 1:
                        break
            if names != [self.NAME]:
                raise BridgeError("reservation_used", "Reserved root must contain only its original reservation record")
            data, self.marker = self._read(self.NAME)
            value = _decode(data)
            info = os.fstat(self.fd)
            if (not isinstance(value, dict) or set(value) != {"version", "nonce", "device", "inode"}
                    or type(value.get("version")) is not int or value["version"] != 1
                    or value.get("nonce") != nonce or type(value.get("device")) is not int
                    or type(value.get("inode")) is not int
                    or (value["device"], value["inode"]) != (info.st_dev, info.st_ino)):
                raise BridgeError("invalid_reservation", "Reservation nonce or root identity differs from the host-created directory")
            self.identity = (info.st_dev, info.st_ino)
            self.check()
            claim = os.open(self.CLAIM, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                            0o600, dir_fd=self.fd)
            try:
                payload = _canonical(value) + b"\n"
                with os.fdopen(claim, "wb", closefd=False) as stream:
                    stream.write(payload)
                    stream.flush()
                    os.fsync(claim)
                self.claim = self._stat_identity(os.fstat(claim))
            finally:
                os.close(claim)
            os.fsync(self.fd)
            self.check()
        except BaseException:
            self.close()
            raise

    @staticmethod
    def _stat_identity(info):
        return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)

    def _read(self, name):
        descriptor = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=self.fd)
        try:
            before = os.fstat(descriptor)
            if (not stat.S_ISREG(before.st_mode) or before.st_uid != os.getuid() or before.st_nlink != 1
                    or stat.S_IMODE(before.st_mode) != 0o600 or not 0 < before.st_size <= 1024):
                raise BridgeError("invalid_reservation", "Reservation record must be a private bounded regular file")
            payload = os.read(descriptor, 1025)
            after = os.fstat(descriptor)
            if len(payload) != before.st_size or self._stat_identity(before) != self._stat_identity(after):
                raise BridgeError("invalid_reservation", "Reservation record changed during verification")
            return payload, self._stat_identity(before)
        finally:
            os.close(descriptor)

    def check(self):
        from .minative import _directory_fd
        current = _directory_fd(self.root, private=True)
        try:
            info = os.fstat(current)
            if (info.st_dev, info.st_ino) != self.identity:
                raise BridgeError("reservation_changed", "The host-created reservation directory was replaced")
        finally:
            os.close(current)
        for name, expected in ((self.NAME, self.marker), (self.CLAIM, self.claim)):
            if expected is not None:
                _, actual = self._read(name)
                if actual != expected:
                    raise BridgeError("reservation_changed", "Reservation record was replaced or edited")

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def __enter__(self):
        return self

    def __exit__(self, *ignored):
        self.close()


def atomic_json(path: Path, value: dict):
    temporary = path.with_name("." + path.name + "." + uuid.uuid4().hex)
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    try:
        with os.fdopen(fd, "w") as stream:
            json.dump(value, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def create_session(root: Path, source: Path | None = None, core_paths: dict | None = None,
                   *, reservation_nonce: str | None = None) -> dict:
    if source and core_paths:
        raise BridgeError("invalid_inputs", "Choose source folder or explicit files, not both")
    inspected = None
    if core_paths:
        from .inputs import inspect_inputs
        inspected = inspect_inputs(core_paths)
    # A host reservation is claimed once; CLI callers may still create a new root.
    reservation = SessionReservation(root, reservation_nonce) if reservation_nonce is not None else None
    if reservation is None:
        root.mkdir(mode=0o700, parents=False, exist_ok=False)
    try:
        root = private_root(root)
        return _populate_session(root, source, inspected, reservation)
    finally:
        if reservation is not None:
            reservation.close()


def _populate_session(root, source, inspected, reservation):
    session = {"version": 1, "id": str(uuid.uuid4()), "directory": str(root), "socket": "control.sock",
               "framebuffer": "frame.raw", "width": 212, "height": 520,
               "capabilities": CAPABILITIES, "guestInstallSupported": False}
    if source:
        # Static bytes only. These pins identify inputs; they do not approve execution.
        session["core"] = {
            "qemu": fingerprint(source / "build/qemu-system-arm", 256 * 1024 * 1024),
            "firmware": fingerprint(source / "firmware/vela_ap.bin", 32 * 1024 * 1024),
            "resource": fingerprint(source / "firmware/vela_resource.bin", 256 * 1024 * 1024)}
    elif inspected:
        session["core"] = inspected
    session["application"] = "AzkiSimulator"
    session["protocol"] = "azki.simulator.session.v1"
    if reservation is not None:
        from .firmware_catalog import _atomic_json
        reservation.check()
        _atomic_json(root / "session.json", session, directory_fd=reservation.fd)
        reservation.check()
    else:
        atomic_json(root / "session.json", session)
    return session


def display_geometry(session):
    width, height = session.get("width"), session.get("height")
    if (type(width) is not int or type(height) is not int or
            (width, height) not in DISPLAY_GEOMETRIES):
        raise BridgeError("invalid_frame", "Unsupported exact device geometry")
    return width, height


def validate_frame(header: bytes, pixels: bytes, expected: tuple[int, int]):
    if len(header) != 32:
        raise BridgeError("frame_not_ready", "Waiting for a complete guest frame")
    magic, version, seq, width, height, stride, fmt = struct.unpack("<8s6I", header)
    if magic != b"Q66FRAME" or version != 1 or fmt != 1:
        raise BridgeError("invalid_frame", "Unsupported shared frame format")
    if ((width, height) != expected or expected not in DISPLAY_GEOMETRIES or
            stride != width * 2 or len(pixels) != stride * height):
        raise BridgeError("invalid_frame", "Frame geometry does not match this exact session")
    if seq == 0 or seq & 1:
        raise BridgeError("frame_not_ready", "Frame is being updated")
    return seq, width, height


def decode_frame(header: bytes, pixels: bytes, expected: tuple[int, int]):
    seq, width, height = validate_frame(header, pixels, expected)
    rgb = bytearray()
    for (word,) in struct.iter_unpack("<H", pixels):
        rgb.extend(((word >> 11) << 3, ((word >> 5) & 63) << 2, (word & 31) << 3))
    return seq, width, height, bytes(rgb)


def read_frame_raw(root: Path, expected: tuple[int, int] = (212, 520),
                   previous_sequence: int | None = None) -> tuple[int, int, int, bytes] | None:
    """Read stable little-endian RGB565 pixels without RGB/PNG conversion.

    A matching sequence skips only the pixel copy, after the same ownership,
    regular-file, size, format and exact-geometry checks as a complete read.
    Callers must scope previous_sequence to this session, not just a device ID.
    """
    if previous_sequence is not None and (type(previous_sequence) is not int or
                                         not 0 <= previous_sequence <= 0xffffffff):
        raise BridgeError("invalid_frame", "Previous frame sequence must be an unsigned 32-bit integer")
    path = root / "frame.raw"
    for _ in range(3):
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        try:
            info = os.fstat(fd)
            if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or
                    info.st_nlink != 1 or info.st_size > MAX_FRAME):
                raise BridgeError("invalid_frame", "Frame exceeds bounded display storage")
            header = os.pread(fd, 32, 0)
            if len(header) != 32:
                raise BridgeError("frame_not_ready", "Waiting for a complete guest frame")
            magic, version, seq, width, height, stride, fmt = struct.unpack("<8s6I", header)
            if magic != b"Q66FRAME" or version != 1 or fmt != 1:
                raise BridgeError("invalid_frame", "Unsupported shared frame format")
            if expected not in DISPLAY_GEOMETRIES or (width, height) != expected or stride != width * 2:
                raise BridgeError("invalid_frame", "Frame geometry does not match this exact session")
            if info.st_size != 32 + stride * height:
                raise BridgeError("invalid_frame", "Frame length does not match the declared geometry")
            if seq == 0 or seq & 1:
                continue
            if seq == previous_sequence:
                if os.pread(fd, 32, 0) != header:
                    continue
                return None
            pixels = os.pread(fd, stride * height, 32)
            if os.pread(fd, 32, 0) != header or len(pixels) != stride * height:
                continue
            return seq, width, height, pixels
        finally:
            os.close(fd)
    raise BridgeError("frame_not_ready", "Frame is being updated")


def read_frame(root: Path, expected: tuple[int, int] = (212, 520)) -> tuple[int, int, int, bytes]:
    """Compatibility RGB888 snapshot using the same validated raw reader."""
    seq, width, height, pixels = read_frame_raw(root, expected)
    header = struct.pack("<8s6I", b"Q66FRAME", 1, seq, width, height, width * 2, 1)
    return decode_frame(header, pixels, expected)


def png_bytes(width, height, rgb):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    scan = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">2I5B", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(scan)) + chunk(b"IEND", b""))


def receive_line(connection, maximum=MAX_MESSAGE, seconds=3):
    """A total deadline, not a per-byte idle timeout (resists slow senders)."""
    deadline = time.monotonic() + seconds
    data = bytearray()
    while len(data) <= maximum:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise BridgeError("timeout", "Message deadline exceeded")
        connection.settimeout(remaining)
        # One byte avoids swallowing a following QMP event/response. Messages
        # are tiny and bounded; no unbounded buffered readline is used.
        chunk = connection.recv(1)
        if not chunk:
            raise BridgeError("closed", "Connection closed before newline")
        data.extend(chunk)
        if chunk == b"\n":
            if len(data) > maximum:
                break
            return bytes(data)
    raise BridgeError("message_limit", "Message exceeds size limit")


class QMP:
    def __init__(self, path, expected_pid=None):
        self.sequence = 0
        self.closed = False
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(2)
        try:
            info = Path(path).lstat()
            if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid():
                raise BridgeError("qmp_error", "Expected this user's real QMP socket, not a link")
            self.socket.connect(str(path))
            if expected_pid is not None and sys.platform == "darwin":
                # Darwin sys/un.h LOCAL_PEERPID (SOL_LOCAL = 0).
                peer = struct.unpack("i", self.socket.getsockopt(0, 2, 4))[0]
                if peer != expected_pid:
                    raise BridgeError("qmp_error", "QMP peer is not the isolated process")
            if "QMP" not in self.receive():
                raise BridgeError("qmp_error", "Invalid QMP greeting")
            self.execute("qmp_capabilities")
        except BaseException:
            self.socket.close()
            raise

    def receive(self):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            line = receive_line(self.socket, seconds=max(.01, deadline - time.monotonic()))
            if not line.endswith(b"\n") or len(line) > MAX_MESSAGE:
                raise BridgeError("qmp_error", "Invalid or closed QMP response")
            response = json.loads(line)
            if not isinstance(response, dict):
                raise BridgeError("qmp_error", "Invalid QMP object")
            if "event" not in response:
                return response
        raise BridgeError("qmp_error", "QMP event budget exceeded")

    def execute(self, command, arguments=None):
        if self.closed:
            raise BridgeError("qmp_error", "Control channel is closed; restart the core")
        self.sequence += 1
        request = {"execute": command, "id": self.sequence}
        if arguments is not None:
            request["arguments"] = arguments
        try:
            self.socket.settimeout(2)
            self.socket.sendall(json.dumps(request).encode() + b"\n")
            response = self.receive()
            if response.get("id") != self.sequence or "error" in response or "return" not in response:
                raise BridgeError("qmp_error", "Guest control command failed or response id mismatched")
            return response["return"]
        except BaseException:
            self.close()
            raise

    def close(self):
        self.closed = True
        self.socket.close()


def sandbox_profile(root, files, frame_path=None, writable_files=None, allow_private_memfd=False):
    # No network, Bluetooth, USB, child process, arbitrary host write, or debugger.
    quoted = lambda p: json.dumps(str(p))
    read = " ".join("(literal " + quoted(p) + ")" for p in files)
    frame_rule = ('(allow file-read* file-write* (literal ' + quoted(frame_path) + '))') if frame_path else ''
    writes = ('(allow file-write* (subpath ' + quoted(root) + '))') if writable_files is None else (
        '(allow file-write* ' + ' '.join('(literal ' + quoted(path) + ')' for path in writable_files) + ')'
        if writable_files else '')
    # Explicit per-invocation opt-in only. QEMU's macOS memfd fallback uses
    # mkstemp then immediate unlink; no system-wide temporary directory grant.
    memfd_rule = ('(allow file-read* file-write* (regex ' + quoted(
        '^' + re.escape(str(root)) + '/memfd-' + '[A-Za-z0-9]' * 6 + '$') + '))') if allow_private_memfd else ''
    return ('(version 1)(deny default)(allow process-exec)(allow process-info*)'
            '(allow sysctl-read)(allow file-read-metadata)'
            '(allow file-read-data (literal "/dev/urandom") (literal "/dev/random"))'
            '(allow file-read* (literal "/") (subpath "/System/Library") (subpath "/usr/lib")'
            ' (subpath "/opt/homebrew/Cellar") (subpath "/opt/homebrew/opt") ' + read + ')'
            '(allow file-read* (subpath ' + quoted(root) + '))' + writes +
            '(allow network-bind network-inbound network-outbound (local unix-socket (subpath '
            + quoted(root) + ')))' + frame_rule + memfd_rule)


class SessionBridge:
    def __init__(self, root: Path, approved_hashes: dict | None = None, max_runtime: int = 0,
                 allow_private_memfd: bool = False):
        self.root = private_root(root)
        self.session = json.loads(regular_bytes(self.root / "session.json", MAX_MESSAGE))
        if (self.session.get("version") != 1 or self.session.get("socket") != "control.sock"
                or self.session.get("framebuffer") != "frame.raw"):
            raise BridgeError("unsafe_session", "Unsupported session descriptor")
        uuid.UUID(self.session["id"])
        self.width, self.height = display_geometry(self.session)
        self.approved_hashes = approved_hashes
        if type(max_runtime) is not int or not 0 <= max_runtime <= 28800:
            raise BridgeError("invalid_request", "Runtime must be integer 0 (unlimited) or 1…28800 seconds")
        self.max_runtime = max_runtime
        self.allow_private_memfd = allow_private_memfd
        self.started_at = None
        self.run_root = None
        self.process = None
        self.qmp = None
        self.last_exit = None
        self.last_error = None
        self.stop_requested = False
        self.pressed = False
        self.point = (0, 0)
        self.lock = threading.RLock()
        self.shutdown = threading.Event()
        from .hardware_inputs import TouchBatchExecutor
        self.touch_batches = TouchBatchExecutor()
        # A helper restart must not accept a batch from its previous process,
        # even when the explicitly reused session descriptor has the same ID.
        # This is an epoch, not an authentication token, and is not persisted.
        self.generation = int.from_bytes(os.urandom(6), "big")

    def status(self):
        from . import APPLICATION_VERSION, PROTOCOL_VERSION
        running = self.process is not None and self.process.poll() is None
        if self.process is not None and not running:
            self.last_exit = self.process.returncode
            if self.last_exit not in (None, 0) and not self.stop_requested:
                self.last_error = self.last_error or "core_exited"
        crashed = not running and self.last_exit not in (None, 0) and self.last_error is not None
        return {"state": "running" if running else "crashed" if crashed else "stopped", "backend": "qemu",
                "applicationVersion": APPLICATION_VERSION, "protocolVersion": PROTOCOL_VERSION,
                "pid": self.process.pid if running else None,
                "lastExit": self.last_exit, "error": self.last_error,
                "executionApproved": self.approved_hashes is not None,
                "maxRuntimeSeconds": self.max_runtime,
                "privateMemfdAllowed": self.allow_private_memfd,
                "hostProcessRunning": running, "bootVerified": False, "guestHealth": "unverified",
                "guestInstallSupported": False, "capabilities": CAPABILITIES,
                "generation": self.generation, "hardwareInputs": ["touch"],
                "id": self.session["id"]}

    def start(self):
        if self.session.get("backend", "qemu") != "qemu" or (self.width, self.height) != (212, 520):
            raise BridgeError("unsupported_backend", "Use the explicit multi-device backend adapter for this session")
        if self.status()["state"] == "running":
            return self.status()
        if self.approved_hashes is None:
            raise BridgeError("execution_not_approved", "Core execution awaits explicit approval of source and hashes")
        if sys.platform != "darwin" or not Path("/usr/bin/sandbox-exec").is_file():
            raise BridgeError("isolation_unavailable", "Required macOS process isolation is unavailable")
        if self.process is not None:
            self.stop()
        if len(list(self.root.glob("run-*"))) >= 8:
            raise BridgeError("quota", "Session run limit reached; preserve evidence and create a new session")
        core = self.session.get("core", {})
        paths = {}
        payloads = {}
        for role, maximum in (("qemu", 256 << 20), ("firmware", 32 << 20), ("resource", 256 << 20)):
            item = core.get(role, {})
            digest = self.approved_hashes.get(role)
            if not digest or digest != item.get("sha256"):
                raise BridgeError("hash_mismatch", "Exact QEMU and both firmware hashes must be approved")
            original = Path(item["path"])
            payload = regular_bytes(original, maximum)
            if hashlib.sha256(payload).hexdigest() != digest:
                raise BridgeError("hash_mismatch", "Core input changed since approval")
            payloads[role] = (original, payload)
        # Validate every input before writing a private run copy. Preserve quarantine.
        try:
            for role, (original, payload) in payloads.items():
                destination = self.root / ("core-" + role)
                fd = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o700 if role == "qemu" else 0o600)
                paths[role] = destination
                with os.fdopen(fd, "wb") as stream:
                    stream.write(payload)
                quarantine_value = quarantine(original)
                if quarantine_value is not None:
                    quarantine(destination, quarantine_value)
        except BaseException:
            for destination in paths.values():
                destination.unlink(missing_ok=True)
            raise
        flags = ("bt-peer", "bt-tx-capture", "bt-hci-reset-fixture", "panel-id-fixture",
                 "lcd-frame-capture", "panel-te-fixture", "spi0-unconnected", "gpu-probe",
                 "gpu-id-fixture", "gpu-software-renderer", "touch", "power-adc-fixture", "bt-hci-reject-fixture")
        machine = "best1503,revision-id=5," + ",".join(f"{flag}=on" for flag in flags)
        machine += ",resource-image=" + str(paths["resource"]).replace(",", ",,")
        machine += ",lcd-frame-file=" + str(self.root / "frame.raw").replace(",", ",,")
        self.run_root = self.root / ("run-" + uuid.uuid4().hex[:12])
        try:
            self.run_root.mkdir(mode=0o700)
        except BaseException:
            for destination in paths.values():
                destination.unlink(missing_ok=True)
            raise
        if len(os.fsencode(self.run_root / "qmp.sock")) > 103:
            self.stop()
            raise BridgeError("unsafe_session", "Run socket path is too long; use a shorter session path")
        outputs = [self.run_root / name for name in ("stderr.log", "qemu.log", "virtual-peer.log", "uart0.log", "uart1.log", "uart2.log", "qmp.sock")]
        args = [sys.executable, "-I", "-S", str(Path(__file__).with_name("launch.py")), str(self.max_runtime),
                str(self.root / "core-process.json"), str(os.getpid()),
                "/usr/bin/sandbox-exec", "-p", sandbox_profile(self.run_root, paths.values(), self.root / "frame.raw", outputs,
                                                            self.allow_private_memfd), str(paths["qemu"]),
                "-M", machine, "-display", "none", "-monitor", "none", "-no-reboot", "-nic", "none",
                "-kernel", str(paths["firmware"]), "-qmp", f"unix:{self.run_root}/qmp.sock,server=on,wait=off",
                "-d", "guest_errors", "-D", str(self.run_root / "qemu.log")]
        for index in range(3):
            args += ["-serial", f"file:{self.run_root}/uart{index}.log"]
        self.last_error = None
        self.stop_requested = False
        try:
            log_fd = os.open(self.run_root / "stderr.log", os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            with os.fdopen(log_fd, "wb") as stream:
                self.process = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=stream, stderr=stream,
                                                cwd=self.run_root, env={"PATH": "/usr/bin:/bin", "LANG": "C", "TMPDIR": str(self.run_root)},
                                                close_fds=True, start_new_session=True)
            self.started_at = time.monotonic()
            threading.Thread(target=self.watchdog, args=(self.process, self.run_root, self.started_at), daemon=True).start()
            deadline = time.monotonic() + 5
            while not (self.run_root / "qmp.sock").exists() or not (self.root / "core-process.json").exists():
                if self.process.poll() is not None or time.monotonic() >= deadline:
                    raise BridgeError("start_failed", "Core did not expose its isolated control socket; inspect logs")
                time.sleep(.02)
            identity = json.loads(regular_bytes(self.root / "core-process.json", 1024))
            if identity.get("supervisorPID") != self.process.pid or type(identity.get("pid")) is not int:
                raise BridgeError("start_failed", "Invalid isolated process identity")
            self.qmp = QMP(self.run_root / "qmp.sock", identity["pid"])
            self.qmp.execute("query-status")
            self.generation += 1
            self.touch_batches.new_generation(self.generation)
            return self.status()
        except BaseException:
            self.last_error = "start_failed"
            self.stop()
            raise

    def watchdog(self, process, run_root, started):
        """Never waits for the RPC lock or untrusted QMP response."""
        while process.poll() is None:
            reason = None
            if self.max_runtime and time.monotonic() - started >= self.max_runtime:
                reason = "runtime_limit_reached"
            try:
                for name in ("stderr.log", "qemu.log", "virtual-peer.log", "uart0.log", "uart1.log", "uart2.log"):
                    path = run_root / name
                    info = path.lstat() if path.exists() else None
                    if info and (not stat.S_ISREG(info.st_mode) or info.st_size > MAX_LOG):
                        reason = "log_limit_exceeded"
                        break
            except OSError:
                reason = "unsafe_run_output"
            if reason:
                if self.process is process:
                    self.last_error = reason
                terminate_supervisor(process)
                return
            time.sleep(.1)

    def input(self, arguments):
        x, y, down = arguments.get("x"), arguments.get("y"), arguments.get("down")
        if type(x) is not int or type(y) is not int or type(down) is not bool or not (0 <= x < self.width and 0 <= y < self.height):
            raise BridgeError("invalid_input", "Input needs integer pixels inside this session's display and boolean down")
        if self.status()["state"] != "running" or self.qmp is None:
            raise BridgeError("not_running", "Core is not running")
        events = [{"type": "abs", "data": {"axis": "x", "value": (x * 32767 + self.width - 2) // (self.width - 1)}},
                  {"type": "abs", "data": {"axis": "y", "value": (y * 32767 + self.height - 2) // (self.height - 1)}},
                  {"type": "btn", "data": {"button": "left", "down": down}}]
        self.qmp.execute("input-send-event", {"events": events})
        self.pressed, self.point = down, (x, y)
        return {"accepted": True}

    def stop(self):
        self.stop_requested = True
        if self.qmp:
            try:
                if self.pressed:
                    self.input({"x": self.point[0], "y": self.point[1], "down": False})
                self.qmp.execute("quit")
            except (OSError, ValueError, KeyError):
                pass
            finally:
                try:
                    self.qmp.close()
                except OSError:
                    pass
                self.qmp = None
        if self.process and self.process.poll() is None:
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                terminate_supervisor(self.process)
        self.pressed = False
        # Only fixed files owned by this session are eligible for replacement.
        for name in ("core-qemu", "core-firmware", "core-resource", "core-process.json", "frame.raw"):
            (self.root / name).unlink(missing_ok=True)
        return self.status()

    def pmu_adc_get(self, arguments):
        raise BridgeError("unsupported_hardware", "This backend has no verified PMU ADC input")

    def pmu_adc_set(self, arguments):
        raise BridgeError("unsupported_hardware", "This backend has no verified PMU ADC input")

    def live_get(self, arguments):
        raise BridgeError("unsupported_hardware", "This backend has no live board inputs")

    def live_set(self, arguments):
        raise BridgeError("unsupported_hardware", "This backend has no live board inputs")

    def dispatch(self, request):
        with self.lock:
            if not isinstance(request, dict) or request.get("version") != 1 or request.get("id") != self.session["id"]:
                raise BridgeError("invalid_request", "Session version/id mismatch")
            command, args = request.get("command"), request.get("arguments", {})
            if not isinstance(args, dict):
                raise BridgeError("invalid_request", "arguments must be an object")
            if command == "status":
                return self.status()
            if command == "start":
                return self.start()
            if command == "stop":
                return self.stop()
            if command in ("guest-status", "guest-connect", "guest-install", "guest-operation", "guest-inventory", "guest-select",
                           "external-open", "external-status", "external-request", "external-result", "external-close", "external-reconnect"):
                from .virtual_peer import execute
                return execute(getattr(self, "adapter", None), command, args)
            if command in ("pmu-adc-get", "pmu-adc-set"):
                from .adc_commands import validate_arguments
                validate_arguments(args, write=command == "pmu-adc-set")
                return self.pmu_adc_get(args) if command == "pmu-adc-get" else self.pmu_adc_set(args)
            if command in ("live-get", "live-set"):
                return self.live_get(args) if command == "live-get" else self.live_set(args)
            if command == "device-button":
                from .device_buttons import click_button
                return click_button(getattr(self, "adapter", None), args)
            if command == "input":
                if "batch" in args:
                    if set(args) != {"batch"}:
                        raise BridgeError("invalid_input", "Choose a single contact event or one explicit batch")
                    if self.status()["state"] != "running":
                        raise BridgeError("not_running", "Core is not running")
                    return self.touch_batches.execute(args["batch"], geometry=(self.width, self.height),
                        send=self.input, cancelled=self.shutdown.is_set)
                return self.input(args)
            if command in ("install", "uninstall"):
                raise BridgeError("unsupported", "The downloaded core has no verified guest installation channel")
            if command == "screenshot":
                if len(list(self.root.glob("screenshot-*.png"))) >= 64:
                    raise BridgeError("quota", "Session screenshot limit reached; start a new session or archive captures")
                seq, width, height, rgb = read_frame(self.root, (self.width, self.height))
                name = "screenshot-" + uuid.uuid4().hex + ".png"
                data = png_bytes(width, height, rgb)
                fd = os.open(self.root / name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
                with os.fdopen(fd, "wb") as stream:
                    stream.write(data)
                return {"file": name, "sequence": seq, "width": width, "height": height,
                        "sha256": hashlib.sha256(data).hexdigest()}
            if command == "logs":
                if args.get("stream") is True:
                    from .log_stream import read_chunk
                    return read_chunk(self.run_root, args)
                name = args.get("name", "stderr.log")
                if name not in ("stderr.log", "qemu.log", "virtual-peer.log", "uart0.log", "uart1.log", "uart2.log"):
                    raise BridgeError("invalid_request", "Unknown log")
                path = self.run_root / name if self.run_root else None
                if path is None or not path.exists():
                    return {"name": name, "text": ""}
                fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
                try:
                    info = os.fstat(fd)
                    if not stat.S_ISREG(info.st_mode):
                        raise BridgeError("unsafe_log", "Expected a regular session log")
                    data = os.pread(fd, 8192, max(0, info.st_size - 8192))
                finally:
                    os.close(fd)
                return {"name": name, "text": data.decode("utf-8", errors="replace"),
                        "truncated": info.st_size > 8192}
            if command in ("inspect-package", "stage-package"):
                from .packages import inspect_package, stage_package
                path = args.get("path")
                if not isinstance(path, str) or not Path(path).is_absolute():
                    raise BridgeError("invalid_request", "Package requires an explicit absolute path")
                report = inspect_package(Path(path)) if command == "inspect-package" else stage_package(Path(path), self.root)
                entries = report.get("entries", [])
                return {**report, "entries": entries[:16], "entryCount": len(entries), "entriesTruncated": len(entries) > 16}
            raise BridgeError("unsupported", "Unknown command")

    def serve(self, *, discovery=True):
        if type(discovery) is not bool:
            raise BridgeError("invalid_request", "Discovery must be an explicit boolean")
        if discovery:
            from .discovery import PROTOCOL, publish, withdraw
        path = self.root / "control.sock"
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        # No stale socket deletion: a second bridge cannot take another's session.
        try:
            server.bind(str(path))
        except BaseException:
            server.close()
            raise
        os.chmod(path, 0o600)
        server.listen(4)
        server.settimeout(.25)
        advertisement = None
        try:
            if discovery and self.session.get("protocol") == PROTOCOL:
                advertisement = publish(self.root, self.session)
            while not self.shutdown.is_set():
                if self.process and self.process.poll() not in (None, 0):
                    self.last_error = self.last_error or "core_exited"
                try:
                    client, _ = server.accept()
                except socket.timeout:
                    continue
                with client:
                    client.settimeout(2)
                    try:
                        line = receive_line(client)
                        if len(line) > MAX_MESSAGE or not line.endswith(b"\n"):
                            raise BridgeError("invalid_request", "Request exceeds size limit or has no newline")
                        result = {"ok": True, "result": self.dispatch(json.loads(line))}
                    except (OSError, ValueError, KeyError, TypeError, RecursionError) as error:
                        result = {"ok": False, "error": {"code": getattr(error, "code", "request_failed"), "message": str(error)[:1024]}}
                    try:
                        output = json.dumps(result, ensure_ascii=False).encode() + b"\n"
                        if len(output) > MAX_MESSAGE:
                            output = b'{"ok":false,"error":{"code":"response_limit","message":"Response exceeds protocol limit"}}\n'
                        client.sendall(output)
                    except OSError:
                        pass
        finally:
            try:
                self.stop()
            finally:
                server.close()
                (self.root / "control.sock").unlink(missing_ok=True)
                if advertisement is not None:
                    withdraw(advertisement)


def request(root: Path, command: str, arguments: dict | None = None):
    root = private_root(root)
    descriptor = json.loads(regular_bytes(root / "session.json", MAX_MESSAGE))
    message = {"version": 1, "id": descriptor["id"], "command": command, "arguments": arguments or {}}
    payload = json.dumps(message).encode() + b"\n"
    if len(payload) > MAX_MESSAGE:
        raise BridgeError("invalid_request", "Request exceeds size limit")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(10)
        connection.connect(str(root / "control.sock"))
        connection.sendall(payload)
        line = receive_line(connection, seconds=20 if command in ("start", "stop") else 10)
        if len(line) > MAX_MESSAGE or not line.endswith(b"\n"):
            raise BridgeError("invalid_response", "Invalid bridge response")
        return json.loads(line)
