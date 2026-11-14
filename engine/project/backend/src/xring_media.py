"""Build the O62M virtual eMMC from the selected full OTA and local preferences."""
from pathlib import Path
import json
import os
import shutil
import struct
import subprocess
import uuid
import zipfile
import zlib

EMMC_BYTES = 1 << 32
PROFILE_FILE = Path(__file__).with_name('xring_board.json')
ROMFS = ('system', 'app', 'i18n', 'font', 'watchface', 'quickapp', 'misc', 'vendor', 'xr_vendor')
MEMBERS = {label: f'vela_{label}.bin' for label in ROMFS}
MEMBERS.update({label: f'c00/vela_{label}.bin' for label in ('dl_nv_d', 'dl_nv_d_bak', 'mg_nv', 'mg_nv_1')})
MEMBERS.update(CP_boot='vela_CP_boot.bin', CPbootf='vela_CP_boot.bin',
               CP='vela_CP.bin', CP_f='vela_CP.bin', CRF='vela_CRF.bin', CRF_f='vela_CRF.bin')
BOOT_MEMBERS = ('vela_ap.bin', 'vela_tee.bin', 'vela_mmap.bin')


def board_profile(profile):
    from device_catalog import inspect_ap
    ap = inspect_ap(profile['ap_path'])
    if profile.get('model') != 'miwear.watch.o62m' or ap.get('chip') != 'xring':
        raise ValueError('XRing requires an O62M AP and its complete OTA')
    if profile.get('version') != '3.1.115':
        raise ValueError('XRing runtime currently supports O62M 3.1.115')
    return {'profile_id': profile['id'], 'chip': 'xring', 'machine': 'azki-marconi-probe',
            'panel_width': 466, 'panel_height': 466, 'emmc_bytes': EMMC_BYTES,
            'local_setup_bypass': True, 'ap_sha256': profile['ap']['sha256']}


def extract_boot(profile, directory):
    """Extract only selected-archive boot images; ZIP validates each member CRC."""
    directory = Path(directory)
    images = {}
    with zipfile.ZipFile(profile['ota_path']) as archive:
        names = archive.namelist()
        for name in set(MEMBERS.values()) | set(BOOT_MEMBERS):
            if names.count(name) != 1:
                raise ValueError('XRing full OTA requires exactly one ' + name)
        for name in BOOT_MEMBERS:
            record = archive.getinfo(name)
            if not 0 < record.file_size <= 32 * 1024**2:
                raise ValueError('Invalid XRing boot image size: ' + name)
            target = directory / name
            with archive.open(record) as source, target.open('xb') as output:
                shutil.copyfileobj(source, output, 1 << 20)
            target.chmod(0o600)
            images[name] = target
    # Boot the imported AP bytes without patching or substituting another image.
    with images['vela_ap.bin'].open('rb') as left, Path(profile['ap_path']).open('rb') as right:
        while block := left.read(1 << 20):
            if block != right.read(len(block)):
                raise ValueError('Selected AP differs from the selected OTA')
        if right.read(1):
            raise ValueError('Selected AP size differs from the selected OTA')
    return images


def _fat16(output, partition, label, files):
    """Format a 64-MiB FAT16 volume with standard 8.3/LFN file entries."""
    total = partition['sectors']
    spc, reserved, fats, roots = 4, 1, 2, 512
    root_sectors = roots * 32 // 512
    fat_sectors = 1
    while True:
        clusters = (total - reserved - fats * fat_sectors - root_sectors) // spc
        needed = ((clusters + 2) * 2 + 511) // 512
        if needed <= fat_sectors:
            break
        fat_sectors = needed
    if not 4085 <= clusters < 65525:
        raise ValueError('XRing volume does not fit FAT16')
    boot = bytearray(512)
    boot[:11] = b'\xeb\x3c\x90MSDOS5.0'
    struct.pack_into('<HBHBHHBHHHII', boot, 11, 512, spc, reserved, fats, roots,
                     total if total < 65536 else 0, 0xf8, fat_sectors, 63, 255,
                     partition['first_lba'], total if total >= 65536 else 0)
    boot[36], boot[38] = 0x80, 0x29
    struct.pack_into('<I', boot, 39, 0x5852494e)
    boot[43:54] = label.encode('ascii').ljust(11, b' ')
    boot[54:62] = b'FAT16   '
    boot[510:] = b'\x55\xaa'
    fat = bytearray(fat_sectors * 512)
    fat[:4] = b'\xf8\xff\xff\xff'
    directory = bytearray(root_sectors * 512)
    directory[:11], directory[11] = label.encode().ljust(11, b' '), 8
    next_cluster, slot = 2, 1
    data_start = (reserved + fats * fat_sectors + root_sectors) * 512
    origin = partition['first_lba'] * 512
    for name, short, content in files:
        checksum = 0
        for byte in short:
            checksum = (((checksum & 1) << 7) + (checksum >> 1) + byte) & 255
        units = name.encode('utf-16le')
        if len(units) < 26:
            units = (units + b'\0\0').ljust(26, b'\xff')
        if len(units) != 26 or len(short) != 11:
            raise ValueError('Unsupported initial FAT filename')
        lfn = bytearray(32)
        lfn[0], lfn[11], lfn[13] = 0x41, 0x0f, checksum
        for index, position in enumerate((1,3,5,7,9,14,16,18,20,22,24,28,30)):
            lfn[position:position+2] = units[index*2:index*2+2]
        entry = bytearray(32)
        entry[:11], entry[11] = short, 0x20
        for offset in (16,18,24):
            struct.pack_into('<H', entry, offset, 0x21)
        struct.pack_into('<HI', entry, 26, next_cluster, len(content))
        directory[slot*32:(slot+2)*32] = lfn + entry
        count = (len(content) + spc*512-1) // (spc*512)
        if next_cluster + count > clusters + 2:
            raise ValueError('Initial FAT content exceeds volume')
        for index in range(count):
            struct.pack_into('<H', fat, (next_cluster+index)*2,
                             0xffff if index+1 == count else next_cluster+index+1)
        output.seek(origin + data_start + (next_cluster-2)*spc*512)
        output.write(content)
        next_cluster += count
        slot += 2
    output.seek(origin)
    output.write(boot)
    for index in range(fats):
        output.seek(origin + (reserved + index*fat_sectors)*512)
        output.write(fat)
    output.seek(origin + (reserved + fats*fat_sectors)*512)
    output.write(directory)


