"""One AES-128 block through macOS CommonCrypto; no custom cryptography."""
import ctypes
import sys


class CryptoError(RuntimeError):
    pass


class CommonCryptoAES:
    provider = "macOS /usr/lib/system/libcommonCrypto.dylib CCCrypt"

    def __init__(self):
        if sys.platform != "darwin":
            raise CryptoError("This provider requires macOS")
        self._library = ctypes.CDLL("/usr/lib/system/libcommonCrypto.dylib")
        self._crypt = self._library.CCCrypt
        self._crypt.restype = ctypes.c_int32
        self._crypt.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
                               ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                               ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                               ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]

    def encrypt_block(self, key, plaintext):
        """NIST AES octet order; exactly one block, no padding or chaining."""
        if not isinstance(key, bytes) or not isinstance(plaintext, bytes) or len(key) != 16 or len(plaintext) != 16:
            raise ValueError("AES-128 needs a 16-byte key and a 16-byte block")
        key_buffer = (ctypes.c_ubyte * 16).from_buffer_copy(key)
        plaintext_buffer = (ctypes.c_ubyte * 16).from_buffer_copy(plaintext)
        output = (ctypes.c_ubyte * 16)()
        moved = ctypes.c_size_t()
        try:
            # CommonCryptor.h: kCCEncrypt=0, kCCAlgorithmAES=0,
            # kCCOptionECBMode=2; no kCCOptionPKCS7Padding.
            status = self._crypt(0, 0, 2, key_buffer, 16, None,
                                 plaintext_buffer, 16, output, 16, ctypes.byref(moved))
            if status != 0 or moved.value != 16:
                raise CryptoError("System AES failed")
            return bytes(output)
        finally:
            for buffer in (key_buffer, plaintext_buffer, output):
                ctypes.memset(buffer, 0, 16)

    def le_encrypt(self, key_little_endian, plaintext_little_endian):
        """HCI LE Encrypt parameters/result are each least-significant octet first."""
        return self.encrypt_block(key_little_endian[::-1], plaintext_little_endian[::-1])[::-1]
