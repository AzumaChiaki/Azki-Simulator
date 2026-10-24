# External developer sessions and saved instances

The bundled Python Runtime can run firmware for an external IDE without creating
a Swift UI broker. An IDE can prepare an instance, run the existing local control
socket service, read the real LCD frame, send input and use the guest installer.
The native app's **Connect Session** action can attach to that same session.
Opening the app itself does not automatically select an external session.

Persistent developer sessions now complete through the existing
`instance-storage-finish` command. Previously that operation required
`native-broker/config.json` and `state.json`, so an externally launched developer
session could run correctly but could not commit its instance storage.

## Lifecycle

Use the selected app's `Contents/Resources/Engine/python/bin/python3.12` and
`Contents/Resources/Runtime/azki-simulator.py`. Use the same Engine and library
roots throughout a lifecycle. The Runtime's `--help` describes the current
arguments and JSON responses.

1. `library-catalog` / `library-import` resolve local firmware assets.
2. `prepare-bundled` accepts an exact profile, a stable `--instance-id` and a new
   private `--session-root`. Supply the supported synthetic virtual-key handoff
   when virtual pairing is needed.
3. `serve <session> --no-discovery --approved-identity-sha256 <approvalDigest>`
   starts the existing control service. Retain the exact service process handle.
4. The authenticated local control protocol handles `start`, `status`, `input`,
   `stop` and guest installation. The shared-frame reader checks stable headers
   and exact device geometry before returning pixels.
5. Wait for `stop` to confirm process cleanup. Call `instance-storage-finish`
   with the same session, library and instance UUID; inspect `committed`.
6. Shut down only the service process owned by this lifecycle. A later
   `prepare-bundled` for the same UUID restores its committed disk snapshot.

Session roots must be private owned directories and short enough for nested Unix
socket paths. Retain failed sessions for diagnostics instead of deleting their
data or constructing ownership files manually. Firmware Native registrations
held only in RAM are not made persistent by disk snapshot support.

## Raw LCD frames

External integrations may import `azki_simulator.bridge.read_frame_raw(root,
expected=(212, 520), previous_sequence=None)` from this app's Runtime. It returns
`(sequence, width, height, pixels)`, where `pixels` is the guest's tightly packed,
row-major **little-endian RGB565** data (`width * height * 2` bytes). This avoids
Python RGB888 conversion and PNG compression on the interactive display path.
It does not request a new guest render or increase the firmware's frame rate.

An unsigned 32-bit `previous_sequence` may return `None` for an unchanged frame.
The reader still checks the owned, non-symlinked, single-link regular file,
bounded exact length, format, exact selected geometry and two stable even
headers before returning `None`. A new frame also requires a complete pixel copy
between matching headers. Scope sequence caches to the **session identity**;
after a restart, the same instance UUID can publish reused sequence numbers.

`read_frame(root, expected)` continues to return RGB888 snapshots with the same
quantization as before, and existing PNG screenshot consumers remain compatible.
Run `python3 -m unittest discover -s tests -p test_frame_raw.py -v` for the format,
ownership, link, length and concurrent-update regression tests.

## Completion evidence

The verified developer adapter publishes `developer-run.json` at the session
root, outside the guest's writable `run/` and instance-data paths. This is an
internal host record, not a public command accepting caller-supplied proof.
Only a non-fixture developer sandbox with a verified execution prefix and
authenticated runner/control identities can publish it.

The record binds the prepared approval digest, session and storage transaction,
direct run directory and its device/inode, the observed supervisor/runner/core
PIDs, adapter outcome and reason, cleanup state and exact result digest.
Publication uses a private file, file `fsync`, Darwin `renameatx_np(RENAME_EXCL)`
and directory `fsync`. It neither overwrites an existing record nor exposes
partial JSON or a multi-link final file if interrupted.

Before committing, storage independently rejects live or reused PIDs, remaining
process-group members, replaced directories, changed runner/result evidence and
failed adapter outcomes. Existing guest health, synchronous writeback, disk
identity, NOR and virtual-peer validation still apply. A host-side failure cannot
be overridden by an otherwise healthy guest result. The native broker path keeps
its existing validation.

