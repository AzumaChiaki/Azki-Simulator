"""Host/runner input contracts with in-memory QMP only; never open sockets."""
import copy
import importlib.util
import hashlib
import json
from pathlib import Path
import sys
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'Sources/AzkiSimulator/Runtime'))
from azki_simulator import adc_commands as adc, instance_settings, live_commands as live, minative
sys.path.insert(0, str(ROOT / 'engine/project'))
from backend.src import runner as backend_runner

spec = importlib.util.spec_from_file_location('bindings_runner_control', ROOT / 'engine/project/backend/src/control.py')
control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(control)


def observation():
    sample = dict(sampleValid=False, generatedVersion=0, registerReads=0, registerVersion=0,
                  fifoReads=0, fifoVersion=0)
    return dict(schema=1, scope='sensor-bus', guestObserved=False, inputVersion=0,
                mode='auto', imu='lsm6dso', accel=sample, gyro=dict(sample))


class FakeQMP:
    def __init__(self):
        self.properties = {
            'health-fixture': 'walk=0;cadence=110;heart-rate=72;worn=1;walked=0',
            'battery-charger': 'unplugged', 'motion-orientation': 'auto', 'motion-raw': 'auto',
            'sensor-observation': json.dumps(observation()),
            'pmu-adc-state': json.dumps(dict(schema=1, fixtureEnabled=True, version=0,
                values=list(range(100, 108)), readCount=[0]*8, lastReadVersion=[0]*8)),
        }
        self.writes, self.reads = [], []
        self.fail_write = self.reject_write = self.fail_readback = False
    def execute(self, command, arguments):
        assert arguments['path'] == '/machine'
        prop = arguments['property']
        if command == 'qom-get':
            self.reads.append(prop)
            if self.writes and self.fail_readback:
                raise ConnectionError('readback disconnected')
            if prop not in self.properties:
                raise RuntimeError('Property not found: ' + prop)
            return self.properties[prop]
        assert command == 'qom-set'
        self.writes.append(copy.deepcopy(arguments))
        if self.fail_write:
            raise ConnectionError('write acknowledgement lost')
        if self.reject_write:
            raise RuntimeError('correlated QMP rejection')
        if prop == 'health-fixture':
            values = dict(item.split('=') for item in self.properties[prop].split(';'))
            values.update(item.split('=') for item in arguments['value'].split(';'))
            self.properties[prop] = ';'.join(k+'='+v for k,v in values.items())
        elif prop == 'pmu-adc-raw':
            state = json.loads(self.properties['pmu-adc-state'])
            state['values'] = list(map(int, arguments['value'].split(',')))
            state['version'] += 1
            self.properties['pmu-adc-state'] = json.dumps(state)
        else:
            self.properties[prop] = arguments['value']
        return {}


