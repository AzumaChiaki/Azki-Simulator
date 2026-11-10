"""Ephemeral Windows CNG AES/P-256 providers; no keystore or third-party crypto.

AES exposes the existing single-block interface. P256 exports the same X9.63
04||X||Y||d representation used by the existing synthetic-instance state.
The ECDH raw secret is converted from CNG's little endian to standard big endian
before the unchanged protocol HKDF. No private material is logged.
"""
from __future__ import annotations

import ctypes as C
import os
import struct
from .crypto import CryptoError


class WindowsCryptoError(CryptoError):
    pass


def _library():
    if os.name != "nt":
        raise WindowsCryptoError("Windows CNG provider requires Windows")
    library = C.WinDLL("bcrypt", use_last_error=True)
    pointer, size, status = C.c_void_p, C.c_uint32, C.c_int32
    declarations = (
        ("BCryptOpenAlgorithmProvider", [C.POINTER(pointer), C.c_wchar_p, C.c_wchar_p, size]),
        ("BCryptCloseAlgorithmProvider", [pointer, size]),
        ("BCryptSetProperty", [pointer, C.c_wchar_p, pointer, size, size]),
        ("BCryptGetProperty", [pointer, C.c_wchar_p, pointer, size, C.POINTER(size), size]),
        ("BCryptGenerateSymmetricKey", [pointer, C.POINTER(pointer), pointer, size, pointer, size, size]),
        ("BCryptEncrypt", [pointer, pointer, size, pointer, pointer, size, pointer, size, C.POINTER(size), size]),
        ("BCryptDestroyKey", [pointer]),
        ("BCryptGenerateKeyPair", [pointer, C.POINTER(pointer), size, size]),
        ("BCryptFinalizeKeyPair", [pointer, size]),
        ("BCryptExportKey", [pointer, pointer, C.c_wchar_p, pointer, size, C.POINTER(size), size]),
        ("BCryptImportKeyPair", [pointer, pointer, C.c_wchar_p, C.POINTER(pointer), pointer, size, size]),
        ("BCryptSecretAgreement", [pointer, pointer, C.POINTER(pointer), size]),
        ("BCryptDeriveKey", [pointer, C.c_wchar_p, pointer, pointer, size, C.POINTER(size), size]),
        ("BCryptDestroySecret", [pointer]),
    )
    for name, arguments in declarations:
        function = getattr(library, name)
        function.argtypes = arguments
        function.restype = status
    return library


def _checked(status, operation):
    if status < 0:
        raise WindowsCryptoError(f"Windows CNG {operation} failed (NTSTATUS 0x{status & 0xffffffff:08x})")


def _buffer(value):
    return (C.c_ubyte * len(value)).from_buffer_copy(value)


def _clear(value):
    if value is not None:
        C.memset(value, 0, C.sizeof(value))


class BCryptAES:
    provider = "Windows bcrypt.dll AES-128 ECB single-block"

    def __init__(self):
        self.handle = C.c_void_p()
        self.library = _library()
        try:
            _checked(self.library.BCryptOpenAlgorithmProvider(C.byref(self.handle), "AES", None, 0), "open AES")
            mode = C.create_unicode_buffer("ChainingModeECB")
            _checked(self.library.BCryptSetProperty(self.handle, "ChainingMode", mode, C.sizeof(mode), 0), "set AES mode")
            self.object_bytes = C.c_uint32()
            returned = C.c_uint32()
            _checked(self.library.BCryptGetProperty(self.handle, "ObjectLength", C.byref(self.object_bytes),
                                                    C.sizeof(self.object_bytes), C.byref(returned), 0), "AES object size")
            if returned.value != 4 or not 1 <= self.object_bytes.value <= 65536:
                raise WindowsCryptoError("Windows CNG AES object size is invalid")
        except BaseException:
            self.close()
            raise

    def encrypt_block(self, key, plaintext):
        if not isinstance(key, bytes) or not isinstance(plaintext, bytes) or len(key) != 16 or len(plaintext) != 16:
            raise ValueError("AES-128 needs a 16-byte key and a 16-byte block")
        if not self.handle:
            raise WindowsCryptoError("Windows CNG AES provider is closed")
        object_memory = (C.c_ubyte * self.object_bytes.value)()
        key_memory, plaintext_memory = _buffer(key), _buffer(plaintext)
        output = (C.c_ubyte * 16)()
        handle, returned = C.c_void_p(), C.c_uint32()
        try:
            _checked(self.library.BCryptGenerateSymmetricKey(self.handle, C.byref(handle), object_memory,
                                                             len(object_memory), key_memory, 16, 0), "create AES key")
            _checked(self.library.BCryptEncrypt(handle, plaintext_memory, 16, None, None, 0,
                                                 output, 16, C.byref(returned), 0), "encrypt AES block")
            if returned.value != 16:
                raise WindowsCryptoError("Windows CNG AES output length is invalid")
            return bytes(output)
        finally:
            if handle:
                self.library.BCryptDestroyKey(handle)
            for value in (object_memory, key_memory, plaintext_memory, output):
                _clear(value)

    def le_encrypt(self, key_little_endian, plaintext_little_endian):
        return self.encrypt_block(key_little_endian[::-1], plaintext_little_endian[::-1])[::-1]

    def close(self):
        if self.handle:
            self.library.BCryptCloseAlgorithmProvider(self.handle, 0)
            self.handle = C.c_void_p()

    def __del__(self):
        self.close()


