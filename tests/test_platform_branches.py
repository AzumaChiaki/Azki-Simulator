"""Linux platform branches in the host layer must be reachable, not just plausible.

discovery and bridge each grew a sys.platform path that no test on macOS would
ever execute; a missing import or a diverging launcher path fails only when a
developer on Linux calls it, so these invariants are pinned here.
"""
from pathlib import Path
import os
import sys
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
    def test_bridge_and_policy_agree(self):
        from azki_simulator import bridge
        from azki_simulator import launch
        from azki_simulator import minative_launch
        expected = "/usr/bin/sandbox-exec" if sys.platform == "darwin" else bwrap_policy.LAUNCHER
        for module in (bridge, launch, minative_launch):
            self.assertEqual(module.EXPECTED_LAUNCHER, expected, module.__name__)


if __name__ == "__main__":
    unittest.main()
