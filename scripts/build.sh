#!/bin/bash
set -euo pipefail
root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
source "$root/scripts/build-common.sh"
case "$(uname -s)" in
  Darwin)
    test "$(uname -m)" = arm64 || { echo 'Build requires Apple Silicon.' >&2; exit 2; }
    ;;
  Linux)
    exec "$root/scripts/build-linux.sh" "$@"
    ;;
  *) echo 'Build requires macOS or Linux.' >&2; exit 2 ;;
esac
test "$#" -le 1 || { echo "Usage: $0 [debug|release]" >&2; exit 2; }
configuration="${1:-debug}"
case "$configuration" in debug|release) ;; *) echo 'Choose debug or release.' >&2; exit 2 ;; esac
output="$(azki_dist_output)"
version="$(tr -d '\n\r' < "$root/VERSION")"
number="$(tr -d '\n\r' < "$root/BUILD_NUMBER")"
name="Azki-Simulator-$version-build$number-$configuration"
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
AZKI_BUILD_CONFIGURATION="$configuration" "$root/scripts/build-app.sh" "$azki_work/Engine" "$stage"
COPYFILE_DISABLE=1 /usr/bin/tar -czf "$azki_work/source.tar.gz" -C "$root" \
  --exclude '__pycache__' --exclude '*.pyc' --exclude '.DS_Store' \
  Package.swift VERSION BUILD_NUMBER LICENSE README.md README.en.md docs Assets Sources engine scripts .github .gitignore
mv -- "$azki_work/engine-sources.tar.gz" "$stage/$name-engine-sources.tar.gz"
mv -- "$azki_work/source.tar.gz" "$stage/$name-source.tar.gz"
# Commit point: the whole build succeeded, so replace dist atomically-ish.
find "$output" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +
for item in "$stage"/*; do mv -- "$item" "$output/"; done
printf '\nApp: %s\n' "$output/Azki Simulator.app"
