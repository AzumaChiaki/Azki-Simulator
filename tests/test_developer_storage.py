"""External developer sessions may commit only host-confirmed stopped runs."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Sources/AzkiSimulator/Runtime"))
from azki_simulator import developer_storage as ds
from azki_simulator.instance_storage import _stopped_health


def write(path, value):
    path.write_text(json.dumps(value))
    path.chmod(0o600)


class DeveloperStorageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="azki-owner-", dir="/private/tmp")
        self.session = Path(self.temp.name)
        self.run = self.session / "m-123456789abc"
        self.run.mkdir(mode=0o700)
        processes = [subprocess.Popen(["/usr/bin/true"]) for _ in range(3)]
        for process in processes:
            process.wait()
        self.supervisor, self.runner, self.core = processes
        self.binding = {"transactionID": "owned-transaction", "dataImage": "data", "spareImage": "spare",
                        "norPersistent": False}
        self.plan = {"approvalDigest": "a" * 64, "identity": {"instanceStorage": self.binding}}
        write(self.session / "session.json", {"id": "owned-session", "approvalDigest": "a" * 64})
        write(self.run / "runner-process.json", {"pid": self.runner.pid, "supervisorPID": self.supervisor.pid})
        self.result = {"returncode": 0, "outcome": "interrupted", "data_writeback": True,
                       "data_image": "data", "data_spare_image": "spare"}
        write(self.run / "result.json", self.result)
        self.adapter = types.SimpleNamespace(plan=self.plan, root=self.session, run=self.run,
            native_broker=None, fixture_only=False, isolation_verified=True, execution_prefix=("fixed-supervisor",),
            process=self.supervisor, _cleanup="reaped", _state="stopped", _reason="stopped",
            _developer_process_identity={"runnerPID": self.runner.pid, "corePID": self.core.pid})

    def tearDown(self):
        self.temp.cleanup()

    def receipt(self):
        return json.loads((self.session / ds.FILENAME).read_text())

    def alter(self, **values):
        value = self.receipt()
        value.update(values)
        write(self.session / ds.FILENAME, value)

    def test_actual_reaped_processes_can_publish_and_pass_existing_health(self):
        self.assertTrue(ds.publish_completion(self.adapter))
        receipt = ds.read_completion(self.session, self.plan, self.binding)
        self.assertEqual(receipt["supervisorPID"], self.supervisor.pid)
        self.assertTrue(_stopped_health(self.session, self.plan, self.binding)[0])
        self.assertFalse((self.session / "native-broker").exists())

    def test_live_pid_is_never_exit_evidence(self):
        self.adapter._developer_process_identity["corePID"] = os.getpid()
        with self.assertRaisesRegex(ValueError, "alive"):
            ds.publish_completion(self.adapter)
        self.assertFalse((self.session / ds.FILENAME).exists())

    def test_reused_or_live_pid_in_receipt_rejected(self):
        ds.publish_completion(self.adapter)
        self.alter(runnerPID=os.getpid())
        with self.assertRaisesRegex(ValueError, "alive"):
            ds.read_completion(self.session, self.plan, self.binding)

    def test_missing_identity_observation_or_fixture_cannot_authorize_commit(self):
        for field, value in [("_developer_process_identity", None), ("fixture_only", True),
                             ("isolation_verified", False), ("execution_prefix", ())]:
            old = getattr(self.adapter, field)
            setattr(self.adapter, field, value)
            self.assertFalse(ds.publish_completion(self.adapter))
            setattr(self.adapter, field, old)
        with self.assertRaisesRegex(ValueError, "host completion"):
            _stopped_health(self.session, self.plan, self.binding)

    def test_plan_transaction_session_and_run_identity_are_exact(self):
        ds.publish_completion(self.adapter)
        original = self.receipt()
        for change in [{"approvalDigest": "b" * 64}, {"transactionID": "other"},
                       {"sessionID": "other"}, {"runInode": original["runInode"] + 1},
                       {"run": "../foreign"}, {"cleanup": "unconfirmed"}, {"version": True},
                       {"state": "running"}, {"reason": None}]:
            write(self.session / ds.FILENAME, {**original, **change})
            with self.assertRaises(ValueError):
                ds.read_completion(self.session, self.plan, self.binding)

    def test_changed_result_or_runner_evidence_is_rejected(self):
        ds.publish_completion(self.adapter)
        write(self.run / "result.json", {**self.result, "outcome": "guest_fault"})
        with self.assertRaisesRegex(ValueError, "changed"):
            ds.read_completion(self.session, self.plan, self.binding)
        write(self.run / "result.json", self.result)
        write(self.run / "runner-process.json", {"pid": self.core.pid, "supervisorPID": self.supervisor.pid})
        with self.assertRaisesRegex(ValueError, "identity differs"):
            ds.read_completion(self.session, self.plan, self.binding)

    def test_symlinked_run_is_rejected(self):
        ds.publish_completion(self.adapter)
        original = self.run.with_name("m-000000000000")
        self.run.rename(original)
        self.run.symlink_to(original, target_is_directory=True)
        with self.assertRaises((OSError, ValueError)):
            ds.read_completion(self.session, self.plan, self.binding)

    def test_host_receipt_cannot_replace_or_override_existing_one(self):
        ds.publish_completion(self.adapter)
        before = (self.session / ds.FILENAME).read_bytes()
        with self.assertRaises(FileExistsError):
            ds.publish_completion(self.adapter)
        self.assertEqual((self.session / ds.FILENAME).read_bytes(), before)

    def test_healthy_exit_record_does_not_override_guest_storage_faults(self):
        write(self.run / "result.json", {**self.result, "storage_writeback_error": "failed"})
        ds.publish_completion(self.adapter)
        healthy, reason = _stopped_health(self.session, self.plan, self.binding)
        self.assertFalse(healthy)
        self.assertEqual(reason, "guest-or-storage-health-unconfirmed")

    def test_healthy_guest_result_does_not_override_host_failure(self):
        self.adapter._state = "failed"
        self.adapter._reason = "storage_quota_exceeded"
        ds.publish_completion(self.adapter)
        self.assertEqual(self.receipt()["state"], "failed")
        self.assertEqual(self.receipt()["reason"], "storage_quota_exceeded")
        healthy, reason = _stopped_health(self.session, self.plan, self.binding)
        self.assertFalse(healthy)
        self.assertEqual(reason, "developer-guest-launch-or-cleanup-failed")

    def test_failed_staging_never_publishes_partial_receipt_and_can_retry(self):
        with mock.patch.object(ds.os, "fsync", side_effect=OSError("write failed")):
            with self.assertRaisesRegex(OSError, "write failed"):
                ds.publish_completion(self.adapter)
        self.assertFalse((self.session / ds.FILENAME).exists())
        self.assertEqual(list(self.session.glob(".developer-completion-*")), [])
        self.assertTrue(ds.publish_completion(self.adapter))

    def test_interruption_after_atomic_publish_leaves_valid_single_link_receipt(self):
        publish = ds._publish_no_replace
        def interrupted(directory, temporary):
            publish(directory, temporary)
            raise InterruptedError("interrupted after publish")
        with mock.patch.object(ds, "_publish_no_replace", side_effect=interrupted):
            with self.assertRaises(InterruptedError):
                ds.publish_completion(self.adapter)
        self.assertEqual((self.session / ds.FILENAME).stat().st_nlink, 1)
        self.assertEqual(list(self.session.glob(".developer-completion-*")), [])
        self.assertTrue(_stopped_health(self.session, self.plan, self.binding)[0])


if __name__ == "__main__":
    unittest.main()
