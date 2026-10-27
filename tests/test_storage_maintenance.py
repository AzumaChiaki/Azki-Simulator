"""Storage accounting, APFS clone snapshots, pruning and instance deletion."""
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Sources/AzkiSimulator/Runtime"))

from azki_simulator import instance_settings
from azki_simulator.instance_storage import InstanceStorage, Firmware
from azki_simulator.maintenance import remove_instance_sessions
from azki_simulator.storage_maintenance import maintain, report

MODEL = "miwear.watch.n66cn"
PROFILE = "miwear.watch.n66cn@3.1.32@489f20502ae2"
IDENTIFIER = "5a0bde5a-018d-42e1-baf5-ff1089944eda"


def firmware():
    return Firmware(model=MODEL, profile=PROFILE, data_offset=0, data_bytes=131072,
                    data_format="yaffs2-inband-2048")


class StorageMaintenanceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="azki-storage-", dir="/private/tmp")
        self.base = Path(self.temp.name)
        self.library = self.base / "library"
        (self.library / "instances").mkdir(parents=True, mode=0o700)
        self.tmp_sessions = self.base / "sessions"
        self.tmp_sessions.mkdir(mode=0o700)

    def tearDown(self):
        self.temp.cleanup()

    def instance(self, identifier=IDENTIFIER):
        folder = self.library / "instances" / identifier
        folder.mkdir(mode=0o700)
        return folder

    def revisions(self, folder, count=3, mutate=True):
        store = InstanceStorage.create(folder / "storage", MODEL)
        for index in range(count):
            transaction = store.begin(firmware())
            data = Path(transaction["data_image"])
            if mutate and index:
                with data.open("r+b") as stream:
                    stream.seek(2048 * index)
                    stream.write(bytes([index + 1]))
            store.finish(success=True)
        store.close()
        return folder / "storage"

    def test_blank_revision_is_a_clone_of_the_shared_erased_template(self):
        folder = self.instance()
        storage = self.revisions(folder, count=1, mutate=False)
        template = self.library / ".storage" / "erased" / "131072.bin"
        self.assertTrue(template.is_file())
        self.assertEqual(template.stat().st_size, 131072)
        self.assertEqual(template.read_bytes(), b"\xff" * 131072)
        revision = json.loads((storage / "head.json").read_text())["revision"]
        self.assertTrue((storage / "revisions" / revision / "nand-data.bin").is_file())

    def test_report_counts_snapshot_and_stopped_session_bytes(self):
        folder = self.instance()
        storage = self.revisions(folder, count=3, mutate=True)
        session = self.tmp_sessions / "azki-b-report-test"
        (session / "session" / "instance-data").mkdir(parents=True, mode=0o700)
        (session / "session" / "instance-storage.json").write_text(json.dumps({"instanceID": IDENTIFIER}))
        (session / "session" / "instance-storage-result.json").write_text("{}")
        (session / "session" / "instance-data" / "nand-data.bin").write_bytes(b"x" * 4096)
        value = report(self.library, tmp_root=self.tmp_sessions)
        self.assertEqual(value["version"], 1)
        self.assertEqual(len(value["instances"]), 1)
        entry = value["instances"][0]
        self.assertEqual(entry["id"], IDENTIFIER)
        self.assertEqual(entry["revisionCount"], 3)
        self.assertEqual(entry["sessionsBytes"], 4096)
        self.assertGreater(entry["prunable2Bytes"], 0)
        self.assertGreater(value["sessionsBytes"], 0)

    def test_maintain_keeps_head_and_previous_and_removes_older_revisions(self):
        storage = self.revisions(self.instance(), count=5, mutate=True)
        head_before = json.loads((storage / "head.json").read_text())["revision"]
        before = sorted(path.name for path in (storage / "revisions").iterdir())
        result = maintain(self.library, keep=2, clean_sessions=False, tmp_root=self.tmp_sessions)
        after = sorted(path.name for path in (storage / "revisions").iterdir())
        self.assertEqual(result["errors"], [])
        self.assertEqual(len(after), 2)
        self.assertIn(head_before, after)
        self.assertEqual(len(before) - len(after), 3)
        # HEAD's own bytes survive the prune unchanged.
        self.assertGreater((storage / "revisions" / head_before / "nand-data.bin").stat().st_size, 0)

    def test_dry_run_reports_without_removing_or_cloning(self):
        storage = self.revisions(self.instance(), count=4, mutate=True)
        # Existing snapshots are independent of the shared template after the
        # clone; removing it must not affect a read-only preview.
        shutil.rmtree(self.library / ".storage")
        before = sorted(path.name for path in (storage / "revisions").iterdir())
        result = maintain(self.library, keep=1, dry_run=True, clean_sessions=False,
                          tmp_root=self.tmp_sessions)
        self.assertTrue(result["dryRun"])
        self.assertEqual(len(result["removedRevisions"]), 3)
        self.assertEqual(sorted(path.name for path in (storage / "revisions").iterdir()), before)
        self.assertFalse((self.library / ".storage" / "erased").exists())

    def test_running_instance_is_skipped_without_changes(self):
        storage = self.revisions(self.instance(), count=4, mutate=True)
        before = sorted(path.name for path in (storage / "revisions").iterdir())
        held = InstanceStorage(storage)
        try:
            result = maintain(self.library, keep=1, clean_sessions=False, tmp_root=self.tmp_sessions)
        finally:
            held.close()
        self.assertTrue(any(item["reason"] == "running" for item in result["skipped"]))
        self.assertEqual(sorted(path.name for path in (storage / "revisions").iterdir()), before)

    def test_dry_run_does_not_touch_an_unsettled_run(self):
        storage = self.revisions(self.instance(), count=2, mutate=True)
        before = sorted(path.name for path in (storage / "revisions").iterdir())
        store = InstanceStorage(storage)
        transaction = store.begin(firmware())
        store.close()
        self.assertTrue((storage / "pending.json").exists())
        result = maintain(self.library, keep=1, dry_run=True, clean_sessions=False,
                          tmp_root=self.tmp_sessions)
        self.assertTrue(any(item["reason"] == "unsettled-run" for item in result["skipped"]))
        self.assertEqual(sorted(path.name for path in (storage / "revisions").iterdir()),
                         sorted(before + [Path(transaction["data_image"]).parent.name]))
        self.assertTrue((storage / "pending.json").exists())

    def test_identical_blank_spare_images_share_one_template(self):
        self.revisions(self.instance("11111111-1111-4111-8111-111111111111"), count=2, mutate=False)
        self.revisions(self.instance("22222222-2222-4222-8222-222222222222"), count=2, mutate=False)
        result = maintain(self.library, keep=4, clean_sessions=False, tmp_root=self.tmp_sessions)
        self.assertEqual(result["errors"], [])
        self.assertGreaterEqual(result["clonedFiles"], 1)
        for identifier in ("11111111-1111-4111-8111-111111111111",
                           "22222222-2222-4222-8222-222222222222"):
            head = json.loads((self.library / "instances" / identifier / "storage" / "head.json").read_text())
            data = (self.library / "instances" / identifier / "storage" / "revisions" /
                    head["revision"] / "nand-data.bin").read_bytes()
            self.assertEqual(data, b"\xff" * 131072)

    def test_instance_delete_removes_its_stopped_session_images(self):
        folder = self.instance()
        self.revisions(folder, count=1, mutate=False)
        session = self.tmp_sessions / "azki-b-delete-test"
        (session / "session" / "instance-data").mkdir(parents=True, mode=0o700)
        (session / "session" / "instance-storage.json").write_text(json.dumps({"instanceID": IDENTIFIER}))
        (session / "session" / "instance-storage-result.json").write_text("{}")
        (session / "session" / "instance-data" / "nand-data.bin").write_bytes(b"x" * 1024)
        removed = remove_instance_sessions(IDENTIFIER, tmp_root=self.tmp_sessions)
        self.assertEqual(removed["kept"], [])
        self.assertEqual(len(removed["removed"]), 1)
        self.assertFalse(session.exists())

    def test_instance_settings_discard_removes_the_whole_instance_folder(self):
        folder = self.instance()
        self.revisions(folder, count=1, mutate=False)
        result = instance_settings.discard(self.library, IDENTIFIER)
        self.assertTrue(result["removed"])
        self.assertFalse(folder.exists())
        self.assertFalse((self.library / "instances").exists())

    def test_old_unknown_session_is_removed_but_a_recent_one_is_kept(self):
        old = self.tmp_sessions / "azki-b-old-unknown"
        (old / "session" / "instance-data").mkdir(parents=True, mode=0o700)
        (old / "session" / "instance-storage.json").write_text(json.dumps({"instanceID": IDENTIFIER}))
        (old / "session" / "instance-data" / "nand-data.bin").write_bytes(b"x" * 1024)
        stamp = 1_000_000_000
        for path in [old, *old.rglob("*")]:
            os.utime(path, (stamp, stamp), follow_symlinks=False)
        recent = self.tmp_sessions / "azki-b-recent-unknown"
        (recent / "session" / "instance-data").mkdir(parents=True, mode=0o700)
        (recent / "session" / "instance-storage.json").write_text(json.dumps({"instanceID": IDENTIFIER}))
        removed = remove_instance_sessions(IDENTIFIER, tmp_root=self.tmp_sessions)
        self.assertFalse(old.exists())
        self.assertTrue(recent.exists())
        self.assertEqual(len(removed["kept"]), 1)

    def test_symlinked_session_is_never_followed(self):
        real = self.tmp_sessions / "real"
        (real / "session").mkdir(parents=True, mode=0o700)
        (real / "session" / "instance-storage.json").write_text(json.dumps({"instanceID": IDENTIFIER}))
        (real / "session" / "instance-storage-result.json").write_text("{}")
        link = self.tmp_sessions / "azki-b-link-test"
        link.symlink_to(real, target_is_directory=True)
        removed = remove_instance_sessions(IDENTIFIER, tmp_root=self.tmp_sessions)
        self.assertEqual(removed["removed"], [])
        self.assertTrue(real.exists())


if __name__ == "__main__":
    unittest.main()
