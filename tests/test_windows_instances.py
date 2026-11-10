"""Transactional storage checks. Native cases use real Win32 private files.

Synthetic bytes and protocol fixtures are deliberately not firmware, physical
pairing, or guest filesystem acceptance. No radio or subprocess is started.
"""
from __future__ import annotations
import copy
import ctypes as c
from ctypes import wintypes as w
import gc
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "engine/project/backend"))
from src.windows_instances import (WindowsInstanceStore, InstanceStorageError,
                                   validate_binding, validate_health, _decode, _encode)

PROFILE = "miwear.watch.q66tc@4.100.155@f1c824fef81d"
AP = "ea0bdf1920cb30223d616432af00565ca67622e6468328f5eab155f8cdc2fb9f"
MODEL = "miwear.watch.q66tc"
GOOD = dict(ownedChildClosed=True, exitCode=0, cleanupComplete=True, forced=False,
            guestFault=False, qmpHealthy=True, storageHealthy=True, peerHealthy=True)


def binding(identity=True, raw=False):
    expected = {"nand-data.bin": {"bytes": 131072}, "nand-spare.bin": {"bytes": 4096}}
    expected["guest-state/guest-nor.bin" if identity else "nor-config.bin"] = {"bytes": 16384}
    if identity:
        expected.update({"guest-state/virtual-peer-state.json": {"maxBytes": 4096},
                         "guest-state/guest-state.json": {"maxBytes": 8192}})
    if raw:
        expected["raw-manufacturing.json"] = {"maxBytes": 8192}
    return {"schema": 1, "model": MODEL, "profileID": PROFILE, "executionProfileID": PROFILE,
            "apSHA256": AP, "boardGeometry": {"nand-data-offset": 0, "nand-data-bytes": 131072},
            "norGeometry": {"profileID": PROFILE, "apSHA256": AP, "bytes": 16384},
            "firstRunSkipOOBE": True,
            "persistence": {"nand": True, "nor": True, "identity": identity, "bindings": True,
                            "volatileAreas": [], **({"manufacturingIdentity": True, "identityKind": "manufacturing-only"} if raw else {})},
            "transportDomain": "raw-usb" if raw else "virtual", "controllerInstanceID": "USB\\OWNED-TEST" if raw else None,
            "controllerAddress": None, "addressSource": "unobserved", "expectedFiles": expected}


class InstanceContractTests(unittest.TestCase):
    def test_binding_rejects_ambiguous_sizes_labels_and_mixed_transport(self):
        self.assertEqual(validate_binding(binding()), binding())
        for change in (lambda b: b["expectedFiles"].update({"../escape": {"bytes": 1}}),
                       lambda b: b["expectedFiles"]["nand-data.bin"].update(bytes=True),
                       lambda b: b["expectedFiles"].pop("nand-spare.bin"),
                       lambda b: b.update(transportDomain="raw-usb"),
                       lambda b: b["persistence"].update(nor=False)):
            candidate = binding()
            change(candidate)
            with self.assertRaises(ValueError):
                validate_binding(candidate)
        self.assertFalse(validate_binding(binding(identity=False, raw=True))["persistence"]["identity"])

    def test_health_unknown_forced_or_partial_never_qualifies(self):
        self.assertTrue(validate_health(GOOD))
        for field, value in (("exitCode", None), ("exitCode", 1), ("forced", True),
                             ("guestFault", True), ("ownedChildClosed", False),
                             ("cleanupComplete", False), ("qmpHealthy", False),
                             ("storageHealthy", False), ("peerHealthy", False)):
            self.assertFalse(validate_health({**GOOD, field: value}))
        with self.assertRaises(ValueError):
            validate_health({**GOOD, "pid": 123})
        with self.assertRaises(ValueError):
            validate_health({**GOOD, "forced": 0})

    def test_metadata_duplicate_nonfinite_and_nonserializable_rejected(self):
        for raw in (b'{"schema":1,"schema":1}', b'{"schema":NaN}'):
            with self.assertRaises(ValueError):
                _decode(raw)
        with self.assertRaises((TypeError, ValueError)):
            _encode({"path": Path("arbitrary")})

    def test_store_is_lazy_and_does_not_create_a_posix_fallback(self):
        absent = Path(tempfile.gettempdir()) / ("azki-not-created-" + uuid.uuid4().hex)
        store = WindowsInstanceStore(absent)
        self.assertIsNone(store._files)
        self.assertFalse(absent.exists())
        if os.name != "nt":
            with self.assertRaises(InstanceStorageError) as raised:
                store.list()
            self.assertEqual(raised.exception.code, "windows_required")
            self.assertFalse(absent.exists())


