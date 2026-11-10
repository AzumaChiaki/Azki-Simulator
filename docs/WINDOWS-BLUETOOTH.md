# Windows Bluetooth backends

The Windows port has two distinct Bluetooth paths. They must not be reported as
equivalent firmware support.

* **WinRT radio / guest ATT proxy** uses the Windows-selected Bluetooth adapter.
  It implements BLE advertisement reception, bounded advertisement publication,
  explicit central connections, service/characteristic/descriptor discovery,
  reads/writes, notifications/indications, and a local GATT service provider.
  When a genuine guest HCI/ATT endpoint is running, the existing `external-*`
  protocol forwards its GATT operations and application bytes through the local
  provider. Windows and the virtual guest hop terminate separate links/security.
* **Dedicated WinUSB controller** carries actual HCI Command/ACL/Event packets
  between the original guest and a selected physical USB controller. It uses
  QEMU's private authenticated Windows named pipe. It does not run WinRT GATT or
  a synthetic guest SMP controller on this path. The current implementation
  does not carry SCO or ISO and is not a complete Bluetooth profile emulator.

Existing QEMU cores must actually reach their HCI transport for either guest
path to be useful. A host scan, a published service or the existence of a QOM
property does not establish that a particular firmware has booted or paired.

## Build and runtime

`engine/windows-bluetooth/AzkiBluetoothHost` is a .NET 10 Windows console helper
with a bounded JSON-lines protocol on its owned stdin/stdout. Build on Windows:

```powershell
dotnet publish engine/windows-bluetooth/AzkiBluetoothHost -c Release `
  -o engine/windows-bluetooth/publish --self-contained false
```

Copy the **entire** publish folder, including the WinRT dependency DLLs. Copying
only `AzkiBluetoothHost.exe` is insufficient. This framework-dependent build
requires a .NET 10 runtime. The portable Windows distribution includes that
runtime in the sibling `dotnet` directory; the broker sets `DOTNET_ROOT` and
`DOTNET_ROOT_X64` only in its owned helper's environment when `dotnet.exe` is
present there. It does not change the user's environment or system installation.
A distributable may instead publish an explicitly tested self-contained RID;
do not claim there are no runtime dependencies.

The Python service resolves `AZKI_WINDOWS_BLUETOOTH_HOST` or the repository's
`engine/windows-bluetooth/publish/AzkiBluetoothHost.exe`. Construction is inert.
It starts one owned helper only when an explicit operation is submitted.
The helper serializes central, peripheral and watcher/publisher operations in
separate domains. Peripheral ATT replies are independent, so a slow connection
attempt cannot hold up a physical client's pending read/write response.

```python
from src.windows_bluetooth import WindowsBluetoothService

radio = WindowsBluetoothService(host_executable=helper_path, guest_rpc=peer.call)
try:
    capabilities = radio.dispatch({"action": "capabilities"})
    radio.dispatch({"action": "scan.start", "active": True,
                    "serviceUUIDs": ["a-known-owned-service-uuid"]})
    # Display radio.dispatch({"action": "events"})["events"] in the UI.
finally:
    radio.close()
