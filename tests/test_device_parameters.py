"""External parameter schema is capability-bound and does not fabricate readings."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'Sources/AzkiSimulator/Runtime'))
from azki_simulator import device_parameters as api
from azki_simulator import instance_settings as settings

ID = '2145689e-edb4-4a13-a025-fd69e58adcc0'
ITEM = {'model':'miwear.watch.o66cn','profileID':'o66-exact','firmwareVersion':'3.1.28','width':212,'height':520}

class DeviceParametersTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.library = Path(self.tmp.name).resolve()
        self.sent = []
        self.live = {'supported':{'heartRate':True,'steps':True,'batteryLevel':True,'batteryRaw':True,'charger':True,'temperature':False},
                     'state':{'heartRate':72,'worn':True,'cadence':100,'batteryRaw':21000,'charger':'unplugged','walked':0,'walkSteps':0},'notes':{}}
        self.adc = {'state':{'values':list(range(8)),'readCount':[1]*8,'lastReadVersion':[0]*8,'version':0}}
    def tearDown(self): self.tmp.cleanup()
    def call(self, command, arguments=None):
        self.sent.append((command,arguments))
        if command == 'status': return {'state':'running','adapterState':'running','capabilities':['live-get','pmu-adc-get'],'width':212,'height':520}
        if command == 'live-get': return copy.deepcopy(self.live)
        if command == 'pmu-adc-get': return copy.deepcopy(self.adc)
        if command == 'live-set': self.live['state'].update(arguments); return copy.deepcopy(self.live)
        if command == 'pmu-adc-set': self.adc['state']['values']=arguments['values']; return {'accepted':True,'guestObserved':False,**self.adc}
        raise AssertionError(command)
    def schema(self, call=None, item=ITEM): return api.describe(self.library,ID,item,call)
    def write(self, key, value, call=None): return api.apply(self.library,ID,ITEM,{key:value},call)
    def row(self,key,schema=None): return next(x for x in (schema or self.schema())['parameters'] if x['id']==key)
    def test_stopped_live_disabled_and_boot_capability(self):
        self.assertFalse(self.row('live.heartRate')['available']); self.assertTrue(self.row('boot.health.heartRate')['available'])
        self.assertFalse(self.schema()['running'])
    def test_no_false_battery_percentage_observation(self):
        row=self.row('live.batteryLevel',self.schema(self.call))
        self.assertIsNone(row['value']);self.assertIsNone(row['observedValue']);self.assertTrue(row['writeOnly'])
    def test_live_request_refetches_actual_input(self):
        result=self.write('live.heartRate',91,self.call)
        self.assertFalse(result['restart_required']);self.assertFalse(result['guestObserved'])
        row=self.row('live.heartRate',result['parameters']);self.assertEqual(91,row['value']);self.assertIsNone(row['observedValue'])
    def test_invalid_type_range_readonly_unknown_and_batch_are_rejected(self):
        for key,value in [('live.heartRate',True),('live.heartRate',256),('live.worn',1),('model','fake'),('arbitrary','bad'),('live.temperatureC',float('nan'))]:
            with self.subTest(key=key,value=value),self.assertRaises(ValueError):self.write(key,value,self.call)
        with self.assertRaises(ValueError):api.apply(self.library,ID,ITEM,{'live.worn':True,'live.heartRate':72},self.call)
        self.assertFalse(any(c.endswith('-set') for c,_ in self.sent))
    def test_unsupported_model_has_no_health_boot_mutation(self):
        schema=self.schema(item={**ITEM,'model':'miwear.watch.q65'})
        self.assertFalse(self.row('boot.health.heartRate',schema)['available'])
        self.assertFalse(self.row('boot.battery.level',schema)['available'])
    def test_no_live_mutation_while_stopped(self):
        with self.assertRaises(ValueError):self.write('live.heartRate',80)
    def test_restart_save_preserves_unrelated_fields(self):
        original={'schema':1,'clock':{'mode':'firmware'},'properties':{'persist.custom':'keep'},'health':{'worn':False}}
        settings.save(self.library,ID,original)
        result=self.write('boot.health.heartRate',99)
        saved=settings.load(self.library,ID)
        self.assertTrue(result['restart_required']);self.assertEqual('keep',saved['properties']['persist.custom'])
        self.assertFalse(saved['health']['worn']);self.assertEqual(99,saved['health']['heartRate'])
    def test_null_unsets_boot_override(self):
        self.write('boot.health.heartRate',99);self.write('boot.health.heartRate',None)
        self.assertNotIn('heartRate',settings.load(self.library,ID).get('health',{}))
    def test_level_raw_mutually_exclusive(self):
        self.write('boot.battery.level',50);self.write('boot.battery.sampleRaw',23000)
        self.assertEqual({'sampleRaw':23000},settings.load(self.library,ID)['battery'])
    def test_adc_changes_only_requested_channel(self):
        self.write('adc.3',456,self.call)
        self.assertEqual([0,1,2,456,4,5,6,7],self.adc['state']['values'])
    def test_property_delete_and_custom_overrides(self):
        self.write('boot.propertyOverrides',json.dumps({'persist.custom':'hello'}))
        self.write('property.persist.custom',None)
        self.assertIsNone(settings.load(self.library,ID)['properties']['persist.custom'])
        with self.assertRaises(ValueError):self.write('boot.propertyOverrides',json.dumps({'unsafe':'x'}))
    def test_observed_property_separated_from_requested(self):
        root=settings._instance(self.library,ID,create=True)
        settings._write(root/settings.OBSERVED,{'properties':{'persist.locale':'zh_CN'},'recordedAtUnix':123})
        self.write('property.persist.locale','en_US')
        row=self.row('property.persist.locale')
        self.assertEqual('en_US',row['requestedValue']);self.assertEqual('zh_CN',row['observedValue']);self.assertEqual(123,row['observedAt'])
    def test_identity_validation_and_start_options(self):
        with self.assertRaises(ValueError):self.write('boot.virtualIdentity','{"serial":"bad space","address":"XX"}')
        self.write('boot.virtualIdentity','{"serial":"DEMO","address":"02:12:34:56:78:9A"}')
        self.write('boot.skipOOBE',False)
        result=api.start_options(self.library,ID,ITEM['model'])
        self.assertFalse(result['skipOOBE']);self.assertEqual('DEMO',result['virtualIdentity']['serial'])
        self.write('boot.virtualIdentity',None);self.assertNotIn('virtualIdentity',api.start_options(self.library,ID,ITEM['model']))
    def test_corrupt_settings_not_silently_replaced(self):
        root=settings._instance(self.library,ID,create=True)
        path=root/settings.SETTINGS;path.write_text('broken');path.chmod(0o600)
        with self.assertRaises(ValueError):self.write('boot.health.heartRate',70)
        self.assertEqual('broken',path.read_text())
    def test_clock_offset_switches_explicitly(self):
        self.write('boot.clock.offsetSeconds',3600)
        self.assertEqual({'mode':'offset','offsetSeconds':3600},settings.load(self.library,ID)['clock'])
        self.write('boot.clock.mode','host');self.assertEqual({'mode':'host'},settings.load(self.library,ID)['clock'])
    def test_write_success_refresh_failure_not_retryable_failure(self):
        original=api.describe
        count=[0]
        def describe(*a,**kw):
            count[0]+=1
            if count[0]>1:raise OSError('temporarily unavailable')
            return original(*a,**kw)
        with mock.patch.object(api,'describe',side_effect=describe):result=self.write('live.heartRate',90,self.call)
        self.assertEqual(['live.heartRate'],result['applied']);self.assertIn('refresh_error',result)
    def test_session_identity_rejected_before_ipc(self):
        root=self.library/'session';root.mkdir(mode=0o700)
        path=root/'session.json';path.write_text(json.dumps({'profileID':'other','instanceStorage':{'instanceID':ID,'profileID':'other'}}));path.chmod(0o600)
        with mock.patch.object(api,'request') as ipc,self.assertRaises(ValueError):api.session_call(root,ID,'o66-exact')
        ipc.assert_not_called()
    def test_live_read_failure_does_not_fake_current_values(self):
        def call(command,args=None):
            if command=='live-get':raise ValueError('unavailable')
            return self.call(command,args)
        schema=self.schema(call);row=self.row('live.heartRate',schema)
        self.assertFalse(row['available']);self.assertIsNone(row['value']);self.assertTrue(schema['notes'])

if __name__=='__main__':unittest.main()
