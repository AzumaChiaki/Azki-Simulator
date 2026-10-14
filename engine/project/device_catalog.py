"""Discover locally available full firmware, keeping model/version/hash identity.

Discovery and image preparation do not assert emulation compatibility. Partition
images are taken from the selected OTA, never borrowed from another device.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import zipfile

from firmware import DEFAULT_AP
from firmware_resources import checked_member, n67_system_members

ROOT = Path(__file__).resolve().parent
WORKSPACE = ROOT.parents[1]
CATALOG = ROOT/'profiles/catalog.json'
CACHE = ROOT/'assets/devices'
LOCAL_OTA_SOURCES = ROOT/'profiles/local-ota-sources.json'


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream,'sha256').hexdigest()


def zip_member_records(source):
    """Index a standalone OTA ZIP without extracting paths onto the host.

    Offsets and exact hashes are needed by the legacy multi-resource adapter;
    recording only filenames and lengths is not sufficient for that format.
    """
    records=[]
    with Path(source).open('rb') as stream, zipfile.ZipFile(source) as archive:
        names=set()
        for index, info in enumerate(archive.infolist()):
            if info.is_dir(): continue
            if info.filename in names:
                raise ValueError('Duplicate member in standalone OTA ZIP')
            names.add(info.filename)
            stream.seek(info.header_offset)
            header=stream.read(30)
            if len(header)!=30 or header[:4]!=b'PK\x03\x04':
                raise ValueError('OTA local ZIP header is missing')
            name_len,extra_len=struct.unpack_from('<HH',header,26)
            with archive.open(info) as payload:
                sha=hashlib.file_digest(payload,'sha256').hexdigest()
            records.append(dict(index=index,name=info.filename,
                header_offset=info.header_offset,
                data_offset=info.header_offset+30+name_len+extra_len,
                compressed_size=info.compress_size,size=info.file_size,
                compression_method=info.compress_type,crc32=f'{info.CRC:08x}',sha256=sha))
    return records


def inspect_ap(path):
    data = Path(path).read_bytes()
    info = dict(sha256=hashlib.sha256(data).hexdigest(), size=len(data))
    match = re.search(rb'(?<![A-Z_])CHIP=([^\r\n\x00 ]+)', data)
    if not match:
        info.update(chip='unidentified', image_format='vector-table-candidate',
                    vector_sp=struct.unpack_from('<I',data,0)[0],
                    vector_reset=struct.unpack_from('<I',data,4)[0],
                    backend_requirement='Identify SoC and its 0x60… execution/memory map')
        return info
    info['chip'] = match[1].decode('ascii')
    block = data[match.start():match.start()+900].split(b'\x00',1)[0]
    build = dict((k.decode(),v.decode(errors='replace')) for k,v in
                 re.findall(rb'(?:^|\n)([A-Z_0-9]+)=([^\r\n]+)',block))
    info['build'] = build
    info['image_format'] = 'bes-header'
    info['ap_offset'] = int(build['OTA_CODE_OFFSET'],0)
    info['flash_bytes'] = int(build['FLASH_SIZE'],0)
    # Header word 3 points to the newline immediately before CHIP. This
    # independently recovers the image's data alias without guessing a board.
    info['data_alias'] = struct.unpack_from('<I',data,12)[0] - (match.start()-1)
    first = struct.unpack_from('<H',data,0x10)[0]
    if first & 0xf800 != 0x4800:
        raise ValueError('Unsupported BES reset prologue')
    literal = 0x14 + (first & 0xff)*4
    value = struct.unpack_from('<I',data,literal)[0]
    if data[0x12:0x14] == b'\x00\x47':
        info['reset_entry'] = value-4
        # Find LDR r0 / MSR MSP rather than assuming one literal-pool offset.
        candidates=[]
        for off in range(0x14,0x30,2):
            ins=struct.unpack_from('<H',data,off)[0]
            if ins & 0xff00 == 0x4800 and data[off+2:off+6] == b'\x80\xf3\x08\x88':
                at=((off+4)&~3)+(ins&0xff)*4
                candidates.append(struct.unpack_from('<I',data,at)[0])
        if len(candidates)!=1:
            raise ValueError('Ambiguous initial MSP')
        info['reset_sp']=candidates[0]
    else:
        info['reset_sp']=value
        info['reset_entry']=info['data_alias']+0x11
    info['backend_requirement'] = {
        'best1503':'BES1503 peripheral and product storage/panel profile',
        'best1600':'BES1600 M55 board, storage and graphics model',
        'best1700':'BES1700 board and its different memory/peripheral map',
        'best1502x':'BES1502X board and its different SRAM/peripherals',
        'best1502p':'BES1502P board and its different SRAM/peripherals',
    }.get(info['chip'],'Board model not implemented')
    return info


def discover():
    archive=WORKSPACE/'firmware/archive'
    manifests=[(archive,json.loads((archive/'manifest.json').read_text()))]
    official=WORKSPACE/'firmware/official'
    manifests.append((official,json.loads((official/'manifest.json').read_text())))
    if LOCAL_OTA_SOURCES.exists():
        manifests.append((LOCAL_OTA_SOURCES.parent,json.loads(LOCAL_OTA_SOURCES.read_text())))
    rows=[]; missing=[]; seen=set(); inspected={}
    for base,manifest in manifests:
        for asset in manifest['assets']:
            if asset['kind']!='full': continue
            sha=asset['sha256']
            source=(base/asset['path']).resolve() if 'path' in asset else base/'objects'/f'{sha}.bin'
            key=(asset['model'],asset['version'],sha)
            if key in seen: continue
            seen.add(key)
            if not source.is_file():
                missing.append(dict(model=asset['model'],version=asset['version'],sha256=sha))
                continue
            research=WORKSPACE/'firmware/research/by-device'/asset['model']/asset['version']
            ap=WORKSPACE/'firmware/ap-images/by-device'/asset['model']/asset['version']/'vela_ap.bin'
            partitions=[]
            pm=research/'partitions/manifest.json'
            if pm.exists() and json.loads(pm.read_text())['source_ota_sha256']==sha:
                partitions=json.loads(pm.read_text())['payloads']
            if not ap.exists() or not partitions:
                # Additional local official full OTA, not yet in research index.
                with zipfile.ZipFile(source) as z:
                    ap_data=z.read('vela_ap.bin');ap_sha=hashlib.sha256(ap_data).hexdigest()
                    ap=CACHE/'ap'/f'{ap_sha}.bin';ap.parent.mkdir(parents=True,exist_ok=True)
                    if ap.exists() and digest(ap)!=ap_sha:
                        raise ValueError(f'Cached AP integrity mismatch: {ap}')
                    if not ap.exists(): ap.write_bytes(ap_data)
                    partitions=zip_member_records(source)
            resolved=str(ap.resolve())
            if resolved not in inspected: inspected[resolved]=inspect_ap(ap)
            metadata=inspected[resolved]
            known_ap=next((x.get('sha256') for x in partitions if x['name']=='vela_ap.bin'),None)
            if known_ap and known_ap!=metadata['sha256']:
                raise ValueError(f'AP differs from selected OTA: {ap}')
            rows.append(dict(id=f"{asset['model']}@{asset['version']}@{sha[:12]}",
                model=asset['model'],version=asset['version'],ota_path=str(source.resolve()),
                ota_sha256=sha,ap_path=str(ap.resolve()),ap=metadata,partitions=partitions,
                resource_path=None,status='unverified',desktop_verified=False))
    for version,ap,resource in [
        ('4.100.177',DEFAULT_AP,ROOT/'assets/4.100.177/vela_resource.bin'),
        ('4.100.071',ROOT/'assets/q66-071/vela_ap.bin',ROOT/'assets/q66-071/vela_resource.bin')]:
        if not ap.is_file() or not resource.is_file(): continue
        metadata=inspect_ap(ap);rsha=digest(resource)
        rows.append(dict(id=f'q66@{version}',model='q66',version=version,
            ota_path=None,ota_sha256=None,ap_path=str(ap.resolve()),ap=metadata,
            partitions=[dict(name='vela_resource.bin',size=resource.stat().st_size,sha256=rsha)],
            resource_path=str(resource.resolve()),status='unverified',desktop_verified=False))
    catalog=dict(schema_version=1,profiles=sorted(rows,key=lambda x:(x['model'],x['version'])),
        absent_archive_entries=missing,available_model_count=len({r['model'] for r in rows}),
        available_profile_count=len(rows),unique_ap_count=len({r['ap']['sha256'] for r in rows}))
    CATALOG.parent.mkdir(exist_ok=True)
    CATALOG.write_text(json.dumps(catalog,indent=2)+'\n')
    return catalog


def load_catalog():
    return json.loads(CATALOG.read_text()) if CATALOG.exists() else discover()


def select(identity):
    profiles=load_catalog()['profiles']
    matches=[p for p in profiles if p['id']==identity or f"{p['model']}@{p['version']}"==identity]
    if len(matches)!=1:
        raise ValueError(f'Expected one exact device/version profile, got {len(matches)} for {identity}')
    return matches[0]


def prepare_resource(profile):
    if Path(profile['ap_path']).stat().st_size != profile['ap']['size']:
        raise ValueError('AP size differs from selected metadata')
    members=[p for p in profile['partitions'] if p['name']=='vela_resource.bin']
    if len(members)!=1:
        raise ValueError('This firmware uses separate resource partitions; its storage layout must be modeled')
    record=members[0]
    expected_sha=record.get('sha256')
    if profile['resource_path']:
        target=Path(profile['resource_path'])
    else:
        source=Path(profile['ota_path'])
        with zipfile.ZipFile(source) as archive:
            ap_members=[i for i in archive.infolist() if i.filename=='vela_ap.bin']
            if len(ap_members)!=1:
                raise ValueError('Selected full OTA must have exactly one AP member')
            if ap_members[0].file_size != profile['ap']['size']:
                raise ValueError('AP member size differs from selected metadata')
            infos=[i for i in archive.infolist() if i.filename==record['name']]
            if len(infos)!=1 or infos[0].file_size!=record['size']:
                raise ValueError('Ambiguous resource member or size mismatch')
        target=CACHE/profile['ota_sha256']/'vela_resource.bin'
        if not target.exists():
            target.parent.mkdir(parents=True,exist_ok=True)
            with zipfile.ZipFile(source) as z, z.open(record['name']) as stream:
                fd,temp_name=tempfile.mkstemp(prefix='resource-',suffix='.part',dir=target.parent)
                os.close(fd)
                temp=Path(temp_name)
                try:
                    with temp.open('wb') as out:
                        while block:=stream.read(4*1024*1024): out.write(block)
                    if temp.stat().st_size != record['size']:
                        raise ValueError('Extracted resource integrity mismatch')
                    temp.replace(target)
                finally:
                    temp.unlink(missing_ok=True)
    if target.stat().st_size != record['size']:
        raise ValueError('Resource size mismatch')
    # ZIP extraction already checked its format CRC. A startup digest is not
    # required for the read-only ROMFS and must not pretend to be measured.
    return target,None


def _nand_system_size(ap_data):
    """Read BES MTD records, allowing repeated identical product-variant tables."""
    matches=set();start=0
    name=b'nand_system'+bytes(93)
    while (at:=ap_data.find(name,start))>=0:
        start=at+1
        if at%4 or at+232>len(ap_data): continue
        first,count,flags=struct.unpack_from('<III',ap_data,at+104)
        next_first=struct.unpack_from('<I',ap_data,at+220)[0]
        if ap_data[at+116:at+220]!=b'nand_data'+bytes(95):continue
        size=count*2048
        if first==0 and flags==0 and next_first==count and 8*1024*1024<=size<=512*1024*1024 and size%131072==0:
            matches.add(size)
    if len(matches)!=1:
        raise ValueError('Ambiguous or missing original nand_system geometry')
    return matches.pop()


def _verify_ota_ap(profile, source):
    records=[r for r in profile['partitions'] if r['name']=='vela_ap.bin']
    if len(records)!=1 or records[0]['size']!=profile['ap']['size']:
        raise ValueError('AP does not belong to selected full OTA')
    checked_member(source,records[0], verify_sha=False)


def _storage_members(profile, source):
    if profile['model'].startswith('miwear.watch.n67'):
        return n67_system_members(source,profile['partitions'])
    named={}
    for record in profile['partitions']:
        if record['name'] in named:raise ValueError('Duplicate legacy OTA member name')
        named[record['name']]=record
    spec=json.loads(checked_member(source,named['ota.json'], verify_sha=False))
    records=[];seen=set()
    for section in spec['sections']:
        if section['section_type']!='main_res':continue
        name=section['location_path']
        if Path(name).name!=name or name in ('.','..') or name in seen:
            raise ValueError('Unexpected or duplicate resource path')
        seen.add(name)
        records.append(dict(named[name],target_path=name))
    if not records:raise ValueError('OTA has no main resources')
    return records


def prepare_storage(profile):
    """Materialize the storage type expected by the selected original rc.sysinit.

    The cache is bound to the OTA, AP, path mapping, geometry and builder. Its
    image digest is rechecked on every reuse; an absent or partial proof never
    makes an image usable. Build in a private directory before atomic publish.
    """
    if any(p['name']=='vela_resource.bin' for p in profile['partitions']):
        path,sha=prepare_resource(profile)
        return path,sha,'romfs'
    if not profile['model'].startswith(('miwear.watch.n66','miwear.watch.o66','miwear.watch.n67')):
        raise ValueError('Resource layout adapter is not implemented for this product')
    ap_data=Path(profile['ap_path']).read_bytes()
    if b'mount -t yaffs -o ro /dev/nand_system /system' not in ap_data:
        raise ValueError('Original startup does not confirm expected YAFFS system mount')
    size=_nand_system_size(ap_data)
    source=Path(profile['ota_path']).read_bytes()
    _verify_ota_ap(profile,source)
    records=_storage_members(profile,source)
    helper=ROOT/'backend/build/yaffs-image'
    expected=dict(schema_version=2,ota_sha256=profile['ota_sha256'],ap_sha256=profile['ap']['sha256'],
        builder_sha256=digest(helper),filesystem='YAFFS2 inband tags',image_bytes=size,
        page_bytes=2048,erase_bytes=131072,original_members=records,remount_content_verified=True)
    directory=CACHE/profile['ota_sha256'];directory.mkdir(parents=True,exist_ok=True)
    target=directory/'nand-system.bin';proof=directory/'nand-system.json'
    if proof.exists():
        info=json.loads(proof.read_text())
        if info.get('schema_version')==2:
            if any(info.get(k)!=v for k,v in expected.items()):
                # A changed builder or AP profile requires a fresh proof.
                if info.get('ota_sha256')!=profile['ota_sha256']:
                    raise ValueError('Cached NAND image source mismatch')
            else:
                if not target.is_file() or target.stat().st_size!=size or digest(target)!=info.get('sha256'):
                    raise ValueError('Cached NAND image integrity mismatch')
                return target,info['sha256'],'yaffs'
    with tempfile.TemporaryDirectory(prefix='build-',dir=directory) as temporary:
        temporary=Path(temporary);root=temporary/'system';root.mkdir()
        for record in records:
            data=checked_member(source,record, verify_sha=False)
            path=root/record['target_path'];path.parent.mkdir(parents=True,exist_ok=True)
            path.write_bytes(data)
        image=temporary/'nand-system.bin'
        subprocess.run([str(helper),str(image),'--bytes',str(size),'--tree',str(root)],check=True)
        if image.stat().st_size!=size:raise ValueError('NAND builder image size mismatch')
        sha=digest(image)
        info=dict(expected,sha256=sha)
        new_proof=temporary/'nand-system.json';new_proof.write_text(json.dumps(info,indent=2)+'\n')
        image.replace(target);new_proof.replace(proof)
    return target,sha,'yaffs'


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--refresh',action='store_true')
    args=parser.parse_args()
    c=discover() if args.refresh else load_catalog()
    print(json.dumps({k:v for k,v in c.items() if k not in ('profiles','absent_archive_entries')},indent=2))
    for p in c['profiles']:
        print(p['id'],p['ap']['chip'],p['ap']['sha256'][:12])
