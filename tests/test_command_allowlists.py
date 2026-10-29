"""Three command allowlists have to stay in step: macOS UI, Linux UI, bridge.

Each desktop client gates a verb before it reaches control.sock, and the Python
dispatcher is the only party that can serve it. A verb added to one list and
missed in another is either a dead button or an unguarded call, and without a
booted guest nothing else notices -- which is exactly the window the Linux port
opens, because its client list was written by hand from the Swift one.
"""
import inspect
import re
import sys
import unittest
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "Sources/AzkiSimulator/Runtime"))

from azki_simulator.bridge import SessionBridge  # noqa: E402


def quoted(text):
    return set(re.findall(r'"([a-z][a-z-]*)"', text))


def swift_commands():
    source = (ROOT / "Sources/AzkiSimulator/Core/SimulatorSessionConnection.swift").read_text()
    block = re.search(r"let permitted = \[(.*?)\]", source, re.S).group(1)
    live = (ROOT / "Sources/AzkiSimulator/Core/SimulatorLiveControls.swift").read_text()
    return quoted(block) | set(re.findall(r'static let (?:get|set) = "([a-z-]+)"', live))


def csharp_commands():
    source = (ROOT / "linux/src/AzkiSimulator.Linux/Core/BridgeClient.cs").read_text()
    block = re.search(r"Permitted = new\(\)\s*\{(.*?)\}", source, re.S).group(1)
    return quoted(block)


def bridge_routes():
    """Verbs the dispatcher actually routes, and the ones it refuses outright.

    A plain literal scan would count "install" as served, when dispatch names
    it only to reject it -- so the refusal branch is read separately and taken
    out of the served set.
    """
    source = inspect.getsource(SessionBridge.dispatch)
    served = set(re.findall(r'command\s*==\s*"([a-z][a-z-]*)"', source))
    refused = set()
    for match in re.finditer(r'command\s+in\s+\(([^)]*)\)\s*:', source):
        verbs = set(re.findall(r'"([a-z][a-z-]*)"', match.group(1)))
        if re.match(r'\s*raise\s+BridgeError\(\s*"unsupported"', source[match.end():]):
            refused |= verbs
        else:
            served |= verbs
    return served, refused


class CommandAllowlistTests(unittest.TestCase):
    def setUp(self):
        self.swift = swift_commands()
        self.csharp = csharp_commands()
        self.served, self.refused = bridge_routes()

    def test_lists_are_not_empty(self):
        # A regex that silently stops matching would otherwise turn every
        # subset check below into a vacuous pass.
        self.assertGreaterEqual(len(self.swift), 20)
        self.assertGreaterEqual(len(self.csharp), 20)
        self.assertGreaterEqual(len(self.served), 20)
        self.assertGreaterEqual(len(self.refused), 1)

    def test_linux_ui_offers_nothing_the_macos_ui_does_not(self):
        self.assertEqual(self.csharp - self.swift, set())

    def test_macos_ui_offers_nothing_the_linux_ui_does_not(self):
        self.assertEqual(self.swift - self.csharp, set())

    def test_every_ui_verb_is_served_by_the_bridge(self):
        self.assertEqual(self.csharp - self.served, set())
        self.assertEqual(self.swift - self.served, set())

    def test_no_ui_verb_is_one_the_bridge_refuses(self):
        self.assertEqual(self.csharp & self.refused, set())
        self.assertEqual(self.swift & self.refused, set())

    def test_dispatcher_fails_closed_on_an_unknown_verb(self):
        self.assertIn("Unknown command", inspect.getsource(SessionBridge.dispatch))


class ConnectionTimeoutArgumentTests(unittest.TestCase):
    """Only guest-* commands may carry the host's wait budget.

    The helper checks an exact field set per command, and among the external
    bridge commands only external-open accepts connectionTimeoutSeconds, so a
    client that adds it everywhere cannot even poll bridge status.
    """

    def setUp(self):
        from azki_simulator import virtual_peer as peer  # noqa: PLC0415
        self.validate = peer.validate_arguments

    def test_helper_accepts_the_budget_on_guest_and_external_open_only(self):
        bridge = str(uuid.uuid4())
        self.validate("guest-connect", {"expected_session_id": "a" * 32, "expected_generation": 0,
                                        "connectionTimeoutSeconds": 30})
        self.validate("external-open", {"connectionTimeoutSeconds": 30})
        for command in ("external-status", "external-close", "external-reconnect"):
            with self.subTest(command), self.assertRaises(Exception):
                self.validate(command, {"bridgeID": bridge, "connectionTimeoutSeconds": 30})
            self.validate(command, {"bridgeID": bridge})  # the same call without the extra key is fine

    def test_linux_client_gates_the_argument_the_same_way(self):
        source = (ROOT / "linux/src/AzkiSimulator.Linux/Core/BridgeClient.cs").read_text()
        gate = re.search(r'if \((?P<condition>[^\n]*\))\n\s*body\["connectionTimeoutSeconds"\]', source)
        self.assertIsNotNone(gate, "connectionTimeoutSeconds assignment not found in BridgeClient")
        condition = gate.group("condition")
        self.assertIn('StartsWith("guest-")', condition)
        self.assertNotIn("external-", condition,
                        "external bridge commands must not carry connectionTimeoutSeconds")
        swift = (ROOT / "Sources/AzkiSimulator/Core/SimulatorSessionConnection.swift").read_text()
        self.assertIn('if command.hasPrefix("guest-") {\n            arguments["connectionTimeoutSeconds"]',
                      swift, "the Swift reference no longer gates the key the way the Linux client copies")


if __name__ == "__main__":
    unittest.main()
