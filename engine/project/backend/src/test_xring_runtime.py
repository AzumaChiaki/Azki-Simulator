"""Source-only checks for XRing media and pointer delivery."""
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zipfile
import zlib

from . import xring_media
from .xring_runner import XRingControl


class XRingRuntimeTests(unittest.TestCase):
    def test_fat16_file_chain_and_long_name(self):
        content = bytes(range(251))*25
        output = io.BytesIO()
        partition = {'first_lba':0, 'sectors':131072}
        xring_media._fat16(output, partition, 'USERDATA', [('persist.db', b'PERSIST DB ', content)])
        raw = output.getvalue()
        self.assertEqual(raw[510:512], b'\x55\xaa')
        sector = struct.unpack_from('<H',raw,11)[0]
        spc, nfats = raw[13], raw[16]
        reserved, roots, fat_sectors = (struct.unpack_from('<H',raw,p)[0] for p in (14,17,22))
        root = (reserved+nfats*fat_sectors)*sector
        self.assertEqual(raw[root+32+11],0x0f)
        self.assertEqual(raw[root+64:root+75],b'PERSIST DB ')
        start,size = struct.unpack_from('<HI',raw,root+64+26)
        self.assertEqual(size,len(content))
        data = root + (roots*32+sector-1)//sector*sector
        actual = bytearray()
        cluster = start
        while cluster < 0xfff8:
            actual.extend(raw[data+(cluster-2)*spc*sector:data+(cluster-1)*spc*sector])
            cluster = struct.unpack_from('<H',raw,reserved*sector+cluster*2)[0]
        self.assertEqual(actual[:size],content)
        self.assertEqual(raw[reserved*sector:(reserved+fat_sectors)*sector],
                         raw[(reserved+fat_sectors)*sector:(reserved+2*fat_sectors)*sector])

    def test_full_media_has_valid_gpt_and_unchanged_signed_members(self):
        config = json.loads(xring_media.PROFILE_FILE.read_text())
        sizes = {xring_media.MEMBERS[p['label']]:p['sectors']*512
                 for p in config['partitions'] if p['label'] in ('CP','CRF')}
        with tempfile.TemporaryDirectory(prefix='xring-media-test-') as temp:
            directory = Path(temp)
            ota = directory/'ota.zip'
            with zipfile.ZipFile(ota,'w',compression=zipfile.ZIP_DEFLATED) as archive:
                for name in set(xring_media.MEMBERS.values()):
                    size = sizes.get(name,444 if name=='vela_CP_boot.bin' else 32)
                    archive.writestr(name,b'\x56'+bytes(size-2)+b'\xa5')
            media = directory/'emmc.img'
            xring_media.initialize_media({'ota_path':str(ota)},media,directory,skip_oobe=False)
            self.assertEqual(media.stat().st_size,xring_media.EMMC_BYTES)
            with media.open('rb') as image:
                arrays = []
                for lba in (1,xring_media.EMMC_BYTES//512-1):
                    image.seek(lba*512)
                    header = bytearray(image.read(512))
                    self.assertEqual(header[:8],b'EFI PART')
                    stored = struct.unpack_from('<I',header,16)[0]
                    struct.pack_into('<I',header,16,0)
                    self.assertEqual(zlib.crc32(header[:92]),stored)
                    image.seek(struct.unpack_from('<Q',header,72)[0]*512)
                    entries = image.read(128*128)
                    self.assertEqual(zlib.crc32(entries),struct.unpack_from('<I',header,88)[0])
                    arrays.append(entries)
                self.assertEqual(arrays[0],arrays[1])
                labels = {}
                for index in range(26):
                    entry = arrays[0][index*128:(index+1)*128]
                    name = entry[56:].decode('utf-16le').split('\0')[0]
                    labels[name] = struct.unpack_from('<QQ',entry,32)
                for name in ('CP','CP_f','CRF','CRF_f'):
                    first,last = labels[name]
                    image.seek(first*512)
                    self.assertEqual(image.read(1),b'\x56')
                    image.seek((last+1)*512-1)
                    self.assertEqual(image.read(1),b'\xa5')
                self.assertEqual(labels['CP_boot'][1]-labels['CP_boot'][0]+1,2)
                first,_ = labels['ppt']
                image.seek(first*512)
                boot = image.read(512)
                root = first*512+(struct.unpack_from('<H',boot,14)[0]+boot[16]*struct.unpack_from('<H',boot,22)[0])*512
                image.seek(root+64+26)
                cluster,size = struct.unpack('<HI',image.read(6))
                data = root+struct.unpack_from('<H',boot,17)[0]*32
                image.seek(data+(cluster-2)*boot[13]*512)
                self.assertEqual(image.read(size),b'resetcause_flag=1\n')

    def test_release_sends_endpoint_move_then_up(self):
        class QMP:
            def __init__(self): self.calls=[]
            def execute(self, command, arguments): self.calls.append((command,arguments))
        qmp = QMP()
        with tempfile.TemporaryDirectory(prefix='xring-touch-test-') as directory:
            control = XRingControl(qmp,directory)
            control.frame_geometry = lambda:(466,466)
            with patch('time.sleep'):
                control.touch(10,20,True)
                control.touch(30,40,False)
            self.assertEqual(control.events,[{'x':10,'y':20,'down':True},
                {'x':30,'y':40,'down':True},{'x':30,'y':40,'down':False}])
            self.assertEqual([call[1]['events'][-1]['data']['down'] for call in qmp.calls],[True,True,False])
            self.assertIsNone(control.last_contact)
            with self.assertRaises(ValueError): control.touch(466,0,True)
            self.assertEqual(len(qmp.calls),3)


if __name__ == '__main__':
    unittest.main()
