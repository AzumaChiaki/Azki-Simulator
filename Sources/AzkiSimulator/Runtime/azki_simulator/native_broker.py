"""One-shot native-owned guest lifecycle. No execution, signals or caller argv.

The native app is the authority: it retains the approved immutable plan and its
own Process/group in memory. Files here only associate one bounded request and
reflect its status. They are not proof of active OS sandboxing or credentials.
"""
import ctypes
import errno
import hashlib
import os
from pathlib import Path
import re
import signal
import stat
import subprocess
import sys
import time
import uuid

from .minative import (INCLUDED_PROFILE_IDS, MAX_RUN_SECONDS, MError, _canonical, _decode,
                       _directory_fd, _open_file, _path)

LIMIT = 4096


def _record(path):
    fd, before = _open_file(path, LIMIT, private=True)
    try:
        data = os.read(fd, LIMIT + 1)
        after = os.fstat(fd)
        identity = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns)
        if len(data) != before.st_size or identity(before) != identity(after):
            raise MError("native_broker_changed", "Native broker record changed during read")
        return _decode(data), (*identity(before), hashlib.sha256(data).hexdigest())
    finally:
        os.close(fd)


def _publish(directory, name, value):
    """Atomic exclusive publication; no partial or replaced request is visible."""
    temp = ".request-" + uuid.uuid4().hex
    fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=directory)
    own = os.fstat(fd)
    try:
        with os.fdopen(fd, "wb", closefd=False) as output:
            output.write(_canonical(value) + b"\n"); output.flush(); os.fsync(fd)
        libc = ctypes.CDLL(None, use_errno=True)
        call = getattr(libc, "renameatx_np" if sys.platform == "darwin" else "renameat2", None)
        if call is None:
            raise MError("native_broker_unavailable", "Exclusive request publication is unavailable")
        call.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
        call.restype = ctypes.c_int
        if call(directory, os.fsencode(temp), directory, os.fsencode(name), 4 if sys.platform == "darwin" else 1):
            error = ctypes.get_errno()
            raise OSError(error, "Native request already exists" if error == errno.EEXIST else "Native request publication failed")
        os.fsync(directory)
    finally:
        os.close(fd)
        try:
            current = os.stat(temp, dir_fd=directory, follow_symlinks=False)
            if (current.st_dev, current.st_ino) == (own.st_dev, own.st_ino):
                os.unlink(temp, dir_fd=directory)
        except FileNotFoundError:
            pass


class NativeBrokerSession:
    def __init__(self, root, plan, seconds):
        self.root = _path(root)
        self.directory = self.root / "native-broker"
        fd = _directory_fd(self.directory, private=True)
        try:
            info = os.fstat(fd)
            self.directory_identity = (info.st_dev, info.st_ino)
        finally:
            os.close(fd)
        self.config, self.config_identity = _record(self.directory / "config.json")
        config = self.config
        if (not isinstance(config, dict) or set(config) != {"version", "id", "run", "approvalDigest", "maxRuntimeSeconds"}
                or type(config.get("version")) is not int or config["version"] != 1
                or not isinstance(config.get("id"), str)
                or not isinstance(config.get("run"), str) or re.fullmatch("m-[0-9a-f]{12}", config["run"]) is None
                or config.get("approvalDigest") != plan.get("approvalDigest")
                or type(config.get("maxRuntimeSeconds")) is not int
                or type(seconds) is not int or not 0 <= config["maxRuntimeSeconds"] <= MAX_RUN_SECONDS
                or config["maxRuntimeSeconds"] != seconds):
            raise MError("native_broker_mismatch", "Native broker differs from the exact approved session and runtime mode")
        try:
            if str(uuid.UUID(config["id"])) != config["id"]:
                raise ValueError()
        except (ValueError, AttributeError) as error:
            raise MError("native_broker_mismatch", "Native broker request identity must be a canonical UUID") from error
        if plan.get("profileID") not in INCLUDED_PROFILE_IDS or plan.get("identity", {}).get("storageKind") not in ("romfs", "yaffs"):
            raise MError("isolation_unavailable", "Native broker requires an included Xiaomi Smart Band profile")
        self.run = self.root / config["run"]
        self.run_identity = self._directory_identity(self.run)
        self.tmp_identity = self._directory_identity(self.run / "tmp")

    @staticmethod
    def _directory_identity(path):
        fd = _directory_fd(path, private=True)
        try:
            info = os.fstat(fd)
            return info.st_dev, info.st_ino
        finally:
            os.close(fd)

    def check(self):
        if (self._directory_identity(self.directory) != self.directory_identity
                or self._directory_identity(self.run) != self.run_identity
                or self._directory_identity(self.run / "tmp") != self.tmp_identity
                or _record(self.directory / "config.json")[1] != self.config_identity):
            raise MError("native_broker_changed", "Native reservation or configuration was replaced")

    def _request(self, name, value):
        self.check()
        fd = _directory_fd(self.directory, private=True)
        try:
            _publish(fd, name, value)
        finally:
            os.close(fd)
        self.check()

    def launch(self, timeout=5):
        self.check()
        for path, expected in ((self.run, {"tmp"}), (self.run / "tmp", set())):
            fd = _directory_fd(path, private=True)
            try:
                with os.scandir(fd) as entries:
                    found = set()
                    for entry in entries:
                        found.add(entry.name)
                        if len(found) > 1:
                            break
                if found != expected:
                    raise MError("native_broker_used", "Native guest run was already used or contains unexpected files")
            finally:
                os.close(fd)
        value = {key: self.config[key] for key in ("version", "id", "run")}
        self._request("launch.json", value)
        process = NativeBrokerProcess(self)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            process.poll()
            if process.protocol_error:
                process.terminate()
                raise MError("native_broker_protocol", process.protocol_error)
            if process.pid is not None or process.returncode is not None:
                return process
            time.sleep(.025)
        process.terminate()
        raise MError("native_broker_timeout", "Native supervisor did not acknowledge the one-shot launch")


