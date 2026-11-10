"""Read genuine Q66FRAME shared mappings on Windows, including RGB888 Q65."""
from __future__ import annotations

import base64
import mmap
import os
from pathlib import Path
import struct

from .windows_runtime_files import WindowsFiles


def decode_header(data, file_bytes, expected=None):
    if len(data) != 32:
        raise ValueError("LCD header must be exactly 32 bytes")
    magic, version, sequence, width, height, stride, pixel_format = struct.unpack("<8s6I", data)
    pixel_bytes = {1: 2, 2: 3}.get(pixel_format)
    if magic != b"Q66FRAME" or version != 1 or pixel_bytes is None:
        raise ValueError("Unsupported LCD shared-frame protocol")
    if not (1 <= width <= 1024 and 1 <= height <= 1024 and stride == width * pixel_bytes):
        raise ValueError("Invalid LCD geometry or stride")
    if expected is not None and (width, height) != tuple(expected):
        raise ValueError("LCD frame geometry differs from selected hardware preset")
    if file_bytes not in (32 + stride * height, 32 + 1024 * 1024 * pixel_bytes):
        raise ValueError("LCD mapping has an unexpected capacity")
    return {"sequence": sequence, "width": width, "height": height,
            "stride": stride, "pixelFormat": "rgb565le" if pixel_format == 1 else "rgb888"}


class WindowsFrameReader:
    def __init__(self, path, *, expected=None, api=None):
        self.path = Path(path)
        self.expected = expected
        self.api = api or WindowsFiles()
        self.fd = self.mapping = self.identity = None

    def _open(self):
        if self.mapping is not None:
            return
        import msvcrt
        handle = self.api.open_read(self.path)
        try:
            identity = self.api.information(handle)
            if not 32 <= identity[2] <= 32 + 1024 * 1024 * 3:
                raise ValueError("LCD mapping length is outside the protocol limit")
            fd = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY)
            handle = None  # CRT now owns the native handle.
            try:
                mapping = mmap.mmap(fd, 0, access=mmap.ACCESS_READ)
            except BaseException:
                os.close(fd)
                raise
            self.fd, self.mapping, self.identity = fd, mapping, identity
        finally:
            self.api.close(handle)

    def _check_identity(self):
        handle = self.api.open_read(self.path)
        try:
            if self.api.information(handle) != self.identity:
                raise PermissionError("LCD mapping was replaced or resized")
        finally:
            self.api.close(handle)

    def snapshot(self):
        self._open()
        for _ in range(3):
            header = self.mapping[:32]
            # A new mapping can exist before the writer initializes its header.
            if header == bytes(32):
                return {"ready": False}
            magic, version, sequence = struct.unpack_from("<8s2I", header)
            if magic == b"Q66FRAME" and version == 1 and (sequence == 0 or sequence & 1):
                # The writer installs protocol metadata before its first
                # frame, and changes geometry/pixels while the sequence is
                # odd. Neither state contains a published frame to validate.
                continue
            state = decode_header(header, self.identity[2], self.expected)
            pixels = self.mapping[32:32 + state["stride"] * state["height"]]
            after = self.mapping[:32]
            self._check_identity()
            if after == header and len(pixels) == state["stride"] * state["height"]:
                return {"ready": True, **state,
                        "pixelsBase64": base64.b64encode(pixels).decode("ascii")}
        return {"ready": False}

    def close(self):
        if self.mapping is not None:
            self.mapping.close()
            self.mapping = None
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None
