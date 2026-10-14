#!/bin/bash
set -euo pipefail
root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
if [ "$#" -ne 2 ]; then
  echo "Usage: $0 <prepared Engine directory> <new app output directory>" >&2
  exit 2
fi
engine="$(cd -- "$1" && pwd)"
mkdir -p "$2"
output="$(cd -- "$2" && pwd)"
app="$output/Azki Simulator.app"
test ! -e "$app" || { echo "Output app already exists: $app" >&2; exit 2; }
test -f "$engine/manifest.json"
test -x "$engine/python/bin/python3.12"
test -x "$engine/project/backend/build/qemu-system-arm"
build="${AZKI_BUILD_DIR:-$root/.build}"
cd "$root"
/usr/bin/xcrun swift build --scratch-path "$build" -c release -j "${AZKI_BUILD_JOBS:-2}"
bin="$(/usr/bin/xcrun swift build --scratch-path "$build" -c release --show-bin-path)"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources/Firmware"
cp "$bin/AzkiSimulator" "$app/Contents/MacOS/AzkiSimulator"
/usr/bin/ditto "$engine" "$app/Contents/Resources/Engine"
/usr/bin/ditto "$root/Sources/AzkiSimulator/Runtime" "$app/Contents/Resources/Runtime"
cp "$root/Assets/AppIcon.icns" "$app/Contents/Resources/AppIcon.icns"
cp "$root/engine/catalog.json" "$app/Contents/Resources/Firmware/catalog.json"
cp "$root/LICENSE" "$app/Contents/Resources/LICENSE"
python3 -B - "$root" "$app" <<'PY'
import pathlib, plistlib, shutil, sys
root, app = map(pathlib.Path, sys.argv[1:])
for path in (app / 'Contents/Resources/Runtime').rglob('__pycache__'):
    shutil.rmtree(path)
version = (root / 'VERSION').read_text().strip()
build = (root / 'BUILD_NUMBER').read_text().strip()
if not build.isdecimal() or int(build) < 1:
    raise SystemExit('BUILD_NUMBER must be a positive integer')
info = {
    'CFBundleExecutable': 'AzkiSimulator', 'CFBundleIdentifier': 'com.azki.AzkiSimulator',
    'CFBundleName': 'Azki Simulator', 'CFBundleDisplayName': 'Azki Simulator',
    'CFBundlePackageType': 'APPL', 'CFBundleShortVersionString': version,
    'CFBundleVersion': build, 'LSMinimumSystemVersion': '15.0',
    'CFBundleIconFile': 'AppIcon', 'NSHighResolutionCapable': True,
    'NSPrincipalClass': 'NSApplication',
    'NSBluetoothAlwaysUsageDescription': 'Advertise the virtual device you enable to nearby Bluetooth clients.',
    'NSBluetoothPeripheralUsageDescription': 'Advertise the virtual device you enable to nearby Bluetooth clients.',
}
(app / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
PY
# Apple Silicon requires a valid Mach-O signature. Ad-hoc uses no identity,
# certificate, timestamp service, Developer ID account or notarization service.
/usr/bin/codesign --force --sign - --timestamp=none "$app"
/usr/bin/codesign --verify --deep --strict "$app"
printf '%s\n' "$app"
