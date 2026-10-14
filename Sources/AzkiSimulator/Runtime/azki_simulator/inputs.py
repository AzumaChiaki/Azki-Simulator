"""Read-only input classification. Recognition is not authenticity or compatibility."""
import hashlib
from pathlib import Path
import struct

from .bridge import BridgeError, regular_bytes


def inspect_inputs(paths):
    if set(paths) != {"qemu", "firmware", "resource"}:
        raise BridgeError("invalid_inputs", "Choose a core, AP firmware and resource image together")
    result = {}
    for role, limit in (("qemu", 256 << 20), ("firmware", (16 << 20) - 0xc0000), ("resource", 128 << 20)):
        path = Path(paths[role])
        if not path.is_absolute():
            raise BridgeError("invalid_inputs", "Inputs require explicit absolute paths")
        data = regular_bytes(path, limit)
        if role == "qemu":
            if len(data) < 32 or struct.unpack_from("<I", data)[0] != 0xfeedfacf or struct.unpack_from("<I", data, 4)[0] != 0x0100000c or struct.unpack_from("<I", data, 12)[0] != 2:
                raise BridgeError("unsupported_core", "Expected a thin arm64 macOS executable; no file was executed")
            kind = "arm64 Mach-O executable; QEMU provenance not verified"
        elif role == "firmware":
            if len(data) < 64 or b"CHIP=best1503" not in data or b"KERNEL=NUTTX" not in data:
                raise BridgeError("unsupported_firmware", "Expected a bounded BEST1503/NuttX AP candidate")
            kind = "BEST1503/NuttX AP candidate; boot/signature compatibility unverified"
        else:
            if len(data) < 32 or data[:8] != b"-rom1fs-":
                raise BridgeError("unsupported_resource", "Expected a bounded ROMFS resource image")
            declared = struct.unpack_from(">I", data, 8)[0]
            if not 32 <= declared <= len(data):
                raise BridgeError("unsupported_resource", "ROMFS declared size exceeds the supplied image")
            kind = "ROMFS resource candidate; target compatibility unverified"
        result[role] = {"path": str(path.resolve()), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                        "classification": kind, "signatureVerified": False}
    return result
