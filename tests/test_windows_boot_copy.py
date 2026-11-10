"""Synthetic-only P65 ROMFS transformation and native private-copy contracts.

No real firmware is patched or executed by these tests. Actual P65 acceptance
requires its separately authorized source/boot-copy provenance and guest run.
"""
from __future__ import annotations
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT/'engine/project'), str(ROOT/'engine/project/backend')]
import p65_setup
from src.windows_boot_copy import build_p65_boot_copy, write_windows_boot_copy
from src.windows_runtime_files import WindowsFiles

IDENTITY = {'ro.factory.psn':'AZKI-P65-012345ABCDEF', 'ro.factory.did':'9901234567890'}


def fixture(*, offset=0x40c, ap_bytes=0x8000, ap_offset='0xC0000', watch=True):
    image = bytearray(ap_bytes)
    header = ('CHIP=best1502p\nKERNEL=NUTTX\nFLASH_SIZE=0x1000000\nOTA_CODE_OFFSET='+ap_offset+'\n').encode()
    image[0x80:0x80+len(header)] = header
    props = b'ro.product.device.devicetype='+(b'watch' if watch else b'band')+b'\npersist.locale=en_US\n'
    def inode(at, nxt, spec, body, name):
        raw = struct.pack('>4I',nxt,spec,len(body),0)+name.encode()+b'\0'
        raw = bytearray(raw.ljust((len(raw)+15)&~15,b'\0'))
        struct.pack_into('>I',raw,12,p65_setup.checksum(bytes(raw)))
        image[offset+at:offset+at+len(raw)] = raw
        image[offset+at+len(raw):offset+at+len(raw)+len(body)] = body
    image[offset:offset+8] = b'-rom1fs-'
    struct.pack_into('>II',image,offset+8,512,0)
    image[offset+16:offset+21] = b'azki\0'
    inode(32,64|1,32,b'', '.')
    inode(64,2,0,props,'build.prop')
    struct.pack_into('>I',image,offset+12,p65_setup.checksum(bytes(image[offset:offset+512])))
    return bytes(image)


def build(original, **kwargs):
    values={'expected_source_sha256':hashlib.sha256(original).hexdigest(),
            'model':'miwear.watch.p65','skip_oobe':True,'identity_properties':IDENTITY}
    values.update(kwargs)
    return build_p65_boot_copy(original,**values)


class P65BootCopyStructureTests(unittest.TestCase):
    def test_transparent_copy_and_original_bytes_unchanged(self):
        original=fixture();before=hashlib.sha256(original).hexdigest();output,receipt=build(original)
        self.assertEqual(hashlib.sha256(original).hexdigest(),before)
        self.assertEqual(len(output),len(original))
        self.assertNotEqual(output,original)
        self.assertEqual(receipt['sourceAPHash'],before)
        self.assertEqual(receipt['bootCopySHA'],hashlib.sha256(output).hexdigest())
        self.assertTrue(receipt['bootCopyModified'])
        self.assertTrue(receipt['sourceBytesUnchanged'])
        self.assertIsNone(receipt['originalAPUnchanged'])
        self.assertFalse(receipt['guestExecutionVerified'])
        self.assertEqual(output[:0x40c],original[:0x40c])
        self.assertNotIn(IDENTITY['ro.factory.psn'],json.dumps(receipt))
        self.assertNotIn(IDENTITY['ro.factory.did'],json.dumps(receipt))
        base,size,link,content=p65_setup._locate_romfs(output)
        self.assertEqual(base,0x40c)  # ROMFS alignment is relative, not absolute.
        self.assertEqual(link['kind'],0)
        self.assertGreater(size,512)
        data=output[base+content['data']:base+content['data']+content['length']]
        for key,value in {**p65_setup.LOCAL_PROPERTIES,**IDENTITY}.items():
            self.assertIn((key+'='+value+'\n').encode(),data)

    def test_local_ui_only_recipe_without_identity(self):
        output,receipt=build(fixture(),identity_properties=None)
        self.assertEqual(receipt['identityPropertyNames'],[])
        self.assertNotIn(b'ro.factory.did=',output)
        self.assertNotIn(b'ro.factory.psn=',output)

    def test_skip_oobe_false_never_calls_existing_unconditional_recipe(self):
        for properties in (None,IDENTITY):
            with self.subTest(identity=properties is not None):
                with mock.patch.object(p65_setup,'local_boot_image') as helper:
                    with self.assertRaisesRegex(ValueError,'OOBE-preserving'):
                        build(fixture(),skip_oobe=False,identity_properties=properties)
                    helper.assert_not_called()

    def test_exact_source_hash_and_immutable_bytes(self):
        with self.assertRaisesRegex(ValueError,'selected firmware hash'):
            build(fixture(),expected_source_sha256='0'*64)
        with self.assertRaises(ValueError):
            build_p65_boot_copy(bytearray(fixture()),expected_source_sha256='0'*64,model='miwear.watch.p65',skip_oobe=True)

    def test_model_and_layout_mismatch_rejected(self):
        with self.assertRaises(ValueError):build(fixture(),model='miwear.watch.q65')
        with self.assertRaisesRegex(ValueError,'AP layout'):build(fixture(ap_offset='0xB0000'))
        with self.assertRaises(ValueError):build(fixture(watch=False))

    def test_identity_limits_no_auth_or_setup_key_injection(self):
        for properties in [{}, {'ro.factory.psn':'real-device','ro.factory.did':'1234567890123'},
                           {**IDENTITY,'ro.factory.psk':'0'*32},
                           {**IDENTITY,'persist.setup_wizard':'1'},
                           {**IDENTITY,'ro.factory.psn':'AZKI-P65-012345ABCDEF\nroot=1'}]:
            with self.subTest(keys=list(properties)),self.assertRaises(ValueError):
                build(fixture(),identity_properties=properties)

    def test_corrupt_ambiguous_and_no_padding_romfs_rejected(self):
        corrupt=bytearray(fixture());corrupt[0x40c+12]^=1
        with self.assertRaises(ValueError):build(bytes(corrupt))
        ambiguous=bytearray(fixture());extra=fixture(offset=0x140c)
        ambiguous[0x140c:0x160c]=extra[0x140c:0x160c]
        with self.assertRaises(ValueError):build(bytes(ambiguous))
        with self.assertRaises(ValueError):build(fixture(ap_bytes=0x40c+512))

    def test_existing_local_copy_not_silently_applied_twice(self):
        output,_=build(fixture())
        with self.assertRaisesRegex(ValueError,'already contains'):
            build(output)

    def test_unexpected_change_outside_recipe_is_rejected(self):
        original=fixture();real=p65_setup.local_boot_image
        def changed(source,properties):
            output=bytearray(real(source,properties));output[0x20]^=1;return bytes(output)
        with mock.patch.object(p65_setup,'local_boot_image',side_effect=changed):
            with self.assertRaisesRegex(ValueError,'outside'):
                build(original)


