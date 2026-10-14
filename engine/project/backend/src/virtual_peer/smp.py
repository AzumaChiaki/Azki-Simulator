"""Local legacy SMP Just Works and authenticated virtual air packets.

Bluetooth c1/s1 byte order checked against BlueZ shared/crypto.c. This initial
profile negotiates legacy, no OOB, no MITM and encryption-key distribution only.
Unsupported methods fail. No key or raw SMP payload is emitted into evidence.
"""
import hmac
import secrets
from .crypto import CommonCryptoAES
from .auth_crypto import AESModes, xor


def c1(key,random,request,response,initiator,responder,iat=0,rat=0):
    if tuple(map(len,(key,random,request,response,initiator,responder)))!=(16,16,7,7,6,6):
        raise ValueError('SMP c1 lengths')
    aes=CommonCryptoAES()
    if iat not in (0,1) or rat not in (0,1):raise ValueError('SMP address type')
    p1=bytes((iat,rat))+request+response
    return aes.le_encrypt(key,xor(aes.le_encrypt(key,xor(random,p1)),responder+initiator+bytes(4)))


def s1(key,responder_random,initiator_random):
    return CommonCryptoAES().le_encrypt(key,initiator_random[:8]+responder_random[:8])


class VirtualAir:
    """Two independent endpoint keys, CCM/MIC verification before delivery.

    HCI remains plaintext by definition. A 13-byte BLE nonce and 4-byte MIC are
    applied in the internal link between controller endpoints. Counters cannot
    repeat in a direction; ciphertext hashes, never keys/payloads, are recorded.
    """
    def __init__(self,peer_ltk,guest_ltk):
        if len(peer_ltk)!=16 or len(guest_ltk)!=16:raise ValueError('Virtual link key size')
        skd=secrets.token_bytes(16);aes=CommonCryptoAES()
        self.peer_key=aes.le_encrypt(peer_ltk,skd)[::-1]
        self.guest_key=aes.le_encrypt(guest_ltk,skd)[::-1]
        self.iv=secrets.token_bytes(8);self.count=[0,0];self.aes=AESModes();self.records=[]
        # Independent keys must prove both directions before Encryption Change.
        self.transfer(0,2,b'Azki virtual LL start')
        self.transfer(1,2,b'Azki virtual LL response')

    def transfer(self,direction,llid,payload):
        import hashlib
        if direction not in (0,1) or llid not in (1,2) or len(payload)>251:raise ValueError('Virtual air bound')
        counter=self.count[direction]
        if counter>=1<<39:raise ValueError('Virtual air counter exhausted')
        nonce=(counter|(direction<<39)).to_bytes(5,'little')+self.iv
        send,receive=(self.peer_key,self.guest_key) if direction==0 else (self.guest_key,self.peer_key)
        sealed=self.aes.ccm_encrypt(send,nonce,payload,bytes((llid,)),4)
        decoded=self.aes.ccm_decrypt(receive,nonce,sealed,bytes((llid,)),4)
        self.count[direction]+=1
        if len(self.records)<32:self.records.append({'direction':direction,'counter':counter,'bytes':len(payload),'ciphertextSHA256':hashlib.sha256(sealed).hexdigest(),'micVerified':True})
        return decoded


class LegacyCentral:
    def __init__(self,controller):
        self.controller=controller;self.events=[];self.bond=None;self.reset()

    def reset(self):self.state='idle';self.remote_ltk=None;self.request=self.response=None

    def resume_bond(self):
        """A bonded central initiates link encryption before application login.

        The guest must supply its independently stored LTK; the controller does
        not signal encrypted until both endpoint keys pass CCM/MIC verification.
        """
        if self.bond is None or self.state != 'idle':
            raise ValueError('Invalid SMP bond resume state')
        self.controller.request_link_key(*self.bond)
        self.state='encrypted_reconnect'
        self.events.append({'bondKeyRequested':True})

    def receive(self,pdu):
        if not pdu:raise ValueError('Empty SMP')
        op=pdu[0];self.events.append({'opcode':op,'bytes':len(pdu),'state':self.state})
        if op==11 and len(pdu)==2 and self.state in ('idle','encrypted_reconnect'):
            self.events[-1]['authReq']=pdu[1]
            # Security Request expresses the peripheral's preference. Our real
            # NoInputNoOutput capability selects Just Works; send the standard
            # request and let the original peripheral accept or reject it.
            # This mode provides encryption, never a claim of MITM protection.
            if self.state=='encrypted_reconnect':
                # A peripheral Security Request may cross the central's LTK
                # request. It must not create a second request or a new bond.
                if self.controller.pending_key is not None or self.controller.air is not None:
                    return []
                raise ValueError('SMP bond resume lost its encryption request')
            if self.bond:
                self.resume_bond();return []
            self.request=bytes((1,3,0,1,16,0,1))
            self.random=secrets.token_bytes(16);self.state='response'
            return [(6,self.request)]
        if op==2 and len(pdu)==7 and self.state=='response':
            self.events[-1]['negotiation']={'ioCapability':pdu[1],'oob':pdu[2],'authReq':pdu[3],'keySize':pdu[4],'initiatorKeys':pdu[5],'responderKeys':pdu[6]}
            if pdu[1]>4 or pdu[2] or pdu[4]!=16 or pdu[5] or pdu[6]&~1:
                raise ValueError('Unsupported SMP legacy negotiation')
            self.response=pdu;self.state='confirm'
            confirm=c1(bytes(16),self.random,self.request,pdu,bytes.fromhex('a00000000002'),self.controller.address[::-1])
            return [(6,b'\x03'+confirm)]
        if op==3 and len(pdu)==17 and self.state=='confirm':
            self.remote_confirm=pdu[1:];self.state='random';return [(6,b'\x04'+self.random)]
        if op==4 and len(pdu)==17 and self.state=='random':
            expected=c1(bytes(16),pdu[1:],self.request,self.response,bytes.fromhex('a00000000002'),self.controller.address[::-1])
            if not hmac.compare_digest(self.remote_confirm,expected):raise ValueError('Guest SMP confirm mismatch')
            self.events.append({'guestConfirmVerified':True,'method':'legacy-just-works','mitmProtected':False})
            stk=s1(bytes(16),pdu[1:],self.random)
            self.state='keys';self.controller.request_link_key(stk,bytes(8),0);return []
        if op==6 and len(pdu)==17 and self.state=='keys' and self.controller.air is not None:
            self.remote_ltk=pdu[1:];return []
        if op==7 and len(pdu)==11 and self.state=='keys' and self.remote_ltk is not None:
            self.bond=(self.remote_ltk,pdu[3:],int.from_bytes(pdu[1:3],'little'))
            self.state='bonded';self.events.append({'guestKeyDistributionVerified':True});return []
        if op==5 and len(pdu)==2:raise ValueError('Guest SMP Pairing Failed '+str(pdu[1]))
        raise ValueError('Unexpected SMP state/opcode')