class LiveBindingTests(unittest.TestCase):
    def setUp(self):
        # Any accidental transport use is a test failure, not a fallback path.
        self.socket_guard = patch('socket.socket', side_effect=AssertionError('real socket forbidden'))
        self.socket_guard.start()
        self.addCleanup(self.socket_guard.stop)
        self.board = control.WatchControl.__new__(control.WatchControl)
        self.board.lock = threading.RLock()
        self.board.qmp = FakeQMP()
        self.board.adc_raw_available = True
        self.board.battery_channel, self.board.ntc_channels = 2, 0
        self.adapter = SimpleNamespace(plan={'identity': {'profile': {'model': 'miwear.watch.n66'}}})
        self.sent = []
    def runner(self, adapter, request, **options):
        self.sent.append(copy.deepcopy(request))
        try:
            result = {'return': self.board.dispatch_input(request)}
        except (ValueError, KeyError, TypeError, OSError, RuntimeError) as error:
            result = {'error': str(error)[:512]}
        return json.dumps(result).encode()
    def write(self, value):
        with patch.object(live, 'runner_exchange', side_effect=self.runner):
            return live.live_set(self.adapter, value)
    def test_alias_selects_identical_hardware_without_changing_model_identity(self):
        for function in (minative.board_hardware, minative.health_sensors, minative.battery_inputs):
            self.assertEqual(function('q66'), function('miwear.watch.q66'))
        self.assertEqual('hp4570', minative.board_hardware('q66')['batteryGauge'])
        self.assertEqual('cw2218', minative.board_hardware('q66')['coulombCounter'])
    def test_cadence_only_restores_without_scheduling_walk(self):
        health = instance_settings._resolve_health({'cadence': 170}, minative.board_hardware('miwear.watch.n66'))
        self.assertEqual(170, health['cadence'])
        self.assertNotIn('walkSteps', health)
    def test_p65_q65_unknown_models_do_not_gain_unverified_health_controls(self):
        for model in ('miwear.watch.p65', 'miwear.watch.q65', 'mijia.watch.o62m', 'unknown'):
            with self.subTest(model=model):
                adapter = SimpleNamespace(plan={'identity': {'profile': {'model': model}}})
                result = live._report(adapter, self.board.live())
                for key in ('steps', 'heartRate', 'orientation', 'motionRaw'):
                    self.assertFalse(result['supported'][key])
                self.assertEqual(model == 'miwear.watch.p65', result['supported']['charger'])
    def test_p65_battery_binding_is_raw_adc_and_charger_only(self):
        self.board.battery_channel, self.board.ntc_channels = 6, 8
        self.adapter.plan['identity']['profile']['model'] = 'miwear.watch.p65'
        hardware = minative.board_hardware('miwear.watch.p65')
        self.assertEqual(6, hardware['batteryADCChannel'])
        self.assertEqual('soft', hardware['batteryGauge'])
        self.assertIsNone(hardware['motionSensor'])
        report = live._report(self.adapter, self.board.live())
        self.assertTrue(report['supported']['batteryRaw'])
        self.assertTrue(report['supported']['charger'])
        self.assertEqual(106, report['state']['batteryRaw'])
        for key in ('batteryLevel', 'temperature', 'steps', 'heartRate', 'orientation', 'motionRaw'):
            self.assertFalse(report['supported'][key])
        result = self.write({'batteryRaw': 15968})
        self.assertTrue(result['accepted']); self.assertFalse(result['guestObserved'])
        self.assertEqual(15968, result['state']['state']['batteryRaw'])
        result = self.write({'charger': 'charging'})
        self.assertTrue(result['accepted']); self.assertFalse(result['guestObserved'])
        self.assertEqual('charging', result['state']['state']['charger'])
        raw = json.loads(self.board.qmp.properties['pmu-adc-state'])
        self.assertEqual([100, 101, 102, 103, 104, 105, 15968, 107], raw['values'])
        self.assertEqual([0]*8, raw['readCount'])
        self.assertNotIn('health-fixture', [row['property'] for row in self.board.qmp.writes])
    def test_p65_boot_settings_resolve_without_fake_percentage_or_health(self):
        settings = instance_settings.default()
        settings.update(battery={'sampleRaw': 15968, 'level': 50, 'charger': 'charging'},
                        health={'heartRate': 120, 'walkSteps': 100})
        battery, health = instance_settings.resolve(settings, 'miwear.watch.p65')
        self.assertEqual({'channel': 6, 'gauge': 'soft', 'coulomb': None,
                          'sampleRaw': 15968, 'charger': 'charging'}, battery)
        self.assertIsNone(health)
        self.assertEqual({}, settings['properties'])
        resolved = {'battery': battery, 'health': health}
        options = backend_runner.instance_settings_options(resolved, adc_fixture=True,
                                                            ntc_channels=8, ntc_sample=12090)
        self.assertEqual(['battery-gauge=soft',
                          'pmu-adc-raw=21000,,21000,,21000,,12090,,21000,,21000,,15968,,21000'], options)
        with self.assertRaises(ValueError):
            backend_runner.instance_settings_options(resolved, adc_fixture=False)
    def test_isolated_raw_adc_requires_exact_machine_and_core_declaration(self):
        capability = dict(schemaVersion=1, controlProtocol='pmu.adcRaw.v1',channels=8,
                          minimum=0,maximum=65535,qemuSHA256='a'*64)
        for model,machine in (('miwear.watch.p65','azki-p65'),('miwear.watch.q65','azki-q65')):
            with self.subTest(model=model):
                caps={'pmuAdcRaw':capability}
                self.assertEqual({},minative.board_runtime_capabilities(model,caps))
                self.assertEqual({},minative.board_runtime_capabilities(model,caps,
                    {'pmuAdcRaw':{'machines':[machine],'qemuSHA256':'b'*64}}))
                self.assertEqual({},minative.board_runtime_capabilities(model,caps,
                    {'pmuAdcRaw':{'machines':['wrong-machine'],'qemuSHA256':'a'*64}}))
                self.assertEqual(caps,minative.board_runtime_capabilities(model,caps,
                    {'pmuAdcRaw':{'machines':[machine],'qemuSHA256':'a'*64}}))
    def test_adc_probe_requires_valid_enabled_schema(self):
        self.assertTrue(self.board.probe_adc_raw())
        state=json.loads(self.board.qmp.properties['pmu-adc-state'])
        state['fixtureEnabled']=False
        self.board.qmp.properties['pmu-adc-state']=json.dumps(state)
        self.assertFalse(self.board.probe_adc_raw())
        self.board.qmp.properties['pmu-adc-state']='{}'
        self.assertFalse(self.board.probe_adc_raw())
        with self.assertRaises(RuntimeError):self.board.adc_raw()
    def test_malformed_health_never_fabricates_default_state(self):
        valid = self.board.qmp.properties['health-fixture']
        for bad in ('garbage', '', None, True, valid+';walk=2', valid.replace('worn=1', 'worn=7'),
                    valid.replace('cadence=110', 'cadence=0'), valid.replace('walk=0', 'walk=-1'),
                    valid.replace(';walked=0', ''), valid.replace('heart-rate=72', 'heart-rate=256')):
            with self.subTest(bad=bad), self.assertRaises((ValueError, TypeError)):
                self.board.qmp.properties['health-fixture'] = bad
                self.board.health()
    def test_null_or_invalid_partial_health_write_never_writes(self):
        for bad in (None, {}, {'worn': 1}, {'cadence': 300}, {'heartRate': 100, 'worn': 'yes'}):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                self.board.dispatch_input({'command':'health.set', 'value':bad})
        self.assertEqual([], self.board.qmp.writes)
    def test_health_partial_write_preserves_other_fields_and_receipt(self):
        result = self.write({'heartRate': 120})
        self.assertTrue(result['accepted']); self.assertFalse(result['guestObserved'])
        self.assertEqual(120, result['state']['state']['heartRate'])
        self.assertEqual(110, result['state']['state']['cadence'])
        self.assertEqual(['live.get', 'health.set'], [x['command'] for x in self.sent])
        self.assertEqual(1, len(self.board.qmp.writes))
    def test_nonidempotent_walk_does_not_issue_extra_runtime_refresh(self):
        result = self.write({'addSteps': 5})
        self.assertEqual(5, result['state']['state']['walkSteps'])
        self.assertFalse(result['guestObserved'])
        self.assertEqual(['live.get', 'health.walk'], [x['command'] for x in self.sent])
        self.assertEqual(1, len(self.board.qmp.writes))
    def test_health_charger_adc_preserve_ack_when_snapshot_fails(self):
        for request in ({'heartRate': 120}, {'charger': 'charging'}, {'batteryRaw': 999}, {'addSteps': 5}):
            with self.subTest(request=request):
                self.board.qmp = FakeQMP(); self.board.qmp.fail_readback = True
                self.assertEqual(dict(accepted=True, guestObserved=False, stateUnavailable=True), self.write(request))
                self.assertEqual(1, len(self.board.qmp.writes))
    def test_uncertain_writes_are_not_retried(self):
        for request in ({'heartRate': 120}, {'charger': 'charging'}, {'batteryRaw': 999}, {'addSteps': 5},
                        {'motionRaw': {'mode':'auto'}}):
            with self.subTest(request=request):
                self.board.qmp = FakeQMP(); self.board.qmp.fail_write = True
                with self.assertRaises(live.MError) as caught:
                    self.write(request)
                self.assertEqual('live_outcome_unknown', caught.exception.code)
                self.assertEqual(1, len(self.board.qmp.writes))
    def test_confirmed_qmp_rejection_is_not_reported_as_applied_or_unknown(self):
        self.board.qmp.reject_write = True
        with self.assertRaises(live.MError) as caught:
            self.write({'addSteps': 5})
        self.assertEqual('invalid_live', caught.exception.code)
        self.assertEqual(1, len(self.board.qmp.writes))
    def test_adc_partial_update_preserves_other_channels(self):
        result = self.board.adc_channels({'2': 999, '5': 1234})
        self.assertEqual([100,101,999,103,104,1234,106,107], result['state']['adc'])
        self.assertFalse(result['guestObserved'])
    def test_absent_optional_charger_does_not_break_q65_raw_adc(self):
        for prop in ('health-fixture','battery-charger','motion-orientation','motion-raw','sensor-observation'):
            del self.board.qmp.properties[prop]
        self.board.battery_channel = None
        result = self.board.live()
        self.assertIsNone(result['charger']); self.assertIsNone(result['health'])
        self.assertEqual(list(range(100,108)), result['adc'])
        self.assertEqual(list(range(100,108)), self.board.adc_raw()['state']['values'])
    def test_snapshot_rejects_invalid_types_ranges_and_fields(self):
        mutations = [{'adc':[]}, {'adc':[0]*7}, {'adc':[True]*8}, {'adc':[65536]*8},
                     {'batteryChannel':-1}, {'batteryChannel':8}, {'batteryChannel':True},
                     {'ntcChannels':-1}, {'ntcChannels':256}, {'ntcChannels':False},
                     {'charger':'unknown'}, {'health':'garbage'}, {'extra':1}]
        for mutation in mutations:
            with self.subTest(mutation=mutation), self.assertRaises(live.MError) as caught:
                value = self.board.live(); value.update(mutation)
                live._report(self.adapter, value)
            self.assertEqual('live_protocol', caught.exception.code)
    def test_malformed_postwrite_envelopes_are_unknown(self):
        for payload in (b'bad-json', b'{}', b'{"error":{}}', b'{"return":{}}'):
            with self.subTest(payload=payload):
                if payload == b'{"return":{}}':
                    with self.assertRaises(live.MError) as caught:
                        live._live_receipt(self.adapter, {})
                else:
                    with patch.object(live, 'runner_exchange', return_value=payload), self.assertRaises(live.MError) as caught:
                        live._exchange(self.adapter, {'command':'health.walk','steps':5}, write=True)
                self.assertEqual('live_outcome_unknown', caught.exception.code)
    def test_invalid_snapshot_after_confirmed_ack_stays_accepted(self):
        result = live._live_receipt(self.adapter, dict(accepted=True, guestObserved=False, state={}))
        self.assertEqual(dict(accepted=True, guestObserved=False, stateUnavailable=True), result)
    def test_raw_adc_ack_rejection_and_unknown_are_distinct(self):
        request = {'command':'pmu.adcRaw.set','values':[0]*8}
        for payload, code in ((b'bad-json','adc_outcome_unknown'), (b'{}','adc_outcome_unknown'),
                              (b'{"error":"input_outcome_unknown: lost"}','adc_outcome_unknown'),
                              (b'{"error":"invalid value"}','adc_rejected')):
            with self.subTest(payload=payload), self.assertRaises(adc.MError) as caught:
                adc._response(payload, request)
            self.assertEqual(code, caught.exception.code)
        result = adc._response(b'{"return":{"accepted":true,"guestObserved":false,"state":{}}}', request)
        self.assertEqual(dict(accepted=True, guestObserved=False, stateUnavailable=True), result)
    def test_motion_raw_roundtrip_and_auto_release(self):
        value = dict(mode='raw', accel=[-32768,0,32767], gyro=[123,-456,789])
        result = self.write({'motionRaw': value})
        self.assertEqual(value, result['state']); self.assertFalse(result['guestObserved'])
        self.assertEqual('ax=-32768;ay=0;az=32767;gx=123;gy=-456;gz=789', self.board.qmp.properties['motion-raw'])
        result = self.write({'motionRaw': {'mode':'auto'}})
        self.assertEqual({'mode':'auto'}, result['state'])
        self.assertEqual('auto', self.board.qmp.properties['motion-raw'])
    def test_invalid_motion_raw_is_atomic(self):
        bad = [None,{}, {'mode':'auto','gyro':[0]*3}, {'mode':'raw','accel':[0]*2,'gyro':[0]*3}]
        for value in (True,32768,-32769,0.5,'1'):
            bad.append(dict(mode='raw',accel=[0,value,0],gyro=[0]*3))
        for value in bad:
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.board.motion_raw(value, write=True)
            with self.subTest(runtime=value), self.assertRaises(live.MError):
                live.validate_motion_raw(value)
        self.assertEqual([],self.board.qmp.writes)
    def test_older_core_has_no_raw_capability_or_write(self):
        del self.board.qmp.properties['motion-raw']
        with self.assertRaises(live.MError) as caught:
            self.write({'motionRaw':{'mode':'auto'}})
        self.assertEqual('unsupported_hardware',caught.exception.code)
        self.assertEqual([],self.board.qmp.writes)
    def test_motion_raw_ack_survives_readback_failure(self):
        self.board.qmp.fail_readback=True
        self.assertEqual(dict(accepted=True,guestObserved=False,stateUnavailable=True),self.write({'motionRaw':{'mode':'auto'}}))
    def test_sensor_observation_is_strict_bus_evidence(self):
        result = live._report(self.adapter,self.board.live())
        self.assertEqual('sensor-bus',result['sensorObservation']['scope'])
        self.assertFalse(result['sensorObservation']['guestObserved'])
        for mutation in ({'guestObserved':True},{'scope':'firmware'},{'schema':True},{'inputVersion':-1}):
            value=observation();value.update(mutation)
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):
                self.board._validate_sensor_observation(value)
            with self.assertRaises(live.MError): live.validate_sensor_observation(value)
        value=observation();value['accel']['registerVersion']=1
        with self.assertRaises(live.MError): live.validate_sensor_observation(value)
    def test_sensor_observation_rejects_duplicate_json_fields(self):
        self.board.qmp.properties['sensor-observation'] = json.dumps(observation()).replace('"schema": 1','"schema": 0, "schema": 1')
        with self.assertRaises(ValueError):self.board.sensor_observation()


