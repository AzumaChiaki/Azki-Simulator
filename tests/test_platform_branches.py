"""Linux platform branches in the host layer must be reachable, not just plausible.

discovery and bridge each grew a sys.platform path that no test on macOS would
ever execute; a missing import or a diverging launcher path fails only when a
developer on Linux calls it, so these invariants are pinned here.
"""
from pathlib import Path
import os
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Sources/AzkiSimulator/Runtime"))

from azki_simulator import bwrap_policy, discovery


class SessionRegistryPathTest(unittest.TestCase):
    def test_matches_platform_temp_root(self):
        path = discovery._registry_path()
        expected_root = "/private/tmp" if sys.platform == "darwin" else "/tmp"
        self.assertEqual(path.name, "azki-simulator-" + str(os.getuid()))
        self.assertEqual(str(path.parent), expected_root)


class LauncherConstantTest(unittest.TestCase):
    def test_the_bridge_launcher_is_one_the_trampolines_accept(self):
        from azki_simulator import bridge
        from azki_simulator import launch
        from azki_simulator import minative_launch
        expected = "/usr/bin/sandbox-exec" if sys.platform == "darwin" else bwrap_policy.LAUNCHER
        # The bridge passes its host's launcher down; the trampolines accept a
        # set, and the launcher they are given has to be in it, otherwise every
        # launch is refused.
        self.assertEqual(bridge.EXPECTED_LAUNCHER, expected)
        for module in (launch, minative_launch):
            self.assertEqual(module.SUPPORTED_LAUNCHERS, ("/usr/bin/sandbox-exec", "/usr/bin/bwrap"),
                             module.__name__)
            self.assertIn(expected, module.SUPPORTED_LAUNCHERS, module.__name__)

    def test_the_policy_launcher_is_among_the_supported_ones(self):
        from azki_simulator import launch
        from azki_simulator import minative_launch
        self.assertIn(bwrap_policy.LAUNCHER, launch.SUPPORTED_LAUNCHERS)
        self.assertIn(bwrap_policy.LAUNCHER, minative_launch.SUPPORTED_LAUNCHERS)


class QuarantineNoOpTest(unittest.TestCase):
    """The quarantine attribute is a macOS concept. On Linux the call has to be
    a no-op that leaves the file exactly as it found it — the import path sets
    it on every firmware it touches."""

    @unittest.skipIf(sys.platform == "darwin", "the quarantine xattr is macOS-only")
    def test_quarantine_changes_nothing_off_darwin(self):
        from azki_simulator import bridge
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "payload.bin"
            path.write_bytes(b"firmware")
            before = path.stat()
            self.assertIsNone(bridge.quarantine(path))
            self.assertIsNone(bridge.quarantine(path, "quarantine-value"))
            after = path.stat()
            self.assertEqual(path.read_bytes(), b"firmware")
            self.assertEqual(after.st_mtime_ns, before.st_mtime_ns)
            self.assertEqual(after.st_mode, before.st_mode)


class WindowBackendRefusalTest(unittest.TestCase):
    """The Linux port renders through the host control surface, so a native QEMU
    window is refused instead of opening a second renderer next to the panel."""

    @classmethod
    def setUpClass(cls):
        backend = Path(__file__).resolve().parents[1] / "engine/project/backend"
        sys.path.insert(0, str(backend))
        from src import runner
        cls.runner = runner

    def test_a_native_window_on_linux_is_refused(self):
        with self.assertRaises(RuntimeError) as failure:
            self.runner.qemu_display_backend("/usr/bin/qemu-system-arm", True, platform="linux")
        self.assertIn("host control surface", str(failure.exception))

    def test_without_a_window_the_display_stays_off(self):
        self.assertEqual(self.runner.qemu_display_backend("/usr/bin/qemu-system-arm", False, platform="linux"), "none")

    def test_macos_keeps_its_native_window(self):
        self.assertEqual(self.runner.qemu_display_backend("/usr/bin/qemu-system-arm", True, platform="darwin"),
                         "cocoa,show-cursor=on")


class NativeParentTest(unittest.TestCase):
    """The native guest refuses to run unless the announced broker really owns it.

    macOS gets that from a launcher that execs in place. bwrap cannot, so the Linux
    branch reads one level further; if that reading is wrong the product path never
    starts a guest at all, which is precisely how the first Linux ladder failed.
    """

    def test_the_direct_parent_is_accepted_and_a_stranger_is_not(self):
        from azki_simulator import minative_launch
        self.assertTrue(minative_launch.native_parent_is(os.getppid()))
        self.assertFalse(minative_launch.native_parent_is(1))

    @unittest.skipUnless(sys.platform.startswith("linux"), "bubblewrap is the Linux launcher")
    @unittest.skipUnless(Path(bwrap_policy.LAUNCHER).exists(), "no bubblewrap on this host")
    def test_a_guest_inside_bwrap_reads_the_host_as_its_grandparent(self):
        import subprocess
        probe = ("import sys; sys.path.insert(0, %r)\n"
                 "from azki_simulator import minative_launch\n"
                 "print('owner' if minative_launch.native_parent_is(%d) else 'stranger')\n")
        root = str(Path(__file__).resolve().parents[1] / "Sources/AzkiSimulator/Runtime")
        for announced, expected in ((os.getpid(), "owner"), (os.getpid() + 7_777, "stranger")):
            result = subprocess.run([*bwrap_policy.PREAMBLE, "--bind", root, root, "--",
                                     sys.executable, "-I", "-S", "-B", "-c", probe % (root, announced)],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), expected)


if __name__ == "__main__":
    unittest.main()
