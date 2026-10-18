"""Fresh synthetic PSK binding plus real guest AppVerify/AppConfirm.

Wire fields are independently encoded from the MIT miconnect binding protocol
and local AzkiDeck protocol sources; exact Q66 PSK MAC/HKDF inputs were checked
against the pinned firmware. No official cloud connection is made.
"""
import hmac
import json
import os
import secrets
import stat
import struct
from pathlib import Path
from .auth_crypto import P256, AESModes, hkdf, mac
from .bind_info import Frames, fields, inspect, encode, SYNTHETIC_USER_ID
from .central import LocalCentral
from .business_session import BusinessChannel
from .smp import LegacyCentral
from .state import descriptor_for, validate_state, psk_bytes


def varint(value):
    if type(value) is not int or not 0<=value<(1<<64):raise ValueError('Protobuf integer bound')
    result=bytearray()
    while value>=128:result.append((value&127)|128);value>>=7
    result.append(value)
    return bytes(result)


def integer(number,value):return varint(number<<3)+varint(value)


def blob(number,value):
    if not isinstance(value,bytes) or len(value)>4096:raise ValueError('Protobuf bytes bound')
    return varint((number<<3)|2)+varint(len(value))+value


def envelope(subtype,field,payload):
    return integer(1,1)+integer(2,subtype)+blob(3,blob(field,payload))


def decode(payload,subtype,field):
    outer=fields(payload)
    if outer.get(1)!=1 or outer.get(2)!=subtype:raise ValueError('Unexpected guest authentication response')
    body=fields(outer.get(3,b''))
    error=body.get(3,outer.get(100))
    if error is not None:
        if type(error) is not int:raise ValueError('Malformed guest authentication error')
        if error:raise ValueError('Guest authentication error '+str(error))
    return fields(body.get(field,b''))


def exact(value,length,label):
    if not isinstance(value,bytes) or len(value)!=length:raise ValueError('Invalid guest '+label+' length')
    return value