```

The helper never silently pairs, changes a driver, changes the system's radio
name, toggles the system radio, or connects to every discovered device. Central
connections require an explicit address selected by the user. Closing a device
detaches its connection/notification callbacks and releases this application's
service/device references, not other apps' links. Failed subscriptions detach
newly added callbacks instead of retaining a false active subscription.

## Runtime request contract

The runtime wraps a flat Bluetooth action in
`{"id":"...","command":"bluetooth","args":{"action":"...",...}}`.

| Action | Fields / result |
| --- | --- |
| `capabilities`, `status` | Actual host adapter/operation state; separate guest and raw-controller status |
| `adapters` | Windows-visible BLE adapters; no automatic driver change/selection |
| `events` | `limit` 1..1024; returns bounded telemetry events |
| `scan.start` | `active`, `extended`, optional `serviceUUIDs` |
| `scan.stop` | Stop only this host's watcher |
| `advertise.start` | `manufacturer:[{companyID,value:hex}]`, permitted `sections:[{type,value:hex}]`, `extended` |
| `advertise.stop` | Stop only this host's publisher |
| `central.connect` | Explicit `address`, `addressType:public/random/unspecified`; returns owned `deviceID` |
| `central.discover` | `deviceID`; returns service/characteristic instance keys, UUIDs and properties |
| `central.read`, `central.write` | `deviceID`, characteristic `key`; write also `value:hex`, `withResponse` |
| `central.subscribe` | `deviceID`, `key`, `enabled`, optional `indicate` |
| `central.descriptors` | `deviceID`, characteristic `key`; returns descriptor instance keys |
| `central.read-descriptor`, `central.write-descriptor` | `deviceID`, descriptor `key`; write also `value:hex` |
| `central.disconnect` | Release this application’s owned device and services |
| `peripheral.start` | Genuine `guest_rpc` required; optional `localName`, `timeout`, `allowServiceOmissions` |
| `peripheral.stop` | Stop the guest proxy and its provider; preserve unrelated OS/app resources |
| `hci.usb.list` | Read-only present interfaces registered under the dedicated Azki GUID; no device open, HCI or driver operation |
| `hci.usb.inspect` | No `devicePath`: software/DLL inspection only; explicit already-WinUSB `devicePath`: descriptor inspection; no HCI commands |

`WindowsBluetoothService` owns a 30 ms background guest pump after
`peripheral.start`. It processes physical ATT requests, guest results and
notifications even when the UI never requests `events`. Telemetry is only a
copy; it is not a Bluetooth transport. Stop/EOF closes the session and owned
helper. Submitted writes are not retried after an unknown outcome. Requests and
late callbacks are scoped to a generation and one admitted logical peer.

The genuine guest RPC callback receives complete schema1 `external-open`,
`external-status`, `external-request`, `external-result` and `external-close`
objects and returns the unwrapped result or an error. It must be independently
thread-safe and bounded; the main JSONL/UI loop is not its scheduler. Runtime
shutdown stops Bluetooth before destroying the peer endpoint/core.

## Limits that affect firmware compatibility

The [WinRT GATT API does not expose raw transport](https://learn.microsoft.com/en-us/windows/apps/develop/devices-sensors/gatt-client).
It cannot forward arbitrary guest HCI commands, reproduce a controller's
vendor-specific behavior or carry arbitrary SMP/link keys through Windows.

Windows reserves [GAP, GATT, Device Information and Scan Parameters services](https://learn.microsoft.com/en-us/windows/apps/develop/devices-sensors/gatt-server):
UUIDs `1800`, `1801`, `180a`, `1813`. The proxy rejects a guest database containing
these by default. Explicit `allowServiceOmissions:true` permits omission and
returns the exact omitted UUID/key list; this is a changed physical database,
never an exact clone. Authentication/application bytes for forwarded operations
are not rewritten or replaced with synthetic success.

The [advertisement publisher reserves system-managed AD types](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.advertisement.bluetoothleadvertisementpublisher).
This includes flags, names, service UUID lists and service-data types. The helper
rejects them instead of removing them and claiming the original payload was
sent. Connectable service advertisements use `GattServiceProvider`; physical
names, handles, advertising scheduling and ATT MTU are Windows-managed. An
accepted publication/notification is not proof of reception or application ACK.

Physical writes larger than the guest ATT MTU are rejected with an ATT error;
arbitrary application values are not split into guessed SAR frames. Prepared
writes/nonzero write offsets are not supported by the current guest proxy.
Oversize notifications are rejected rather than truncated. Full firmware
behavior should use a compatible dedicated raw controller when these
terminating-proxy differences matter.

## Dedicated controller lifecycle and driver state

The WinUSB module accepts only an explicit USB device-interface path and checks
the actual Bluetooth interface/endpoints. Legacy USB uses class control for HCI
commands, interrupt-IN for events and bulk for ACL; it is not UART H4 on USB.
Asynchronous buffers remain alive until exact owned I/O has completed or been
cancelled/drained. Failures latch the session terminal; no packet is replayed.

The selector lists only present interfaces already registered with
`{a4b85fea-0ae3-4be1-b238-9dca9ed299a8}`. It reads their instance, name and driver
metadata through SetupAPI without opening a WinUSB handle. Other USB/Bluetooth
classes are not scanned. An empty list is honest: it can mean the dedicated
registration/device is absent, so `driverBindingRequired` is unknown, rather
than inventing a controller path. Metadata-only candidates that are registered
but not WinUSB-bound are returned separately as `unavailableDevices`. Listing
does not prove endpoint, HCI, radio or firmware compatibility. The runtime routes
both inspection actions independently of WinRT/guest construction.

An advanced explicitly entered path is resolved by
`SetupDiOpenDeviceInterfaceW` and device-detail APIs before any WinUSB open.
Controller identity comes from the actual PnP instance, not from parsing a
symbolic link. Different interface paths for the same instance therefore share
one lease. The lease is an empty regular file opened with no sharing under the
Windows KnownFolder `LocalAppData/AzkiSimulatorRawControllers/v1`; it is scoped
to the current user across processes and sessions. It uses TokenUser ownership,
a protected private DACL and no-reparse/single-link checks. Neither an instance
workspace nor an environment variable can choose another production lock root.

The native USB object owns that lease until overlapped requests drain and both
WinUSB and file handles really close. Failed cancellation/free/close retains the
controller, buffers and lease in a terminal quarantine until the owner process
exits. Another simulator cannot take over by changing interface GUID/case.
Initial setup failures also retain their cleanup evidence. The runtime descriptor
reports `controllerInstanceID`; `controllerAddress` remains null with
`addressSource:not-queried` until an actual controller address is measured.
It never substitutes a virtual factory address.
The bridge's `observedAddress` statistic changes only for a successful, full
six-byte physical Read BD_ADDR reply matching an outstanding guest query;
`observedAddressSource` then reads `physical-hci-read-bdaddr`. Unsolicited,
failed, malformed and local replies cannot create this evidence. An observation
does not automatically create a persistent controller/bond binding.

Trusted runtime integration uses `prepare_raw_hci(device_path,reset_on_stop=True)`
before QEMU Popen, merges the descriptor's `qemuEnvironment`, then calls
`attach_core(owned_popen)` after the owned core/job is registered. The protected
pipe verifies the actual QEMU PID, private DACL and opposite-side client PID.
An external JSON request cannot supply a PID or replace the Popen registry.
The research loopback-TCP bridge is unauthenticated and is not the production
fallback.

The controller bridge rejects every vendor OGF `0x3f` command locally with
Unknown HCI Command (`0x01`); it never forwards a BES vendor opcode to another
vendor's controller. Standard commands are serialized and wait for the actual
matching physical Command Complete/Command Status plus available command credit.
ACL traffic has an independent ordered queue and continues while commands wait.
A local vendor rejection is queued after preceding physical completions, returns
at most one already-observed available credit, and does not modify physical
credits or count as a controller acknowledgement. A vendor command arriving
before any actual command-credit observation fails closed. There is no vendor
allowlist/bypass switch; adding a vendor implementation requires documented
semantics and a separate verified controller-specific path. Bounded diagnostics
label these failures `local-unsupported-vendor` and `physicalSent:false`.

Normal stop attempts exactly one standard controller Reset only after a verified
session and sufficient observed command credit. It waits for an attributable
real Command Complete. If I/O/credits/acknowledgement prevent this, status remains
`radioTeardownVerified:false`; closing USB handles alone does not prove the
controller stopped its last RF activity. The dedicated dongle may need removal
when a certain RF-off state is required. No reset is sent by metadata inspection.
`close()` returns and preserves `close_report`, including the final controller
statistics and separate resource-cleanup and radio-teardown results. Runtime
exit receipts retain this report even after the service is discarded. A failed
pipe/controller close cannot become a successful cleanup merely because the
worker recorded its error without throwing an exception.
Outstanding guest commands block stop Reset until an ordered physical drain.
A late Command Complete for the guest's Reset cannot acknowledge the subsequent
stop Reset. Final statistics preserve `outstandingCommands`,
`teardownCommandPending` and `commandOutcomeUncertain`; resource closure is
reported independently of unresolved command/radio outcomes.

`Plan-DedicatedUsb.ps1` defaults to inspection/backup, requires an explicit exact
USB `InstanceID`, and never binds a driver. It has no machine-specific target or
VID/PID scan default. Apply additionally requires the reviewed `ExpectedHardwareID`
and an exact `ConfirmInstanceID`. It preserves original driver and interface
GUID metadata and supplies normal signed-inbox WinUSB/manual rollback steps.
Any actual binding/registry application requires review and normal Windows
administrator authority. It does not touch the Intel adapter, delete pairings,
install an unsigned INF/certificate or disable signature/SecureBoot policy.

The explicit `windows_bluetooth_usb.py --probe-controller --device-path ...`
operation sends one Reset followed by bounded capability reads driven by real
command credits. It does not advertise, scan, connect or invent vendor replies.
Never infer the dongle's HCI version/features merely from VID/PID/REV.

`windows_bluetooth_usbdk.py` is a prepared alternative transport, currently not
wired into runtime mode selection. It checks only the normal installed helper
under KnownFolder ProgramFiles, requires a trusted helper signature, maps the
actual exact USB PnP instance, and acquires the same controller lease before an
explicit redirect. It never installs UsbDk or uses an extracted/private fallback
DLL. Only fixture/native-inspection tests have run. Installing UsbDk is a separate
system change: its official installer adds a USB class filter and can reset USB
host controllers. Metadata availability is not authorization to install or
redirect a device. SCO/ISO and physical-controller acceptance remain unverified.

## Verified on the development host

On the authorized Windows 11 x64 build26200 host:

* .NET10.0.403 compiled/published the helper with zero warnings/errors.
* Native WinRT reported the Intel radio On with central/peripheral/LE and
  extended-advertising support. One six-second scan received119 advertisements
  from26 addresses; receipts preserve deidentified statistics.
* A self-owned manufacturer advertisement reached Publisher `Started/Success`.
  A test GATT service also reached provider `Started/Success`; neither alone is
  an over-air delivery or firmware-pairing result.
* Windows Python3.13 first ran66 host/protocol/USB/bridge tests, then the updated
  Read_BD_ADDR/trusted-lifecycle version ran70 tests successfully, followed by
  71 tests including the isolated portable-runtime environment check. The final
  shutdown-evidence/strict Command Status version passed75 tests. Subsequent
  dedicated-interface/identity-lease versions passed79 and91 tests. A real Win32
  regular-file proof refused a second process using a case alias, released from
  an I/O thread, and allowed a later process to reacquire the same identity;
  that proof opened no USB/radio handle. A separate
  real WinUSB DLL inspection opened no device and sent no HCI commands.
* Production private guest peer plus a separate H4/ATT **fixture process** drove
  the real WinRT service provider for10 seconds without UI polling (294 guest
  status calls). This proves the platform plumbing, not an original firmware
  boot, SMP pairing, or a physical central's ATT/notification acknowledgement.
* The final duplex helper passed native build, concurrent JSONL response and
  bounded EOF checks. Its RF regression received92 advertisements from21
  addresses and again reached Publisher `Started/Success`.

The external `0a12:0001` dongle initially had Microsoft `BTHUSB/bth.inf` and PnP
problem31; original GUID values were absent. Inspection/backup did not fix or
rebind it. Raw physical controller/firmware tests and SCO/ISO remain separate
acceptance work. The current `firmwareCompleteBluetooth` flag is deliberately
false until the complete profile-specific path is actually demonstrated.
