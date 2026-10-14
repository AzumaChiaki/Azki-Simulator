# Engine source and licenses

Azki Simulator's host and project-owned runtime code are licensed under GPL-2.0.
Third-party files retain their original notices and license terms. The root
license does not replace those terms.

| Component | Source | License |
| --- | --- | --- |
| QEMU 10.1.0 | https://download.qemu.org/qemu-10.1.0.tar.xz | GPLv2 overall; individual files retain their own terms |
| BES1503 model and storage extensions | `qemu-overlay/` | GPL-2.0-or-later, as marked in files |
| CPython 3.12.10 | https://www.python.org/ftp/python/3.12.10/Python-3.12.10.tar.xz | PSF and historical licenses in `LICENSE` |
| Pillow 12.3.0 | https://pypi.org/project/pillow/12.3.0/ | MIT-CMU and included notices |
| littlefs-python 0.19.0 | https://pypi.org/project/littlefs-python/0.19.0/ | BSD-3-Clause; included littlefs retains BSD-3-Clause |
| YAFFS2 | `third-party/yaffs2/` | GPLv2, see `LICENSE` |
| UnQLite | `third-party/unqlite/` | BSD-2-Clause |
| MiConnect protocol reference | `project/backend/src/virtual_peer/MIT-miconnect.txt` | MIT |
| GLib/GIO/GObject/GModule | Homebrew formula source recorded by build | LGPL-2.1-or-later |
| gettext/libintl | Homebrew formula source recorded by build | LGPL-2.1-or-later for libintl |
| libffi, PCRE2, zstd, libfdt, xz/liblzma | Homebrew formula sources recorded by build | Original per-component licenses, shipped with build |

The build creates an Engine source archive alongside the binary. It contains
the exact downloaded QEMU and Python sources, the applied overlay, helper
sources, Python package source distributions, native-library source archives,
formula metadata and build scripts. Generated binaries, firmware, SDKs,
identities, user instances and development logs are not source inputs.

`catalog.json` contains model/version recipes only. Firmware and resource bytes
are excluded; the user must import their own firmware. Metadata does not grant
redistribution rights to those files. Native-library source retrieval must
succeed before the build publishes its result.
