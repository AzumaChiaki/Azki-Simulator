#!/bin/bash
set -euo pipefail
root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
  echo "Usage: $0 <prepared Engine directory> [dist]" >&2
  exit 2
fi
source "$root/scripts/build-common.sh"
engine="$(cd -- "$1" && pwd)"
if [ "$#" -ge 2 ]; then
  # Staging output directory (used by build.sh); must be outside the repo tree.
  candidate="$(mkdir -p -- "$2" && cd -- "$2" && pwd)"
  case "$candidate" in "$root" | "$root"/*) echo "Staging output must not live inside the repository." >&2; exit 2 ;; esac
  output="$candidate"
else
  output="$(azki_dist_output)"
fi
final_app="$output/Azki Simulator.app"
test -f "$engine/manifest.json"
test -x "$engine/python/bin/python3.12"
test -x "$engine/project/backend/build/qemu-system-arm"
cd "$root"
configuration="${AZKI_BUILD_CONFIGURATION:-release}"
case "$configuration" in debug|release) ;; *) echo "Invalid build configuration: $configuration" >&2; exit 2 ;; esac
azki_temporary_work
build="$azki_work/swift"
app="$azki_work/Azki Simulator.app"
/usr/bin/xcrun swift build --disable-sandbox --scratch-path "$build" -c "$configuration" -j "${AZKI_BUILD_JOBS:-2}"
bin="$(/usr/bin/xcrun swift build --scratch-path "$build" -c "$configuration" --show-bin-path)"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources/Firmware"
cp "$bin/AzkiSimulator" "$app/Contents/MacOS/AzkiSimulator"
/usr/bin/ditto "$engine" "$app/Contents/Resources/Engine"
/usr/bin/ditto "$root/Sources/AzkiSimulator/Runtime" "$app/Contents/Resources/Runtime"
cp "$root/Assets/AppIcon.icns" "$app/Contents/Resources/AppIcon.icns"
cp "$root/engine/catalog.json" "$app/Contents/Resources/Firmware/catalog.json"
cp "$root/LICENSE" "$app/Contents/Resources/LICENSE"
"$app/Contents/Resources/Engine/python/bin/python3.12" -I -S -B - "$app/Contents/Resources" <<'PY'
import pathlib, sys
resources = pathlib.Path(sys.argv[1])
sys.path.insert(0, str(resources / 'Runtime'))
from azki_simulator.bundled_runtime import _manifest
from azki_simulator.firmware_catalog import FirmwareCatalog
from azki_simulator.instance_storage import checked_capabilities
info = _manifest(resources / 'Engine', verify=True)
record = info['files'][info['manifest']['entrypoints']['qemu']]
checked_capabilities(info['manifest'], record['sha256'])
catalog = FirmwareCatalog(resources / 'Firmware')
catalog.list_items()
if not catalog.imports:
    raise RuntimeError('Firmware import recipes are missing')
print('Packaged runtime metadata ready')
PY
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
rm -rf -- "$final_app"
mv -- "$app" "$final_app"
printf '%s\n' "$final_app"
