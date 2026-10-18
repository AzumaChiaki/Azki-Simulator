"""Private, synthetic-only identity and guest-confirmed binding state.

The caller owns the storage transaction. This module never updates its HEAD,
reads real device material, or persists a key to the system Keychain.
"""
import copy
import binascii
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import stat
import struct
import string
from types import MappingProxyType
import uuid

PROFILE = 'q66@4.100.177'
AP_SHA256 = 'ff74c6f467529963b6afd40669437d8ec39a02e48c10d0893739847b3ba3dc2b'
INPUT_NAME = 'virtual-peer-state.json'
OUTPUT_NAME = 'virtual-peer-state.pending.json'

# Exact firmware candidates. Enabling an authored seed permits a bounded guest
# attempt, not acceptance. No ROM fallback identity or key is ever imported.
_PROFILE_ROWS = (
    (PROFILE, AP_SHA256, 'miwear.watch.q66nfc', None),
    ('miwear.watch.q66glt@4.200.078@754da125b286', 'bf36cc88905271e7f596b1de9b8c314e6a9737d5e36f8d67f60535f003ee229c', 'miwear.watch.q66glt', None),
    ('miwear.watch.q66tc@4.100.155@f1c824fef81d', 'ea0bdf1920cb30223d616432af00565ca67622e6468328f5eab155f8cdc2fb9f', 'miwear.watch.q66tc', None),
    ('miwear.watch.q66u@4.100.108@6ef691453a86', '9315ca353f624cec25dfcfc98a95ba959e2d7b24573bf1d6adf16ea10341bd99', 'miwear.watch.q66u', None),
    ('miwear.watch.n66cn@3.1.32@489f20502ae2', '9c02dab4020b2cc9666ee7d34cf27d311b76aadcec519a38361bbcbd94c53264', 'miwear.watch.n66cn', 'legacy_factory8k_identity_encoding_unverified'),
    ('miwear.watch.n66nfc@3.1.32@489f20502ae2', '9c02dab4020b2cc9666ee7d34cf27d311b76aadcec519a38361bbcbd94c53264', 'miwear.watch.n66nfc', 'legacy_factory8k_identity_encoding_unverified'),
    ('miwear.watch.n66tc@3.1.32@489f20502ae2', '9c02dab4020b2cc9666ee7d34cf27d311b76aadcec519a38361bbcbd94c53264', 'miwear.watch.n66tc', 'legacy_factory8k_identity_encoding_unverified'),
    ('miwear.watch.n67cn@3.1.187@6d33afe20089', 'acb9b0e867140f09c90747c0ada98f340e2edb540eb69b999c4b78adbc102595', 'miwear.watch.n67cn', 'n67_flash_config_identity_encoding_unverified'),
    ('miwear.watch.o66cn@3.1.28@03ac3e2b8325', 'fb726472416e99ba11d1c20ce8f0d4f0831a6e9a673d63bb5918e2c3921d8845', 'miwear.watch.o66cn', 'legacy_factory8k_identity_encoding_unverified'),
    ('miwear.watch.o66lj@3.1.26@d4e61778eee0', '185fc5737fa203ff3f109a282feca89a09c12d84bccd44fd7b61d74853f7fa49', 'miwear.watch.o66lj', 'legacy_factory8k_identity_encoding_unverified'),
    ('miwear.watch.o66nfc@3.1.28@03ac3e2b8325', 'fb726472416e99ba11d1c20ce8f0d4f0831a6e9a673d63bb5918e2c3921d8845', 'miwear.watch.o66nfc', 'legacy_factory8k_identity_encoding_unverified'),
    ('miwear.watch.o66tc@3.1.28@03ac3e2b8325', 'fb726472416e99ba11d1c20ce8f0d4f0831a6e9a673d63bb5918e2c3921d8845', 'miwear.watch.o66tc', 'legacy_factory8k_identity_encoding_unverified'),
    ('miwear.watch.p67cn@3.101.043@a4ce85647786', '519307675665e4866d722a8119a98589c397b614ac3294cb87bfc86de45756ec', 'miwear.watch.p67cn', 'p67_psk_property_reader_and_login_lifecycle_unverified'),
    ('miwear.watch.p67tc@3.101.043@a4ce85647786', '519307675665e4866d722a8119a98589c397b614ac3294cb87bfc86de45756ec', 'miwear.watch.p67tc', 'p67_psk_property_reader_and_login_lifecycle_unverified'),
)
def _descriptor(identity, ap, model, _historical_reason):
    family = model.rsplit('.', 1)[1][:3]
    legacy = family in ('n66', 'o66')
    sku = model.rsplit('.', 1)[1][3:]
    return MappingProxyType({
        'profileID': identity, 'apSHA256': ap, 'model': model,
        'family': family, 'unsupportedReason': None,
        'protocol': 'miwear-psk1-q66' if family in ('q65', 'q66') else 'miwear-psk1-literal16',
        'verifyMode': 1, 'requiresSMPBond': True,
        'factoryFormat': 'legacy-fixed-8k' if legacy else 'n67-config-journal' if family == 'n67' else 'littlefs-8192x16',
        # P65 carries the same 128 KiB littlefs factory partition as Q65/Q66
        # (p65-oobe evidence: the guest does not mount it as /misc but boots
        # fine with a valid image present; the reference runner always wrote one).
        'factoryPropertyFile': 'app_nv.prop' if family == 'p67' else 'factory.prop' if family in ('p65', 'q65', 'q66') else None,
        'factoryBytes': 8192 if legacy or family == 'n67' else 128 * 1024,
        'configBytes': 16384 if family == 'n67' else 0,
        'pskEncoding': 'ascii-hex32' if family in ('q65', 'q66') else 'hex-of-ascii16',
        'pskProperty': 'ro.factory.psk' if family in ('q65', 'q66') else 'ro.factory.key' if family in ('n67', 'p67') else 'factory.param.key',
        'didDigits': 9 if family == 'n66' else 11 if family == 'o66' else 13,
        'serialMaximum': 24 if legacy else 21 if family == 'n67' else 31,
        'boardID': {'cn': 2, 'nfc': 1, 'tc': 5, 'lj': 7}.get(sku) if legacy else None,
        'guestVerified': False,
        'authConfigurationEvidence': 'q66-static-protocol-equivalence' if family == 'q66' else 'legacy-static-identity-runtime-auth-pending',
    })


