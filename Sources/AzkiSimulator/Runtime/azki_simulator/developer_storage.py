"""Host-issued completion evidence for the developer sandbox's owned run.

The receipt lives beside the run, outside the guest's writable sandbox. Only
the adapter that retained Popen and authenticated the runner/control endpoints
publishes it, after reaping the entire process group. Storage commit still
performs the normal guest health and disk-binding checks.
"""
import hashlib
import ctypes
import os
from pathlib import Path
import re
import secrets
import sys

from .minative import _canonical, _directory_fd, _group_members, _json, _read

FILENAME = "developer-run.json"
FIELDS = {"version", "mode", "approvalDigest", "sessionID", "transactionID", "run",
          "runDevice", "runInode", "supervisorPID", "runnerPID", "corePID",
          "exitCode", "cleanup", "state", "reason", "resultSHA256"}


def _publish_no_replace(directory, temporary):
    # Atomic no-replace rename: Darwin renameatx_np(RENAME_EXCL); Linux
    # renameat2(RENAME_NOREPLACE). Unlike hard-link publication, it never
    # leaves two links on a crash.
    darwin = sys.platform == "darwin"
    function = "renameatx_np" if darwin else "renameat2"
    if not darwin and not sys.platform.startswith("linux"):
        raise OSError(None, "Atomic no-replace rename is unavailable", FILENAME)
    library = ctypes.CDLL(None, use_errno=True)
    rename = getattr(library, function, None)
    if rename is None:
        raise OSError(None, "Atomic no-replace rename is unavailable", FILENAME)
    rename.argtypes = (ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint)
    rename.restype = ctypes.c_int
    if rename(directory, os.fsencode(temporary), directory, os.fsencode(FILENAME), 0x4 if darwin else 1):
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), FILENAME)


def _dead(pid):
    if type(pid) is not int or pid < 2:
        raise ValueError("Invalid developer process identity")
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return
    # PermissionError is not evidence of exit.
    raise ValueError("Developer process is still alive; snapshot left unchanged")


def _run_identity(session, run):
    if type(run) is not str or re.fullmatch(r"m-[0-9a-f]{12}", run) is None:
        raise ValueError("Developer run must be a direct private session child")
    fd = _directory_fd(session / run, private=True)
    try:
        info = os.fstat(fd)
        return info.st_dev, info.st_ino
    finally:
        os.close(fd)


def read_completion(session, plan, binding):
    """Validate host evidence and independently reject live/reused process IDs."""
    session = Path(session)
    descriptor = _json(session / "session.json", private=True)
    value = _json(session / FILENAME, private=True)
    if (type(value) is not dict or set(value) != FIELDS
            or type(value["version"]) is not int or value["version"] != 1
            or value["mode"] != "developer-inner-sandbox"
            or value["approvalDigest"] != plan.get("approvalDigest")
            or value["sessionID"] != descriptor.get("id")
            or value["transactionID"] != binding.get("transactionID")
            or descriptor.get("approvalDigest") != plan.get("approvalDigest")
            or value["cleanup"] != "reaped" or type(value["exitCode"]) is not int
            or value["state"] not in ("exited", "failed")
            or type(value["reason"]) is not str or len(value["reason"]) > 512):
        raise ValueError("Developer completion differs from the exact prepared transaction")
    identity = _run_identity(session, value["run"])
    if (type(value["runDevice"]) is not int or type(value["runInode"]) is not int
            or identity != (value["runDevice"], value["runInode"])):
        raise ValueError("Developer run directory was replaced")
    for key in ("supervisorPID", "runnerPID", "corePID"):
        _dead(value[key])
    if _group_members(value["supervisorPID"]):
        raise ValueError("Developer process group cleanup is not confirmed")
    metadata = _json(session / value["run"] / "runner-process.json", private=True)
    if metadata != {"pid": value["runnerPID"], "supervisorPID": value["supervisorPID"]}:
        raise ValueError("Developer runner identity differs from the observed process")
    result = _read(session / value["run"] / "result.json", private=True)
    if hashlib.sha256(result).hexdigest() != value["resultSHA256"]:
        raise ValueError("Developer result changed after the supervisor reaped the run")
    return value


def publish_completion(adapter):
    """Called only by the trusted adapter, never a socket command or caller JSON."""
    binding = adapter.plan["identity"].get("instanceStorage")
    observed = getattr(adapter, "_developer_process_identity", None)
    if (not binding or adapter.native_broker is not None or adapter.fixture_only
            or not adapter.isolation_verified or not adapter.execution_prefix
            or observed is None or adapter.process is None):
        return False
    if adapter.process.poll() is None or adapter._cleanup != "reaped":
        raise ValueError("Developer child and its process group have not been reaped")
    run = adapter.run.name
    if adapter.run.parent != adapter.root:
        raise ValueError("Developer run escaped its private session")
    device, inode = _run_identity(adapter.root, run)
    descriptor = _json(adapter.root / "session.json", private=True)
    value = {"version": 1, "mode": "developer-inner-sandbox",
             "approvalDigest": adapter.plan["approvalDigest"], "sessionID": descriptor["id"],
             "transactionID": binding["transactionID"], "run": run, "runDevice": device, "runInode": inode,
             "supervisorPID": adapter.process.pid, "runnerPID": observed["runnerPID"],
             "corePID": observed["corePID"], "exitCode": adapter.process.returncode,
             "cleanup": "reaped", "state": "failed" if adapter._state == "failed" else "exited",
             "reason": str(adapter._reason or "")[:512],
             "resultSHA256": hashlib.sha256(_read(adapter.run / "result.json", private=True)).hexdigest()}
    for key in ("supervisorPID", "runnerPID", "corePID"):
        _dead(value[key])
    if _group_members(value["supervisorPID"]):
        raise ValueError("Developer descendants remain alive")
    # Exclusive host publication. Guest policy permits writing run/ and
    # instance-data/ only, never this session-level completion receipt.
    directory = _directory_fd(adapter.root, private=True)
    temporary = ".developer-completion-" + secrets.token_hex(16)
    created = False
    try:
        fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                     0o600, dir_fd=directory)
        created = True
        with os.fdopen(fd, "wb") as output:
            output.write(_canonical(value) + b"\n")
            output.flush()
            os.fsync(output.fileno())
        _publish_no_replace(directory, temporary)
        created = False
        os.fsync(directory)
    finally:
        try:
            if created:
                try:
                    os.unlink(temporary, dir_fd=directory)
                except FileNotFoundError:
                    pass  # Atomic publication succeeded before interruption.
                os.fsync(directory)
        finally:
            os.close(directory)
    read_completion(adapter.root, adapter.plan, binding)
    return True
