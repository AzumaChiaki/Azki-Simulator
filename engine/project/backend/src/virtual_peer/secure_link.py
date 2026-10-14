"""LE encryption is available only after SMP yields and guest supplies a key."""
import struct
from .hci import Command, complete
from .link import LinkController, HANDLE
from .smp import VirtualAir

COMMANDS={0x201A:Command('LE_Long_Term_Key_Request_Reply',18,28,1,2),
          0x201B:Command('LE_Long_Term_Key_Request_Negative_Reply',2,28,2,2)}


class SecureLinkController(LinkController):
    def reset(self):
        super().reset();self.air=None;self.pending_key=None
        if not hasattr(self,'encryption_events'):self.encryption_events=[]

    def disconnect_local(self,reason=0x13):
        self.air=None;self.pending_key=None
        return super().disconnect_local(reason)

    def command_bits(self):
        result=bytearray(super().command_bits())
        for entry in COMMANDS.values():result[entry.supported_byte]|=1<<entry.supported_bit
        return bytes(result)

    def request_link_key(self,expected,random,ediv):
        if not self.connected or self.pending_key or self.air or len(expected)!=16 or len(random)!=8:
            raise ValueError('Invalid virtual LL key request state')
        event=self.enabled_event(0x3E,struct.pack('<H',HANDLE)+random+struct.pack('<H',ediv),5)
        if not event:raise ValueError('Guest did not enable LTK request')
        self.pending_key=expected;self.link_events+=event

    def respond(self,opcode,params):
        if opcode==0x2003 and not params:return complete(opcode,0,b'\1'+bytes(7))
        if opcode==0x2016 and len(params)==2 and self.connected and struct.unpack('<H',params)[0]==HANDLE:
            from .link import status
            self.link_events+=self.enabled_event(0x3E,struct.pack('<BH',0,HANDLE)+b'\1'+bytes(7),4)
            return status(opcode)
        if opcode not in COMMANDS:return super().respond(opcode,params)
        handle=struct.unpack_from('<H',params)[0] if len(params)>=2 else 0
        if len(params)!=COMMANDS[opcode].length:return complete(opcode,0x12,struct.pack('<H',handle))
        if handle!=HANDLE or not self.connected:return complete(opcode,2,struct.pack('<H',handle))
        if self.pending_key is None:return complete(opcode,0x0c,struct.pack('<H',handle))
        expected,self.pending_key=self.pending_key,None
        try:
            if opcode==0x201B:raise ValueError('Guest rejected LTK request')
            self.air=VirtualAir(expected,params[2:])
        except ValueError:
            self.encryption_events.append({'generation':self.generation,'independentKeyMICVerified':False})
            self.link_events+=self.enabled_event(8,struct.pack('<BHB',6,HANDLE,0))
            self.link_events+=self.disconnect_local(5)
            return complete(opcode,0,struct.pack('<H',handle))
        self.encryption_events.append({'generation':self.generation,'independentKeyMICVerified':True,'startPackets':list(self.air.records)})
        self.link_events+=self.enabled_event(8,struct.pack('<BHB',0,HANDLE,1))
        return complete(opcode,0,struct.pack('<H',handle))

    def transfer_air(self,direction,pb,data):
        if self.air is None:return data
        result=self.air.transfer(direction,2 if pb in (0,2) else 1,data)
        self.encryption_events[-1]['packetCounts']=list(self.air.count)
        self.encryption_events[-1]['packetEvidence']=list(self.air.records)
        return result
