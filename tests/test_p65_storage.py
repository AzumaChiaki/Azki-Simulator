"""P65 original audio extraction and actual NOR installer; no firmware execution."""
import hashlib
import copy
import io
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'engine/project'))
import multi_device
from backend.src import runner
from backend.src.virtual_peer import state as peer_state
sys.path.insert(0,str(ROOT/'Sources/AzkiSimulator/Runtime'))
from azki_simulator import minative


class P65StorageTests(unittest.TestCase):
    def test_production_partition_boundaries_and_original_bytes(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        pkg = shutil.which('pkg-config')
        if not compiler or not pkg:
            self.skipTest('C compiler and GLib required as for QEMU')
        flags = shlex.split(subprocess.check_output([pkg,'--cflags','--libs','glib-2.0'], text=True))
        source = (ROOT / 'engine/qemu-overlay/hw/arm/azki-p65.c').read_text()
        functions = []
        for name in ('p65_install_audio','p65_resource_image_valid','best1503_set_factory_image'):
            match = re.search(r'^static [^;{}]*\b'+name+r'\([^;{}]*\)\n\{.*?^\}\n', source, re.M|re.S)
            self.assertIsNotNone(match, name)
            functions.append(match.group())
        (ROOT / 'build').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='p65-storage-',dir=ROOT/'build') as temporary:
            temporary=Path(temporary)
            (temporary/'p65-storage-functions.inc').write_text('\n'.join(functions))
            nor=temporary/'nor-config.bin'
            nor.write_bytes(b'\xff'*0x340000); nor.chmod(0o600)
            binary=temporary/'p65-storage'
            subprocess.run([compiler,'-O1','-g','-fsanitize=address,undefined','-I',str(temporary),
                            '-I',str(ROOT/'engine/qemu-overlay/hw/arm'),
                            str(ROOT/'tests/test_p65_storage.c'),*flags,'-o',str(binary)],check=True)
            extra=[]
            if os.environ.get('AZKI_P65_AUDIO_IMAGE'):
                original=Path(os.environ['AZKI_P65_AUDIO_IMAGE']).resolve()
                print('P65 original audio SHA256',hashlib.sha256(original.read_bytes()).hexdigest())
                extra=[str(original)]
            result=subprocess.run([str(binary),*extra],text=True,capture_output=True,
                env={**os.environ,'ASAN_OPTIONS':'abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1',
                     'AZKI_P65_TEST_NOR':str(nor)})
            self.assertEqual(0,result.returncode,result.stdout+result.stderr)
            self.assertIn('P65 storage production contract:',result.stdout)
            print(result.stdout.strip())


