"""Build a hash-bound YAFFS /data seed using OpenVela's actual UnQLite engine.

This is local UI state, not device or account provisioning. The stock AP and
same-OTA resource image stay unchanged. Runtime desktop success is separate.
"""
from __future__ import annotations
import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

from device_catalog import CACHE, ROOT, digest, select

PREFERENCES = {
    'persist.setup_wizard': '1',
    'persist.locale': 'zh_CN',
    'persist.screen.auto_screen_timeout': '0',
    'persist.screen.dimmer_timeout': '2147483647',
}
UNQLITE = ROOT / 'references/openvela-unqlite'
DB_HELPER = ROOT / 'backend/build/legacy-ui-db'
YAFFS_HELPER = ROOT / 'backend/build/yaffs-image'


def data_partition(ap_data):
    """Read the original NAND data descriptor; prefer the modeled 512 MiB map."""
    marker = b'nand_data' + bytes(95)
    found = []
    start = 0
    while (at := ap_data.find(marker, start)) >= 0:
        start = at + 1
        if at % 4 or at + 116 > len(ap_data):
            continue
        page, count, flags = struct.unpack_from('<3I', ap_data, at + 104)
        if flags == 0 and page and count and (page + count) * 2048 <= 512 * 1024**2:
            found.append(dict(offset=page * 2048, size=count * 2048,
                              descriptor_offset=at))
    if not found:
        raise ValueError('Original NAND data partition was not found')
    result = max(found, key=lambda r: r['offset'] + r['size'])
    if result['size'] % 131072 or result['size'] < 8 * 1024**2:
        raise ValueError('NAND data partition is not erase-block aligned')
    return result


def prepare_ui_seed(profile, data_bytes=None):
    """Return (image Path, image SHA-256, proof dict) for a fresh local session."""
    if not profile['model'].startswith(('miwear.watch.n66', 'miwear.watch.o66', 'miwear.watch.n67')):
        raise ValueError('This seed is only validated for legacy UnQLite/YAFFS firmware')
    ap_path = Path(profile['ap_path'])
    ap = ap_path.read_bytes()
    if len(ap) != profile['ap']['size']:
        raise ValueError('AP size differs from selected metadata')
    if b'/data/persist.db\x00' not in ap:
        raise ValueError('Original firmware does not identify /data/persist.db')
    partition = data_partition(ap)
    if data_bytes is not None and data_bytes != partition['size']:
        raise ValueError('Requested data image size differs from original partition')
    # The legacy binaries lack auto_screen_timeout, but retain dimmer_timeout.
    # Record that fact instead of claiming the unused forward-compatible key works.
    references = {key: ap.find(key.encode() + b'\x00') for key in PREFERENCES}
    if any(references[key] < 0 for key in (
            'persist.setup_wizard', 'persist.locale', 'persist.screen.dimmer_timeout')):
        raise ValueError('Required local UI property is absent in original AP')
    source_files = [ROOT / 'backend/src/legacy_ui_db.c', ROOT / 'backend/src/yaffs_image.c',
                    UNQLITE / 'unqlite.c', UNQLITE / 'unqlite.h']
    expected = dict(schema_version=2, profile_id=profile['id'],
        ap_sha256=profile['ap']['sha256'], ota_sha256=profile['ota_sha256'],
        partition=partition, preferences=PREFERENCES,
        property_literal_offsets=references, database_path='/data/persist.db',
        database_engine='OpenVela UnQLite 1.1.9', key_and_value_terminating_nul=True,
        sources={str(path.relative_to(ROOT)): digest(path) for path in source_files},
        db_builder_sha256=digest(DB_HELPER), yaffs_builder_sha256=digest(YAFFS_HELPER),
        account_binding_completed=False, desktop_verified=False)
    # SKU aliases may share an OTA but must not overwrite each other's input.
    # Source/builder changes and UI preferences also get immutable cache slots.
    canonical = json.dumps(expected, sort_keys=True, separators=(',', ':'),
                           ensure_ascii=True).encode('ascii')
    config_sha = hashlib.sha256(canonical).hexdigest()
    expected['cache_config_sha256'] = config_sha
    directory = CACHE / profile['ota_sha256'] / 'local-ui' / config_sha
    directory.mkdir(parents=True, exist_ok=True)
    # Two native windows can request the same configuration concurrently. Hold
    # the lock through both files' publication and recheck the cache inside it.
    with (directory / '.build.lock').open('a+b') as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        return _prepare_locked_seed(directory, expected, partition)


def _prepare_locked_seed(directory, expected, partition):
    image = directory / 'nand-data.bin'
    proof_path = directory / 'nand-data.json'
    if image.exists() and proof_path.exists():
        proof = json.loads(proof_path.read_text())
        if not all(proof.get(key) == value for key, value in expected.items()):
            raise ValueError('Cached local UI proof configuration mismatch')
        if image.stat().st_size != partition['size'] or digest(image) != proof['sha256']:
            raise ValueError('Cached local UI image hash or size mismatch')
        return image, proof['sha256'], proof
    with tempfile.TemporaryDirectory(prefix='ui-seed-', dir=directory) as temp:
        staging = Path(temp)
        tree = staging / 'tree'
        tree.mkdir()
        database = tree / 'persist.db'
        subprocess.run([str(DB_HELPER), str(database)], check=True, capture_output=True, text=True)
        built = staging / 'nand-data.bin'
        completed = subprocess.run([str(YAFFS_HELPER), str(built), '--bytes',
                                    str(partition['size']), '--tree', str(tree)],
                                   check=True, capture_output=True, text=True)
        if built.stat().st_size != partition['size']:
            raise ValueError('YAFFS builder produced incorrect data partition size')
        proof = dict(expected, sha256=digest(built), database_sha256=digest(database),
                     database_bytes=database.stat().st_size,
                     cold_remount_verification=completed.stdout.strip())
        staged_proof = staging / 'nand-data.json'
        staged_proof.write_text(json.dumps(proof, indent=2) + '\n')
        built.replace(image)
        staged_proof.replace(proof_path)
    return image, proof['sha256'], proof


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', required=True)
    args = parser.parse_args()
    image, sha, proof = prepare_ui_seed(select(args.device))
    print(json.dumps(dict(image=str(image), sha256=sha, proof=proof), indent=2))


if __name__ == '__main__':
    main()