PROFILE_DESCRIPTORS = MappingProxyType({identity: _descriptor(identity, ap, model, reason)
    for identity, ap, model, reason in _PROFILE_ROWS})


# Any firmware build of these families: "<model>@<version>@<OTA SHA prefix>".
_FIRMWARE_PROFILE = re.compile(r'(miwear\.watch\.(n66|n67|o66|p65|p67|q65|q66)[a-z]{0,8})@[0-9][0-9.]{0,31}@[0-9a-f]{12}')


def descriptor_for(profile_id, ap_sha256=None):
    """Return the synthetic identity descriptor of a profile.

    The identity encoding follows the hardware family and SKU, so a firmware
    build outside the measured table (another catalog version or an imported
    OTA) uses its model's descriptor with its own AP digest. Static identity
    encodings permit a real guest attempt; the guest decides acceptance.
    """
    if ap_sha256 is not None and not _hex(ap_sha256, 64):
        raise ValueError('Invalid descriptive firmware digest')
    if type(profile_id) is str and profile_id not in PROFILE_DESCRIPTORS:
        match = _FIRMWARE_PROFILE.fullmatch(profile_id)
        if match is None or ap_sha256 is None:
            raise ValueError('Unknown synthetic identity profile')
        return _descriptor(profile_id, ap_sha256, match.group(1), None)
    if type(profile_id) is not str:
        raise ValueError('Unknown synthetic identity profile')
    descriptor = PROFILE_DESCRIPTORS[profile_id]
    if descriptor['unsupportedReason'] is not None:
        raise ValueError('Synthetic identity profile unsupported: ' + descriptor['unsupportedReason'])
    return descriptor


def _hex(value, length):
    return type(value) is str and re.fullmatch('[0-9a-f]{'+str(length)+'}', value) is not None


def canonical_uuid(value):
    try:
        return type(value) is str and str(uuid.UUID(value)) == value
    except (ValueError, AttributeError):
        return False


def strict_json(raw):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result: raise ValueError('Duplicate JSON key')
            result[key] = value
        return result
    return json.loads(raw, object_pairs_hook=pairs,
                      parse_constant=lambda _: (_ for _ in ()).throw(ValueError('Nonfinite JSON value')))