class BCryptP256:
    """Nonpersistent, system-validated P-256 ECDH with explicit handle lifetime."""
    PUBLIC_MAGIC = 0x314b4345
    PRIVATE_MAGIC = 0x324b4345

    def __init__(self, private_data=None):
        self.handle, self.private = C.c_void_p(), C.c_void_p()
        self.library = _library()
        try:
            _checked(self.library.BCryptOpenAlgorithmProvider(C.byref(self.handle), "ECDH_P256", None, 0), "open P256")
            if private_data is None:
                _checked(self.library.BCryptGenerateKeyPair(self.handle, C.byref(self.private), 256, 0), "generate P256")
                _checked(self.library.BCryptFinalizeKeyPair(self.private, 0), "finalize P256")
            else:
                if not isinstance(private_data, bytes) or len(private_data) != 97 or private_data[0] != 4:
                    raise ValueError("Self-generated P256 private representation required")
                blob = _buffer(struct.pack("<II", self.PRIVATE_MAGIC, 32) + private_data[1:])
                try:
                    _checked(self.library.BCryptImportKeyPair(self.handle, None, "ECCPRIVATEBLOB", C.byref(self.private),
                                                              blob, len(blob), 0), "import P256 private key")
                finally:
                    _clear(blob)
            public = self._export("ECCPUBLICBLOB", self.PUBLIC_MAGIC, 72)
            self.public = public[8:]
        except BaseException:
            self.close()
            raise

    def _export(self, kind, magic, expected):
        if not self.private:
            raise WindowsCryptoError("Windows CNG P256 key is closed")
        returned = C.c_uint32()
        output = (C.c_ubyte * expected)()
        try:
            _checked(self.library.BCryptExportKey(self.private, None, kind, output, expected,
                                                  C.byref(returned), 0), "export P256 key")
            raw = bytes(output)
            if returned.value != expected or struct.unpack_from("<II", raw) != (magic, 32):
                raise WindowsCryptoError("Windows CNG P256 key format is invalid")
            return raw
        finally:
            _clear(output)

    def exchange(self, public):
        if not isinstance(public, bytes) or len(public) != 64:
            raise ValueError("P256 public key must be 64 bytes")
        if not self.private:
            raise WindowsCryptoError("Windows CNG P256 key is closed")
        blob = _buffer(struct.pack("<II", self.PUBLIC_MAGIC, 32) + public)
        peer, secret, returned = C.c_void_p(), C.c_void_p(), C.c_uint32()
        output = (C.c_ubyte * 32)()
        try:
            _checked(self.library.BCryptImportKeyPair(self.handle, None, "ECCPUBLICBLOB", C.byref(peer),
                                                      blob, len(blob), 0), "import P256 peer")
            _checked(self.library.BCryptSecretAgreement(self.private, peer, C.byref(secret), 0), "P256 agreement")
            _checked(self.library.BCryptDeriveKey(secret, "TRUNCATE", None, output, 32,
                                                  C.byref(returned), 0), "derive raw P256 agreement")
            if returned.value != 32:
                raise WindowsCryptoError("Windows CNG P256 shared secret length is invalid")
            return bytes(output)[::-1]
        finally:
            if secret:
                self.library.BCryptDestroySecret(secret)
            if peer:
                self.library.BCryptDestroyKey(peer)
            _clear(blob)
            _clear(output)

    def export_private(self):
        return b"\x04" + self._export("ECCPRIVATEBLOB", self.PRIVATE_MAGIC, 104)[8:]

    def close(self):
        if self.private:
            self.library.BCryptDestroyKey(self.private)
            self.private = C.c_void_p()
        if self.handle:
            self.library.BCryptCloseAlgorithmProvider(self.handle, 0)
            self.handle = C.c_void_p()

    def __del__(self):
        self.close()
