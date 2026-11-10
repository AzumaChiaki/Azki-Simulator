# Windows

The native Windows port uses the same three QEMU boards (`best1503`,
`azki-p65`, `azki-q65`) and firmware catalog as the macOS application. The
Windows desktop interface is a Python/Tk application. SwiftUI is not a Windows
dependency. Firmware is supplied by the user and is never included in the
portable package.

## Build

Use Windows 11 x64, a private MSYS2 UCRT64 installation, Python 3.13 with Tk,
Pillow and littlefs-python, and a private .NET 10 SDK. Required UCRT64 packages
include GCC, GLib, pixman, zstd, libfdt, Python, packaging, Meson, Ninja, and
pkgconf. The MSYS environment must be `UCRT64`, not `MSYS` or `MINGW64`.

Use the exact QEMU 10.1.0 source archive:

```powershell
./scripts/build-windows.ps1 -ToolsRoot C:/AzkiTools `
  -WorkRoot C:/AzkiBuild -SourceArchive C:/Downloads/qemu-10.1.0.tar.xz
```

`ToolsRoot` contains `msys64`, `python`, and `dotnet` directories. The wrapper
first publishes the Bluetooth host with the private .NET 10 SDK, its own CLI
home and automatic ASP.NET development-certificate generation disabled. The build
verifies the QEMU archive SHA-256, preserves a private marked build cache, builds
the real native Windows executable and three storage helpers, and resolves the
portable DLL dependency closure. Internal source archive symlinks become file
copies; no Developer Mode or system symlink privilege is needed. QEMU's optional
macOS X11 development link is omitted.

The output is `dist/windows`. Extract or copy its complete directory and run
`AzkiSimulator.cmd`. Python/Tk/Pillow and the Bluetooth helper's .NET runtime are
included. Retain the licenses and corresponding source directories when copying
the package.

## Firmware and control

Select an exact catalog version and its full OTA. For an additional firmware
version, select the matching hardware import preset. A raw AP also needs its
matching resource image; P65/Q65 require their own sensor image. A selected
catalog AP must match its recorded hash. Images from different products are not
interchangeable.

P65 local setup follows the existing macOS recipe: it creates a separate AP
boot copy with local ROMFS properties. The original imported AP remains intact;
the preparation receipt records its hash, the boot-copy hash and changed ranges,
and execution reports `firmwareModified: true`. P65's latest raw 256 KiB
factory-data format is unverified, so WinRT virtual identity mode is explicitly
unavailable for that model. It is never replaced with Q65 LittleFS or the
historical 128 KiB synthetic layout. Disabling local setup keeps the original
P65 AP in offline mode.

The frontend displays the firmware's actual shared framebuffer, preserving
RGB565 or RGB888 precision and each panel's geometry. Pointer input uses QMP
hardware touch events. Watch buttons and crown input are available on the
modeled watch boards. An input acknowledgement only proves that QEMU accepted
the event; it does not assert that a guest application handled it.

The same controller is available over bounded UTF-8 JSONL:

```powershell
cd engine/project/backend
python -m src.windows_runtime --project-root .. `
  --qemu ../../../dist/windows/qemu-system-arm.exe --workspace C:/AzkiInstances
```

Requests have `id`, `command`, and `args` fields. Commands include `profiles`,
`prepare`, `start`, `status`, `frame`, `touch`, `button`, `crown`, `input`,
`instances`, `bluetooth`, and `stop`. `profiles` works without a built core or
imported firmware. Closing the
controller closes its own firmware processes using a Windows Job Object.

Runtime state uses current-user ownership, inherited private Windows ACLs,
regular-file identity checks, no-reparse path validation, real file locks, and
coherent shared mappings. Elevated OpenSSH sessions can otherwise default file
ownership to Administrators: the runtime selects its actual TokenUser as the
default creator owner only within its own process. This does not change machine
policy or grant privileges.

## Saved instances and sensors