def validate_state(value):
    required = {'schema','origin','instanceID','profileID','apSHA256','identityVersion',
                'identity','psk','p256Private','binding'}
    if type(value) is not dict or set(value) - {'guestLinkBond'} != required:
        raise ValueError('Invalid synthetic state schema')
    descriptor = descriptor_for(value['profileID'], value['apSHA256'])
    if (type(value['schema']) is not int or value['schema'] != 1 or
        value['origin'] != 'synthetic-virtual-instance' or not canonical_uuid(value['instanceID']) or
        not _hex(value['apSHA256'], 64) or
        type(value['identityVersion']) is not int or not 1 <= value['identityVersion'] <= 0xffffffff):
        raise ValueError('Synthetic state identity mismatch')
    identity = value['identity']
    if type(identity) is not dict or set(identity) != {'serial','address','did','model'}:
        raise ValueError('Invalid synthetic identity fields')
    if (type(identity['serial']) is not str or not 1 <= len(identity['serial']) <= descriptor['serialMaximum'] or
        any(not 33 <= ord(char) <= 126 for char in identity['serial']) or
        type(identity['did']) is not str or not re.fullmatch('99[0-9]{'+str(descriptor['didDigits']-2)+'}', identity['did']) or
        identity['model'] != descriptor['model'] or type(identity['address']) is not str or
        not re.fullmatch(r'[0-9A-F]{2}(:[0-9A-F]{2}){5}', identity['address'])):
        raise ValueError('Identity must be a locally generated virtual device')
    if not _hex(value['psk'], 32) or not _hex(value['p256Private'], 194) or not value['p256Private'].startswith('04'):
        raise ValueError('Invalid synthetic private material')
    if descriptor['pskEncoding'] == 'hex-of-ascii16':
        key = bytes.fromhex(value['psk'])
        if not re.fullmatch(rb'[A-Za-z0-9]{16}', key):
            raise ValueError('Legacy synthetic PSK must encode exactly 16 property-safe ASCII bytes')
    binding = value['binding']
    bond = value.get('guestLinkBond')
    if bond is not None and (type(bond) is not dict or set(bond) != {'ltk','random','ediv'} or
            not _hex(bond['ltk'],32) or not _hex(bond['random'],16) or
            type(bond['ediv']) is not int or not 0 <= bond['ediv'] <= 65535):
        raise ValueError('Invalid guest-confirmed external link bond')
    if binding is not None:
        if (type(binding) is not dict or set(binding) != {'authKey','ltk','random','ediv'} or
            not _hex(binding['authKey'],32) or not _hex(binding['ltk'],32) or
            not _hex(binding['random'],16) or type(binding['ediv']) is not int or not 0 <= binding['ediv'] <= 65535):
            raise ValueError('Invalid guest-confirmed synthetic binding')
    return value


def create_state(instance_id, identity_version=1, *, private_data=None, profile_id=PROFILE, ap_sha256=None):
    """Create only a new synthetic identity, optionally using its host-made key.

    The native host supplies a newly generated P-256 X9.63 representation;
    restoring it needs no random-key service from inside the static sandbox.
    macOS Security validates the key representation before state is returned.
    """
    descriptor = descriptor_for(profile_id, ap_sha256)
    from .auth_crypto import P256
    if not canonical_uuid(instance_id) or type(identity_version) is not int or identity_version != 1:
        raise ValueError('Only a new empty virtual instance can create its first identity')
    if private_data is not None and (type(private_data) is not bytes or
                                    len(private_data) != 97 or private_data[0] != 4):
        raise ValueError('Invalid synthetic P256 representation')
    key = P256(private_data)
    try: private = key.export_private().hex()
    finally: key.close()
    address = bytes((2,)) + secrets.token_bytes(5)
    digits = descriptor['didDigits'] - 2
    psk = secrets.token_hex(16) if descriptor['pskEncoding'] == 'ascii-hex32' else ''.join(
        secrets.choice(string.ascii_letters + string.digits) for _ in range(16)).encode('ascii').hex()
    return validate_state({'schema':1,'origin':'synthetic-virtual-instance',
        'instanceID':instance_id,'profileID':profile_id,'apSHA256':descriptor['apSHA256'],'identityVersion':1,
        'identity':{'serial':'AZKI-'+descriptor['family'].upper()+'-'+secrets.token_hex(6).upper(),
                    'address':':'.join(f'{byte:02X}' for byte in address),
                    'did':'99'+str(secrets.randbelow(10**digits)).zfill(digits), 'model':descriptor['model']},
        'psk':psk,'p256Private':private,'binding':None})