@unittest.skipUnless(os.name=='nt','Requires native Windows private file APIs')
class WindowsBootCopyFileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.api=WindowsFiles();cls.api.initialize_private_creator()
        parent=Path(os.environ.get('AZKI_WINDOWS_BOOT_COPY_TEST_ROOT',tempfile.gettempdir()))
        cls.root=cls.api.private_directory(parent/('synthetic-boot-copy-'+uuid.uuid4().hex))

    def transaction(self):
        area=self.api.private_directory(self.root/uuid.uuid4().hex)
        source=area/'原始-AP🌙.bin';source.write_bytes(fixture())
        return area,source,area/'独立-boot-copy.bin'

    def write(self,source,target,**kwargs):
        values={'expected_source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
                'model':'miwear.watch.p65','skip_oobe':True,'identity_properties':IDENTITY,'api':self.api}
        values.update(kwargs)
        return write_windows_boot_copy(source,target,**values)

    def test_native_private_copy_source_and_disk_readback(self):
        _,source,target=self.transaction();before=source.read_bytes();receipt=self.write(source,target)
        self.assertEqual(source.read_bytes(),before)
        self.assertTrue(receipt['originalAPUnchanged'])
        self.assertTrue(receipt['diskReadbackVerified'])
        self.assertTrue(receipt['bootCopyModified'])
        self.assertEqual(hashlib.sha256(target.read_bytes()).hexdigest(),receipt['bootCopySHA'])
        h=self.api.open_read(target);self.api.close(h)
        print(json.dumps({'fixtureOnly':True,'actualGuestStarted':False,'receipt':receipt}),flush=True)

    def test_existing_destination_and_source_alias_preserved(self):
        _,source,target=self.transaction();target.write_bytes(b'keep-existing')
        with self.assertRaises(OSError):self.write(source,target)
        self.assertEqual(target.read_bytes(),b'keep-existing')
        with self.assertRaises(ValueError):self.write(source,source)
        self.assertEqual(source.read_bytes(),fixture())

    def test_rejected_hash_or_false_oobe_creates_no_output(self):
        _,source,target=self.transaction()
        with self.assertRaises(ValueError):self.write(source,target,expected_source_sha256='0'*64)
        self.assertFalse(target.exists())
        with self.assertRaises(ValueError):self.write(source,target,skip_oobe=False)
        self.assertFalse(target.exists())

    def test_native_readonly_source_is_preserved(self):
        _,source,target=self.transaction();k=self.api.kernel
        k.SetFileAttributesW.argtypes=[c.c_wchar_p,c.c_uint32];k.SetFileAttributesW.restype=c.c_int
        self.assertTrue(k.SetFileAttributesW(str(source),1))
        try:
            receipt=self.write(source,target)
            self.assertTrue(receipt['originalAPUnchanged'])
        finally:k.SetFileAttributesW(str(source),0x80)


if __name__=='__main__':unittest.main(verbosity=2)