`committed: true` confirms synchronous storage writes and these checks; it does
not claim that an arbitrary guest filesystem was cleanly unmounted.

## Regression tests

From the repository root on macOS:

```sh
python3 -m unittest discover -s tests -p test_developer_storage.py -v
```

Tests use actually reaped harmless processes and private temporary directories.
They cover live process rejection, exact identity and transaction binding,
replaced/symlinked runs, changed guest evidence, guest storage faults, host
failures with a healthy guest result, exclusive publication and interruptions
before/after atomic publication.

Build the app with the documented workflow in [BUILDING.md](BUILDING.md). Keep
Engine, firmware images, private instance data and local completion records out
of the source commit.

## Discoverable device parameters

The Runtime also exposes `instance-parameters`. It resolves the exact imported
profile, then combines supported board inputs with saved instance settings and
post-stop firmware property observations. It never opens a window, starts a
core, downloads firmware, or changes another instance.

```sh
"$ENGINE/python/bin/python3.12" -I -S -B "$RUNTIME/azki-simulator.py" \
  instance-parameters --engine-root "$ENGINE" --library-root "$LIBRARY" \
  --instance-id "$INSTANCE" --profile-id "$PROFILE"
```

Add `--session "$SESSION"` for authenticated live input readback. The session
must match both the requested UUID and exact profile before any IPC. Add
`--set '{"live.heartRate":90}'` to change one advertised field. A restart setting
example is `--set '{"boot.health.heartRate":90}'`; saving it does not restart an
instance. Do not send the same incremental action again after an uncertain
reply. Only the fixed existing board input and settings interfaces are used.

Responses have `schema_version: 1`, `instance`, `model`, `running`, `parameters`
and `notes`. Each parameter has `id`, `label`, `group`, `type`, `value`,
`available`, `readOnly`, `applyMode` (`live` or `restart`), and `description`.
Numeric fields include `minimum`, `maximum`, `step`; enumerations use
`options: [{value,label}]`. Unsupported fields explain `reason`. Null resets
only fields marked `nullable`; `writeOnly` actions have no current value.

`valueSource` distinguishes modeled hardware input, next-start configuration,
and the firmware's last stopped property dump. `requestedValue` is a saved
configuration, while `observedValue` is present only when the firmware property
dump actually reported it; `observedAt` is Unix seconds. Live pulse/battery
inputs are not firmware-display measurements. Battery percentage writes change
voltage and the firmware follows its own estimation curve; its visible percent
may change gradually. Raw ADC counters indicate register consumption, not that
an application displayed the requested result.

Successful writes return `applied`, `restart_required`, `guestObserved: false`
and a fresh `parameters` schema. `receipt` contains the underlying live reply.
A `refresh_error` after a successful write must not trigger automatic resending.

External launchers should use the returned `start_options`: pass `skipOOBE` as
`--skip-oobe` / `--no-skip-oobe` and, when explicitly configured,
`virtualIdentity` as `--virtual-serial` / `--virtual-address` to
`prepare-bundled`. Existing native app preferences are separate and unchanged.

| Native app controls | External schema / behavior |
| --- | --- |
| Device model, firmware, LCD dimensions | Read-only resolved identity/geometry; create a corresponding instance to change model |
| Instance display name | SDK instance-management metadata; no firmware property |
| Skip first setup | `boot.skipOOBE`, next start; existing settings/data preserved |
| Virtual serial/address | `identity.current` last saved public identity; `boot.virtualIdentity` validated JSON, next start |
| Worn, pulse, cadence | `live.*` when the exact running board reports support; `boot.health.*` for next start |
| Walk now / walk at boot | `live.addSteps` one-shot input; `boot.health.walkSteps` |
| Battery level/raw voltage, charger | `live.batteryLevel`/`batteryRaw`/`charger`; `boot.battery.*`; no claimed displayed battery percentage |
| Temperature | `live.temperatureC` only for verified board/thermistor mapping |
| PMU sampling channels | Eight `adc.*` words with read counts and input versions, only when the pinned core advertises support |
| Clock/date | `boot.clock.mode` and `boot.clock.offsetSeconds` |
| Language, 12/24 hour, timezone | Dynamically returned `property.persist.*` values |
| Every observed/custom persistent property | Union of firmware observations and saved overrides; `boot.propertyOverrides` allows adding/removing validated custom keys |
| Touch and swipe | Existing real LCD `input` interface; no synthetic Home page or fabricated physical key |
| Virtual guest connection | Read-only capability; dedicated pairing/install protocol performs operations |
| External Bluetooth advertising | Read-only unavailable with reason: managed by native CoreBluetooth permission host |
| GDB enable/wait/port | Read-only with reason: native host launch feature, not exposed by developer runner |
| Native connection timeout | Read-only with reason: native host preference; SDK install retains its explicit timeout option |
| Log follow/viewport zoom/panels | Host UI preferences, not device parameters |

