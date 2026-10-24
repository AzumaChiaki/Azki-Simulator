"""Exercise the public parameter, runtime, runner and real IMU sample paths."""
import copy
import importlib.util
import json
import math
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import threading
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'Sources/AzkiSimulator/Runtime'))
from azki_simulator import live_commands as live
from azki_simulator import device_parameters as parameters
from test_device_parameters import ID, ITEM

spec = importlib.util.spec_from_file_location('orientation_runner_control', ROOT / 'engine/project/backend/src/control.py')
control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(control)

class FakeQMP:
    def __init__(self):
        self.value = 'auto'
        self.writes = []
        self.fail_write = False
        self.fail_readback = False
        self.reject_write = False
    def execute(self, command, arguments):
        assert arguments['path'] == '/machine' and arguments['property'] == 'motion-orientation'
        if command == 'qom-get':
            if self.writes and self.fail_readback: raise ConnectionError('readback disconnected')
            return self.value
        self.writes.append(arguments['value'])
        if self.fail_write: raise ConnectionError('uncertain transport failure')
        if self.reject_write: raise RuntimeError({'class':'GenericError','desc':'rejected'})
        self.value = arguments['value']

class OrientationTests(unittest.TestCase):
    def setUp(self):
        self.board = control.WatchControl.__new__(control.WatchControl)
        self.board.lock = threading.RLock()
        self.board.qmp = FakeQMP()
        self.adapter = types.SimpleNamespace(plan={'identity': {'profile': {'model': ITEM['model']}}})
        self.snapshot = dict(health=None, charger=None, adc=None, batteryChannel=2, ntcChannels=0,
                             orientation={'mode':'auto','rollDegrees':None,'pitchDegrees':None})
        self.sent = []
    def exchange(self, adapter, request, *, write):
        self.sent.append(request)
        if request['command'] == 'live.get': return copy.deepcopy(self.snapshot)
        reply = self.board.dispatch_input(request)
        if 'state' in reply: self.snapshot['orientation'] = reply['state']
        return copy.deepcopy(reply)
    def serialized_runner(self, adapter, request, **options):
        # Match serve_input's actual success/error envelope to exercise _exchange.
        try: result = {'return': self.exchange(adapter,request,write=options['write'])}
        except (ValueError, KeyError, TypeError, OSError, RuntimeError) as error: result = {'error':str(error)[:512]}
        return json.dumps(result).encode()
    def runtime(self, command, arguments=None):
        if command == 'status':
            return {'state':'running','capabilities':['live-get'],'width':212,'height':520}
        if command == 'live-get': return live.live_get(self.adapter, arguments or {})
        if command == 'live-set': return live.live_set(self.adapter, arguments)
        raise AssertionError(command)
    def test_public_parameter_roundtrip_to_fixed_qom_property_and_reset(self):
        with tempfile.TemporaryDirectory() as root, mock.patch.object(live, '_exchange', side_effect=self.exchange):
            value={'rollDegrees':30.5,'pitchDegrees':-90}
            reply=parameters.apply(Path(root).resolve(),ID,ITEM,{'live.orientation':value},self.runtime)
            row=next(r for r in reply['parameters']['parameters'] if r['id']=='live.orientation')
            self.assertEqual(value,row['value']); self.assertFalse(reply['guestObserved'])
            self.assertIsNone(row['observedValue']); self.assertEqual('degrees',row['units'])
            self.assertEqual(['roll=30.5;pitch=-90'],self.board.qmp.writes)
            reply=parameters.apply(Path(root).resolve(),ID,ITEM,{'live.orientation':None},self.runtime)
            self.assertEqual('auto',self.board.qmp.value)
            self.assertFalse(reply['restart_required'])
    def test_invalid_pose_never_writes_or_partially_updates(self):
        bad=[{},[],True,'auto',{'rollDegrees':0}, {'rollDegrees':0,'pitchDegrees':0,'extra':1}]
        for v in [True,None,'1',math.nan,math.inf,-math.inf,181,-181,10**1000]:
            bad.append({'rollDegrees':v,'pitchDegrees':0})
            bad.append({'rollDegrees':0,'pitchDegrees':v})
        for value in bad:
            with self.subTest(value=value), self.assertRaises(ValueError): live.validate_orientation(value)
            with self.subTest(runner=value), self.assertRaises(ValueError): self.board.orientation(value,write=True)
        self.assertEqual([],self.board.qmp.writes)
    def test_older_core_is_unavailable_and_existing_snapshot_still_works(self):
        self.snapshot.pop('orientation')
        with mock.patch.object(live,'_exchange',side_effect=self.exchange):
            self.assertFalse(live.live_get(self.adapter,{})['supported']['orientation'])
            with self.assertRaisesRegex(ValueError,'不支持'): live.live_set(self.adapter,{'orientation':None})
        self.assertEqual([],self.board.qmp.writes)
    def test_write_transport_failure_is_not_retried(self):
        self.board.qmp.fail_write=True
        with self.assertRaisesRegex(RuntimeError,'outcome_unknown'): self.board.orientation({'rollDegrees':0,'pitchDegrees':0},write=True)
        self.assertEqual(1,len(self.board.qmp.writes))
    def test_public_applied_write_with_failed_board_readback_stays_applied(self):
        self.board.qmp.fail_readback=True
        with tempfile.TemporaryDirectory() as root, mock.patch.object(live,'runner_exchange',side_effect=self.serialized_runner):
            reply=parameters.apply(Path(root).resolve(),ID,ITEM,{'live.orientation':{'rollDegrees':30,'pitchDegrees':45}},self.runtime)
        self.assertEqual(['live.orientation'],reply['applied'])
        self.assertEqual({'accepted':True,'guestObserved':False,'stateUnavailable':True},reply['receipt'])
        self.assertEqual('roll=30;pitch=45',self.board.qmp.value)
        self.assertEqual(1,len(self.board.qmp.writes))
    def test_public_write_transport_failure_is_explicitly_unknown_once(self):
        self.board.qmp.fail_write=True
        with mock.patch.object(live,'runner_exchange',side_effect=self.serialized_runner):
            with self.assertRaises(live.MError) as caught:
                live.live_set(self.adapter,{'orientation':{'rollDegrees':30,'pitchDegrees':45}})
        self.assertEqual('live_outcome_unknown',caught.exception.code)
        self.assertEqual(1,len(self.board.qmp.writes))
    def test_public_confirmed_rejection_is_distinct_from_unknown(self):
        self.board.qmp.reject_write=True
        with mock.patch.object(live,'runner_exchange',side_effect=self.serialized_runner):
            with self.assertRaises(live.MError) as caught:
                live.live_set(self.adapter,{'orientation':{'rollDegrees':30,'pitchDegrees':45}})
        self.assertEqual('invalid_live',caught.exception.code)
        self.assertEqual('auto',self.board.qmp.value)
    def test_orientation_success_does_not_refresh_runtime_twice(self):
        def exchange(adapter,request,*,write):
            if request['command']=='live.get' and self.board.qmp.writes: raise ConnectionError('postwrite refresh failure')
            return self.exchange(adapter,request,write=write)
        with mock.patch.object(live,'_exchange',side_effect=exchange):
            result=live.live_set(self.adapter,{'orientation':None})
        self.assertTrue(result['accepted']); self.assertEqual('auto',result['state']['mode'])
        self.assertEqual(1,len(self.board.qmp.writes))
    def test_parameter_schema_refresh_failure_preserves_orientation_ack(self):
        def call(command,arguments=None):
            if command=='status' and self.board.qmp.writes: raise ConnectionError('schema refresh unavailable')
            return self.runtime(command,arguments)
        with tempfile.TemporaryDirectory() as root, mock.patch.object(live,'runner_exchange',side_effect=self.serialized_runner):
            reply=parameters.apply(Path(root).resolve(),ID,ITEM,{'live.orientation':None},call)
        self.assertEqual(['live.orientation'],reply['applied']); self.assertTrue(reply['receipt']['accepted'])
        self.assertIn('refresh_error',reply); self.assertEqual(1,len(self.board.qmp.writes))
    def test_malformed_postwrite_receipt_is_unknown(self):
        for payload in [b'broken',b'{}',b'{"return":{}}',b'{"unexpected":1}']:
            def runner(adapter,request,**options):
                return payload if options['write'] else self.serialized_runner(adapter,request,**options)
            with self.subTest(payload=payload),mock.patch.object(live,'runner_exchange',side_effect=runner):
                with self.assertRaises(live.MError) as caught:live.live_set(self.adapter,{'orientation':None})
                self.assertEqual('live_outcome_unknown',caught.exception.code)
    def test_unsupported_preset_never_advertises_orientation(self):
        self.adapter.plan['identity']['profile']['model']='miwear.watch.q65'
        self.assertFalse(live._report(self.adapter,self.snapshot)['supported']['orientation'])
    def test_stopped_pose_is_unavailable_and_no_setting_is_written(self):
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaises(ValueError): parameters.apply(Path(root).resolve(),ID,ITEM,{'live.orientation':None})
    def test_production_imu_sample_registers(self):
        compiler=shutil.which('clang') or shutil.which('cc'); pkg=shutil.which('pkg-config')
        if not compiler or not pkg: self.skipTest('C compiler and GLib required as for QEMU')
        flags=shlex.split(subprocess.check_output([pkg,'--cflags','--libs','glib-2.0'],text=True))
        (ROOT/'build').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='orientation-',dir=ROOT/'build') as root:
            binary=Path(root)/'orientation'
            subprocess.run([compiler,'-O1','-g','-fsanitize=address,undefined',str(ROOT/'tests/test_orientation.c'),*flags,'-o',str(binary)],check=True)
            output=subprocess.check_output([str(binary)],text=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1','ASAN_OPTIONS':'abort_on_error=1'})
            self.assertIn('production IMU regression passed',output)

if __name__ == '__main__': unittest.main()
