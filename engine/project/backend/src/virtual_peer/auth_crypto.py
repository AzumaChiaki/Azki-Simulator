"""Protocol crypto with macOS AES/Security and stdlib SHA256/HMAC.

P-256 keys are ephemeral SecKey objects, explicitly non-permanent. No Keychain,
external key stores, cloud services or caller-provided device identity material.
AES-CCM/CTR follow the standard mode constructions over system AES; known-answer
tests cover the exact nonce/tag layouts before use with a guest.
"""
import ctypes as C
import hashlib
import hmac
from .crypto import CommonCryptoAES


def mac(key, message):
    return hmac.new(key, message, hashlib.sha256).digest()


def hkdf(key, salt, info, length=64):
    if not 1 <= length <= 255*32: raise ValueError('HKDF output bound')
    extracted = mac(salt, key)
    result, previous = b'', b''
    for index in range(1, (length+31)//32+1):
        previous = mac(extracted, previous+info+bytes((index,)))
        result += previous
    return result[:length]


def xor(a, b):
    return bytes(x^y for x,y in zip(a,b))


class AESModes:
    def __init__(self): self.aes = CommonCryptoAES()

    def ctr(self, key, counter, data):
        if len(counter)!=16 or len(data)>65536: raise ValueError('CTR bounds')
        result = bytearray()
        value = int.from_bytes(counter,'big')
        for offset in range(0,len(data),16):
            result += xor(data[offset:offset+16],self.aes.encrypt_block(key,value.to_bytes(16,'big')))
            value = (value+1) % (1<<128)
        return bytes(result)

    def _ccm_parameters(self, nonce, data, aad, tag):
        if not 7<=len(nonce)<=13 or tag not in (4,6,8,10,12,14,16): raise ValueError('CCM nonce/tag')
        length_bytes=15-len(nonce)
        if len(data)>65536 or len(data)>=(1<<(8*length_bytes)) or len(aad)>=0xff00: raise ValueError('CCM data bound')
        return length_bytes

    def _ccm_mac(self,key,nonce,data,aad,tag):
        length_bytes=self._ccm_parameters(nonce,data,aad,tag)
        flags=(0x40 if aad else 0)|(((tag-2)//2)<<3)|(length_bytes-1)
        blocks=bytes((flags,))+nonce+len(data).to_bytes(length_bytes,'big')
        if aad:
            framed=len(aad).to_bytes(2,'big')+aad
            blocks+=framed+bytes((-len(framed))%16)
        blocks+=data+bytes((-len(data))%16)
        value=bytes(16)
        for offset in range(0,len(blocks),16):
            value=self.aes.encrypt_block(key,xor(value,blocks[offset:offset+16]))
        return value[:tag]

    def ccm_encrypt(self,key,nonce,data,aad=b'',tag=4):
        length_bytes=self._ccm_parameters(nonce,data,aad,tag)
        prefix=bytes((length_bytes-1,))+nonce
        authentication=self._ccm_mac(key,nonce,data,aad,tag)
        masked=xor(authentication,self.aes.encrypt_block(key,prefix+bytes(length_bytes)))
        ciphertext=self.ctr(key,prefix+(1).to_bytes(length_bytes,'big'),data)
        return ciphertext+masked

    def ccm_decrypt(self,key,nonce,data,aad=b'',tag=4):
        if len(data)<tag: raise ValueError('CCM truncated tag')
        length_bytes=self._ccm_parameters(nonce,data[:-tag],aad,tag)
        prefix=bytes((length_bytes-1,))+nonce
        plaintext=self.ctr(key,prefix+(1).to_bytes(length_bytes,'big'),data[:-tag])
        expected=self.ccm_encrypt(key,nonce,plaintext,aad,tag)[-tag:]
        if not hmac.compare_digest(expected,data[-tag:]): raise ValueError('CCM authentication failed')
        return plaintext


class P256:
    """Ephemeral standard P-256 ECDH through Apple's public SecKey API."""
    def __init__(self, private_data=None):
        self.cf=C.CDLL('/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation')
        self.sec=C.CDLL('/System/Library/Frameworks/Security.framework/Security')
        p=C.c_void_p; n=C.c_long
        for library,name,restype,args in (
            (self.cf,'CFRelease',None,[p]),(self.cf,'CFDataCreate',p,[p,p,n]),
            (self.cf,'CFErrorGetCode',n,[p]),
            (self.cf,'CFDataGetLength',n,[p]),(self.cf,'CFDataGetBytePtr',p,[p]),
            (self.cf,'CFNumberCreate',p,[p,C.c_int,p]),
            (self.cf,'CFDictionaryCreate',p,[p,p,p,n,p,p]),
            (self.sec,'SecKeyCreateRandomKey',p,[p,p]),
            (self.sec,'SecKeyCopyPublicKey',p,[p]),
            (self.sec,'SecKeyCopyExternalRepresentation',p,[p,p]),
            (self.sec,'SecKeyCreateWithData',p,[p,p,p]),
            (self.sec,'SecKeyCopyKeyExchangeResult',p,[p,p,p,p,p])):
            fn=getattr(library,name);fn.restype=restype;fn.argtypes=args
        bits=C.c_int(256)
        number=self.cf.CFNumberCreate(None,9,C.byref(bits))  # kCFNumberIntType
        attrs=self._dictionary({'kSecAttrKeyType':self._symbol('kSecAttrKeyTypeECSECPrimeRandom'),
             'kSecAttrKeySizeInBits':number,'kSecAttrIsPermanent':C.c_void_p.in_dll(self.cf,'kCFBooleanFalse').value})
        try:
            if private_data is None:
                self.private=self._checked(self.sec.SecKeyCreateRandomKey,attrs)
            else:
                if not isinstance(private_data,bytes) or len(private_data)!=97 or private_data[0]!=4:
                    raise ValueError('Self-generated P256 private representation required')
                raw=C.create_string_buffer(private_data)
                data=self.cf.CFDataCreate(None,raw,len(private_data))
                private_attrs=self._dictionary({'kSecAttrKeyType':self._symbol('kSecAttrKeyTypeECSECPrimeRandom'),
                    'kSecAttrKeyClass':self._symbol('kSecAttrKeyClassPrivate')})
                try:self.private=self._checked(self.sec.SecKeyCreateWithData,data,private_attrs)
                finally:self.cf.CFRelease(data);self.cf.CFRelease(private_attrs)
        finally:self.cf.CFRelease(attrs);self.cf.CFRelease(number)
        public=self.sec.SecKeyCopyPublicKey(self.private)
        if not public: raise ValueError('System P256 public-key export failed')
        try:self.public=self._data(self._checked(self.sec.SecKeyCopyExternalRepresentation,public))[1:]
        finally:self.cf.CFRelease(public)
        if len(self.public)!=64: raise ValueError('Unexpected P256 public key')

    def _symbol(self,name): return C.c_void_p.in_dll(self.sec,name).value

    def _dictionary(self,values):
        pairs=[(self._symbol(k),v) for k,v in values.items()]
        keys=(C.c_void_p*len(pairs))(*(k for k,_ in pairs))
        vals=(C.c_void_p*len(pairs))(*(v for _,v in pairs))
        # Callbacks retain referenced CF constants/data for dictionary lifetime.
        kc=C.addressof(C.c_byte.in_dll(self.cf,'kCFTypeDictionaryKeyCallBacks'))
        vc=C.addressof(C.c_byte.in_dll(self.cf,'kCFTypeDictionaryValueCallBacks'))
        return self.cf.CFDictionaryCreate(None,keys,vals,len(pairs),kc,vc)

    def _checked(self,fn,*args):
        error=C.c_void_p()
        result=fn(*args,C.byref(error))
        code=self.cf.CFErrorGetCode(error) if error.value else None
        if error.value:self.cf.CFRelease(error)
        if not result:raise ValueError('System P256 operation failed: '+str(code))
        return result

    def _data(self,value):
        try:
            length=self.cf.CFDataGetLength(value)
            if not 0<=length<=128: raise ValueError('System P256 data bound')
            return C.string_at(self.cf.CFDataGetBytePtr(value),length)
        finally:self.cf.CFRelease(value)

    def exchange(self,public):
        if not isinstance(public,bytes) or len(public)!=64: raise ValueError('P256 public key must be 64 bytes')
        raw=C.create_string_buffer(b'\x04'+public)
        data=self.cf.CFDataCreate(None,raw,65)
        attrs=self._dictionary({'kSecAttrKeyType':self._symbol('kSecAttrKeyTypeECSECPrimeRandom'),
                                'kSecAttrKeyClass':self._symbol('kSecAttrKeyClassPublic')})
        params=self._dictionary({});peer=None
        try:
            peer=self._checked(self.sec.SecKeyCreateWithData,data,attrs)
            result=self._data(self._checked(self.sec.SecKeyCopyKeyExchangeResult,self.private,
                self._symbol('kSecKeyAlgorithmECDHKeyExchangeStandard'),peer,params))
            if len(result)!=32:raise ValueError('P256 shared-secret length')
            return result
        finally:
            if peer:self.cf.CFRelease(peer)
            for value in (data,attrs,params):self.cf.CFRelease(value)

    def export_private(self):
        """Export only this object's newly generated key for its private sandbox."""
        return self._data(self._checked(self.sec.SecKeyCopyExternalRepresentation,self.private))

    def close(self):
        if self.private:self.cf.CFRelease(self.private);self.private=None