Run `python3 -m unittest discover -s tests -p test_device_parameters.py -v`.

## Live IMU orientation

`live.orientation` adds a bounded, atomic **hardware input** for the modeled
LSM6DSO and ICM-42607 on N66, O66, N67, P67 and Q66 presets. It is available only
when the running core reports the `motion-orientation` board property and the
selected preset has a modeled IMU. Older cores continue to expose their existing
inputs and report orientation unavailable. This control does not execute Lua,
replace a sensor callback, write guest memory, or claim a guest observation.

```sh
"$ENGINE/python/bin/python3.12" -I -S -B "$RUNTIME/azki-simulator.py" \
  instance-parameters --engine-root "$ENGINE" --library-root "$LIBRARY" \
  --instance-id "$INSTANCE" --profile-id "$PROFILE" --session "$SESSION" \
  --set '{"live.orientation":{"rollDegrees":30,"pitchDegrees":-45}}'
```

The same selected private session supports
`call "$SESSION" live-set --arguments '{"orientation":{"rollDegrees":30,"pitchDegrees":-45}}'`.
An orientation write returns `{accepted:true, guestObserved:false, state:...}`.
If the board accepted the write but readback failed, the receipt instead carries
`stateUnavailable:true`; it remains a successful write. An uncertain transport
outcome is `live_outcome_unknown` and must not be automatically retried. The
runtime does not perform a second live refresh after this board acknowledgement.
`instance-parameters` separately refreshes its schema, retaining `applied` and
the receipt if that later refresh fails.
Use `{"live.orientation":null}` (or `{"orientation":null}` for `live-set`) to
release the held pose and restore the automatic motion model. A fresh process
starts in automatic mode; this live input is not persisted as an instance setting.

Both fields are required, finite numbers within **-180 to 180 degrees**; booleans,
strings, missing/extra fields and non-finite values are rejected before any write.
The parameter schema uses `type: "object"`, `nullable: true`, `units: "degrees"`
and two bounded numeric `fields`. A null current value means automatic mode when
the capability is available; it does not mean a measured zero angle. As with
other live inputs, `guestObserved` remains false and `observedValue` remains null.

The band body frame has +Z out through the display. For roll `r` and pitch `p`,
the held pose generates the acceleration vector
`(-sin(p), sin(r)*cos(p), cos(r)*cos(p))` in **g**, then applies the unchanged
board installation transform and the chip's configured scale/ODR/register/FIFO
path. Thus `(0,0)` gives +Z, `(90,0)` +Y and `(0,90)` -X before installation.
N67/P67 retain their `+y+x-z` chip mount. The firmware still performs its own
axis conversion and sensor publication; Lua `sensor_accel` units must be checked
against that firmware rather than inferred from the host input unit.

Holding a pose pauses the synthetic walk/handling progression and retains the
existing small sensor noise. Reset resumes automatic motion. This is a sequence
of held poses, not an inertial rotation solver: it does not synthesize the angular
velocity of transitions. It is suited to acceleration/gravity-driven UI tests.
The Q65/P65 sensor-CPU machines are not covered by this shared IMU model.

Run `python3 -B -m unittest discover -s tests -p test_orientation.py -v` for
parameter-to-board round trips, unsupported/old core behavior, invalid atomic
writes, uncertain-write no-retry behavior, and the production C sample generator
with both IMUs, six body axes, an oblique vector, board mount and auto reset.
