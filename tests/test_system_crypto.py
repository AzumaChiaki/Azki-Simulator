"""System crypto providers must match the pinned byte contracts on every host.

AES is checked against the FIPS-197 known-answer vectors; ECDH is checked
against an independent pure-Python P-256 implementation, so the shared-secret
bytes, the 0x04||X||Y||K private layout and the raw-X (no KDF) result are
pinned identically on macOS and Linux instead of merely agreeing with
themselves. Silent drift would corrupt pairing/install payloads on devices.
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "engine/project/backend/src"))

from virtual_peer.crypto import aes_provider, CryptoError  # noqa: E402
from virtual_peer.auth_crypto import P256  # noqa: E402

# FIPS 186-4 curve parameters, written out here on purpose: the system
# provider has to agree with these numbers, not with itself.
P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
A = P - 3
B = 0x5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B
G = (0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296,
     0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5)
N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
# A published P-256 key pair (OpenSSL's vendored curve table), so the oracle is
# checked against something outside this repository before it judges the host.
PUBLISHED_PRIVATE = 0xC477F9F65C22CCE20657FAA5B2D1D8122336F851A508A1ED04E479C34985BF96
PUBLISHED_PUBLIC = (0xB7E08AFDFE94BAD3F1DC8C734798BA1C62B3A0AD1E9EA2A38201CD0889BC7A19,
                    0x3603F747959DBF7A4BB226E41928729063ADC7AE43529E61B563BBC606CC5E09)
SCALAR_ONE = 0x0102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F20
SCALAR_TWO = 0x2122232425262728292A2B2C2D2E2F303132333435363738393A3B3C3D3E3F40


def point_add(first, second):
    if first is None: return second
    if second is None: return first
    x1, y1 = first; x2, y2 = second
    if x1 == x2:
        if (y1 + y2) % P == 0: return None
        slope = (3*x1*x1 + A) * pow(2*y1, P-2, P) % P
    else:
        slope = (y2 - y1) * pow(x2 - x1, P-2, P) % P
    x3 = (slope*slope - x1 - x2) % P
    return (x3, (slope*(x1 - x3) - y1) % P)


def point_mul(scalar, point):
    result, addend = None, point
    while scalar:
        if scalar & 1: result = point_add(result, addend)
        addend = point_add(addend, addend)
        scalar >>= 1
    return result


def affine_bytes(point):
    return point[0].to_bytes(32, "big") + point[1].to_bytes(32, "big")

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


class CurveOracleTests(unittest.TestCase):
    """The oracle itself has to be a valid P-256 before it can judge anything."""

    def test_generator_is_on_the_curve(self):
        self.assertEqual(pow(G[1], 2, P), (pow(G[0], 3, P) + A*G[0] + B) % P)

    def test_scalar_multiplication_is_consistent(self):
        self.assertEqual(point_mul(1, G), G)
        self.assertEqual(point_mul(N, G), None)
        one = point_mul(SCALAR_ONE, G)
        two = point_mul(SCALAR_TWO, G)
        self.assertEqual(point_mul(SCALAR_ONE, two), point_mul(SCALAR_TWO, one))
        self.assertEqual(point_mul(SCALAR_ONE, two), point_mul(SCALAR_ONE*SCALAR_TWO % N, G))
        self.assertEqual(point_add(one, None), one)

    def test_published_key_pair(self):
        self.assertEqual(point_mul(PUBLISHED_PRIVATE, G), PUBLISHED_PUBLIC)


class P256KnownAnswerTests(unittest.TestCase):
    """Byte-for-byte agreement with the oracle, which is what both hosts share."""

    @classmethod
    def setUpClass(cls):
        try:
            cls.first = P256(b"\x04" + affine_bytes(point_mul(SCALAR_ONE, G))
                             + SCALAR_ONE.to_bytes(32, "big"))
            cls.second = P256(b"\x04" + affine_bytes(point_mul(SCALAR_TWO, G))
                              + SCALAR_TWO.to_bytes(32, "big"))
        except ValueError as error:
            raise unittest.SkipTest(f"no system P-256 provider: {error}")
        cls.addClassCleanup(cls.first.close)
        cls.addClassCleanup(cls.second.close)

    def test_public_key_matches_the_expected_point(self):
        self.assertEqual(self.first.public, affine_bytes(point_mul(SCALAR_ONE, G)))
        self.assertEqual(self.second.public, affine_bytes(point_mul(SCALAR_TWO, G)))

    def test_shared_secret_is_the_raw_x_coordinate(self):
        expected = point_mul(SCALAR_ONE*SCALAR_TWO % N, G)[0].to_bytes(32, "big")
        self.assertEqual(self.first.exchange(self.second.public), expected)
        self.assertEqual(self.second.exchange(self.first.public), expected)

    def test_private_export_layout(self):
        exported = self.first.export_private()
        self.assertEqual(exported, b"\x04" + self.first.public + SCALAR_ONE.to_bytes(32, "big"))

    def test_reimported_key_keeps_the_same_bytes(self):
        restored = P256(self.first.export_private())
        self.addCleanup(restored.close)
        self.assertEqual(restored.public, self.first.public)
        self.assertEqual(restored.exchange(self.second.public),
                         point_mul(SCALAR_ONE*SCALAR_TWO % N, G)[0].to_bytes(32, "big"))


if __name__ == "__main__":
    unittest.main()