@unittest.skipUnless(os.name == "nt", "Requires actual Win32 storage and locking")
class WindowsInstancesNativeTests(unittest.TestCase):
    def setUp(self):
        from src.windows_runtime_files import WindowsFiles, WindowsTemporaryDirectory
        self.api = WindowsFiles()
        self.api.initialize_private_creator()
        self.temporary = WindowsTemporaryDirectory(prefix="azki-instances-native-", dir=Path(tempfile.gettempdir()))
        self.root = Path(self.temporary.__enter__())
        self.workspace = self.api.private_directory(self.root / "workspace")
        self.seeds = self.api.private_directory(self.root / "seeds")
        self.store = WindowsInstanceStore(self.workspace, api=self.api)
        self.info = self.store.create(PROFILE, "Native storage fixture", MODEL)
        self.identity = self.info["instanceID"]
        self.fs = self.store.fs
        self.inputs = {"firmware.bin": self.seeds / "original-input.bin"}
        self.fs.write_new(self.inputs["firmware.bin"], b"SYNTHETIC-ORIGINAL-INPUT-NOT-FIRMWARE")
        self.binding = binding()
        from src.virtual_peer.state import create_state, encode_state
        state = create_state(self.identity, profile_id=PROFILE, ap_sha256=AP)
        values = {"nand-data.bin": b"\xff" * 131072, "nand-spare.bin": b"\xff" * 4096,
                  "guest-state/guest-nor.bin": b"\xff" * 16384,
                  "guest-state/virtual-peer-state.json": encode_state(state),
                  "guest-state/guest-state.json": _encode({"schema": 1, "instanceID": self.identity,
                      "firstRunSkipOOBE": True, "fixtureOnly": True})}
        self.initial = {}
        for name, raw in values.items():
            path = self.seeds / name
            self.fs.write_new(path, raw)
            self.initial[name] = path

    def tearDown(self):
        gc.collect()
        self.temporary.__exit__(None, None, None)

    def initialize(self):
        with self.store.lease(self.identity) as lease:
            return lease.initialize(self.binding, self.inputs, self.initial)

    def session(self):
        return self.api.private_directory(self.workspace / ("session-" + uuid.uuid4().hex))

    def overwrite(self, path, raw):
        handle = self.api.kernel.CreateFileW(str(path), 0xc0020000, 0, None, 3, 0x00200000, None)
        if handle == self.api.INVALID:
            raise self.api.error("Open own synthetic working fixture")
        try:
            self.api.check_private(handle)
            self.fs.seek(handle)
            self.fs.write(handle, raw)
            self.assertTrue(self.api.kernel.FlushFileBuffers(handle))
        finally:
            self.fs.close(handle)

    def read(self, row):
        return self.fs.read(Path(row["path"]), row["bytes"])

    def test_two_open_rounds_clone_isolation_joint_commit_and_stable_private_identity(self):
        initial = self.initialize()
        old_data = self.read(initial["files"]["nand-data.bin"])
        with self.store.lease(self.identity) as lease:
            work = lease.begin_work(self.session())
            self.assertEqual(work["files"]["nand-data.bin"]["sha256"], initial["files"]["nand-data.bin"]["sha256"])
            self.overwrite(Path(work["files"]["nand-data.bin"]["path"]), b"OWNED-UPDATE")
            self.overwrite(Path(work["files"]["guest-state/guest-nor.bin"]["path"]), b"NOR-UPDATE")
            state_path = Path(work["files"]["guest-state/virtual-peer-state.json"]["path"])
            state = self.fs.json(state_path)
            state["guestLinkBond"] = {"ltk": "11" * 16, "random": "22" * 8, "ediv": 7}
            self.fs.atomic_json(state_path, state)
            self.assertEqual(self.read(initial["files"]["nand-data.bin"]), old_data)
            result = lease.commit_work(work, GOOD)
            self.assertTrue(result["committed"])
            self.assertFalse(result["guestFilesystemClean"])
            self.assertEqual(lease.commit_work(work, GOOD)["reason"], "already-published")
        with self.store.lease(self.identity) as lease:
            accepted = lease.head()
            again = lease.begin_work(self.session())
            self.assertEqual(again["baseRevision"], result["revision"])
            for name in self.initial:
                self.assertEqual(again["files"][name]["sha256"], accepted["files"][name]["sha256"])
            self.assertTrue(self.read(again["files"]["nand-data.bin"]).startswith(b"OWNED-UPDATE"))
            state = self.fs.json(Path(again["files"]["guest-state/virtual-peer-state.json"]["path"]))
            self.assertEqual(state["instanceID"], self.identity)
            self.assertEqual(state["guestLinkBond"]["ediv"], 7)
            self.assertEqual(again["binding"]["firstRunSkipOOBE"], True)
            self.assertTrue(lease.commit_work(again, GOOD)["committed"])
        public = self.store.list()["instances"][0]
        self.assertEqual(public["status"], "ready")
        self.assertNotIn("ltk", json.dumps(public))
        self.assertNotIn("psk", json.dumps(public))

    def test_failed_health_preserves_head_and_requires_explicit_recovery(self):
        initial = self.initialize()
        with self.store.lease(self.identity) as lease:
            work = lease.begin_work(self.session())
            self.overwrite(Path(work["files"]["nand-data.bin"]["path"]), b"UNACCEPTED")
            receipt = lease.commit_work(work, {**GOOD, "forced": True})
            self.assertFalse(receipt["committed"])
            self.assertEqual(lease.head()["revision"], initial["revision"])
            with self.assertRaises(InstanceStorageError) as raised:
                lease.begin_work(self.session())
            self.assertEqual(raised.exception.code, "recovery_required")
            recovered = lease.recover()
            self.assertFalse(recovered["committed"])
            self.assertTrue((lease.root / ".quarantine" / recovered["quarantine"] / "work").is_dir())
            again = lease.begin_work(self.session())
            self.assertEqual(again["files"]["nand-data.bin"]["sha256"], initial["files"]["nand-data.bin"]["sha256"])
            lease.commit_work(again, {**GOOD, "qmpHealthy": False})
            lease.recover()

    def test_busy_lease_and_active_writer_refuse_other_owner_and_recovery(self):
        self.initialize()
        with self.store.lease(self.identity) as lease:
            with self.assertRaises(OSError):
                self.store.lease(self.identity)
            busy = next(item for item in self.store.list()["instances"] if item["instanceID"] == self.identity)
            self.assertEqual(busy["status"], "inUse")
            self.assertEqual(busy["name"], "Native storage fixture")
            work = lease.begin_work(self.session())
            path = Path(work["files"]["nand-data.bin"]["path"])
            handle = self.api.kernel.CreateFileW(str(path), 0xc0020000, 3, None, 3, 0x00200000, None)
            self.assertNotEqual(handle, self.api.INVALID)
            try:
                with self.assertRaises(OSError):
                    lease.recover()
                self.assertTrue(lease.pending_path.exists())
            finally:
                self.fs.close(handle)
            self.assertTrue(lease.recover()["recovered"])

    def test_failure_before_head_preserves_previous_joint_revision(self):
        initial = self.initialize()
        with self.store.lease(self.identity) as lease:
            work = lease.begin_work(self.session())
            self.overwrite(Path(work["files"]["nand-spare.bin"]["path"]), b"NEW-OOB")
            original = self.fs.atomic_json
            def fail_head(path, value):
                if Path(path).name == "head.json":
                    raise OSError("Injected metadata publication failure")
                return original(path, value)
            with mock.patch.object(self.fs, "atomic_json", side_effect=fail_head):
                with self.assertRaises(OSError):
                    lease.commit_work(work, GOOD)
            self.assertEqual(lease.head()["revision"], initial["revision"])
            self.assertEqual(lease.head()["files"], initial["files"])
            lease.recover()

    def test_post_head_receipt_failure_is_committed_and_idempotent(self):
        self.initialize()
        with self.store.lease(self.identity) as lease:
            work = lease.begin_work(self.session())
            original = self.fs.write_new
            def fail_receipt(path, raw):
                if Path(path).parent.name == "receipts":
                    raise OSError("Injected redundant receipt failure")
                return original(path, raw)
            with mock.patch.object(self.fs, "write_new", side_effect=fail_receipt):
                result = lease.commit_work(work, GOOD)
            self.assertTrue(result["committed"])
            self.assertTrue(result["warnings"])
            self.assertEqual(lease.commit_work(work, GOOD)["reason"], "already-published")
            head = lease.head()
            self.overwrite(Path(head["files"]["nand-data.bin"]["path"]), b"CORRUPTED")
            with self.assertRaises(ValueError):
                lease.commit_work(work, GOOD)

    def test_head_replaced_readback_failure_never_reports_a_false_rollback(self):
        self.initialize()
        for verification_available in (True, False):
            with self.store.lease(self.identity) as lease:
                work = lease.begin_work(self.session())
                self.overwrite(Path(work["files"]["nand-data.bin"]["path"]), b"NEW-ACCEPTED-ENDPOINT")
                original = self.fs.atomic_json
                def changed_then_failed(path, value):
                    result = original(path, value)
                    if Path(path).name == "head.json":
                        raise InstanceStorageError("metadata_publication_unconfirmed", "Injected post-replace readback failure")
                    return result
                with mock.patch.object(self.fs, "atomic_json", side_effect=changed_then_failed):
                    if verification_available:
                        result = lease.commit_work(work, GOOD)
                        self.assertTrue(result["committed"])
                        self.assertTrue(result["warnings"])
                        self.assertTrue(result["pendingPreserved"])
                    else:
                        with mock.patch.object(lease, "_verify_accepted", side_effect=OSError("Injected verification failure")):
                            with self.assertRaises(InstanceStorageError) as raised:
                                lease.commit_work(work, GOOD)
                        self.assertEqual(raised.exception.code, "commit_outcome_unknown")
                self.assertEqual(lease.head()["revision"], work["transactionID"])
                self.assertTrue(lease.pending_path.exists())
                self.assertTrue(lease.recover()["committed"])
                self.assertFalse(lease.pending_path.exists())

    def test_partial_initialization_quarantines_without_adopting_or_reseeding(self):
        with self.store.lease(self.identity) as lease:
            original = self.fs.copy
            counter = []
            def failed_copy(*args, **kwargs):
                counter.append(1)
                if len(counter) == 3:
                    raise OSError("Injected partial seed copy failure")
                return original(*args, **kwargs)
            with mock.patch.object(self.fs, "copy", side_effect=failed_copy):
                with self.assertRaises(OSError):
                    lease.initialize(self.binding, self.inputs, self.initial)
            self.assertIsNone(lease.head())
            with self.assertRaises(InstanceStorageError):
                lease.initialize(self.binding, self.inputs, self.initial)
            self.assertTrue(lease.recover()["recovered"])
            head = lease.initialize(self.binding, self.inputs, self.initial)
            with self.assertRaises(InstanceStorageError) as raised:
                lease.initialize(self.binding, self.inputs, self.initial)
            self.assertEqual(raised.exception.code, "already_initialized")
            self.assertEqual(lease.head()["revision"], head["revision"])

    def test_identity_replacement_and_manifest_replacement_never_publish(self):
        initial = self.initialize()
        for role in ("guest-state/virtual-peer-state.json", "guest-state/guest-state.json"):
            with self.store.lease(self.identity) as lease:
                work = lease.begin_work(self.session())
                path = Path(work["files"][role]["path"])
                value = self.fs.json(path)
                if "virtual-peer" in role:
                    value["identity"]["serial"] = "AZKI-REPLACED"
                else:
                    value["fixtureOnly"] = False
                self.fs.atomic_json(path, value)
                with self.assertRaises(ValueError):
                    lease.commit_work(work, GOOD)
                self.assertEqual(lease.head()["revision"], initial["revision"])
                lease.recover()

    def test_private_owner_single_link_exact_size_and_no_shared_source_adoption(self):
        self.initialize()
        with self.store.lease(self.identity) as lease:
            head = lease.head()
            for row in list(head["inputs"].values()) + list(head["files"].values()):
                with self.fs.source(Path(row["path"]), maximum=row["bytes"], exact=row["bytes"]) as (handle, _):
                    self.api.check_private(handle)
            data = Path(head["files"]["nand-data.bin"]["path"])
            alias = self.seeds / "forbidden-hardlink.bin"
            os.link(data, alias)
            try:
                with self.assertRaises(PermissionError):
                    lease.head()
            finally:
                alias.unlink()
            with data.open("ab") as stream:
                stream.write(b"x")
            with self.assertRaises(ValueError):
                lease.head()

    def test_delete_is_a_locked_tombstone_and_never_global_cleanup(self):
        self.initialize()
        other = self.store.create(PROFILE, "Independent fixture", MODEL)
        with self.store.lease(self.identity):
            with self.assertRaises(OSError):
                self.store.delete(self.identity)
        removed = self.store.delete(self.identity)
        self.assertTrue(removed["deleted"])
        self.assertFalse((self.store.root / self.identity).exists())
        self.assertTrue((self.store.root / ".trash" / removed["tombstone"] / "head.json").is_file())
        self.assertTrue((self.store.root / other["instanceID"] / "instance.json").is_file())

    def test_raw_nor_and_manufacturing_metadata_never_become_virtual_bond_state(self):
        info = self.store.create(PROFILE, "Raw-only synthetic storage fixture", MODEL)
        identity = info["instanceID"]
        raw_binding = binding(identity=False, raw=True)
        metadata_path = self.seeds / "raw-manufacturing.json"
        metadata = {"schema": 1, "instanceID": identity, "model": MODEL,
                    "psn": "AZKI-SYNTHETIC-STORAGE-TEST", "did": "9900000000000",
                    "factoryMAC": "02:11:22:33:44:55", "identityKind": "manufacturing-only",
                    "addressSource": "synthetic-manufacturing", "controllerAddress": None}
        self.fs.write_new(metadata_path, _encode(metadata))
        initial = {"nand-data.bin": self.initial["nand-data.bin"],
                   "nand-spare.bin": self.initial["nand-spare.bin"],
                   "nor-config.bin": self.initial["guest-state/guest-nor.bin"],
                   "raw-manufacturing.json": metadata_path}
        with self.store.lease(identity) as lease:
            head = lease.initialize(raw_binding, self.inputs, initial)
            work = lease.begin_work(self.session())
            self.overwrite(Path(work["files"]["nor-config.bin"]["path"]), b"RAW-GUEST-STORAGE-FIXTURE")
            self.assertTrue(lease.commit_work(work, GOOD)["committed"])
        with self.store.lease(identity) as lease:
            head = lease.head()
            self.assertFalse(head["binding"]["persistence"]["identity"])
            self.assertNotIn("guest-state/virtual-peer-state.json", head["files"])
            work = lease.begin_work(self.session())
            self.assertTrue(self.read(work["files"]["nor-config.bin"]).startswith(b"RAW-GUEST-STORAGE-FIXTURE"))
            path = Path(work["files"]["raw-manufacturing.json"]["path"])
            value = self.fs.json(path)
            value["ltk"] = "11" * 16
            self.fs.atomic_json(path, value)
            with self.assertRaises(ValueError):
                lease.commit_work(work, GOOD)
            self.assertEqual(lease.head()["revision"], head["revision"])
            lease.recover()

    def test_handle_count_does_not_grow_across_independent_leases(self):
        self.initialize()
        self.api.kernel.GetProcessHandleCount.argtypes = [w.HANDLE, c.POINTER(w.DWORD)]
        self.api.kernel.GetProcessHandleCount.restype = w.BOOL
        def count():
            value = w.DWORD()
            self.assertTrue(self.api.kernel.GetProcessHandleCount(self.api.kernel.GetCurrentProcess(), c.byref(value)))
            return value.value
        baseline = count()
        for _ in range(20):
            with self.store.lease(self.identity) as lease:
                lease.head()
                lease.public_info()
        gc.collect()
        self.assertLessEqual(count(), baseline + 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
