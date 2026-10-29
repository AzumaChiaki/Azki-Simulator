"""The Linux CI job has to stay honest about what it builds and what it runs.

The workflow is the only place where the package name, the Release tag and the
test selection meet. Three ways it can go wrong, none of them visible until a
push: the build number written into the artifact name disagrees with the tag,
the release job looks for archives the build never writes, or a test that needs
a user namespace is not excluded from the container job that cannot create one.
Each case here reads the shipped files rather than a restatement of them.
"""
import os
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / ".github/workflows/build-linux.yml"
BUILD_SCRIPT = ROOT / "scripts/build-linux.sh"
TEST_ROOT = ROOT / "linux/tests/AzkiSimulator.Linux.Tests"


class LinuxWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.workflow = WORKFLOW.read_text()
        self.build = BUILD_SCRIPT.read_text()

    def test_the_run_number_reaches_the_file_the_package_name_is_built_from(self):
        # build-linux.sh names the artifact after the BUILD_NUMBER file, while
        # the Release tag comes from the workflow's own arithmetic. The two
        # agree only if the step writes the number back to the file.
        self.assertRegex(self.workflow, r"printf '%s\\n' \"\$number\" > BUILD_NUMBER")
        self.assertLess(self.workflow.index("> BUILD_NUMBER"),
                        self.workflow.index("./scripts/build-linux.sh"),
                        "the number has to be written before the build reads it")
        self.assertRegex(self.build, r'number="\$\(tr -d .\\n\\r. < "\$root/BUILD_NUMBER"\)')

    def test_both_jobs_compute_the_same_number(self):
        numbers = re.findall(r"number=\$\(\(base - 1 \+ GITHUB_RUN_NUMBER\)\)", self.workflow)
        self.assertEqual(len(numbers), 2, "the build and the release job must agree on the number")

    def test_the_release_job_looks_for_the_archives_the_build_writes(self):
        name = re.search(r'name="Azki-Simulator-\$version-build\$number-\$configuration-linux-\$arch"',
                         self.build)
        self.assertIsNotNone(name, "the package name template changed")
        self.assertIn("-linux-", name.group(0))
        self.assertIn("-name '*-linux-*.tar.gz'", self.workflow)
        # The two source archives are excluded by name, because they contain
        # "-linux-" as well and would otherwise be uploaded as packages.
        self.assertIn("! -name '*-source.tar.gz'", self.workflow)
        self.assertIn("! -name '*-engine-sources.tar.gz'", self.workflow)
        self.assertIn('-source.tar.gz', self.build)
        self.assertIn('-engine-sources.tar.gz', self.build)

    def test_the_container_job_excludes_the_tests_that_need_a_user_namespace(self):
        # A GitHub container job cannot unshare a user namespace, so bwrap
        # cannot start there; the sandboxed suites are excluded by category.
        self.assertIn('dotnet test linux/AzkiSimulator.Linux.sln --filter "Category!=Integration"',
                      self.workflow)

    def test_every_sandboxed_test_carries_that_category(self):
        # A test that constructs the sandboxed runner really runs bwrap, so it
        # has to be excluded by the filter above or the container job fails.
        sources = {path: path.read_text() for path in sorted(TEST_ROOT.glob("*.cs"))}
        for path, source in sources.items():
            if "new InspectionRunner" not in source:
                continue
            with self.subTest(path.name):
                if "[Fact]" not in source and "[Theory]" not in source:
                    # A shared helper runs bwrap only through the tests that call it,
                    # and each of those is checked here as its own file. Naming the
                    # helper is enough: a test that stops calling it loses the trait
                    # check but also loses the bwrap call it needed the trait for.
                    for caller, text in sources.items():
                        if caller == path or path.stem not in text:
                            continue
                        self.assertIn('Trait("Category", "Integration")', text,
                                      f"{caller.name} runs {path.stem} without the Integration trait")
                    continue
                self.assertIn('Trait("Category", "Integration")', source,
                              "a test that starts bwrap without the Integration trait "
                              "breaks the container job")

    def test_the_macos_job_names_the_interpreter_it_installed(self):
        # The runner's own python3 is the Xcode command line tools one, not the
        # interpreter the engine was built against.
        macos = self.workflow[self.workflow.index("tests-macos:"):]
        self.assertIn("python3.12 -m unittest", macos)

    def test_the_boot_acceptance_has_an_entry_point(self):
        # The container job cannot unshare a user namespace, so the full boot
        # ladder needs a runner that can; the script is that entry point and
        # it drives the integration suite rather than a copy of it.
        script = ROOT / "scripts/verify-linux-boot.sh"
        self.assertTrue(script.is_file(), "the boot acceptance entry point is missing")
        self.assertTrue(os.access(script, os.X_OK), "the boot acceptance script is not executable")
        text = script.read_text()
        self.assertIn("AZKI_FIRMWARE", text)
        self.assertIn('Category=Integration', text)
        # The sandbox has to be there and has to start; a script that only
        # mentions bwrap would report a firmware failure for a sandbox one.
        self.assertIn("command -v bwrap", text)
        self.assertIn("bwrap --ro-bind", text)

    def test_the_boot_job_waits_for_a_runner_that_can(self):
        # A job that starts a boot without bwrap would fail on the runner
        # instead of on the firmware, so it only runs where it was asked for.
        self.assertIn("boot:", self.workflow)
        boot = self.workflow[self.workflow.index("\n  boot:"):]
        boot = boot[:boot.index("\n  tests-macos:")]
        self.assertIn("self-hosted", boot)
        self.assertIn("AZKI_BOOT_RUNNER", boot)
        self.assertIn("workflow_dispatch", boot)
        self.assertIn("verify-linux-boot.sh", boot)


if __name__ == "__main__":
    unittest.main()
