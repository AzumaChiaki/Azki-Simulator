"""Fixed macOS runner supervisor, invoked only by the local adapter launcher.

The sandbox encloses the supplied runner and its QEMU child. This process owns
one process group, monitors its parent, and never leaves a surviving descendant
in that group after the bounded stop grace. No launch agent is installed.
"""
import ctypes
import json
import os
from pathlib import Path
import resource
import re
import select
import signal
import subprocess
import sys
import time


MAX_RUN_SECONDS = 28800
# Exact membership is verified by the native parent against the sealed plan; this only bounds the shape.
PROFILE_ID = r"[A-Za-z0-9._@-]{1,96}"
# Generated storage caches (YAFFS seed up to ~400 MiB) are written by the guest tree.
FSIZE_LIMIT = 1 << 30
# QEMU is multi-threaded: CPU seconds are consumed faster than wall seconds.
CPU_FACTOR = 8
# Independent of session duration. Normal stop must collect bounded health
# evidence, reap QEMU and publish the storage receipt before forced cleanup.
STOP_GRACE_SECONDS = 8


def publish_metadata(path, child_pid):
    path = Path(path)
    temporary = path.with_name(".runner-process-" + str(os.getpid()))
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    try:
        with os.fdopen(fd, "w") as stream:
            json.dump({"pid": child_pid, "supervisorPID": os.getpid()}, stream)
            stream.flush()
            os.fsync(stream.fileno())
        # Atomic publication without replacing another session's metadata.
        os.link(temporary, path, follow_symlinks=False)
    finally:
        temporary.unlink(missing_ok=True)