def encode_state(value):
    raw = (json.dumps(validate_state(value), sort_keys=True, separators=(',',':'))+'\n').encode('ascii')
    if len(raw) > 4096: raise ValueError('Synthetic state exceeds file bound')
    return raw


def load_state(path, expected_sha256):
    path = Path(path)
    if path.name != INPUT_NAME or not _hex(expected_sha256,64):
        raise ValueError('Invalid private state input contract')
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    try:
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or
            stat.S_IMODE(info.st_mode) != 0o600 or info.st_nlink != 1 or not 1 <= info.st_size <= 4096):
            raise ValueError('Unsafe private state input')
        raw = os.read(fd,4097)
        if len(raw) != info.st_size or hashlib.sha256(raw).hexdigest() != expected_sha256:
            raise ValueError('Private state input hash mismatch')
    finally: os.close(fd)
    return validate_state(strict_json(raw))


def write_pending(path, state, binding):
    path = Path(path)
    parent = path.parent.stat()
    if (path.name != OUTPUT_NAME or not stat.S_ISDIR(parent.st_mode) or
        parent.st_uid != os.getuid() or parent.st_mode & 0o077):
        raise ValueError('Unsafe private state output')
    value = copy.deepcopy(state); value['binding'] = binding
    raw = encode_state(value)
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    temporary = '.virtual-peer-'+secrets.token_hex(8)+'.tmp'
    fd = None
    try:
        # A pending output belongs to this runner only; never follow a link.
        try:
            st = os.stat(path.name, dir_fd=directory, follow_symlinks=False)
            if not stat.S_ISREG(st.st_mode) or st.st_nlink != 1 or st.st_uid != os.getuid() or stat.S_IMODE(st.st_mode) != 0o600:
                raise ValueError('Unsafe existing state output')
        except FileNotFoundError: pass
        fd = os.open(temporary,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600,dir_fd=directory)
        with os.fdopen(fd,'wb') as stream:
            fd = None; stream.write(raw); stream.flush(); os.fsync(stream.fileno())
        os.replace(temporary,path.name,src_dir_fd=directory,dst_dir_fd=directory)
        os.fsync(directory)
    finally:
        if fd is not None: os.close(fd)
        try: os.unlink(temporary,dir_fd=directory)
        except FileNotFoundError: pass
        os.close(directory)
    return hashlib.sha256(raw).hexdigest()


def psk_bytes(state):
    """Preserve the schema's hex storage while selecting the guest byte encoding."""
    validate_state(state)
    descriptor = descriptor_for(state['profileID'], state['apSHA256'])
    return state['psk'].encode('ascii') if descriptor['pskEncoding'] == 'ascii-hex32' else bytes.fromhex(state['psk'])


def boot_properties(state):
    """Identity properties a guest property daemon reads from /etc/build.prop.

    P65 never mounts the Q65/Q66 factory LittleFS: its kvdbd sources only
    /tmp/factory.prop and the AP ROMFS /etc/build.prop, and nothing authors
    the former in this machine. Without these keys the firmware reports an
    erased 20-byte PSN and a negative DID inside BindInfo, so the synthetic
    identity can never match. Only plain identity text is delivered here; the
    binding key is established by the authentication exchange itself.
    """
    validate_state(state)
    identity = state['identity']
    return {'ro.factory.psn': identity['serial'], 'ro.factory.did': identity['did']}


def _fresh(state):
    validate_state(state)
    if state['binding'] is not None:
        raise ValueError('A bound identity cannot initialize empty storage')
    return descriptor_for(state['profileID'], state['apSHA256'])


def _fixed_text(image, offset, capacity, value):
    raw = value.encode('ascii')
    if not raw or len(raw) > capacity or any(c < 33 or c > 126 for c in raw):
        raise ValueError('Synthetic factory text exceeds its exact field')
    image[offset:offset+capacity] = raw + bytes(capacity-len(raw))


