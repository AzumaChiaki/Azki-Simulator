"""The Linux sandbox argv must be one bubblewrap accepts, not one we assumed.

An option that bwrap does not implement fails at spawn time, which is the first
guest boot on a developer machine rather than a test run here.
"""
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Sources/AzkiSimulator/Runtime"))
from azki_simulator import minative_isolation

LAUNCHER = "/usr/bin/bwrap"


@unittest.skipUnless(sys.platform == "linux", "the bubblewrap policy is Linux-only")
class BwrapPolicyTest(unittest.TestCase):
    def arguments(self, root):
        run = root / "run"
        run.mkdir()
        (root / "python").mkdir()
        (root / "project").mkdir()
        adapter = types.SimpleNamespace(root=root, seconds=0,
                                        runner_metadata=root / "runner-metadata.json",
                                        plan={"spec": {}})
        prefix = minative_isolation.bwrap_prefix(adapter, run, [run], root / "python", root / "project", [])
        return prefix[list(prefix).index(LAUNCHER):]

    def test_pid_namespace_stays_shared(self):
        with tempfile.TemporaryDirectory() as temporary:
            arguments = self.arguments(Path(temporary))
        self.assertEqual(arguments[0], LAUNCHER)
        self.assertEqual(arguments[-1], "--")
        # A private pid namespace hides the guest supervisor's real pid and process
        # group from the host, which both SO_PEERCRED and process-group reaping need.
        for forbidden in ("--share-pid", "--unshare-pid", "--unshare-all"):
            self.assertNotIn(forbidden, arguments, forbidden)
        for required in ("--unshare-user", "--unshare-ipc", "--unshare-uts", "--unshare-net"):
            self.assertIn(required, arguments)
        # Without the usr-merge links the bound /usr is visible but every exec fails,
        # because binaries name their interpreter as /lib64/ld-linux-x86-64.so.2.
        for link in ("/lib64", "/lib", "/bin"):
            self.assertIn(link, arguments, link)

    def test_bwrap_accepts_generated_argv(self):
        if not Path(LAUNCHER).exists():
            self.skipTest("bubblewrap is not installed")
        with tempfile.TemporaryDirectory() as temporary:
            arguments = self.arguments(Path(temporary))
            # The bound trees are the scratch directory itself, so the probe has to
            # run before it is removed.
            probe = subprocess.run([*arguments, "/usr/bin/true"], capture_output=True, text=True, timeout=60)
        if probe.returncode != 0 and ("user namespace" in probe.stderr
                                      or "Operation not permitted" in probe.stderr):
            self.skipTest("unprivileged user namespaces are unavailable here")
        self.assertEqual(probe.returncode, 0, probe.stderr)


if __name__ == "__main__":
    unittest.main()
