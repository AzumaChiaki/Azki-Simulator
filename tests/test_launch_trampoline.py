"""The launch trampolines must refuse anything that is not a sandbox launcher.

Both trampolines are spawned by the host with a fixed argv: the launcher they
exec has to be one of the two supported ones, and the parent has to be the
process that spawned them. A refusal is a hard stop — the trampoline never
falls back to running the payload without a sandbox.
"""
from pathlib import Path
import os
import subprocess
import sys
import unittest

RUNTIME = Path(__file__).resolve().parents[1] / "Sources/AzkiSimulator/Runtime"
sys.path.insert(0, str(RUNTIME))
from azki_simulator import bwrap_policy

LAUNCHERS = ("/usr/bin/sandbox-exec", "/usr/bin/bwrap")
REFUSALS = {"launch.py": "Invalid isolated launch invocation",
            "minative_launch.py": "Invalid isolated runner invocation"}


class TrampolineLauncherTest(unittest.TestCase):
    def scripts(self):
        package = RUNTIME / "azki_simulator"
        return [package / "launch.py", package / "minative_launch.py"]

    def invoke(self, script, launcher, parent=None):
        metadata = Path(os.environ.get("TMPDIR", "/tmp")) / "runner-process.json"
        arguments = ["0", str(metadata), str(os.getpid() if parent is None else parent), launcher]
        # The runner insists on owning its process group, so the child gets its
        # own session: the launcher stays the only variable in the invocation.
        return subprocess.run([sys.executable, "-I", "-S", str(script), *arguments],
                              capture_output=True, text=True, timeout=60, start_new_session=True)

    def test_a_launcher_outside_the_set_is_refused(self):
        for script in self.scripts():
            with self.subTest(script=script.name):
                result = self.invoke(script, "/usr/bin/not-a-launcher")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(REFUSALS[script.name], result.stdout + result.stderr)

    def test_a_foreign_parent_is_refused_even_with_a_supported_launcher(self):
        for script in self.scripts():
            with self.subTest(script=script.name):
                result = self.invoke(script, bwrap_policy.LAUNCHER, parent=1)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(REFUSALS[script.name], result.stdout + result.stderr)

    def test_both_trampolines_accept_exactly_the_two_supported_launchers(self):
        for script in self.scripts():
            with self.subTest(script=script.name):
                source = script.read_text()
                self.assertIn("SUPPORTED_LAUNCHERS", source)
                self.assertIn('"/usr/bin/sandbox-exec"', source)
                self.assertIn('"/usr/bin/bwrap"', source)
                self.assertIn("not in SUPPORTED_LAUNCHERS", source)
                # The platform-exact form is what the Linux port replaced: a
                # trampoline that only accepts this host's launcher cannot run
                # the other host's sandbox.
                self.assertNotIn("EXPECTED_LAUNCHER", source)

    def test_the_shared_policy_names_one_of_the_supported_launchers(self):
        self.assertIn(bwrap_policy.LAUNCHER, LAUNCHERS)
        self.assertEqual(bwrap_policy.PREAMBLE[0], bwrap_policy.LAUNCHER)


if __name__ == "__main__":
    unittest.main()
