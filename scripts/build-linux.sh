#!/bin/bash
# Linux build entry: engine from source + engine/runtime package into dist.
# The repository build/ directory caches downloads and build trees; dist holds
# only final artifacts, replaced atomically-ish after everything succeeded.
set -euo pipefail
root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
source "$root/scripts/build-common.sh"
test "$(uname -s)" = Linux || { echo 'build-linux.sh runs on Linux; use ./scripts/build.sh on macOS.' >&2; exit 2; }
test "$#" -le 1 || { echo "Usage: $0 [debug|release]" >&2; exit 2; }
configuration="${1:-debug}"
case "$configuration" in debug|release) ;; *) echo 'Choose debug or release.' >&2; exit 2 ;; esac
output="$(azki_dist_output)"
version="$(tr -d '\n\r' < "$root/VERSION")"
number="$(tr -d '\n\r' < "$root/BUILD_NUMBER")"
arch="$(uname -m)"
name="Azki-Simulator-$version-build$number-$configuration-linux-$arch"
azki_temporary_work
# Everything is produced inside the disposable work directory first; dist is
# only touched at the final commit below, so a failed build never disturbs
# the previously shipped artifacts.
stage="$azki_work/stage"
mkdir -p "$stage"
python3 -B - "$root" "$azki_work" <<'PY'
import pathlib, runpy, sys
root, work = map(pathlib.Path, sys.argv[1:])
module = runpy.run_path(str(root / 'scripts/build-engine.py'))
module['build_engine'](work / 'engine-build', work / 'Engine', work / 'engine-sources.tar.gz')
PY
package="$azki_work/package/azki-simulator"
mkdir -p "$package/Engine"
cp -a -- "$azki_work/Engine/." "$package/Engine/"
mkdir -p "$package/Runtime"
cp -a -- "$root/Sources/AzkiSimulator/Runtime/." "$package/Runtime/"
# Host byte-code caches are the wrong interpreter version and belong to nobody:
# the shipped Runtime must contain exactly the sources the manifest hashes.
find "$package/Runtime" -name '__pycache__' -type d -prune -exec rm -rf -- {} +
# azki-simulator.py resolves the import recipes as Engine's sibling, exactly like
# Resources/Firmware inside the macOS bundle.
mkdir -p "$package/Firmware"
cp -- "$root/engine/catalog.json" "$package/Firmware/catalog.json"
# Same packaged-runtime contract the macOS build verifies: manifest hashes, the
# emulator's capability claim, and that import recipes survived packaging.
"$package/Engine/python/bin/python3.12" -I -S -B - "$package" <<'PY'
import pathlib, sys
package = pathlib.Path(sys.argv[1])
sys.path.insert(0, str(package / 'Runtime'))
from azki_simulator.bundled_runtime import _manifest
from azki_simulator.firmware_catalog import FirmwareCatalog
from azki_simulator.instance_storage import checked_capabilities
info = _manifest(package / 'Engine', verify=True)
record = info['files'][info['manifest']['entrypoints']['qemu']]
checked_capabilities(info['manifest'], record['sha256'])
catalog = FirmwareCatalog(package / 'Firmware')
catalog.list_items()
if not catalog.imports:
    raise RuntimeError('Firmware import recipes are missing')
print('Packaged runtime metadata ready')
PY
# The Avalonia desktop app is part of the Linux deliverable: a missing dotnet SDK
# fails the build instead of quietly shipping an engine-only package. AZKI_SKIP_APP=1
# is the explicit opt-out for an engine/runtime-only artifact.
if [ -d "$root/linux" ]; then
  if ! command -v dotnet >/dev/null 2>&1; then
    if [ "${AZKI_SKIP_APP:-0}" = 1 ]; then
      echo 'AZKI_SKIP_APP=1：本包不含桌面应用，仅 Engine/Runtime。' >&2
    else
      echo '未找到 dotnet SDK。安装 dotnet-sdk，或显式设置 AZKI_SKIP_APP=1 只出引擎包。' >&2
      exit 2
    fi
  else
    mkdir -p "$package/App"
    # Publish the app project only: a solution-level --output copies the test
    # assembly into the shipped App directory as well.
    (cd "$root/linux" && dotnet publish -c Release -o "$package/App" src/AzkiSimulator.Linux/AzkiSimulator.Linux.csproj)
  fi
fi
tar -czf "$azki_work/linux-package.tar.gz" -C "$azki_work/package" azki-simulator
source_items=(Package.swift VERSION BUILD_NUMBER LICENSE README.md README.en.md docs Assets Sources engine scripts .github .gitignore)
test -d "$root/linux" && source_items+=(linux)
tar -czf "$azki_work/source.tar.gz" -C "$root" \
  --exclude '__pycache__' --exclude '*.pyc' --exclude '.DS_Store' \
  --exclude 'bin' --exclude 'obj' "${source_items[@]}"
mv -- "$azki_work/engine-sources.tar.gz" "$stage/$name-engine-sources.tar.gz"
mv -- "$azki_work/source.tar.gz" "$stage/$name-source.tar.gz"
mv -- "$azki_work/linux-package.tar.gz" "$stage/$name.tar.gz"
# Commit point: the whole build succeeded, so replace dist atomically-ish.
find "$output" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +
for item in "$stage"/*; do mv -- "$item" "$output/"; done
printf '\nPackage: %s\n' "$output/$name.tar.gz"