def _legacy_crc(data):
    # Original loop: init0 / reflected IEEE polynomial / no final XOR.
    return binascii.crc32(data, 0xffffffff) ^ 0xffffffff


def factory_image(state, *, skip_oobe=True):
    """Author only a fresh synthetic partition; never overlay an accepted one.

    Legacy app records deliberately do not forge the incompatible BES radio
    manufacturing header. The real guest decides whether that record is needed.
    N67 identity is in config_image, not its separate tail factory partition.
    """
    descriptor = _fresh(state)
    identity = state['identity']
    if descriptor['factoryFormat'] == 'n67-config-journal':
        return b'\xff' * descriptor['factoryBytes']
    if descriptor['factoryFormat'] == 'legacy-fixed-8k':
        image = bytearray(b'\xff' * descriptor['factoryBytes'])
        image[:0x50c] = bytes(0x50c)
        struct.pack_into('<H', image, 0, 0x9a55)
        image[0x8a:0x90] = bytes.fromhex(identity['address'].replace(':', ''))
        _fixed_text(image, 0x92, descriptor['didDigits'], identity['did'])
        _fixed_text(image, 0x9e, 16, psk_bytes(state).decode('ascii'))
        image[0xe2] = descriptor['boardID']
        _fixed_text(image, 0x14f, 24, identity['serial'])
        struct.pack_into('<I', image, 4, _legacy_crc(image[12:0x50b]))
        return bytes(image)
    if descriptor['factoryFormat'] != 'littlefs-8192x16':
        raise ValueError('Unsupported synthetic factory format')
    from ..factory import littlefs_class
    fs = littlefs_class()(block_size=8192,block_count=16,read_size=2048,
        prog_size=2048,cache_size=2048,lookahead_size=8,name_max=96)
    firmware_model = 'miwear.watch.p67cn' if descriptor['family'] == 'p67' else identity['model']
    properties = {'persist.screen.auto_screen_timeout':'0','persist.screen.dimmer_timeout':'2147483647',
        'ro.factory.mac_bt':identity['address'],'ro.factory.psn':identity['serial'],
        'ro.factory.did':identity['did'],descriptor['pskProperty']:psk_bytes(state).decode('ascii'),
        'product.model':firmware_model,'ro.factory.model':firmware_model}
    if skip_oobe:
        properties.update({'persist.setup_wizard':'1','persist.locale':'zh_CN'})
        if descriptor['family'] == 'q65':
            properties.update({'persist.system_novice_guidance':'false',
                               'persist.settings_novice':'true'})
    with fs.open(descriptor['factoryPropertyFile'],'w') as stream:
        stream.write(''.join(f'{key}={value}\n' for key,value in properties.items()))
    fs.unmount()
    image = bytes(fs.context.buffer)
    if len(image) != descriptor['factoryBytes']:
        raise ValueError('Synthetic factory geometry mismatch')
    return image


def config_image(state):
    """N67's exact pre-AP compact config extent; other families have none.

    One authored 1024-byte record, remaining journal slots erased. Unknown
    calibration/markey/SKU fields stay neutral zero, never copied from ROM or a
    device. Their guest acceptance is a runtime result, not implied here.
    """
    descriptor = _fresh(state)
    if not descriptor['configBytes']:
        return None
    if descriptor['factoryFormat'] != 'n67-config-journal':
        raise ValueError('Unsupported synthetic config format')
    image = bytearray(b'\xff' * descriptor['configBytes'])
    record = bytearray(1024); identity = state['identity']
    record[0] = record[0xc0] = 0xa5
    _fixed_text(record, 0x39, 12, identity['address'].replace(':', ''))
    _fixed_text(record, 0x69, 21, identity['serial'])
    _fixed_text(record, 0xc1, 16, identity['did'])
    _fixed_text(record, 0xd2, 16, psk_bytes(state).decode('ascii'))
    struct.pack_into('<H', record, 0xfe, binascii.crc_hqx(record[0xc0:0xfe], 0))
    record[0x3fe] = binascii.crc_hqx(record[:0x3fe], 0) & 255
    record[0x3ff] = 0
    image[:1024] = record
    return bytes(image)
