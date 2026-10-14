"""Content identity for the Python code that prepares and runs original firmware.

QEMU's executable hash is intentionally separate. Paths are relative so moving
an otherwise identical checkout does not invalidate evidence; every listed
file is required, and missing code must never silently disappear from the hash.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path


RUNTIME_FILES = (
    'multi_device.py',
    'device_catalog.py',
    'firmware_resources.py',
    'legacy_ui_seed.py',
    'backend/src/legacy_ui_db.c',
    'backend/src/yaffs_image.c',
    'backend/src/runner.py',
    'backend/src/factory.py',
    'backend/src/control.py',
    'backend/src/qmp.py',
    'backend/src/instance_data.py',
    'runtime_fingerprint.py',
    'backend/src/virtual_peer/MIT-miconnect.txt',
    'backend/src/virtual_peer/__init__.py',
    'backend/src/virtual_peer/auth_crypto.py',
    'backend/src/virtual_peer/auth_protocol.py',
    'backend/src/virtual_peer/bind_info.py',
    'backend/src/virtual_peer/business_session.py',
    'backend/src/virtual_peer/central.py',
    'backend/src/virtual_peer/crypto.py',
    'backend/src/virtual_peer/hci.py',
    'backend/src/virtual_peer/installer.py',
    'backend/src/virtual_peer/link.py',
    'backend/src/virtual_peer/packages.py',
    'backend/src/virtual_peer/production_client.py',
    'backend/src/virtual_peer/secure_link.py',
    'backend/src/virtual_peer/service.py',
    'backend/src/virtual_peer/session_api.py',
    'backend/src/virtual_peer/smp.py',
    'backend/src/virtual_peer/state.py',
    'backend/src/virtual_peer/virtual_packages.py',
    'backend/src/virtual_peer/wire.py',
)


def runtime_fingerprint(root=None, *, hash_contents=True):
    root = Path(root) if root is not None else Path(__file__).resolve().parent
    files = []
    for relative in RUNTIME_FILES:
        path = root/relative
        if hash_contents:
            data = path.read_bytes()
            files.append(dict(path=relative, bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
        else:
            files.append(dict(path=relative, bytes=path.stat().st_size))
    manifest = dict(schema='minative-python-runtime-v1', files=files)
    if not hash_contents:
        return dict(sha256=None, hashVerification='not-required-for-startup', **manifest)
    canonical = json.dumps(manifest, sort_keys=True, separators=(',', ':'), ensure_ascii=True).encode('ascii')
    return dict(sha256=hashlib.sha256(canonical).hexdigest(), **manifest)


if __name__ == '__main__':
    print(json.dumps(runtime_fingerprint(), indent=2))
