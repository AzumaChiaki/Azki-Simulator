# Building Azki Simulator on Linux

[简体中文](BUILDING-LINUX.md) · [Home](../README.en.md) · [macOS build notes](BUILDING.en.md)

## Status

Linux support is currently **development/CI grade**: build the engine from source, run firmware headless, and drive it through the local control surface. There are no `.deb`/Flatpak packages and no cross-distribution promises; Arch Linux (Wayland) is the reference environment, and other distributions map the same dependency list onto their package managers. Real-phone Bluetooth radio pairing (BlueZ) is not implemented yet; the virtual peer protocol layer (quick-app and watchface installation and so on) already works on Linux.

## Requirements

- Linux on x86_64 or aarch64. The build host is the run host.
- Build dependencies (Arch package names): `base-devel meson ninja pkgconf python glib2 pixman zlib libffi pcre2 zstd dtc libcap openssl`.
- Runtime: `bubblewrap` (process isolation, requires unprivileged user namespaces) and `ffmpeg` (optional, screen recording).
- Desktop app build dependency: `dotnet-sdk` (.NET SDK, builds the Avalonia app under `linux/`).
- The first build downloads upstream sources for QEMU, CPython and the Python wheels.

On Debian/Ubuntu the equivalent packages are: `build-essential meson ninja-build pkg-config python3 libglib2.0-dev libpixman-1-dev zlib1g-dev libffi-dev libpcre2-dev libzstd-dev device-tree-compiler libcap-dev libssl-dev bubblewrap`.

## Building

```sh
git clone git@github.com:AzumaChiaki/Azki-Simulator.git
cd Azki-Simulator
./scripts/build.sh debug
```

On Linux `build.sh` dispatches to `scripts/build-linux.sh`: it builds the customized QEMU (arm-softmmu, TCG, no display backends), bundles CPython 3.12 with Pillow/littlefs, compiles the YAFFS/UnQLite tools, verifies every ELF's dynamic links, and packages the result together with the host runtime. The desktop app is part of the deliverable: with `linux/` present and no dotnet SDK on the host the build fails, so set `AZKI_SKIP_APP=1` deliberately when an engine-only package is wanted. Use `./scripts/build-linux.sh release` for Release.

Standalone engine build: `python3 -B scripts/build-engine.py`, emitting `dist/Engine` and `dist/Engine-sources.tar.gz`. `build/` is a reusable cache; `dist/` holds only final artifacts; one-off staging happens outside the repository and is cleaned up, exactly as on macOS.

### Downloads and indexes

Upstream tarballs and PyPI sdists are fetched from the canonical host first and from public mirrors afterwards, always verified against the SHA-256 published in the PyPI metadata, so a mirror can never substitute an artifact. Building a wheel also makes the PEP 517 backend install its own build requirements (Cython, meson-python, …); that index is pinned to `https://pypi.org/simple` instead of inheriting the host `pip.conf`, because a dead mirror there only surfaces as `from versions: none`. On a throttled link, point it at a reachable index explicitly:

```sh
AZKI_PIP_INDEX_URL=https://mirrors.cloud.tencent.com/pypi/simple ./scripts/build-linux.sh
```

Replacing the index changes who supplies the build-time toolchain; the runtime dependency closure is still checked file by file against its SHA-256.

The desktop app (`linux/`) reads file metadata through glibc's x86_64 `struct stat` layout; other hosts fail with an explicit error instead of guessing field offsets.

Faults that escape a desktop command are appended to `~/.local/share/AzkiSimulator/com.azki.AzkiSimulator/logs/gui.log` (the prefix moves with `AZKI_DATA_ROOT`); the window only shows a one-line status, so read this file when diagnosing.

## Differences from the macOS build

| Aspect | macOS | Linux |
| --- | --- | --- |
| Process isolation | `/usr/bin/sandbox-exec` (Seatbelt SBPL) | `/usr/bin/bwrap` (bubblewrap mount namespaces) |
| Binary integrity | Mach-O dependency rewrite + ad-hoc codesign | Links system libraries; `ldd`/RUNPATH closure check, per-file SHA-256 in `manifest.json` |
| Signing record | `signing.adHoc = true` | `signing.scheme = manifest-sha256` |
| GPL corresponding source | Homebrew recipes and upstream tarballs | Upstream tarballs + `DEPENDENCIES.json` (system libraries come from the build host's distribution) |
| Device screen | Native window + `frame.raw` | `frame.raw` only (no GTK/SDL compiled into QEMU; `--window` errors out) |
| Virtual Bluetooth radio | CoreBluetooth (experimental) | Not implemented; `--bluetooth reject/virtual`, BlueZ later |

bwrap and Seatbelt differ in grant granularity: files inside the bound read-only trees (bundled interpreter, prepared project) are fully readable, not only the manifest-pinned files; the writable scope stays strictly inside the session's private directory. A failed bwrap spawn is a hard failure with no unsandboxed fallback. If unprivileged user namespaces are disabled (some containers, hardened kernels), sessions cannot start; restore `kernel.unprivileged_userns_clone=1` or run in an environment that allows userns.

## Tests

Regression without engine artifacts:

```sh
python3 -m unittest discover -s tests -p 'test_*.py'
```

Compilation tests for C device code need `gcc` (or clang) and `pkg-config glib-2.0`; they skip themselves when no compiler is present. `tests/test_system_crypto.py` pins the byte contract of each host's system crypto bridge with FIPS-197 known-answer vectors plus ECDH symmetry/round-trip properties.

The desktop app has its own suite: `dotnet test linux/AzkiSimulator.Linux.sln` covers the frame header contract, the bwrap argv shape, the session descriptor, PMU ADC values, and the `open` flag constants with symlink rejection (a wrong octal-to-hex translation is only caught here).

End-to-end acceptance requires importing your own firmware (the repository contains none): import → create instance → `start` → frames appear in `frame.raw` → touch injection → logs → no leftover processes after `stop`.

## GitHub Actions

`.github/workflows/build-linux.yml` builds the engine and the Linux package inside an Arch Linux container and runs the same Python tests. Containers cannot use bwrap, so full boot acceptance stays on local or self-hosted runners. The release pattern matches the macOS workflow: draft release, build number as “starting point + run number − 1”, checksums attached, and only `contents: write` permission.
