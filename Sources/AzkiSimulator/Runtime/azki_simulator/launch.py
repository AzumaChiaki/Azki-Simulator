"""Private exec trampoline for child resource limits, never a general service.

Run in a fresh isolated Python interpreter instead of a threaded preexec_fn.
The bridge supplies a fixed sandbox launcher invocation and fixed audited core.
"""
import os
import resource
import signal
import subprocess
import sys
import time
import json

# The trampoline runs outside the sandbox and execs exactly one supported
# launcher per host; a mismatched launcher is a hard failure, never a fallback.
EXPECTED_LAUNCHER = "/usr/bin/sandbox-exec" if sys.platform == "darwin" else "/usr/bin/bwrap"


def main():
    duration = int(sys.argv[1])
    metadata_path = sys.argv[2]
    parent_pid = int(sys.argv[3])
    if not 0 <= duration <= 28800 or sys.argv[4] != EXPECTED_LAUNCHER or os.getppid() != parent_pid:
        raise SystemExit("Invalid isolated launch invocation")
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_FSIZE, (16 * 1024 * 1024, 16 * 1024 * 1024))
    if duration:
        resource.setrlimit(resource.RLIMIT_CPU, (duration + 1, duration + 2))
    resource.setrlimit(resource.RLIMIT_NOFILE, (128, 128))
    stopping = False
    def stop(*_):
        nonlocal stopping
        stopping = True
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    child = subprocess.Popen(sys.argv[4:], close_fds=True)
    try:
        temporary = metadata_path + "." + str(os.getpid()) + ".pending"
        fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        try:
            with os.fdopen(fd, "w") as stream:
                json.dump({"pid": child.pid, "supervisorPID": os.getpid()}, stream)
                stream.flush()
                os.fsync(stream.fileno())
            os.link(temporary, metadata_path, follow_symlinks=False)
        finally:
            os.unlink(temporary)
        deadline = time.monotonic() + duration if duration else None
        # Parent death and explicit stop remain active in unlimited mode.
        while child.poll() is None:
            if stopping or os.getppid() != parent_pid or (deadline is not None and time.monotonic() >= deadline):
                child.terminate()
                try:
                    child.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=1)
                break
            time.sleep(.05)
        return child.returncode if child.returncode >= 0 else 128 - child.returncode
    finally:
        if child.poll() is None:
            child.kill()
            child.wait(timeout=1)


if __name__ == "__main__":
    raise SystemExit(main())
