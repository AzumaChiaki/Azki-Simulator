# Building Azki Simulator

[简体中文](BUILDING.md) · [Home](../README.en.md)

## Requirements

- Apple Silicon Mac running macOS 15 or later.
- Full Xcode with Swift 6.2 or later. Command Line Tools alone may lack the SwiftUI macro plugins.
- Homebrew build dependencies: `python@3.12 meson ninja pkgconf glib gettext libffi pcre2 zstd dtc`.
- Internet access for the initial download of public upstream sources and Python build dependencies.

End users do not need these development dependencies.

## Create a DMG

```sh
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
export PATH="$(brew --prefix python@3.12)/libexec/bin:$PATH"
./scripts/build-dmg.sh dist
```

The script builds the customized QEMU core, embedded Python and required dependencies from source, then builds the native app and creates a DMG. Missing engine components fail the build instead of producing a UI-only package. If a DMG with the same name already exists, choose a new output directory or move the previous artifact yourself.

| Option | Purpose |
| --- | --- |
| First argument to `build-dmg.sh` | Output directory; defaults to `dist` |
| `AZKI_WORK_DIR` | Parent directory for temporary build work; defaults to `build` |
| `AZKI_BUILD_DIR` | Swift build cache directory |
| `AZKI_BUILD_JOBS` | Swift build parallelism; defaults to 2 |
| `DEVELOPER_DIR` | Full Xcode developer directory to use |

`VERSION` and `BUILD_NUMBER` populate `CFBundleShortVersionString` and `CFBundleVersion`. The build number starts at 1 and is incremented explicitly by the maintainer. GitHub run numbers do not rewrite the repository or the build number.

## Outputs

- `*-unsigned.dmg`: the native application and an Applications shortcut.
- `*-unsigned-source.tar.gz`: the exact product source and build instructions.
- `*-unsigned-engine-sources.tar.gz`: corresponding engine/dependency sources and licenses.

Provide the matching source archives when distributing the DMG. Neither the DMG nor the app is Developer ID signed or Apple notarized. The build applies certificate-free ad-hoc signatures only to satisfy Apple Silicon code-loading requirements.

The firmware catalog contains model/import metadata only. It excludes AP and OTA payloads, resource images, real-device identities and vendor SDKs. Import your own firmware into the application's local private library. Keep local instances, identity files, private keys, Apple passwords and development logs out of the source tree.

## GitHub Actions

`.github/workflows/build-dmg.yml` runs on pushes to `main`, including merge commits, squash merges, rebase merges and direct commits. It also supports manual `workflow_dispatch` runs.

The workflow uses an Apple Silicon macOS 15 runner with Xcode 26.2. It builds from source and uploads the DMG and source archives with a 30-day retention period. Its token has only `contents: read` permission. It creates no commits or Releases, changes no project membership and uses no signing or notarization credentials.

See the [official runner image inventory](https://github.com/actions/runner-images/blob/main/images/macos/macos-15-arm64-Readme.md) for the software available on GitHub's actual runners.
