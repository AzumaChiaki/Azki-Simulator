"""One AES-128 block through a system crypto library; no custom cryptography.

macOS uses CommonCrypto (CCCrypt); Linux uses libcrypto's EVP AES-128-ECB.
Both providers expose the same encrypt_block/le_encrypt contract, byte for
byte, and are covered by known-answer tests in tests/ because silent drift
would corrupt pairing and install payloads.
"""
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


class OpenSSLAES:
    provider = "Linux libcrypto EVP AES-128-ECB"

    def __init__(self):
        if not sys.platform.startswith("linux"):
            raise CryptoError("This provider requires Linux")
        import ctypes.util
        name = ctypes.util.find_library("crypto") or "libcrypto.so.3"
        self._library = ctypes.CDLL(name)
        pointer, integer = ctypes.c_void_p, ctypes.c_int
        byte_pointer = ctypes.POINTER(ctypes.c_ubyte)
        for function, restype, argtypes in (
                ("EVP_get_cipherbyname", pointer, [ctypes.c_char_p]),
                ("EVP_CIPHER_CTX_new", pointer, []),
                ("EVP_CIPHER_CTX_free", None, [pointer]),
                ("EVP_CIPHER_CTX_set_padding", integer, [pointer, integer]),
                ("EVP_EncryptInit_ex", integer, [pointer, pointer, pointer, pointer, pointer]),
                ("EVP_EncryptUpdate", integer, [pointer, byte_pointer, ctypes.POINTER(integer),
                                                 byte_pointer, integer]),
                ("EVP_EncryptFinal_ex", integer, [pointer, byte_pointer, ctypes.POINTER(integer)])):
            call = getattr(self._library, function)
            call.restype = restype
            call.argtypes = argtypes
        self._cipher = self._library.EVP_get_cipherbyname(b"AES-128-ECB")
        if not self._cipher:
            raise CryptoError("System AES cipher unavailable")

    def encrypt_block(self, key, plaintext):
        """NIST AES octet order; exactly one block, no padding or chaining."""
        if not isinstance(key, bytes) or not isinstance(plaintext, bytes) or len(key) != 16 or len(plaintext) != 16:
            raise ValueError("AES-128 needs a 16-byte key and a 16-byte block")
        key_buffer = (ctypes.c_ubyte * 16).from_buffer_copy(key)
        plaintext_buffer = (ctypes.c_ubyte * 16).from_buffer_copy(plaintext)
        output = (ctypes.c_ubyte * 16)()
        written = ctypes.c_int(0)
        final_written = ctypes.c_int(0)
        context = self._library.EVP_CIPHER_CTX_new()
        if not context:
            raise CryptoError("System AES failed")
        try:
            if (self._library.EVP_EncryptInit_ex(context, self._cipher, None, key_buffer, None) != 1 or
                    self._library.EVP_CIPHER_CTX_set_padding(context, 0) != 1 or
                    self._library.EVP_EncryptUpdate(context, output, ctypes.byref(written),
                                                    plaintext_buffer, 16) != 1 or
                    written.value != 16 or
                    self._library.EVP_EncryptFinal_ex(context, ctypes.cast(ctypes.addressof(output) + 16,
                                                                           ctypes.POINTER(ctypes.c_ubyte)),
                                                      ctypes.byref(final_written)) != 1 or
                    final_written.value != 0):
                raise CryptoError("System AES failed")
            return bytes(output)
        finally:
            self._library.EVP_CIPHER_CTX_free(context)
            for buffer in (key_buffer, plaintext_buffer, output):
                ctypes.memset(buffer, 0, 16)

    def le_encrypt(self, key_little_endian, plaintext_little_endian):
        """HCI LE Encrypt parameters/result are each least-significant octet first."""
        return self.encrypt_block(key_little_endian[::-1], plaintext_little_endian[::-1])[::-1]


def aes_provider():
    """The system AES provider for this host; unknown hosts fail closed."""
    if sys.platform == "darwin":
        return CommonCryptoAES()
    if sys.platform.startswith("linux"):
        return OpenSSLAES()
    raise CryptoError("No system AES provider for this host")