class FirmwareProvenanceTests(unittest.TestCase):
    def test_changed_boot_copy_reports_modification_and_both_digests(self):
        original = hashlib.sha256(b'original synthetic AP').hexdigest()
        boot = hashlib.sha256(b'local build.prop synthetic AP').hexdigest()
        result = backend_runner.firmware_provenance(original, boot, skip_binding_applied=True)
        self.assertTrue(result['firmware_modified']); self.assertTrue(result['skip_binding_applied'])
        self.assertEqual(original, result['original_input_sha256'])
        self.assertEqual(boot, result['boot_ap_sha256'])
        self.assertEqual(boot, result['firmware_sha256'])
    def test_equal_bytes_are_not_reported_modified_even_when_setup_was_applied(self):
        digest = hashlib.sha256(b'already configured synthetic AP').hexdigest()
        result = backend_runner.firmware_provenance(digest, digest, skip_binding_applied=True)
        self.assertFalse(result['firmware_modified']); self.assertTrue(result['skip_binding_applied'])
    def test_storage_only_setup_does_not_claim_an_ap_patch(self):
        digest = hashlib.sha256(b'unchanged synthetic AP').hexdigest()
        result = backend_runner.firmware_provenance(digest, digest, skip_binding_applied=False)
        self.assertFalse(result['firmware_modified']); self.assertFalse(result['skip_binding_applied'])
    def test_missing_or_malformed_digest_is_not_described_as_unmodified(self):
        for invalid in (None, '', '0'*63, 'g'*64, True):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                backend_runner.firmware_provenance(invalid, '0'*64, skip_binding_applied=False)


if __name__ == '__main__':
    unittest.main()
