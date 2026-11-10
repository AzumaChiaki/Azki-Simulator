#!/usr/bin/env bash
set -euo pipefail
pacman-key --init
pacman-key --populate msys2
pacman --noconfirm -Syuu
