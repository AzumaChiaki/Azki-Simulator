#!/bin/bash
set -euo pipefail
root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
source "$root/scripts/build-common.sh"
test "$(uname -s)" = Darwin || { echo 'Build requires macOS.' >&2; exit 2; }
test "$(uname -m)" = arm64 || { echo 'Build requires Apple Silicon.' >&2; exit 2; }
test "$#" -le 1 || { echo "Usage: $0 [debug|release]" >&2; exit 2; }
configuration="${1:-debug}"
case "$configuration" in debug|release) ;; *) echo 'Choose debug or release.' >&2; exit 2 ;; esac
output="$(azki_dist_output)"
version="$(tr -d '\n\r' < "$root/VERSION")"
number="$(tr -d '\n\r' < "$root/BUILD_NUMBER")"
name="Azki-Simulator-$version-build$number-$configuration"
for target in "$output/Azki Simulator.app" "$output/$name-engine-sources.tar.gz" "$output/$name-source.tar.gz"; do
  test ! -e "$target" || { echo "Output exists: $target; move or clean it first." >&2; exit 2; }
done
azki_temporary_work
python3 -B - "$root" "$azki_work" <<'PY'
import pathlib, runpy, sys
root, work = map(pathlib.Path, sys.argv[1:])
module = runpy.run_path(str(root / 'scripts/build-engine.py'))
module['build_engine'](work / 'engine-build', work / 'Engine', work / 'engine-sources.tar.gz')
PY
AZKI_BUILD_CONFIGURATION="$configuration" "$root/scripts/build-app.sh" "$azki_work/Engine"
COPYFILE_DISABLE=1 /usr/bin/tar -czf "$azki_work/source.tar.gz" -C "$root" \
  --exclude '__pycache__' --exclude '*.pyc' --exclude '.DS_Store' \
  Package.swift VERSION BUILD_NUMBER LICENSE README.md README.en.md docs Assets Sources engine scripts .github .gitignore
mv -- "$azki_work/engine-sources.tar.gz" "$output/$name-engine-sources.tar.gz"
mv -- "$azki_work/source.tar.gz" "$output/$name-source.tar.gz"
printf '\nApp: %s\n' "$output/Azki Simulator.app"
