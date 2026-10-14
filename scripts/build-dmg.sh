#!/bin/bash
set -euo pipefail
root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
test "$(uname -s)" = Darwin || { echo 'Build requires macOS.' >&2; exit 2; }
test "$(uname -m)" = arm64 || { echo 'Build requires Apple Silicon.' >&2; exit 2; }
output="${1:-$root/dist}"
mkdir -p "$output"
output="$(cd -- "$output" && pwd)"
version="$(tr -d '\n\r' < "$root/VERSION")"
number="$(tr -d '\n\r' < "$root/BUILD_NUMBER")"
name="Azki-Simulator-$version-build$number-arm64-unsigned"
test ! -e "$output/$name.dmg" || { echo 'Output already exists; choose another output directory.' >&2; exit 2; }
work_parent="${AZKI_WORK_DIR:-$root/build}"
mkdir -p "$work_parent"
work="$(mktemp -d "$work_parent/package-XXXXXXXX")"
cleanup() { rm -rf -- "$work/volume"; }
trap cleanup EXIT
python3 -B "$root/scripts/build-engine.py" --work-dir "$work/engine-build" \
  --output "$work/Engine" --source-output "$output/$name-engine-sources.tar.gz"
AZKI_BUILD_DIR="${AZKI_BUILD_DIR:-$work/swift}" "$root/scripts/build-app.sh" "$work/Engine" "$work/volume"
ln -s /Applications "$work/volume/Applications"
/usr/bin/hdiutil create -volname 'Azki Simulator' -srcfolder "$work/volume" \
  -format UDZO -ov "$output/$name.dmg"
# Ship the exact product source and build instructions next to the binary.
COPYFILE_DISABLE=1 /usr/bin/tar -czf "$output/$name-source.tar.gz" -C "$root" \
  --exclude '__pycache__' --exclude '*.pyc' --exclude '.DS_Store' \
  Package.swift VERSION BUILD_NUMBER LICENSE README.md README.en.md docs Assets Sources engine scripts .github .gitignore
printf '\nDMG: %s\n' "$output/$name.dmg"
