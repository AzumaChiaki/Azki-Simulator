# Azki Simulator

[简体中文](README.md) · [Build instructions](docs/BUILDING.en.md) · [GPL-2.0](LICENSE)

A standalone wearable-device simulator for Apple Silicon Macs. Its native SwiftUI/AppKit interface runs device firmware through a separate QEMU process. The emulator is not embedded in Azki Studio.

## Features

- Create, name and manage independent instances, with multiple device displays.
- Mouse-based touch input, live logs, screenshots and recordings saved to the Desktop.
- Start, stop, cold restart and persistent instance storage, with unlimited sessions by default.
- Install quick apps and watchfaces; configure synthetic serial numbers, virtual Bluetooth addresses and initial setup options.
- Edit raw PMU ADC inputs. Confirmed purposes are labelled; uncalibrated values do not imply physical units.

The catalog covers Xiaomi Smart Band 9, 9 Pro, 10, 10 Pro and 11 families. Firmware compatibility depends on the implemented hardware and storage layout; a catalog entry does not mean every firmware version has been verified. Firmware switching uses a cold restart.

External Bluetooth remains experimental. Successful pairing and communication with phone system settings or Mi Fitness are not guaranteed.

## Use

Requires **Apple Silicon and macOS 15 or later**. The app bundles its interpreter and emulator core; users do not need Python, Homebrew or development tools.

1. Open a successful run in [Actions](https://github.com/AzumaChiaki/Azki-Simulator/actions/workflows/build-dmg.yml), download its artifact and extract it.
2. Open the DMG and drag **Azki Simulator** into **Applications**.
3. Import firmware you are entitled to use, create an instance, select a firmware version and start it.

The repository and automated builds **exclude vendor firmware, real-device identities, test instances and personal logs**. Firmware is imported locally and is not uploaded to this repository. Preserve your instance data; the application retains the previous storage snapshot when switching versions.

Actions artifacts are not Developer ID signed or Apple notarized. Apple Silicon executables use only the ad-hoc signatures required to run. macOS may report an unidentified developer. Do not disable system security; use the system's per-application opening controls only after verifying the source.

## Build

```sh
git clone git@github.com:AzumaChiaki/Azki-Simulator.git
cd Azki-Simulator
brew install python@3.12 meson ninja pkgconf glib gettext libffi pcre2 zstd dtc
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
export PATH="$(brew --prefix python@3.12)/libexec/bin:$PATH"
./scripts/build-dmg.sh
```

Full Xcode with Swift 6.2 or later is required. Outputs in `dist/` include the DMG, product source and engine dependency sources. See [build instructions](docs/BUILDING.en.md) for details and options.

`VERSION` controls the application version and `BUILD_NUMBER` controls its build number, currently **0.1.0 (1)**. Merging or pushing to `main` starts a build; manual runs are also available in Actions. No Apple certificates, passwords or notarization profile are required.

## Layout

| Path | Contents |
| --- | --- |
| `Sources/AzkiSimulator` | Native UI, instance management and runtime communication |
| `engine/project` | Firmware runtime wrapper and installation transport |
| `engine/qemu-overlay` | Device implementation and customized files for QEMU 10.1.0 |
| `engine/third-party` | Third-party helper sources needed for the build |
| `Assets` | Application icons |
| `scripts` | Engine, application and DMG build scripts |
| `.github/workflows` | GitHub Actions build workflow |
| `docs` | Chinese and English build documentation |

## License

Copyright © 2026 AzumaChiaki.

Project-owned code is licensed under **GNU General Public License version 2 (GPL-2.0-only)**; see [LICENSE](LICENSE). Third-party code retains its original copyright and license notices. The root license does not replace those grants. Notices are retained in `engine/licenses`, `engine/third-party` and source headers; build artifacts include the corresponding license and source materials.
