"""System crypto providers must match the pinned byte contracts on every host.

AES is checked against the FIPS-197 known-answer vectors; ECDH is checked for
symmetry, private-representation round-trip and exact export length because
silent drift would corrupt pairing/install payloads on real devices.
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "engine/project/backend/src"))

from virtual_peer.crypto import aes_provider, CryptoError  # noqa: E402
from virtual_peer.auth_crypto import P256  # noqa: E402

FIPS197_KEY = bytes.fromhex("000102030405060708090a0b0c0d0e0f")
FIPS197_PLAINTEXT = bytes.fromhex("00112233445566778899aabbccddeeff")
FIPS197_CIPHERTEXT = bytes.fromhex("69c4e0d86a7b0430d8cdb78070b4c55a")
# Second vector from FIPS-197 Appendix C.2 (first AES-128 ECB block).
FIPS197_KEY_2 = bytes.fromhex("2b7e151628aed2a6abf7158809cf4f3c")
FIPS197_BLOCK_2 = bytes.fromhex("3243f6a8885a308d313198a2e0370734")
FIPS197_CIPHER_2 = bytes.fromhex("3925841d02dc09fbdc118597196a0b32")


class SystemAESTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.aes = aes_provider()
        except CryptoError as error:
            raise unittest.SkipTest(f"no system AES provider: {error}")

    def test_fips197_known_answers(self):
        self.assertEqual(self.aes.encrypt_block(FIPS197_KEY, FIPS197_PLAINTEXT), FIPS197_CIPHERTEXT)
        self.assertEqual(self.aes.encrypt_block(FIPS197_KEY_2, FIPS197_BLOCK_2), FIPS197_CIPHER_2)

    def test_le_encrypt_is_byte_reversal(self):
        result = self.aes.le_encrypt(FIPS197_KEY[::-1], FIPS197_PLAINTEXT[::-1])
        self.assertEqual(result, FIPS197_CIPHERTEXT[::-1])

    def test_rejects_wrong_sizes(self):
        with self.assertRaises(ValueError):
            self.aes.encrypt_block(FIPS197_KEY, FIPS197_PLAINTEXT[:-1])
        with self.assertRaises(ValueError):
            self.aes.encrypt_block(FIPS197_KEY[:-1], FIPS197_PLAINTEXT)


class P256Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.key = P256()
            cls.peer = P256()
        except ValueError as error:
            raise unittest.SkipTest(f"no system P-256 provider: {error}")

    def test_exchange_is_symmetric(self):
        self.assertEqual(len(self.key.public), 64)
        first = self.key.exchange(self.peer.public)
        second = self.peer.exchange(self.key.public)
        self.assertEqual(first, second)
        self.assertEqual(len(first), 32)

    def test_private_representation_round_trip(self):
        exported = self.key.export_private()
        self.assertEqual(len(exported), 97)
        self.assertEqual(exported[0], 4)
        restored = P256(exported)
        self.addCleanup(restored.close)
        self.assertEqual(restored.public, self.key.public)
        self.assertEqual(restored.exchange(self.peer.public), self.key.exchange(self.peer.public))

    def test_rejects_malformed_inputs(self):
        with self.assertRaises(ValueError):
            P256(b"\x04" * 96)
        with self.assertRaises(ValueError):
            self.key.exchange(b"\x00" * 63)

    def test_close_releases_without_error(self):
        key = P256()
        key.close()
        key.close()


if __name__ == "__main__":
    unittest.main()