class P65FactoryContracts(unittest.TestCase):
    def test_prepared_p65_ap_boot_copy_needs_no_factory_seed(self):
        board={'machine':'azki-p65','local_setup_bypass':True,'factory_bytes':256*1024,
               'geometry':{'factory-offset':0xcc0000},'nor_geometry':{'factoryOffset':0xcc0000}}
        minative._validate_prepared_factory('miwear.watch.p65',board,None)
        old=copy.deepcopy(board)
        old['factory_bytes']=128*1024
        old['geometry']['factory-offset']=old['nor_geometry']['factoryOffset']=0xfe0000
        with self.assertRaisesRegex(minative.MError,'not migrated'):
            minative._validate_prepared_factory('miwear.watch.p65',old,None)
        for size in (128*1024,256*1024):
            with self.assertRaisesRegex(minative.MError,'unverified'):
                minative._validate_prepared_factory('miwear.watch.p65',board,{'bytes':size})
        with self.assertRaisesRegex(minative.MError,'128 KiB'):
            minative._validate_prepared_factory('miwear.watch.q65',{'local_setup_bypass':True},None)
        minative._validate_prepared_factory('miwear.watch.q65',{'local_setup_bypass':True},{'bytes':128*1024})

    def test_board_uses_real_partition_extent_and_preserves_identity(self):
        data=bytes(512)
        digest=hashlib.sha256(data).hexdigest()
        with tempfile.TemporaryDirectory() as directory:
            ap=Path(directory)/'ap.bin'; ap.write_bytes(data)
            profile={'id':'miwear.watch.p65@3.100.031@0123456789ab',
                     'model':'miwear.watch.p65','ap_path':str(ap),
                     'ap':{'size':len(data),'sha256':digest,'reset_sp':0x20010000}}
            board=multi_device.board_profile(profile,16)
        self.assertEqual(profile['id'],board['profile_id'])
        self.assertEqual(digest,board['ap_sha256'])
        self.assertEqual(0xcc0000,board['geometry']['factory-offset'])
        self.assertEqual(0xcc0000,board['nor_geometry']['factoryOffset'])
        self.assertEqual(256*1024,board['factory_bytes'])
        self.assertEqual(0xd00000,board['nor_geometry']['factoryOffset']+board['factory_bytes'])
        self.assertEqual(220*1024**2,board['geometry']['nand-data-offset'])
        self.assertEqual(292*1024**2,board['geometry']['nand-data-bytes'])
        runner.validate_p65_geometry(board)
        for change in ('old-factory','old-nand','config-extent'):
            old=copy.deepcopy(board)
            if change=='old-factory':
                old['factory_bytes']=128*1024
                old['geometry']['factory-offset']=old['nor_geometry']['factoryOffset']=0xfe0000
            elif change=='old-nand':
                old['geometry']['nand-data-offset']=240*1024**2
            else:
                old['geometry']['config-bytes']=old['nor_geometry']['configBytes']=8192
            with self.subTest(change=change),self.assertRaisesRegex(ValueError,'not migrated'):
                runner.validate_p65_geometry(old)

    def test_direct_factory_input_refused_without_mutation_and_q65_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'factory.bin'
            for size in (128*1024,256*1024):
                original=b'x'*size; path.write_bytes(original)
                with self.assertRaisesRegex(ValueError,'unverified'):
                    runner.validate_factory_input('azki-p65',path)
                self.assertEqual(original,path.read_bytes())
                if size==128*1024:
                    runner.validate_factory_input('azki-q65',path)
                else:
                    with self.assertRaises(ValueError):
                        runner.validate_factory_input('azki-q65',path)
            with self.assertRaisesRegex(ValueError,'unverified'):
                runner.validate_factory_input('azki-p65',Path(directory)/'absent')
            runner.validate_factory_input('azki-p65',None)

    def test_p65_startup_never_calls_generic_factory_generator_or_writes_nor(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'nor-config.bin'
            original=b'\xff'*0x340000; path.write_bytes(original)
            storage={'norInitialize':True,'norImage':str(path),'factoryBytes':256*1024}
            with patch.object(runner,'initialize_instance_nor',side_effect=AssertionError('unexpected write')), \
                    patch('backend.src.factory.local_desktop_image',side_effect=AssertionError('Q65 seed used')):
                for skip in (False,True):
                    self.assertTrue(runner.initialize_startup_nor(storage,None,skip_oobe=skip,
                        property_file='factory.prop',machine='azki-p65'))
                    with self.assertRaisesRegex(ValueError,'unverified'):
                        runner.initialize_startup_nor(storage,{},skip_oobe=skip,
                            property_file='factory.prop',machine='azki-p65')
                self.assertFalse(runner.initialize_startup_nor({**storage,'norInitialize':False},None,
                    skip_oobe=True,property_file='factory.prop',machine='azki-p65'))
            self.assertEqual(original,path.read_bytes())

    def test_p65_peer_factory_descriptor_refuses_seed_and_q65_unchanged(self):
        descriptor=peer_state._descriptor('p65','0'*64,'miwear.watch.p65',None)
        self.assertEqual(256*1024,descriptor['factoryBytes'])
        self.assertEqual('p65-raw-unverified',descriptor['factoryFormat'])
        self.assertIsNone(descriptor['factoryPropertyFile'])
        with patch.object(peer_state,'_fresh',return_value=descriptor):
            with self.assertRaisesRegex(ValueError,'unverified'):
                peer_state.factory_image({},skip_oobe=True)
        q65=peer_state._descriptor('q65','0'*64,'miwear.watch.q65',None)
        self.assertEqual(128*1024,q65['factoryBytes'])
        self.assertEqual('littlefs-8192x16',q65['factoryFormat'])
        self.assertEqual('factory.prop',q65['factoryPropertyFile'])


class P65AudioExtractionTests(unittest.TestCase):
    def package(self, payload=None, name='vela_audio.bin'):
        if payload is None:
            payload=bytes.fromhex('1cec57be')+bytes(range(256))*2
        stream=io.BytesIO()
        with zipfile.ZipFile(stream,'w',compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr(name,payload)
            info=archive.getinfo(name)
            record=dict(name=name,header_offset=info.header_offset,data_offset=info.header_offset+30+len(name),
                        compressed_size=info.compress_size,size=info.file_size,compression_method=info.compress_type,
                        crc32=f'{info.CRC:08x}',sha256=hashlib.sha256(payload).hexdigest())
        return stream.getvalue(),record,payload
    def test_checked_original_bytes_and_private_exclusive_output(self):
        package,record,payload=self.package()
        with tempfile.TemporaryDirectory() as directory:
            result=multi_device.extract_p65_audio(package,[record],directory)
            self.assertEqual(payload,result.read_bytes())
            self.assertEqual(0o600,result.stat().st_mode&0o777)
            with self.assertRaises(FileExistsError):
                multi_device.extract_p65_audio(package,[record],directory)
            self.assertEqual(payload,result.read_bytes())
    def test_missing_duplicate_oversize_or_truncated_inventory_fails_before_write(self):
        package,record,_=self.package()
        cases=[[],[record,record],[{**record,'size':3*1024**2+1}],[{**record,'size':0x127}]]
        with tempfile.TemporaryDirectory() as directory:
            for records in cases:
                with self.subTest(records=len(records)),self.assertRaises(ValueError):
                    multi_device.extract_p65_audio(package,records,directory)
                self.assertEqual([],list(Path(directory).iterdir()))
    def test_romfs_audio_is_not_accepted_as_p65_cp(self):
        package,record,_=self.package(b'-rom1fs-'+bytes(512),name='vela_audio_img.bin')
        with tempfile.TemporaryDirectory() as directory,self.assertRaisesRegex(ValueError,'raw BES'):
            multi_device.extract_p65_audio(package,[record],directory)
    def test_member_identity_mismatch_is_not_written(self):
        package,record,_=self.package()
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError,'SHA256'):
                multi_device.extract_p65_audio(package,[{**record,'sha256':'0'*64}],directory)
            self.assertEqual([],list(Path(directory).iterdir()))


if __name__ == '__main__':
    unittest.main()
