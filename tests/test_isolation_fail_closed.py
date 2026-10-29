"""A missing sandbox launcher must stop the launch, not loosen it.

The Linux isolation refuses to run a payload without bubblewrap: a session that
started unguarded would look like a working one and give the guest the whole
host. The refusal has to be a hard error, with no argv produced at all.
"""
from pathlib import Path
import sys
import types
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Sources/AzkiSimulator/Runtime"))
from azki_simulator import minative_isolation
from azki_simulator.minative import MError


def adapter():
    """The smallest adapter the launcher check needs. The identity carries just
    enough for the guard to be passed, so the next refusal is about the plan's
    Python library rather than about the launcher."""
    return types.SimpleNamespace(root=Path("/tmp"), run_root=Path("/tmp/run"), seconds=0,
                                  runner_metadata=Path("/tmp/runner-metadata.json"),
                                  plan={"identity": {"profile": {"resource_path": "/tmp/resource"},
                                                     "python": {"invocation": "/tmp/python/bin/python3.12",
                                                                "links": []}}})


@unittest.skipUnless(sys.platform.startswith("linux"), "the bubblewrap guard is Linux-only")
class LauncherGuardTest(unittest.TestCase):
    def test_a_missing_launcher_refuses_the_launch(self):
        with mock.patch.object(minative_isolation, "BWRAP_LAUNCHER", "/nonexistent/bwrap"):
            with self.assertRaises(MError) as failure:
                minative_isolation.execution_prefix(adapter())
        self.assertEqual(failure.exception.code, "isolation_unavailable")

    def test_a_launcher_that_is_not_a_file_also_refuses(self):
        # A directory at the launcher path is not a launcher: exec would fail
        # later, inside the sandbox setup, where it reads like a guest problem.
        with mock.patch.object(minative_isolation, "BWRAP_LAUNCHER", "/tmp"):
            with self.assertRaises(MError) as failure:
                minative_isolation.execution_prefix(adapter())
        self.assertEqual(failure.exception.code, "isolation_unavailable")

    def test_an_installed_launcher_gets_past_the_guard(self):
        # The guard must not refuse a host that does have bubblewrap: the next
        # failure is about the plan, not about the launcher.
        if not Path(minative_isolation.BWRAP_LAUNCHER).is_file():
            self.skipTest("bubblewrap is not installed")
        with self.assertRaises(MError) as failure:
            minative_isolation.execution_prefix(adapter())
        self.assertNotEqual(failure.exception.code, "isolation_unavailable")

    def test_the_argv_and_the_check_read_the_same_launcher(self):
        # One constant decides both what is execed and what is looked for, so a
        # policy change cannot leave the check looking at a different path.
        from azki_simulator import bwrap_policy
        self.assertEqual(bwrap_policy.LAUNCHER, minative_isolation.BWRAP_LAUNCHER)
        self.assertEqual(minative_isolation.BWRAP_PREAMBLE[0], bwrap_policy.LAUNCHER)


if __name__ == "__main__":
    unittest.main()
