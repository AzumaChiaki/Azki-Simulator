"""sessions-cleanup decides what may be deleted under the shared temp root.

The desktop app's 清理过期会话 button calls this through the sandboxed CLI, so the
rules here are the difference between reclaiming a leaked session directory and
wiping one that a running emulator still has mounted. Every case drives the shipped
implementation and asserts on the filesystem, not on a re-implementation of its
conditions.
"""
from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Sources/AzkiSimulator/Runtime"))

from azki_simulator import maintenance


def _dead_pid():
    child = subprocess.Popen([sys.executable, "-c", "pass"])
    child.wait()
    return child.pid


class CleanupTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.tmp = Path(self.directory.name)
        self.addCleanup(self.directory.cleanup)

    def root(self, name, files=None, session_files=None):
        path = self.tmp / name
        (path / "session").mkdir(parents=True)
        for file, value in (files or {}).items():
            (path / file).write_text(value)
        for file, value in (session_files or {}).items():
            (path / "session" / file).write_text(value)
        return path

    def flat_root(self, name, files=None):
        # A helper command root that leaked holds at most its scratch folder, and
        # removal only accepts the documented leftovers, so build it without the
        # session/ layout the emulator roots have.
        path = self.tmp / name
        path.mkdir(parents=True)
        for file, value in (files or {}).items():
            (path / file).write_text(value)
        return path

    def reasons(self, result):
        return {Path(kept).name: result["reasons"][kept] for kept in result["kept"]}

    def test_removes_a_stopped_root_and_keeps_everything_else(self):
        stale = self.root("azki-b-stale", {"runner-process.json": json.dumps({"pid": _dead_pid()})})
        live = subprocess.Popen(["sleep", "30"])
        self.addCleanup(lambda: (live.terminate(), live.wait()))
        running = self.root("azki-b-running", {"runner-process.json": json.dumps({"pid": live.pid})})
        fresh = self.root("azki-sim-fresh", {"note.txt": "prepared a moment ago"})
        foreign = self.root("not-azki-mine", {"runner-process.json": json.dumps({"pid": _dead_pid()})})
        outside = self.root("keepme-target", {"runner-process.json": json.dumps({"pid": _dead_pid()})})
        os.symlink(outside, self.tmp / "azki-b-link")

        result = maintenance.cleanup_sessions(None, self.tmp)

        self.assertEqual([str(stale)], result["removed"])
        self.assertFalse(stale.exists())
        self.assertTrue(running.exists() and fresh.exists() and foreign.exists())
        self.assertTrue(outside.exists(), "清理跟着符号链接删到了别人指的东西")
        self.assertTrue((self.tmp / "azki-b-link").is_symlink(), "链接本身不是本次能判断的东西")
        self.assertEqual(self.reasons(result),
                         {"azki-b-running": "running", "azki-sim-fresh": "recent"})

    def test_an_unknown_state_never_loses_a_recent_root(self):
        # No process or socket evidence at all: the grace window is what keeps a
        # session that is still being prepared from going under its creator.
        root = self.root("azki-b-preparing", {"state.json": json.dumps({"state": "starting"})})
        maintenance.cleanup_sessions(None, self.tmp)
        self.assertTrue(root.exists())
        self.assertEqual(self.reasons(maintenance.cleanup_sessions(None, self.tmp)),
                         {"azki-b-preparing": "recent"})
        # Past the window an unknown root is still removable, because nothing here
        # claims it belongs to a helper command.
        maintenance.cleanup_sessions(None, self.tmp, now=time.time() + 3 * maintenance.GRACE_SECONDS)
        self.assertFalse(root.exists())

    def test_helper_roots_only_go_when_they_hold_only_known_leftovers(self):
        clean = self.flat_root("azki-cmd-finished", {"native-p256.bin": "x" * 32})
        busy = self.flat_root("azki-cmd-busy", {"native-p256.bin": "x", "trace.bin": "y"})
        maintenance.cleanup_sessions(None, self.tmp, now=time.time() + 24 * 3600)
        self.assertFalse(clean.exists())
        self.assertTrue(busy.exists())
        later = time.time() + 24 * 3600
        self.assertEqual(self.reasons(maintenance.cleanup_sessions(None, self.tmp, now=later)),
                         {"azki-cmd-busy": "unknown-helper-files"})

    def test_the_root_the_command_itself_runs_in_is_protected(self):
        root = self.root("azki-b-mine", {"runner-process.json": json.dumps({"pid": _dead_pid()})})
        previous = os.environ.get("AZKI_SESSION_ROOT")
        os.environ["AZKI_SESSION_ROOT"] = str(root / "session")
        try:
            result = maintenance.cleanup_sessions(None, self.tmp, now=time.time() + 24 * 3600)
        finally:
            if previous is None:
                os.environ.pop("AZKI_SESSION_ROOT", None)
            else:
                os.environ["AZKI_SESSION_ROOT"] = previous
        self.assertTrue(root.exists())
        self.assertEqual(self.reasons(result), {"azki-b-mine": "in-use-by-this-command"})

    def test_a_session_whose_instance_cannot_be_settled_waits(self):
        # The binding says an instance transaction is still open for this session.
        # Settling it means writing inside the instance folder, so a folder that is
        # not a plain directory we own has to stop the cleanup: deleting the guest
        # disks is the one mistake that cannot be undone.
        library = self.tmp / "library"
        (library / "instances").mkdir(parents=True)
        instance = "11111111-1111-1111-1111-111111111111"
        elsewhere = self.tmp / "somewhere-else"
        (elsewhere / "storage").mkdir(parents=True)
        root = self.root("azki-b-pending", {"runner-process.json": json.dumps({"pid": _dead_pid()})},
                         {"instance-storage.json": json.dumps({"instanceID": instance})})
        (elsewhere / "storage" / "pending.json").write_text(
            json.dumps({"sessionRoot": str(root / "session")}))
        os.symlink(elsewhere, library / "instances" / instance)

        result = maintenance.cleanup_sessions(library, self.tmp, now=time.time() + 24 * 3600)

        self.assertTrue(root.exists(), "未结算的实例事务被清理掉了")
        self.assertEqual(self.reasons(result), {"azki-b-pending": "awaiting-instance-reset"})
        self.assertTrue((elsewhere / "storage" / "pending.json").exists())

    def test_a_binding_that_names_no_instance_never_reaches_the_instance_code(self):
        # A corrupt binding must not be enough to make cleanup walk into the library
        # at an arbitrary path; it refuses before resolving anything.
        root = self.root("azki-b-corrupt", {"runner-process.json": json.dumps({"pid": _dead_pid()})},
                         {"instance-storage.json": json.dumps({"instanceID": "../../escape"})})
        result = maintenance.cleanup_sessions(self.tmp / "library", self.tmp, now=time.time() + 24 * 3600)
        self.assertTrue(root.exists())
        self.assertEqual(self.reasons(result), {"azki-b-corrupt": "awaiting-instance-reset"})

    def test_a_removal_that_cannot_finish_reports_instead_of_passing_silently(self):
        # Removal refuses to recurse past 64 levels. The root must survive with a
        # reason; a cleanup that quietly reports success here would hide a leak.
        root = self.root("azki-b-deep", {"runner-process.json": json.dumps({"pid": _dead_pid()})})
        node = root / "session"
        for _ in range(80):
            node = node / "d"
            node.mkdir()

        result = maintenance.cleanup_sessions(None, self.tmp, now=time.time() + 24 * 3600)

        self.assertTrue(root.exists())
        self.assertIn("remove-failed", self.reasons(result)["azki-b-deep"])
        self.assertEqual(result["removed"], [])


if __name__ == "__main__":
    unittest.main()