class AuthenticationCentral(LocalCentral):
    def __init__(self,controller,private_state,on_confirm,connection_timeout=30):
        validate_state(private_state)
        self.profile = descriptor_for(private_state['profileID'], private_state['apSHA256'])
        self.firmware_models=(self.profile['model'],)
        # P67 TC was observed rejecting only its model comparison while serial,
        # address and DID matched: its firmware compares against the CN model
        # (the TC and CN builds share one AP), so accept both for P67 TC.
        if self.profile['model']=='miwear.watch.p67tc':
            self.firmware_models+=('miwear.watch.p67cn',)
        if self.profile['protocol'] not in ('miwear-psk1-q66', 'miwear-psk1-literal16') or self.profile['requiresSMPBond'] is not True:
            raise ValueError('Unsupported synthetic authentication protocol')
        super().__init__(controller,bind_info=True,bind_dynamic_code=0,connection_timeout=connection_timeout)
        self.reconnect_delay=3.0
        self.private_state=private_state
        self.on_confirm=on_confirm
        self.psk=psk_bytes(private_state)
        private=bytes.fromhex(private_state["p256Private"])
        self.p256=P256(private)
        self.aes=AESModes()
        self.auth_key=None
        self.authenticated=False
        self.auth_stage='bind_info'
        self.next_auth=None
        self.sequence=0
        from collections import deque
        self.auth_events=deque(maxlen=128)
        self.auth_frames=Frames(maximum=65535)
        self.auth_generation=None
        self.now=0.0
        self.companion_platform=1  # explicitly synthetic Apple/BLE peer profile
        self.business=BusinessChannel();self.api=self.business.api
        self.hold_business=True
        self.smp=LegacyCentral(controller)
        self.smp.events=deque(maxlen=128)
        self.resumed=private_state["binding"] is not None
        if self.resumed:
            binding=private_state["binding"]
            self.auth_key=bytes.fromhex(binding["authKey"])
            self.smp.bond=(bytes.fromhex(binding["ltk"]),bytes.fromhex(binding["random"]),binding["ediv"])
        self.state="inactive"
        self.next_at=0.0
        self.failure_diagnostic=None

    def _fail(self,message):
        if self.state != 'failed':
            stage=self.auth_stage if self.auth_stage in ('bind_info','server_hello','bind_confirm','bind_record','link_encryption','app_verify','app_confirm') else 'unknown'
            phase='business' if self.state in ('business','authenticated_settle') else stage if self.state in ('auth_send','wait_auth','wait_link_encryption','wait_link_completion') else 'binding' if self.state in ('bind_info','wait_bind_info','binding_settle') else 'gatt'
            if message == 'Guest transport closed': code='transport_closed'
            elif message == 'Guest business SAR ACK timeout':code='business_sar_timeout'
            elif message.startswith('Guest disconnected'):code='guest_disconnect'
            elif message.startswith('Timed out awaiting guest'):
                code='authentication_timeout' if phase in ('app_verify','app_confirm','server_hello','bind_confirm','bind_record','link_encryption') else 'binding_timeout' if phase=='binding' else 'gatt_timeout'
            elif message.startswith('Guest ATT error'):code='guest_att_error'
            elif message == 'Guest DeviceConfirm did not confirm':code='guest_confirm_rejected'
            elif message == 'Guest confirmation lacks verified link encryption and bond':code='link_encryption_missing'
            elif message.startswith('Guest BindInfo rejected'):code='bind_info_rejected'
            elif message == 'Guest identity differs from its synthetic state':code='synthetic_identity_mismatch'
            elif message == 'Guest verify mode differs from its synthetic profile':code='verify_mode_mismatch'
            elif 'SMP' in message:code='smp_rejected'
            elif message.startswith('Guest authentication error'):code='authentication_rejected'
            elif phase=='business':code='business_protocol_error'
            else:code='authentication_protocol_error'
            self.failure_diagnostic={'failureCode':code,'failurePhase':phase,'failureGeneration':self.controller.generation,
                'disconnectReason':self.controller.disconnect_reason,
                'guestDisconnectReason':getattr(self.controller,'guest_disconnect_reason',None)}
            # Protocol enums only: enough to distinguish legacy-family SMP
            # negotiation from application confirmation, never keys or PDUs.
            self.failure_diagnostic.update(linkEncrypted=self.controller.air is not None,
                bondStored=self.smp.bond is not None, smpState=self.smp.state)
            if isinstance(self.session,dict) and isinstance(self.session.get('bindInfo'),dict):
                info=self.session['bindInfo']
                self.failure_diagnostic['bindInfoChecks']={k:v for k,v in info.items()
                    if k in ('accepted','serialMatchesSyntheticSeed','addressMatchesSyntheticSeed',
                             'modelMatchesSyntheticSeed','modelMatchesFirmwareProfile','didMatchesSyntheticSeed','guestError','verifyMode','oobMode')
                    and type(v) in (bool,int)}
                for key in ('reportedModel','expectedModel','debugReportedSerial','debugReportedDid'):
                    if type(info.get(key)) is str:self.failure_diagnostic[key]=info[key]
            details={'SMP MITM not implemented':'mitm_unsupported',
                'Unsupported SMP legacy negotiation':'legacy_negotiation_unsupported',
                'Guest SMP confirm mismatch':'confirm_mismatch',
                'Unexpected SMP state/opcode':'unexpected_state_opcode'}
            if message in details:self.failure_diagnostic['smpFailure']=details[message]
            if self.smp.events:
                event=self.smp.events[-1]
                if type(event.get('opcode')) is int:self.failure_diagnostic['smpOpcode']=event['opcode']
                if isinstance(event.get('negotiation'),dict):
                    self.failure_diagnostic['smpNegotiation']={k:v for k,v in event['negotiation'].items()
                        if k in ('ioCapability','oob','authReq','keySize','initiatorKeys','responderKeys')
                        and type(v) is int and 0 <= v <= 255}
        super()._fail(message)
        self.authenticated=False
        if hasattr(self,'business'):self.business.invalidate(self.controller.generation)

    def _next(self,stage,protobuf):
        self.auth_stage=stage
        self.next_auth=protobuf
        self.state='auth_send'
        self.deadline=None

    def _begin_verify(self):
        self.app_random=secrets.token_bytes(16)
        # Optional accountId selects the guest's secondary-account key table;
        # primary PSK binding stores miwear_token_key and must omit that field.
        self._next('app_verify',envelope(26,30,blob(1,self.app_random)))

    def tick(self,now):
        self.now=now
        if self.state=="inactive": return []
        if self.auth_generation!=self.controller.generation:
            self.authenticated=False
            self.auth_generation=self.controller.generation
            self.sequence=255 if self.auth_key is not None else 0
            self.business.invalidate(self.controller.generation)
            self.smp.reset()
        if self.state=='bind_info' and self.auth_key is not None:
            self.auth_frames=Frames(maximum=65535)
            if self.smp.bond is not None and self.controller.air is None:
                self.auth_stage='link_encryption'
                self.smp.resume_bond()
                self.state='wait_link_encryption';self.deadline=now+self.connection_timeout
                self.auth_events.append({'stage':'link_encryption','bondKeyRequested':True,
                                         'generation':self.controller.generation})
                return self.controller.drain_events()
            self._begin_verify()
        if self.state=='wait_link_encryption' and self.controller.air is not None:
            self.auth_events.append({'stage':'link_encryption','independentKeyMICVerified':True,
                                     'generation':self.controller.generation})
            self._begin_verify()
        if self.state=='wait_link_completion' and self.smp.bond is not None and self.controller.air is not None:
            self._complete_confirmation()
        if self.state=='binding_settle' and now>=self.next_at:
            self.state='disconnect'
        if self.state=='authenticated_settle' and now>=self.next_at:
            self.state='business' if self.hold_business else 'disconnect'
        if self.state=='business':
            if not self.controller.connected:
                self._fail('Guest disconnected during business session');return []
            sequence=(self.sequence+1)&255
            wire=self.business.next_wire(sequence,now)
            if wire is None:return []
            outgoing=self.business.outgoing_sequence
            if outgoing==sequence:self.sequence=sequence
            return [packet for cid,pdu in self._sar_writes(encode(3,outgoing,wire)) for packet in self.link.from_peer(cid,pdu)]
        if self.state=='auth_send':
            if not self.controller.connected:
                self._fail('Guest disconnected before '+self.auth_stage);return []
            self.sequence=(self.sequence+1)&255
            wire=encode(3,self.sequence,b'\x01\x01'+self.next_auth)
            self.next_auth=None
            self.state='wait_auth';self.deadline=now+self.connection_timeout
            self.auth_events.append({'stage':self.auth_stage,'sent':True,'generation':self.controller.generation})
            return [packet for cid,pdu in self._sar_writes(wire) for packet in self.link.from_peer(cid,pdu)]
        return super().tick(now)

    def consume(self,cid,pdu):
        if cid==6:
            self.auth_events.append({'stage':self.auth_stage,'seconds':round(self.now,3),'smpOpcode':pdu[0] if pdu else None,'smpBytes':len(pdu)})
            try:return self.smp.receive(pdu)
            except ValueError as error:
                self._fail(str(error));return [] if pdu[:1]==b'\x05' else [(6,b'\x05\x08')]
        if cid==4 and pdu and pdu[0] in (4,6,8,10,12,16,18):
            # This central advertises no local phone GATT services (ANCS/AMS).
            # Honest ATT error response; a guest discovery request is not an
            # unsolicited response to our own peripheral discovery procedure.
            if len(pdu)<3:raise ValueError('Truncated central GATT request')
            self.auth_events.append({'stage':self.auth_stage,'centralGATTRequest':pdu[0],'bytes':len(pdu),'attributeDatabase':'empty'})
            return [(4,bytes((1,pdu[0]))+pdu[1:3]+bytes((10 if pdu[0] in (4,6,8,16) else 1,)))]
        if cid==4 and self.state=='binding_settle' and pdu[:1] in (b'\x01',b'\x13'):
            if pdu==b'\x13':
                self.auth_events.append({'stage':'bind_record','ackWriteRequestStatus':0})
            elif len(pdu)==5 and pdu[1]==0x12:
                self.auth_events.append({'stage':'bind_record','ackWriteRequestStatus':pdu[4]})
            else:raise ValueError('Unexpected binding completion write response')
            return []
        if cid!=4 or len(pdu)<3 or pdu[0]!=0x1b or struct.unpack_from('<H',pdu,1)[0]!=self.notify:
            return super().consume(cid,pdu)
        if self.state not in ('wait_bind_info','wait_auth','wait_link_completion','binding_settle','authenticated_settle','business'):
            return super().consume(cid,pdu)
        outgoing=[]
        parser=self.bind_frames if self.state=='wait_bind_info' else self.auth_frames
        for kind,sequence,payload in parser.feed(pdu[3:]):
            if kind==1 and not payload:
                self.auth_events.append({'stage':self.auth_stage,'transportACK':True})
                if self.state=='wait_bind_info':self.session['bindInfo']['transportAcknowledged']=True
                if self.authenticated:self.business.ack(sequence)
                continue
            if kind==0 and not payload and self.authenticated:
                self.business.nak(sequence);continue
            if kind!=3:raise ValueError('Unexpected authentication SAR frame')
            outgoing+=self._sar_writes(encode(1,sequence,b''))
            self.auth_events.append({'stage':self.auth_stage,'receivedSequence':sequence,'payloadBytes':len(payload)})
            try:
                if self.state=='wait_bind_info':
                    address=':'.join(f'{v:02x}' for v in self.controller.address)
                    self.session['bindInfo'].update(inspect(payload,self.private_state['identity']['serial'],address,self.private_state['identity']['did'],model=self.profile['model'],model_aliases=self.firmware_models))
                    if not self.session['bindInfo'].get('accepted'):
                        raise ValueError('Guest BindInfo rejected: ' + str(self.session['bindInfo'].get('guestError')))
                    if not all(self.session['bindInfo'].get(k) for k in ('accepted','serialMatchesSyntheticSeed','addressMatchesSyntheticSeed','modelMatchesFirmwareProfile','didMatchesSyntheticSeed')):
                        raise ValueError('Guest identity differs from its synthetic state')
                    info=decode(payload[2:],17,12)
                    if info.get(1)!=self.profile['verifyMode']:raise ValueError('Guest verify mode differs from its synthetic profile')
                    self.device_random=exact(info.get(6),16,'bind nonce')
                    self.server_random=secrets.token_bytes(16)
                    signature=mac(self.psk,self.device_random+self.p256.public)
                    hello=blob(1,self.p256.public)+blob(2,signature)+blob(3,self.server_random)
                    self._next('server_hello',envelope(18,13,hello))
                elif self.state in ('authenticated_settle','business'):
                    if payload[:2]!=b'\x01\x01':self.business.receive(sequence,payload)
                elif self.state not in ('binding_settle','wait_link_completion'):
                    self._advance(payload)
            except ValueError as error:
                self.auth_events.append({'stage':self.auth_stage,'failure':str(error)})
                self._fail(str(error));self.authenticated=False
        return outgoing

    def _advance(self,payload):
        if payload[:2]!=b'\x01\x01':
            self.auth_events.append({'stage':self.auth_stage,'asynchronousMessageIgnored':True,
                                     'channel':payload[0] if payload else None,'opcode':payload[1] if len(payload)>1 else None,
                                     'bytes':len(payload)})
            return
        payload=payload[2:]
        meta=fields(payload)
        expected={'server_hello':18,'bind_confirm':19,'bind_record':25,'app_verify':26,'app_confirm':27}[self.auth_stage]
        if meta.get(1)!=1 or meta.get(2)!=expected:
            body=fields(meta[3]) if isinstance(meta.get(3),bytes) else {}
            self.auth_events.append({'stage':self.auth_stage,'asynchronousMessageIgnored':True,
                'type':meta.get(1) if type(meta.get(1)) is int else None,
                'subtype':meta.get(2) if type(meta.get(2)) is int else None,'bytes':len(payload),
                'scalarFields':{str(k):v for k,v in body.items() if type(v) is int},
                'byteFieldLengths':{str(k):len(v) for k,v in body.items() if isinstance(v,bytes)}})
            return
        if self.auth_stage=='server_hello':
            hello=decode(payload,18,14)
            public=exact(hello.get(1),64,'P256 public key')
            signature=exact(hello.get(2),32,'PSK signature')
            if not hmac.compare_digest(signature,mac(self.psk,self.server_random+public)):
                raise ValueError('Guest PSK signature mismatch')
            shared=self.p256.exchange(public)
            material=hkdf(shared,bytes(range(0x10,0x1c)),b'miwear-bind')
            self.device_key=material[:16]
            # Q66 stores derived_key.token_key at BindRecord (c99170c),
            # independent of the opaque server bookkeeping bind_key field.
            self.auth_key=material[16:32]
            # Exact Q66 hashes server public || device public; signed LE first
            # word modulo 1e6 is its six-digit OOB code (never log the code).
            import hashlib
            code=int.from_bytes(hashlib.sha256(self.p256.public+public).digest()[:4],'little',signed=True)%1000000
            self.oob=f'{code:06d}'.encode()
            self.app_random=secrets.token_bytes(16)
            bind_key=secrets.token_bytes(16).hex().encode()
            confirm=blob(1,self.app_random)+blob(2,mac(self.device_key,self.app_random+self.oob))+blob(3,bind_key)
            self.auth_events.append({'stage':'server_hello','guestPSKSignatureVerified':True,'P256SharedSecretDerived':True})
            self._next('bind_confirm',envelope(19,15,confirm))
        elif self.auth_stage=='bind_confirm':
            confirm=decode(payload,19,16)
            nonce=exact(confirm.get(1),16,'confirm nonce')
            signature=exact(confirm.get(2),32,'confirm signature')
            if not hmac.compare_digest(signature,mac(self.device_key,nonce+self.oob)):
                raise ValueError('Guest binding signature mismatch')
            self.auth_events.append({'stage':'bind_confirm','guestSignatureVerified':True})
            companion=integer(1,self.companion_platform)+blob(3,b'Azki Synthetic Apple BLE Peer')+blob(5,b'CN')
            record=blob(1,SYNTHETIC_USER_ID)+blob(2,companion)
            sealed=self.aes.ccm_encrypt(self.device_key,bytes(range(0x10,0x1c)),record,b'bind-data',4)
            self._next('bind_record',envelope(25,29,blob(1,sealed)))
        elif self.auth_stage=='bind_record':
            outer=fields(payload)
            if outer.get(1)!=1 or outer.get(2)!=25:raise ValueError('Unexpected bind record response')
            body=fields(outer.get(3,b''));error=body.get(3,outer.get(100))
            if error not in (None,0):raise ValueError('Guest bind record error '+str(error))
            self.auth_events.append({'stage':'bind_record','guestResponded':True})
            self.session['bindingRecordAccepted']=True
            # Flush the SAR ACK before the guest's binding-completion callback
            # advances its lifecycle. Continue acknowledging retransmissions.
            self.state='binding_settle';self.deadline=None;self.next_at=self.now+1.5
        elif self.auth_stage=='app_verify':
            verify=decode(payload,26,31)
            nonce=exact(verify.get(1),16,'verify nonce')
            signature=exact(verify.get(2),32,'verify signature')
            material=hkdf(self.auth_key,self.app_random+nonce,b'miwear-auth')
            self.session_device_key,self.session_app_key=material[:16],material[16:32]
            if not hmac.compare_digest(signature,mac(self.session_device_key,nonce+self.app_random)):
                raise ValueError('Guest DeviceVerify signature mismatch')
            companion=integer(1,self.companion_platform)+blob(3,b'Azki Synthetic Apple BLE Peer')+blob(5,b'CN')
            sealed=self.aes.ccm_encrypt(self.session_app_key,material[36:40]+bytes(8),companion)
            signature=mac(self.session_app_key,self.app_random+nonce)
            self.auth_events.append({'stage':'app_verify','guestSignatureVerified':True})
            self._next('app_confirm',envelope(27,32,blob(1,signature)+blob(2,sealed)))
        elif self.auth_stage=='app_confirm':
            confirm=decode(payload,27,33)
            if confirm.get(1)!=1:raise ValueError('Guest DeviceConfirm did not confirm')
            if self.smp.bond is None or self.controller.air is None:
                # Application confirmation and SMP key distribution are separate
                # channels. Older guests confirm the application while SMP is
                # still exchanging confirms; keep pumping both until the same
                # outstanding deadline, never manufacture a bond/encrypted link.
                self.auth_events.append({'stage':'app_confirm','guestConfirmed':True,'linkPending':True})
                self.auth_stage='link_encryption';self.state='wait_link_completion'
                if self.deadline is None:self.deadline=self.now+self.connection_timeout
                return
            self._complete_confirmation()

    def _complete_confirmation(self):
        if self.smp.bond is None or self.controller.air is None:
            raise ValueError('Guest confirmation lacks verified link encryption and bond')
        ltk,random,ediv=self.smp.bond
        self.on_confirm({'authKey':self.auth_key.hex(),'ltk':ltk.hex(),'random':random.hex(),'ediv':ediv})
        self.authenticated=True
        self.session['authenticated']=True
        self.business.confirm_from_guest(self.controller.generation,self.session_app_key,self.session_device_key,
                                         self.session['sarConfiguration']['mps'],
                                         self.session['sarConfiguration']['sendTimeoutMilliseconds'])
        self.auth_events.append({'stage':'app_confirm','guestConfirmed':True,'generation':self.controller.generation})
        self.state='authenticated_settle';self.deadline=None;self.next_at=self.now+1.0
