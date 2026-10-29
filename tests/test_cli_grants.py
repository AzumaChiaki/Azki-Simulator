"""The CLI sandbox grants must match the reference policy, tree for tree.

The one-shot helper runs the Python CLI inside the sandbox the desktop app builds. On
macOS the same command's write access comes from StandaloneProcessSandbox.makeInspection.
A tree missing there is a command that dies on a read-only file system; a tree granted too
widely hands the whole firmware library to a command that only needed one instance folder.
Neither shows up until a user presses the button, and on Linux the buttons were wired by
hand, so the two tables are compared here instead of trusted.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SWIFT = ROOT / "Sources/AzkiSimulator/Core/StandaloneProcessSandbox.swift"
GRANTS = ROOT / "linux/src/AzkiSimulator.Linux/Core/CliGrants.cs"
MAINTENANCE = ROOT / "Sources/AzkiSimulator/Runtime/azki_simulator/maintenance.py"

# What the reference grants for the actions this app actually launches. The Swift switch
# is the source; these names are only what a reader would expect, and the test derives the
# sets from the file rather than from this list.
SHARED_ONLY_IN_REFERENCE = {"library-probe", "instance-identity", "instance-storage-finish", "prepare"}
# macOS has no instance-delete case at all: its own call site reports the helper refusing
# the command, so the instance's data is left behind there. Linux grants the instance
# container and re-binds the siblings read-only, and must not gain anything else.
LINUX_ONLY = {"instance-delete"}


def swift_grants():
    """The per-action write targets of makeInspection's switch."""
    body = SWIFT.read_text()
    start = body.index("static func makeInspection")
    end = body.index('if command != "prepare"', start)
    section = body[start:end]
    cases = {}
    marks = [(match.start(), match.end(), match.group(1))
             for match in re.finditer(r'case ("[a-z-]+"(?:, "[a-z-]+")*):', section)]
    for index, (_, label_end, labels) in enumerate(marks):
        block_end = marks[index + 1][0] if index + 1 < len(marks) else len(section)
        block = section[label_end:block_end]
        targets = set()
        for match in re.finditer(r"writes(?:\.append\(| \+= \[)([^)\]]*)", block):
            names = re.findall(r"[a-z_]+(?:Root)?", match.group(1))
            for name in names:
                if name == "library":
                    targets.add("library")
                elif name == "instance":
                    targets.add("instance")
                elif name in ("root", "session"):
                    targets.add("session")
        for label in re.findall(r'"([a-z-]+)"', labels):
            targets_for_label = set(targets)
            # The cleanup grant is not a bind but a name-prefix rule over the temp root,
            # which is what the Linux side reproduces by binding the roots it enumerated.
            if "sessionCleanupRules" in block:
                targets_for_label.add("session")
            cases.setdefault(label, set()).update(targets_for_label)
    return cases


def csharp_grants():
    """The write targets CliGrants.Apply grants, read off its own switch. An arm may stack
    several case labels above one body, so the arms are split on break rather than on labels."""
    text = GRANTS.read_text()
    start = text.index("case \"library-catalog\"")
    end = text.index("default:", start)
    section = text[start:end]
    cases = {}
    for arm in re.split(r"\bbreak;", section):
        labels = re.findall(r'case "([a-z-]+)":', arm)
        if not labels:
            continue
        targets = set()
        for match in re.finditer(r"ReadWriteTrees\.Add\(([^;]*)\)", arm):
            argument = match.group(1)
            if "InstanceFolder" in argument:
                targets.add("instance")
            elif "libraryRoot" in argument:
                targets.add("library")
            elif "StaleSessionRoots" in argument:
                targets.add("session")
            elif argument == "root" and "StaleSessionRoots(" in arm:
                targets.add("session")  # the foreach over the roots this host enumerated
            elif '"--session-root"' in argument:
                targets.add("session")
            else:
                raise AssertionError(f"unclassified write grant in CliGrants: {argument}")
        if "DeleteGrant(" in arm:
            # The delete grant is the instance container, because the command removes the
            # instance folder itself and a bound folder cannot be removed from inside.
            targets.add("instances")
        for label in labels:
            cases[label] = targets
    return cases


class CliGrantTableTests(unittest.TestCase):
    def setUp(self):
        self.swift = swift_grants()
        self.csharp = csharp_grants()

    def test_tables_are_not_empty(self):
        self.assertGreaterEqual(len(self.swift), 8)
        self.assertGreaterEqual(len(self.csharp), 6)

    def test_shared_actions_grant_the_same_trees(self):
        for action in sorted(set(self.swift) & set(self.csharp)):
            with self.subTest(action):
                self.assertEqual(self.swift[action], self.csharp[action])

    def test_only_the_documented_actions_differ(self):
        self.assertEqual(set(self.csharp) - set(self.swift), LINUX_ONLY)
        missing = SHARED_ONLY_IN_REFERENCE - set(self.csharp)
        self.assertEqual(missing, SHARED_ONLY_IN_REFERENCE,
                         "an action the Linux app does not launch must not be granted silently")

    def test_instance_delete_grants_the_container_and_protects_the_siblings(self):
        # The reference has no case for it, so the grant is pinned against what the CLI
        # itself touches. It removes the instance folder, and a folder bwrap binds is a
        # mount point that cannot be unlinked from inside, so the container is granted
        # and every sibling inside it is re-bound read-only.
        self.assertEqual(self.csharp["instance-delete"], {"instances"})
        grants = GRANTS.read_text()
        self.assertIn("ProtectedTrees.Add", grants)
        policy = (ROOT / "linux/src/AzkiSimulator.Linux/Core/SandboxPolicy.cs").read_text()
        self.assertIn("ProtectedTrees", policy)
        # The re-bind has to come after the writable bind, or the writable one wins.
        writable = policy.index('"--bind", tree, tree')
        protected = policy.index('"--ro-bind", tree, tree', writable)
        self.assertGreater(protected, writable)

    def test_session_prefixes_agree_with_the_cleaner(self):
        declared = re.search(r"SESSION_PREFIXES = \(([^)]*)\)", MAINTENANCE.read_text()).group(1)
        python = set(re.findall(r'"([^"]+)"', declared))
        csharp = set(re.findall(r'"([^"]+)"', re.search(r"SessionPrefixes = \{([^)]*)\}",
                                                        GRANTS.read_text()).group(1)))
        self.assertEqual(python, csharp)


if __name__ == "__main__":
    unittest.main()
