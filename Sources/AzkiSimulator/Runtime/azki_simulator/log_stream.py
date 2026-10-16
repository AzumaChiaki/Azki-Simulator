"""Bounded, read-only incremental logs. Never follows paths supplied by a client."""
import base64
import os
import re
import stat

NAMES = frozenset(("stderr.log", "qemu.log", "virtual-peer.log", "uart0.log", "uart1.log", "uart2.log"))
MAX_CHUNK = 32768  # Base64 plus JSON stays below the v1 bridge message bound.
MAX_BACKLOG = 262144


def read_chunk(root, arguments):
    from .bridge import BridgeError
    name = arguments.get("name", "stderr.log")
    limit = arguments.get("limit", 16384)
    cursor = arguments.get("cursor")
    if (not isinstance(name, str) or name not in NAMES or type(limit) is not int or not 1 <= limit <= MAX_CHUNK
            or set(arguments) - {"name", "stream", "limit", "cursor"}):
        raise BridgeError("invalid_request", "Invalid incremental log request")
    if cursor is not None and (not isinstance(cursor, dict) or set(cursor) != {"streamID", "offset"}
            or not isinstance(cursor.get("streamID"), str)
            or re.fullmatch(r"[0-9]+:[0-9]+", cursor["streamID"]) is None
            or len(cursor["streamID"]) > 64 or type(cursor.get("offset")) is not int
            or not 0 <= cursor["offset"] <= 2**53 - 1):
        raise BridgeError("invalid_request", "Invalid incremental log cursor")
    missing = {"version": 2, "name": name, "streamID": None, "nextOffset": 0,
               "dataBase64": "", "reset": True, "missing": True, "hasMore": False, "skippedBytes": 0}
    if root is None:
        return missing
    # Open the run directory atomically; names below are a fixed whitelist.
    from .minative import _directory_fd
    parent = _directory_fd(root, private=True)
    try:
        try:
            fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC, dir_fd=parent)
        except FileNotFoundError:
            return missing
    finally:
        os.close(parent)
    try:
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1
                or info.st_mode & 0o022 or not 0 <= info.st_size <= 2**53 - 1):
            raise BridgeError("unsafe_log", "Log type, ownership or permissions rejected")
        stream = str(info.st_dev) + ":" + str(info.st_ino)
        reset = cursor is None or cursor["streamID"] != stream or cursor["offset"] > info.st_size
        start = max(0, info.st_size - limit) if reset else cursor["offset"]
        skipped = start if reset else max(0, info.st_size - start - MAX_BACKLOG)
        if not reset:
            start += skipped
        data = os.pread(fd, min(limit, max(0, info.st_size - start)), start)
        after = os.fstat(fd)
        if after.st_size < start + len(data):
            # Truncation raced the read. Retry from a fresh cursor next tick.
            return {**missing, "missing": False}
        return {"version": 2, "name": name, "streamID": stream,
                "nextOffset": start + len(data), "dataBase64": base64.b64encode(data).decode("ascii"),
                "reset": reset or skipped > 0, "missing": False,
                "hasMore": start + len(data) < after.st_size, "skippedBytes": skipped}
    finally:
        os.close(fd)