class NativeBrokerProcess:
    """Popen-shaped status proxy. It never executes, signals, or reaps a PID."""
    native_owned = True

    def __init__(self, session):
        self.session = session
        self.pid = self.returncode = None
        self.protocol_error = self.native_error = None
        self.cleanup = None
        self._last_state = self._terminal = None
        self._stop_sent = False

    def _state(self):
        self.session.check()
        value, _ = _record(self.session.directory / "state.json")
        base = {"version", "id", "run", "state"}
        if (not isinstance(value, dict) or type(value.get("version")) is not int or value["version"] != 1
                or value.get("id") != self.session.config["id"] or value.get("run") != self.session.config["run"]):
            raise MError("native_broker_protocol", "Native status does not match this launch")
        state = value.get("state")
        if state == "running":
            if set(value) != base | {"pid"} or type(value["pid"]) is not int or not 2 <= value["pid"] <= 2**31 - 1:
                raise MError("native_broker_protocol", "Invalid native running status")
            if self.pid is not None and self.pid != value["pid"]:
                raise MError("native_broker_protocol", "Native supervisor PID changed")
        elif state in ("exited", "failed"):
            if (not base | {"exitCode", "cleanup"} <= value.keys()
                    or set(value) - (base | {"exitCode", "cleanup", "pid", "error"})
                    or type(value.get("exitCode")) is not int or not -255 <= value["exitCode"] <= 255
                    or value.get("cleanup") not in ("reaped", "unconfirmed")
                    or ("pid" in value and (type(value["pid"]) is not int or not 2 <= value["pid"] <= 2**31 - 1))
                    or ("error" in value and (not isinstance(value["error"], str) or len(value["error"].encode()) > 1024))
                    or (self.pid is not None and value.get("pid") != self.pid)):
                raise MError("native_broker_protocol", "Invalid native terminal status")
        else:
            raise MError("native_broker_protocol", "Unknown native lifecycle state")
        if self._terminal is not None and value != self._terminal:
            raise MError("native_broker_protocol", "Native terminal status changed or regressed")
        if state != "running":
            self._terminal = value
        return value

    def poll(self):
        try:
            value = self._state()
        except FileNotFoundError:
            if self._last_state is not None:
                self.protocol_error = "Native status disappeared; guest exit is unconfirmed"
            return None
        except (OSError, ValueError) as error:
            self.protocol_error = str(error)[:1024]
            return None
        if self.protocol_error is not None:
            return None  # Never recover silently from an invalid/replayed status.
        self._last_state = value["state"]
        self.pid = value.get("pid", self.pid)
        if value["state"] != "running":
            self.returncode = value["exitCode"]
            self.cleanup = value["cleanup"]
            self.native_error = value.get("error")
        return self.returncode

    def wait(self, timeout=None):
        if timeout is None or not 0 <= timeout <= 15:
            raise MError("native_broker_timeout", "Native waits must be explicitly bounded to at most fifteen seconds")
        end = time.monotonic() + timeout
        while True:
            result = self.poll()
            if self.protocol_error:
                raise MError("native_broker_protocol", self.protocol_error)
            if result is not None:
                return result
            if time.monotonic() >= end:
                raise subprocess.TimeoutExpired("native-owned-guest", timeout)
            time.sleep(.025)

    def terminate(self):
        value = {key: self.session.config[key] for key in ("version", "id")}
        try:
            self.session._request("stop.json", value)
        except FileExistsError:
            actual, _ = _record(self.session.directory / "stop.json")
            if actual != value:
                raise MError("native_broker_changed", "A conflicting stop request already exists")
        self._stop_sent = True

    def send_signal(self, sig):
        if sig not in (signal.SIGINT, signal.SIGTERM, signal.SIGKILL):
            raise MError("native_broker_protocol", "Native guest accepts only the bounded stop operation")
        self.terminate()

    def kill(self):
        self.terminate()

    def confirm_cleanup(self, timeout=13):
        if self.poll() is not None and self.cleanup == "reaped" and not self.protocol_error:
            return True
        try:
            self.terminate()
            self.wait(timeout=timeout)
        except (OSError, ValueError, subprocess.TimeoutExpired):
            return False
        return self.cleanup == "reaped" and self.protocol_error is None
