"""Bounded business queue owned by AuthenticationCentral after DeviceConfirm.

Only SessionView is handed to business consumers. Its methods cannot supply
keys or set authentication state. SAR framing/CRC remains with the controller;
this module owns per-message encryption and application event delivery.
"""
from collections import deque
import secrets
from .auth_crypto import AESModes
from .session_api import SessionStatus, SessionEvent


class SessionView:
    def __init__(self,owner):self.__owner=owner
    def status(self):return self.__owner.status()
    def submit(self,channel,opcode,payload,*,expected_generation):
        return self.__owner.submit(channel,opcode,payload,expected_generation=expected_generation)
    def poll(self):
        owner=self.__owner;events=list(owner.events);owner.events.clear();return events


class BusinessChannel:
    def __init__(self):
        self.session_id=secrets.token_hex(16)
        self.generation=0;self.authenticated=False;self.ready=False;self.mps=4096
        self.app_key=self.device_key=None
        self.events=deque();self.queue=deque();self.inflight=deque();self.retries=deque()
        self.window=8;self.ack_timeout=10.0;self.last_progress=None;self.now=0.0
        self.outgoing_sequence=None;self.nak_streak=0;self.replayed=False
        self.metrics={'sentFrames':0,'acknowledgedFrames':0,'retransmittedFrames':0,'nakCount':0}
        self.counter=0;self.last_rx=None;self.aes=AESModes();self.api=SessionView(self)

    def status(self):
        return SessionStatus(self.session_id,self.generation,self.authenticated,
                             self.ready,self.mps,'guest',self.window)

    def _event(self,kind,**values):
        if len(self.events)>=64:raise ValueError('Business event queue bound')
        self.events.append(SessionEvent(self.session_id,self.generation,kind,**values))

    def invalidate(self,generation):
        if self.authenticated or self.inflight or self.queue:self._event('disconnected')
        self.authenticated=False;self.ready=False
        self.app_key=self.device_key=None
        self.queue.clear();self.inflight.clear();self.retries.clear();self.last_rx=None;self.generation=generation
        self.last_progress=None

    def confirm_from_guest(self,generation,app_key,device_key,remote_mps,send_timeout=10000):
        # Called exclusively by the verified DeviceConfirm branch, not exposed
        # through SessionView or accepted from serialized application input.
        if generation!=self.generation or len(app_key)!=16 or len(device_key)!=16:
            raise ValueError('Authenticated session material mismatch')
        if type(remote_mps) is not int or remote_mps<64:raise ValueError('Guest SAR MPS too small')
        self.mps=min(remote_mps,65535);self.app_key=app_key;self.device_key=device_key
        self.ack_timeout=min(20,max(2,send_timeout/1000));self.window=8
        self.ready=True;self.authenticated=True

    def submit(self,channel,opcode,payload,*,expected_generation):
        if not self.authenticated or not self.ready or expected_generation!=self.generation:
            raise ValueError('Guest session is not authenticated for this generation')
        if (channel,opcode) not in ((1,2),(2,1)) or type(channel) is not int or type(opcode) is not int:
            raise ValueError('Business channel/opcode not allowed')
        if not isinstance(payload,bytes) or not 1<=len(payload)<=self.mps-2:
            raise ValueError('Business message exceeds negotiated SAR bound')
        if len(self.queue)+len(self.inflight)>=8:raise ValueError('Business submission queue bound')
        self.counter+=1;request_id=f'{self.generation}:{self.counter}'
        self.queue.append((request_id,channel,opcode,payload))
        return request_id

    def next_wire(self,sequence,now):
        self.now=now
        if not self.authenticated:return None
        if self.inflight:
            head=self.inflight[0];stall=now-self.last_progress
            if stall>=120:
                self._event('error',request_id=head['id'],error='Guest SAR ACK timeout')
                self.invalidate(self.generation)
                raise ValueError('Guest business SAR ACK timeout')
            if not self.retries and now>=head['deadline']:
                if stall>=75:self.window=1;limit=1
                elif stall>=45 and not self.replayed:self.replayed=True;limit=len(self.inflight)
                else:limit=len(self.inflight) if head['attempts']==2 else 1
                self.retries.extend(list(self.inflight)[:limit]);head['attempts']+=1
                boundary=45 if stall<45 else 75 if stall<75 else 120
                head['deadline']=min(now+min(20,self.ack_timeout*head['attempts']),self.last_progress+boundary)
        while self.retries:
            item=self.retries.popleft()
            if item not in self.inflight:continue
            self.outgoing_sequence=item['sequence'];self.metrics['retransmittedFrames']+=1
            return item['wire']
        if not self.queue or len(self.inflight)>=self.window:return None
        request_id,channel,opcode,payload=self.queue.popleft()
        if opcode==2:payload=self.aes.ctr(self.app_key,self.app_key,payload)
        wire=bytes((channel,opcode))+payload
        if not self.inflight:self.last_progress=now;self.replayed=False
        self.inflight.append({'id':request_id,'sequence':sequence,'deadline':now+self.ack_timeout,
                             'channel':channel,'opcode':opcode,'wire':wire,'attempts':1})
        self.outgoing_sequence=sequence;self.metrics['sentFrames']+=1
        return wire

    def ack(self,sequence):
        if not self.inflight or ((self.inflight[-1]['sequence']-sequence)&255)>=128:return
        changed=False
        while self.inflight and ((sequence-self.inflight[0]['sequence'])&255)<128:
            request=self.inflight.popleft();changed=True;self.metrics['acknowledgedFrames']+=1
            self._event('transport_ack',request_id=request['id'],channel=request['channel'],opcode=request['opcode'])
        if changed:
            self.last_progress=self.now;self.replayed=False;self.nak_streak=0
            if self.inflight:self.inflight[0]['deadline']=self.now+self.ack_timeout

    def nak(self,sequence):
        if not self.inflight:return
        # NAK N acknowledges everything before N, then requests retransmission
        # from N. Ignore future/stale NAKs outside this bounded 8-frame window.
        if sequence not in [item['sequence'] for item in self.inflight] and sequence!=(self.inflight[-1]['sequence']+1)&255:return
        self.ack((sequence-1)&255)
        self.metrics['nakCount']+=1
        if not self.inflight:return
        self.nak_streak+=1
        if self.nak_streak>=3:self.window=max(1,self.window//2);self.nak_streak=0
        self.retries=deque(self.inflight)

    def receive(self,sequence,payload):
        if not self.authenticated:raise ValueError('Business data before guest confirmation')
        if self.last_rx==sequence:return
        if len(payload)<3 or len(payload)>self.mps:raise ValueError('Guest business message bound')
        channel,opcode=payload[:2];data=payload[2:]
        if (channel,opcode) not in ((1,2),(2,1)):
            raise ValueError('Guest business channel/opcode not allowed')
        if opcode==2:data=self.aes.ctr(self.device_key,self.device_key,data)
        self.last_rx=sequence
        self._event('business_message',channel=channel,opcode=opcode,payload=data)