Choose a temporary session or create a saved instance. The first import fixes
its exact firmware, board geometry and local-setup choice. Reopening an
initialized instance requires only its instance ID and Bluetooth transport;
firmware inputs are retained and checked, and storage is not seeded again.
Each run uses a separate writable copy. A healthy owned exit jointly publishes
NAND, OOB, NOR and supported virtual identity state through one atomic HEAD.
This provides crash consistency, not a claim that the guest filesystem was
cleanly unmounted. Failed or uncertain runs retain the previous HEAD and expose
unpublished work for explicit recovery. Deleting an idle instance moves it to
the owned trash directory.

Capabilities are exact-profile and core-property dependent. P65 currently saves
compact NOR, including original BES audio, while its NAND remains RAM-only and
its factory identity remains unsupported. Raw USB instances belong to a
separate transport domain and the same PnP controller; they never import virtual
SMP keys or synthetic peer state. An unknown physical controller address remains
unknown. Stored raw NOR does not itself prove a real bond/reconnect works.

The sensor panel routes battery/ADC, charging, health, walking and orientation
through the existing validated QMP hardware controls. Unsupported model
properties remain unavailable. A model readback or accepted input is not proof
that firmware consumed that sensor update. A timeout after a submitted write
is reported as an unknown outcome and is never silently retried.

## Bluetooth

See [WINDOWS-BLUETOOTH.md](WINDOWS-BLUETOOTH.md) for the two distinct radio paths:
Windows WinRT roles and an explicitly selected dedicated USB HCI controller.
The USB path needs an appropriate controller interface and driver; a device's
presence alone does not mean that it is usable. Serial HCI is not currently
implemented and returns an explicit unavailable error.

Capabilities, scans, GATT operations, guest protocol fixtures, actual firmware,
and physical RF acknowledgements are separate acceptance boundaries. A successful
build or static catalog entry must not be reported as firmware boot, GPU effect,
pairing, or complete firmware Bluetooth verification.

## Validation

Host contract checks are `tests/test_windows_runtime.py` and
`tests/test_windows_qmp.py`. `tests/test_windows_runtime_io.py` exercises real
Windows ACLs, file sharing, two-view RGB565/RGB888 mappings, handle lifetimes, and
kill-on-close of an exact owned child. The latter requires actual Windows; it
does not substitute for firmware acceptance.

Native acceptance records should retain the core, AP and resource hashes,
profile/geometry, UART and QEMU logs, real frames, input results, fault registers,
and owned-process exit receipt. Verify every catalog version separately, even
when AP hashes are shared. An untested profile remains unverified.

The initial Windows core (`ff424752…`) executed all 39 catalog entries with
visually reviewed normal watchfaces, zero final CFSR/HFSR, unchanged original
AP/core hashes and clean owned exits. Those records are historical; a later
main/core requires its own acceptance matrix. P67 needs about a minute before
its normal watchface appears. N67 .187 ordinary unchanged-AP display testing is
separate from an old stopped SDK child-status/waitpid experiment; the latter is
not part of the Windows runner.

The latest-main Windows core is `db086b4e…` (main `fa81077` plus the reviewed
horizontal LINEAR GPU correction). Its Windows source passed native contract,
protocol, storage, Bluetooth and Win32 tests. Original Q66TC firmware passed two save/reopen runs
with genuine NAND/NOR writes and unchanged virtual identity; the actual Tk
application also passed two launches with joint saves and nested owned Jobs.
P65 passed a separate two-run NOR-only test using its disclosed local-setup boot
copy. A natural early core exit preserved the previous HEAD, exposed pending
work and released its lease for explicit recovery.

A separate latest-core run executed all 39 catalog versions, each with an
unchanged original AP, real normal watchface, zero final CFSR/HFSR and clean
owned exit. The user's Q65 OTA also displayed its normal clock and GPU-rendered
application grid: 186 submitted GPU commands completed with zero rejection.
The guest crown driver consumed three input samples; that scene did not verify
a visible scroll after those samples.

Keep the exact per-build catalog matrix and its real frames alongside any
release report. A normal frame does not verify every GPU operation, long-term
firmware behavior or physical Bluetooth. Physical central pairing, SMP/bonding,
a usable dedicated USB controller and complete firmware Bluetooth remain
unverified.
