"""XRing whole-media snapshot isolation, persistence and stopped-run commit."""
from pathlib import Path
import json
import os
import sys
import tempfile
import unittest
import uuid
from unittest.mock import patch
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'Sources/AzkiSimulator/Runtime'))
from azki_simulator import instance_storage as storage, xring_storage as xring
from azki_simulator.minative import _digest


class XRingStorageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='azki-xring-storage-', dir='/private/tmp')
        self.root = Path(self.tmp.name)
        self.library = self.root / 'library'; self.library.mkdir(mode=0o700)
        (self.library / 'instances').mkdir(mode=0o700)

    def tearDown(self):
        self.tmp.cleanup()

    def store(self):
        identifier = str(uuid.uuid4())
        parent = self.library / 'instances' / identifier; parent.mkdir(mode=0o700)
        return storage.InstanceStorage.create(parent / 'storage', xring.MODEL)

    def firmware(self):
        return storage.Firmware(xring.MODEL, 'miwear.watch.o62m@1.0', 0, xring.EMMC_BYTES, xring.KIND)

    def test_four_gib_remains_separate_from_nand(self):
        fw = self.firmware(); fw.validate()
        self.assertEqual(fw.images, [('emmc.img', 4294967296, None, 'emmcImage')])
        with self.assertRaisesRegex(ValueError, 'NAND'):
            storage.Firmware('miwear.watch.q66tc', 'q66', 0, xring.EMMC_BYTES, 'unknown').validate()
        with self.assertRaisesRegex(ValueError, 'XRing'):
            storage.Firmware(xring.MODEL, 'o62m', 0, 512*1024**2, xring.KIND).validate()

    def test_commit_second_boot_and_instance_isolation_without_disk_hash(self):
        a, b = self.store(), self.store()
        try:
            with patch.object(storage, '_initial_pin', side_effect=AssertionError('whole media hash')):
                first = a.begin(self.firmware()); self.assertTrue(first['emmc_initialize'])
                image = a.active / 'emmc.img'
                self.assertEqual(image.stat().st_size, xring.EMMC_BYTES)
                with image.open('r+b') as stream:
                    stream.seek(xring.EMMC_BYTES-4096); stream.write(b'persisted-name')
                a.finish(success=True); head = a.head()[0].name
                second = a.begin(self.firmware()); self.assertFalse(second['emmc_initialize'])
                with (a.active/'emmc.img').open('r+b') as stream:
                    stream.seek(xring.EMMC_BYTES-4096); self.assertEqual(stream.read(14), b'persisted-name')
                    stream.seek(xring.EMMC_BYTES-4096); stream.write(b'failed-change!')
                a.finish(success=False); self.assertEqual(a.head()[0].name, head)
                with (a.head()[0]/'emmc.img').open('rb') as stream:
                    stream.seek(xring.EMMC_BYTES-4096); self.assertEqual(stream.read(14), b'persisted-name')
                self.assertTrue(b.begin(self.firmware())['emmc_initialize'])
                with (b.active/'emmc.img').open('rb') as stream:
                    stream.seek(xring.EMMC_BYTES-4096); self.assertEqual(stream.read(14), b'\0'*14)
                b.finish(success=False)
        finally:
            a.close(); b.close()

    def test_sparse_copy_fallback_preserves_tail(self):
        source, target = self.root/'source.bin', self.root/'copy.bin'
        fd = os.open(source, os.O_RDWR|os.O_CREAT|os.O_EXCL, 0o600)
        os.ftruncate(fd, xring.EMMC_BYTES); os.pwrite(fd,b'tail',xring.EMMC_BYTES-4); os.close(fd)
        with patch.object(storage,'_clone_file',return_value=False):xring.copy_image(source,target)
        self.assertEqual(target.stat().st_size,xring.EMMC_BYTES)
        self.assertLess(target.stat().st_blocks*512, 8*1024**2)
        with target.open('rb') as stream:
            stream.seek(xring.EMMC_BYTES-4);self.assertEqual(stream.read(),b'tail')

    def session(self):
        root = self.root / ('azki-b-'+uuid.uuid4().hex[:12]); root.mkdir(mode=0o700)
        project = root/'prepared/project';project.mkdir(parents=True,mode=0o700)
        session = root/'session';session.mkdir(mode=0o700)
        instance_id=str(uuid.uuid4())
        identity={'profile':{'model':xring.MODEL,'ap':{'chip':'xring'}},'ap':{'path':'/unused/ap.bin','bytes':123,'sha256':'a'*64},
                  'qemu':{'sha256':'b'*64},'runtimeCapabilities':{}}
        plan={'identity':identity,'profileID':self.firmware().profile,'spec':{'project':str(project)}}
        binding=storage.prepare_session_storage(self.library,session,instance_id,plan)
        identity['instanceStorage']=binding;plan['approvalDigest']=_digest(identity)
        storage._atomic_json(session/'backend-plan.json',plan)
        storage._atomic_json(session/'session.json',{'approvalDigest':plan['approvalDigest']})
        run='m-0123456789ab';(session/run).mkdir(mode=0o700)
        broker=session/'native-broker';broker.mkdir(mode=0o700)
        storage._atomic_json(broker/'config.json',{'approvalDigest':plan['approvalDigest'],'id':'owned','run':run})
        storage._atomic_json(broker/'state.json',{'id':'owned','run':run,'state':'exited','cleanup':'reaped','exitCode':0})
        result={'returncode':0,'outcome':'window_closed','emmc_writeback':True,
                'emmc_image':binding['emmcImage'],'emmc_bytes':xring.EMMC_BYTES,'emmc_initialized':True}
        storage._atomic_json(session/run/'result.json',result)
        return session,instance_id,binding,plan,result

    def test_real_finish_binding_and_repeat_receipt(self):
        session,instance_id,binding,plan,result=self.session()
        with Path(binding['emmcImage']).open('r+b') as stream:stream.seek(4096);stream.write(b'guest')
        self.assertEqual(storage.instance_arguments(binding),['--instance-emmc-image',binding['emmcImage'],'--instance-emmc-initialize'])
        with patch.object(storage,'_initial_pin',side_effect=AssertionError('whole media hash')):
            done=storage.finish_session_storage(self.library,session,instance_id)
            self.assertTrue(done['committed'])
            self.assertEqual(storage.finish_session_storage(self.library,session,instance_id),done)
        store=storage.InstanceStorage(storage._instance_root(self.library,instance_id))
        try:
            with (store.head()[0]/'emmc.img').open('rb') as stream:stream.seek(4096);self.assertEqual(stream.read(5),b'guest')
        finally:store.close()

    def test_unconfirmed_writeback_rolls_back(self):
        session,instance_id,binding,plan,result=self.session()
        result['emmc_writeback']=False
        storage._atomic_json(session/'m-0123456789ab/result.json',result)
        receipt=storage.finish_session_storage(self.library,session,instance_id)
        self.assertFalse(receipt['committed']);self.assertEqual(receipt['reason'],'emmc-writeback-result-differs-from-instance')
        self.assertFalse((storage._instance_root(self.library,instance_id)/'head.json').exists())

    def test_reset_preserves_emmc_history_and_repairs_head(self):
        from azki_simulator.maintenance import reset_instance
        from azki_simulator.storage_maintenance import report
        store=self.store();identifier=store.root.parent.name
        revisions=[]
        try:
            for _ in range(3):
                store.begin(self.firmware()); revisions.append(store.active.name)
                store.finish(success=True)
        finally:store.close()
        reset_instance(self.library,identifier)
        self.assertEqual(sorted(p.name for p in (store.root/'revisions').iterdir()),sorted(revisions))
        overview=report(self.library,tmp_root=self.root)
        self.assertEqual(overview['instances'][0]['committedCount'],3)
        storage._atomic_json(store.root/'head.json',{'revision':'r-'+'0'*32})
        result=reset_instance(self.library,identifier)
        self.assertTrue(result['headRepaired'] in revisions)
        fixed=storage.InstanceStorage(store.root)
        try:self.assertIsNotNone(fixed.head())
        finally:fixed.close()

    def test_reset_keeps_failed_session_receipt(self):
        from azki_simulator.maintenance import reset_instance
        session,instance_id,binding,plan,result=self.session()
        result['emmc_writeback']=False
        storage._atomic_json(session/'m-0123456789ab/result.json',result)
        receipt=storage.finish_session_storage(self.library,session,instance_id)
        saved=(session/'instance-storage-result.json').read_bytes()
        reset_instance(self.library,instance_id)
        self.assertFalse(receipt['committed'])
        self.assertEqual((session/'instance-storage-result.json').read_bytes(),saved)

    def test_running_owner_cannot_publish(self):
        session,instance_id,binding,plan,result=self.session()
        state=storage._json(session/'native-broker/state.json');state['state']='running'
        storage._atomic_json(session/'native-broker/state.json',state)
        with self.assertRaisesRegex(ValueError,'尚未确认退出'):storage.finish_session_storage(self.library,session,instance_id)
        self.assertTrue((storage._instance_root(self.library,instance_id)/'pending.json').exists())

if __name__=='__main__':unittest.main()
