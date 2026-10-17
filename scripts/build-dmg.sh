#!/bin/bash
set -euo pipefail
root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
test "$(uname -s)" = Darwin || { echo 'Build requires macOS.' >&2; exit 2; }
test "$(uname -m)" = arm64 || { echo 'Build requires Apple Silicon.' >&2; exit 2; }
source "$root/scripts/build-common.sh"
test "$#" -le 1 || { echo "Usage: $0 [dist]" >&2; exit 2; }
output="$(azki_dist_output "${1:-$root/dist}")"
version="$(tr -d '\n\r' < "$root/VERSION")"
number="$(tr -d '\n\r' < "$root/BUILD_NUMBER")"
name="Azki-Simulator-$version-build$number-arm64-unsigned"
for target in "$output/$name.dmg" "$output/$name-engine-sources.tar.gz" "$output/$name-source.tar.gz"; do
  test ! -e "$target" || { echo "Output exists: $target; move or clean it first." >&2; exit 2; }
done
azki_temporary_work
work="$azki_work"
"$root/scripts/build.sh" release
mkdir -p "$work/volume"
/usr/bin/ditto "$output/Azki Simulator.app" "$work/volume/Azki Simulator.app"
ln -s /Applications "$work/volume/Applications"
/usr/bin/hdiutil create -volname 'Azki Simulator' -srcfolder "$work/volume" \
  -format UDZO "$work/$name.dmg"
mv -- "$work/$name.dmg" "$output/$name.dmg"
mv -- "$output/Azki-Simulator-$version-build$number-release-source.tar.gz" "$output/$name-source.tar.gz"
mv -- "$output/Azki-Simulator-$version-build$number-release-engine-sources.tar.gz" "$output/$name-engine-sources.tar.gz"
printf '\nDMG: %s\n' "$output/$name.dmg"
