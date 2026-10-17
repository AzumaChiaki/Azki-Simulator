# Building Azki Simulator

[简体中文](BUILDING.md) · [Home](../README.en.md)

## Requirements

- Apple Silicon Mac running macOS 15 or later.
- Full Xcode with Swift 6.2 or later. Command Line Tools alone may lack the SwiftUI macro plugins.
- Homebrew build dependencies: `python@3.12 meson ninja pkgconf glib gettext libffi pcre2 zstd dtc`.
- Internet access for the initial download of public upstream sources and Python build dependencies.

End users do not need these development dependencies.

## Output policy and Debug builds

Every build puts its final artifacts directly in the repository's `dist/` directory. Only deliverable apps, DMGs and corresponding source archives belong there, never caches, logs, downloads or intermediate files. Work happens in a dedicated temporary directory outside the repository and is removed on success, failure or ordinary interruption (except SIGKILL, power loss or system crashes). No `.build/`, `.build-debug/` or `build/` workspace is retained.

```sh
./scripts/build.sh debug
```

The application is `dist/Azki Simulator.app`, accompanied by matching source archives. Use `./scripts/build.sh release` for Release. Both commands build the complete engine from source without firmware. Do not use a bare `swift build`, which leaves `.build` behind. With an existing complete engine, `AZKI_BUILD_CONFIGURATION=debug ./scripts/build-app.sh /absolute/path/Engine` packages just the app, still exclusively into `dist/`.

Custom final output directories are not supported. Move or clean an existing artifact before rebuilding; scripts do not silently overwrite it.

## Create a DMG

```sh
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
export PATH="$(brew --prefix python@3.12)/libexec/bin:$PATH"
./scripts/build-dmg.sh dist
```

The script builds the customized QEMU core, embedded Python and required dependencies from source, then builds the native app and creates a DMG. Missing engine components fail the build instead of producing a UI-only package. Move or clean existing artifacts before rebuilding.

| Option | Purpose |
| --- | --- |
| First argument to `build-dmg.sh` | Optional; only this repository's `dist` is accepted |
| `AZKI_BUILD_CONFIGURATION` | `debug` / `release` configuration for `build-app.sh` |
| `AZKI_BUILD_JOBS` | Swift build parallelism; defaults to 2 |
| `DEVELOPER_DIR` | Full Xcode developer directory to use |

`AZKI_WORK_DIR` and `AZKI_BUILD_DIR` are no longer supported, preventing persistent work directories. Use `python3 -B scripts/build-engine.py` for a standalone engine: the defaults are `dist/Engine` and `dist/Engine-sources.tar.gz`, and both outputs must be direct children of `dist/`.

`VERSION` and `BUILD_NUMBER` populate `CFBundleShortVersionString` and `CFBundleVersion`. Local builds use the file value. GitHub Actions treats it as a base and increments automatically as base + run number − 1, writing only on the runner, never back to the repository; raise `BUILD_NUMBER` to skip ahead.

## Outputs

- `Azki Simulator.app`: the runnable native application.
- `*-debug-source.tar.gz` / `*-release-source.tar.gz`: project source for Debug / Release builds.
- `*-debug-engine-sources.tar.gz` / `*-release-engine-sources.tar.gz`: corresponding engine/dependency sources and licenses.
- `*-unsigned.dmg`: the native application and an Applications shortcut.
- `*-unsigned-source.tar.gz`: the exact product source and build instructions.
- `*-unsigned-engine-sources.tar.gz`: corresponding engine/dependency sources and licenses.

Provide the matching source archives when distributing the DMG. Neither the DMG nor the app is Developer ID signed or Apple notarized. The build applies certificate-free ad-hoc signatures only to satisfy Apple Silicon code-loading requirements.

The firmware catalog contains model/import metadata only. It excludes AP and OTA payloads, resource images, real-device identities and vendor SDKs. Import your own firmware into the application's local private library. Keep local instances, identity files, private keys, Apple passwords and development logs out of the source tree.

## GitHub Actions

`.github/workflows/build-dmg.yml` runs on pushes to `main`, including merge commits, squash merges, rebase merges and direct commits. It also supports manual `workflow_dispatch` runs.

The workflow uses an Apple Silicon macOS 15 runner with Xcode 26.2. It builds from source and publishes a Release tagged `v<version>-build<number>`: the DMG is a standalone asset for direct download, and the GPL corresponding sources are separate optional assets. The Release is created as a draft and published only after every upload succeeds. Its token needs only `contents: write`. It creates no commits, changes no project membership and uses no signing or notarization credentials. Re-running the same run keeps its build number and replaces that Release's assets.

See the [official runner image inventory](https://github.com/actions/runner-images/blob/main/images/macos/macos-15-arm64-Readme.md) for the software available on GitHub's actual runners.
