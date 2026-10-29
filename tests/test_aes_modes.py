"""AES-CCM and AES-CTR layouts, pinned to published known answers.

auth_crypto promises known-answer coverage of the exact nonce/tag layouts
before these modes seal anything for a guest, so the coverage has to exist.
The CCM cases are the NIST SP 800-38C vectors (as shipped in OpenSSL's
evpciph_aes_ccm_cavs.txt) and the CTR cases are SP 800-38A F.2.1. NIST
publishes no 8-byte-nonce / 1-byte-AAD / 4-byte-tag case, which is the exact
shape SMP seals with, so those two were produced with python-cryptography
49.0.0 as an independent implementation of the same construction.
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "engine/project/backend/src"))

from virtual_peer.crypto import CryptoError  # noqa: E402
from virtual_peer.auth_crypto import AESModes  # noqa: E402

# label, key, nonce, associated data, plaintext, ciphertext, tag
CCM_VECTORS = [
    ("7n0a4t", "19EBFDE2D5468BA0A3031BDE629B11FD", "5A8AA485C316E9", "",
     "3796CF51B8726652A4204733B8FBB047CF00FB91A9837E22",
     "A90E8EA44085CED791B2FDB7FD44B5CF0BD7D27718029BB7", "03E1FA6B"),
    ("7n32a4t", "F9FDCA4AC64FE7F014DE0F43039C7571", "5A8AA485C316E9",
     "3796CF51B8726652A4204733B8FBB047CF00FB91A9837E22EC22B1A268F88E2C",
     "A265480CA88D5F536DB0DC6ABC40FAF0D05BE7A966977768",
     "6BE31860CA271EF448DE8F8D8B39346DAF4B81D7E92D65B3", "38F125FA"),
    ("7n0a16t", "197AFB02FFBD8F699DACAE87094D5243", "5A8AA485C316E9", "",
     "3796CF51B8726652A4204733B8FBB047CF00FB91A9837E22",
     "24AB9EEB0E5508CAE80074F1070EE188A637171860881F1F", "2D9A3FBC210595B7B8B1B41523111A8E"),
    ("7n32a16t", "A7AA635EA51B0BB20A092BD5573E728C", "5A8AA485C316E9",
     "3796CF51B8726652A4204733B8FBB047CF00FB91A9837E22EC22B1A268F88E2C",
     "A265480CA88D5F536DB0DC6ABC40FAF0D05BE7A966977768",
     "B351AB96B2E45515254558D5212673EE6C776D42DBCA3B51", "2CF3A20B7FD7C49E6E79BEF475C2906F"),
    ("8n32a16t", "0B6256BD328A4CDA2510D527C0F73ED4", "21FD9011D6D9484A",
     "66FF35C4F86AD7755B149E14E299034763023E7384F4AF8C35277D2C7E1A7DE2",
     "78A292662B8E05ABC2D44FBEFD0840795E7493028015D9F2",
     "5A0BE834C57B59D47A4590D8D19A1206D3C06E937A9B57F7", "4034D9FDB43C3F48932AA72177B23BF6"),
    ("10n32a16t", "6CCB68D3838D4DDF660B9CD904CAD40F", "C4FB7519A19F13D9D1FC",
     "092E64FEF08B5655A86CDB8DE63FFAA7772E8730844E9016141AF8BAD2216246",
     "5EA35C082E2B190E9D98E6B2DAAD8672F587B4F2968072FC",
     "CDA5FE3D15D00150B99120C7F206B88A4C2C4A39CA914342", "5603AB284A73A38CC916F8B653C92AB4"),
    ("12n32a16t", "005E8F4D8E0CBF4E1CEEB5D87A275848", "0EC3AC452B547B9062AAC8FA",
     "2F1821AA57E5278FFD33C17D46615B77363149DBC98470413F6543A6B749F2CA",
     "B6F345204526439DAF84998F380DCFB4B4167C959C04FF65",
     "9575E16F35DA3C88A19C26A7B762044F4D7BBBAFEFF05D75", "4829E2A7752FA3A14890972884B511D8"),
    ("13n0a4t", "197AFB02FFBD8F699DACAE87094D5243", "5A8AA485C316E9403AFF859FBB", "",
     "A16A2E741F1CD9717285B6D882C1FC53655E9773761AD697",
     "4A550134F94455979EC4BF89AD2BD80D25A77AE94E456134", "A3E138B9"),
    ("13n1a16t", "08B0DA255D2083808A1B4D367090BACC", "777828B13679A9E2CA89568233", "DD",
     "1B156D7E2BF7C9A25AD91CFF7B0B02161CB78FF9162286B0",
     "E8B80AF4960D5417C15726406E345C5C46831192B03432EE", "D16B6282283E16602331BCCA9D51CE76"),
    ("13n8a16t", "98A42D7A0C5917DEAF3B4DE3F0CBE0A1", "03D33AB0C2DF7BFCE88B5EE4C4",
     "2D5438B728B950D9", "9AA9C8358117564371366BEEEC923051EF433252197AAAD5",
     "9FF942BAA60F440C17A78E9581216B9A947A67F04D54911F", "EECFFF971FDFAA856310B014AA59C978"),
    ("8n1a4t", "303132333435363738393A3B3C3D3E3F", "1112131415161718", "07",
     "00112233445566778899AABB", "8016C80BDA4D79DCC8F3DA33", "CF03A06E"),
    ("8n1a4t", "303132333435363738393A3B3C3D3E3F", "1CACCBA91D54FF27", "2A",
     "BDB6CC92ED893C3A396B1EAEF7B4A31DEF5C4F",
     "BA2227BB6E1760BEA25936C8F9DBB10E9244B8", "88CF95D6"),
]

# SP 800-38A F.2.1: key, counter block, plaintext, ciphertext.
CTR_VECTORS = [
    ("AE6852F8121067CC4BF7A5765577F39E", "00000030000000000000000000000001",
     "53696E676C6520626C6F636B206D7367", "E4095D4FB7A7B3792D6175A3261311B8"),
    ("7E24067817FAE0D743D6CE1F32539163", "006CB6DBC0543B59DA48D90B00000001",
     "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F",
     "5104A106168A72D9790D41EE8EDAD388EB2E1EFC46DA57C8FCE630DF9141BE28"),
    ("7691BE035E5020A8AC6E618529F9A0DC", "00E0017B27777F3F4A1786F000000001",
     "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F20212223",
     "C1CF48A89F2FFDD9CF4652E9EFDB72D74540A42BDE6D7836D59A5CEAAEF3105325B2072F"),
]


class AesModeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.modes = AESModes()
        except CryptoError as error:
            raise unittest.SkipTest(f"no system AES provider: {error}")

    def test_ccm_known_answers(self):
        for label, key, nonce, aad, plaintext, ciphertext, tag in CCM_VECTORS:
            key, nonce = bytes.fromhex(key), bytes.fromhex(nonce)
            aad, plaintext = bytes.fromhex(aad), bytes.fromhex(plaintext)
            tag = bytes.fromhex(tag)
            sealed = bytes.fromhex(ciphertext) + tag
            with self.subTest(label):
                self.assertEqual(self.modes.ccm_encrypt(key, nonce, plaintext, aad, len(tag)), sealed)
                self.assertEqual(self.modes.ccm_decrypt(key, nonce, sealed, aad, len(tag)), plaintext)

    def test_ccm_rejects_a_changed_byte(self):
        key, nonce, aad, plaintext = bytes(16), bytes(8), b"\x07", b"azki"
        sealed = self.modes.ccm_encrypt(key, nonce, plaintext, aad, 4)
        for index, label in ((0, "ciphertext"), (len(sealed) - 1, "tag")):
            tampered = sealed[:index] + bytes([sealed[index] ^ 1]) + sealed[index + 1:]
            with self.subTest(label):
                with self.assertRaises(ValueError):
                    self.modes.ccm_decrypt(key, nonce, tampered, aad, 4)
        with self.assertRaises(ValueError):
            self.modes.ccm_decrypt(key, nonce, sealed, b"\x08", 4)

    def test_ccm_rejects_unsupported_layouts(self):
        key, plaintext = bytes(16), b"azki"
        with self.assertRaises(ValueError):
            self.modes.ccm_encrypt(key, bytes(6), plaintext, b"", 4)
        with self.assertRaises(ValueError):
            self.modes.ccm_encrypt(key, bytes(14), plaintext, b"", 4)
        with self.assertRaises(ValueError):
            self.modes.ccm_encrypt(key, bytes(8), plaintext, b"", 5)
        with self.assertRaises(ValueError):
            self.modes.ccm_decrypt(key, bytes(8), b"\x00" * 3, b"", 4)

    def test_ctr_known_answers(self):
        for key, counter, plaintext, ciphertext in CTR_VECTORS:
            with self.subTest(counter):
                data = self.modes.ctr(bytes.fromhex(key), bytes.fromhex(counter), bytes.fromhex(plaintext))
                self.assertEqual(data, bytes.fromhex(ciphertext))
                # CTR is its own inverse, and the counter has to roll per block.
                self.assertEqual(self.modes.ctr(bytes.fromhex(key), bytes.fromhex(counter), data),
                                 bytes.fromhex(plaintext))

    def test_ctr_rejects_oversized_input(self):
        with self.assertRaises(ValueError):
            self.modes.ctr(bytes(16), bytes(16), bytes(65537))
        with self.assertRaises(ValueError):
            self.modes.ctr(bytes(16), bytes(15), b"azki")


if __name__ == "__main__":
    unittest.main()
