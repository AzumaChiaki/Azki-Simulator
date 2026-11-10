#!/usr/bin/env bash
set -euo pipefail
pacman --noconfirm -Syuu
pacman --noconfirm -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-glib2 mingw-w64-ucrt-x86_64-pixman mingw-w64-ucrt-x86_64-zstd mingw-w64-ucrt-x86_64-dtc mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-packaging mingw-w64-ucrt-x86_64-meson mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf make diffutils git tar