def group_members():
    library = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
    function = library.proc_listpids
    function.argtypes = (ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_int)
    function.restype = ctypes.c_int
    buffer = (ctypes.c_int * 512)()
    count = function(2, os.getpid(), buffer, ctypes.sizeof(buffer))  # PROC_PGRP_ONLY
    if count <= 0 or count >= ctypes.sizeof(buffer) or count % ctypes.sizeof(ctypes.c_int):
        raise RuntimeError("Could not verify the bounded owned process group")
    return [pid for pid in buffer[:count // ctypes.sizeof(ctypes.c_int)] if pid > 0 and pid != os.getpid()]


def terminate_group(child):
    # First let the runner collect result.json and close its sole QMP channel.
    if child.poll() is None:
        try:
            child.send_signal(signal.SIGINT)
        except ProcessLookupError:
            pass
    deadline = time.monotonic() + STOP_GRACE_SECONDS
    while time.monotonic() < deadline:
        child.poll()
        try:
            if not group_members():
                return
        except RuntimeError:
            break
        time.sleep(.025)
    # This also signals us; our installed handler is deliberately non-exiting.
    os.killpg(os.getpid(), signal.SIGTERM)
    deadline = time.monotonic() + .5
    while time.monotonic() < deadline:
        child.poll()
        try:
            if not group_members():
                return
        except RuntimeError:
            break
        time.sleep(.025)
    # Do not exit first and orphan a blocked child. The embedding helper reaps
    # this supervisor; SIGKILL covers every process still in our owned group.
    os.killpg(os.getpid(), signal.SIGKILL)


def native_invocation(arguments):
    """Only the native parent constructs these eight fixed positional values."""
    # Optional user-requested debugger, appended last by the native parent:
    # --gdb PORT [--gdb-wait]. PORT 0 lets QEMU pick a free loopback port.
    debugger = []
    if arguments and arguments[-1] == "--gdb-wait":
        debugger, arguments = ["--gdb-wait"], arguments[:-1]
    if len(arguments) >= 2 and arguments[-2] == "--gdb":
        if re.fullmatch(r"0|[1-9][0-9]{0,4}", arguments[-1]) is None or int(arguments[-1]) > 65535:
            raise ValueError("Invalid native debugger port")
        debugger, arguments = ["--gdb", arguments[-1]] + debugger, arguments[:-2]
    elif debugger:
        raise ValueError("Debugger wait requires a debugger port")
    oobe = arguments[-1:] if arguments and arguments[-1] in ("--skip-oobe", "--no-skip-oobe") else []
    if oobe:
        arguments = arguments[:-1]
    settings = arguments[-2:] if len(arguments) >= 2 and arguments[-2] == "--instance-settings" else []
    if settings:
        arguments = arguments[:-2]
    if len(arguments) not in (8, 16, 20, 21, 26, 27) or arguments[0] != "--native":
        raise ValueError("Invalid native guest invocation")
    virtual = arguments[-6:] if len(arguments) in (26, 27) else []
    core_arguments = arguments[:-6] if virtual else arguments
    _, raw_duration, raw_metadata, raw_parent, raw_engine, raw_project, profile, raw_run = core_arguments[:8]
    if type(raw_duration) is not str or re.fullmatch(r"0|[1-9][0-9]{0,4}", raw_duration) is None:
        raise ValueError("Invalid native runtime mode")
    duration, parent = int(raw_duration), int(raw_parent)
    engine, project, run, metadata = map(Path, (raw_engine, raw_project, raw_run, raw_metadata))
    if (not 0 <= duration <= MAX_RUN_SECONDS or parent < 2 or re.fullmatch(PROFILE_ID, profile) is None
            or not all(path.is_absolute() and ".." not in path.parts for path in (engine, project, run, metadata))
            or re.fullmatch("m-[0-9a-f]{12}", run.name) is None
            or metadata != run / "runner-process.json"
            or project != run.parent.parent / "prepared/project"):
        raise ValueError("Native guest paths or fixed profile differ")
    # All foreign code starts below the one native-installed G sandbox. These
    # fixed paths are additionally verified/pinned by the native parent.
    command = [str(engine / "python/bin/python3.12"), "-I", "-S", "-B", str(engine / "bootstrap.py"),
               "--project", str(project), "--runner", "multi_device.py", "--", "--device", profile,
               "--headless", "--log-dir", str(run)]
    if duration:
        command += ["--seconds", str(duration)]
    if len(core_arguments) >= 16:
        extra = core_arguments[8:16]
        if (extra[::2] != ["--instance-data-image", "--instance-spare-image",
                          "--instance-data-sha256", "--instance-spare-sha256"]
                or extra[1] != str(run.parent / "instance-data/nand-data.bin")
                or extra[3] != str(run.parent / "instance-data/nand-spare.bin")
                or not all(re.fullmatch(r"[0-9a-f]{64}", value) for value in (extra[5], extra[7]))):
            raise ValueError("Native instance disk arguments differ from private session files")
        command += extra
        if len(core_arguments) > 16:
            nor = core_arguments[16:]
            if (nor[:1] != ["--instance-nor-image"]
                    or nor[1] != str(run.parent / "instance-data/nor-config.bin")
                    or nor[2] != "--instance-nor-sha256"
                    or re.fullmatch(r"[0-9a-f]{64}", nor[3]) is None
                    or (len(nor) == 5 and nor[4] != "--instance-nor-initialize")):
                raise ValueError("Native NOR arguments differ from the exact private profile file")
            command += nor
    if virtual:
        if (len(core_arguments) not in (20, 21)
                or virtual[::2] != ["--virtual-peer-state", "--virtual-peer-state-sha256", "--virtual-peer-output"]
                or virtual[1] != str(run.parent / "instance-data/virtual-peer-state.json")
                or re.fullmatch(r"[0-9a-f]{64}", virtual[3]) is None
                or virtual[5] != str(run / "virtual-peer-state.pending.json")):
            raise ValueError("Native virtual peer arguments differ from the private instance and owned run")
        command += virtual
    if settings:
        if len(core_arguments) < 16 or settings[1] != str(run.parent / "instance-data/instance-settings.json"):
            raise ValueError("Native instance settings differ from the private session file")
        command += settings
    return duration, metadata, parent, command + oobe + debugger


def await_native_go(parent, stopped, *, descriptor=0, timeout=2):
    """Native alone owns this inherited pipe; H has no group-start authority."""
    expected = b"AZKI-GUEST-GO\n"
    deadline, data = time.monotonic() + min(timeout, 2), b""
    while time.monotonic() < deadline:
        if stopped() or os.getppid() != parent:
            raise RuntimeError("Native parent exited or cancelled before guest start")
        ready, _, _ = select.select([descriptor], [], [], min(.05, max(0, deadline - time.monotonic())))
        if ready:
            chunk = os.read(descriptor, 32 - len(data))
            if not chunk:
                raise RuntimeError("Native group-start pipe closed")
            data += chunk
            if not expected.startswith(data):
                raise RuntimeError("Invalid native group-start message")
            if data == expected:
                return
    raise RuntimeError("Native group-start acknowledgement timed out")


def main():
    native = len(sys.argv) > 1 and sys.argv[1] == "--native"
    if native:
        duration, metadata, parent, command = native_invocation(sys.argv[1:])
        if sys.platform != "darwin" or os.getppid() != parent:
            raise SystemExit("Invalid native parent")
        if os.getpgrp() != os.getpid():
            os.setsid()
        if os.getpgrp() != os.getpid():
            raise SystemExit("Native guest failed to establish its owned process group")
    else:
        duration, metadata, parent = int(sys.argv[1]), Path(sys.argv[2]), int(sys.argv[3])
        if (sys.platform != "darwin" or not 0 <= duration <= MAX_RUN_SECONDS + 15 or
                os.getppid() != parent or os.getpgrp() != os.getpid() or
                sys.argv[4] != "/usr/bin/sandbox-exec" or metadata.name != "runner-process.json"):
            raise SystemExit("Invalid isolated runner invocation")
        command = sys.argv[4:]
    # The helper rejects any run output readable by others; the runner and QEMU
    # create files with default modes, so make them private at the source.
    os.umask(0o077)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_FSIZE, (FSIZE_LIMIT, FSIZE_LIMIT))
    if duration:
        cpu_budget = duration * CPU_FACTOR + 30
        resource.setrlimit(resource.RLIMIT_CPU, (cpu_budget, cpu_budget + 5))
    resource.setrlimit(resource.RLIMIT_NOFILE, (128, 128))
    stopping = False
    def stop(*_):
        nonlocal stopping
        stopping = True
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    deadline = time.monotonic() + duration if duration else None
    if native:
        await_native_go(parent, lambda: stopping)
    child = subprocess.Popen(command, close_fds=True, stdin=subprocess.DEVNULL if native else None)
    try:
        publish_metadata(metadata, child.pid)
        while child.poll() is None:
            if stopping or os.getppid() != parent or (deadline is not None and time.monotonic() >= deadline):
                terminate_group(child)
                break
            time.sleep(.05)
        child.wait(timeout=.5)
        return child.returncode if child.returncode >= 0 else 128 - child.returncode
    finally:
        # Includes grandchildren whose runner exited without collecting them.
        try:
            remaining = group_members()
        except RuntimeError:
            remaining = True
        if remaining:
            terminate_group(child)


if __name__ == "__main__":
    raise SystemExit(main())
