"""Refresh build records after explicitly updating a local development engine."""
import hashlib
import json
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
path = root / 'manifest.json'
manifest = json.loads(path.read_text())
for record in manifest['files']:
    file = root / record['path']
    record.update(bytes=file.stat().st_size,
                  sha256=hashlib.sha256(file.read_bytes()).hexdigest(),
                  executable=bool(file.stat().st_mode & 0o111))
manifest['payloadSha256'] = hashlib.sha256(json.dumps(
    manifest['files'], sort_keys=True, separators=(',', ':'), ensure_ascii=True).encode()).hexdigest()
path.write_text(json.dumps(manifest, indent=2) + '\n')
