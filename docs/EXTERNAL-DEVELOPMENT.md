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
