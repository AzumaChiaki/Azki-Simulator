"""Do not advertise the new Q65 register inputs on older or unrelated cores."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] /
    "Sources/AzkiSimulator/Runtime"))
from azki_simulator.minative import board_runtime_capabilities


class Q65CapabilitiesTest(unittest.TestCase):
    def setUp(self):
        self.capabilities = {"nandPersistence": {}, "norPersistence": {},
            "virtualPeer": {}, "pmuAdcRaw": {"qemuSHA256": "a" * 64}}
        self.manifest = {"pmuAdcRaw": {"qemuSHA256": "a" * 64,
            "machines": ["best1503", "azki-q65"]}}

    def test_only_new_exact_q65_core_enables_adc(self):
        self.assertIn("pmuAdcRaw", board_runtime_capabilities(
            "miwear.watch.q65", self.capabilities, self.manifest))
        for manifest in (None, {}, {"pmuAdcRaw": {"qemuSHA256": "a" * 64}},
                {"pmuAdcRaw": {"qemuSHA256": "b" * 64, "machines": ["azki-q65"]}},
                {"pmuAdcRaw": {"qemuSHA256": "a" * 64, "machines": "azki-q65"}}):
            with self.subTest(manifest=manifest):
                filtered = board_runtime_capabilities("miwear.watch.q65", self.capabilities, manifest)
                self.assertNotIn("pmuAdcRaw", filtered)
                self.assertIn("norPersistence", filtered)

    def test_p65_and_existing_bands_keep_their_contract(self):
        self.assertNotIn("pmuAdcRaw", board_runtime_capabilities(
            "miwear.watch.p65", self.capabilities, self.manifest))
        self.assertEqual(self.capabilities, board_runtime_capabilities(
            "miwear.watch.q66", self.capabilities, self.manifest))


if __name__ == "__main__":
    unittest.main()