def initialize_media(profile, path, directory, *, skip_oobe):
    path, directory = Path(path), Path(directory)
    config = json.loads(PROFILE_FILE.read_text())
    parts = config['partitions']
    preferences = []
    if skip_oobe:
        database = directory / 'persist.db'
        helper = Path(__file__).parents[1] / 'build/xring-ui-db'
        subprocess.run([str(helper), str(database)], check=True, timeout=30,
                       stdout=subprocess.DEVNULL)
        preferences = [('persist.db', b'PERSIST DB ', database.read_bytes())]
    # The caller owns this fresh transaction. Never reseed an existing instance.
    with path.open('r+b' if path.exists() else 'x+b') as output, zipfile.ZipFile(profile['ota_path']) as archive:
        if path.stat().st_size not in (0, EMMC_BYTES):
            raise ValueError('XRing instance eMMC must be 4 GiB')
        output.truncate(0)
        output.truncate(EMMC_BYTES)
        entries = bytearray(128 * 128)
        disk_guid = uuid.uuid4().bytes_le
        for index, part in enumerate(parts):
            first, sectors = part['first_lba'], part['sectors']
            struct.pack_into('<16s16sQQQ72s', entries, index*128,
                uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4').bytes_le,
                uuid.uuid4().bytes_le, first, first+sectors-1, 0,
                part['label'].encode('utf-16le'))
            member = MEMBERS.get(part['label'])
            if member is not None:
                info = archive.getinfo(member)
                if not 0 < info.file_size <= sectors * 512:
                    raise ValueError('XRing member exceeds partition: ' + member)
                if part['label'] in ('CP', 'CP_f', 'CRF', 'CRF_f') and info.file_size != sectors*512:
                    raise ValueError('Signed image footer must remain at partition end: ' + member)
                output.seek(first*512)
                with archive.open(info) as source:
                    shutil.copyfileobj(source, output, 1 << 20)
        last = EMMC_BYTES // 512 - 1
        crc = zlib.crc32(entries)
        for current, other, array in ((1,last,2),(last,1,last-32)):
            header = bytearray(512)
            struct.pack_into('<8sIIIIQQQQ16sQIII', header, 0, b'EFI PART',0x10000,92,0,0,
                             current,other,34,last-33,disk_guid,array,128,128,crc)
            struct.pack_into('<I', header, 16, zlib.crc32(header[:92]))
            output.seek(current*512)
            output.write(header)
            output.seek(array*512)
            output.write(entries)
        mbr = bytearray(512)
        struct.pack_into('<B3sB3sII', mbr, 446, 0,b'\0\x02\0',0xee,b'\xff'*3,1,last)
        mbr[510:] = b'\x55\xaa'
        output.seek(0)
        output.write(mbr)
        by_name = {part['label']:part for part in parts}
        _fat16(output, by_name['ppt'], 'PPT', [('boot_flag.txt', b'BOOT_F~1TXT', b'resetcause_flag=1\n')])
        _fat16(output, by_name['user_data'], 'USERDATA', preferences)
        output.flush()
        os.fsync(output.fileno())
    path.chmod(0o600)


def machine_options(images, media, directory, frame, *, writeback):
    config = json.loads(PROFILE_FILE.read_text())
    properties = config['properties'].copy()
    for key, spec in config['public_inputs'].items():
        target = Path(directory) / (key + '.bin')
        target.write_bytes(bytes(spec['zero_bytes']) if 'zero_bytes' in spec else bytes.fromhex(spec['public_hex']))
        target.chmod(0o600)
        properties[key] = str(target)
    properties.update({'mmap-image':str(images['vela_mmap.bin']),
                       'tee-ap-image':str(images['vela_ap.bin']),
                       'normal-emmc-media':str(media), 'lcd-frame-file':str(frame)})
    if writeback:
        properties['normal-emmc-writeback'] = str(media)
    return config['machine'] + ''.join(','+key+'='+str(value).replace(',',',,') for key,value in properties.items())
