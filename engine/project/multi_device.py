"""Native stock-firmware launcher with explicit device-specific profiles."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

from device_catalog import ROOT, digest, load_catalog, prepare_storage, select

BACKEND = ROOT/'backend'


def partition_records(data):
    """Recover 116-byte MTD descriptors, retaining offsets as evidence.

    Units are deliberately not inferred here: NOR descriptors use 512-byte
    sectors and NAND descriptors use 2048-byte pages on these BES1503 images.
    """
    records=[]
    for match in re.finditer(rb'(?<![A-Za-z_])[A-Za-z_][A-Za-z_0-9]{1,31}\x00',data):
        at=match.start(); name=match.group()[:-1]
        if at % 4 or at+116>len(data) or data[at+len(name):at+104]!=bytes(104-len(name)):
            continue
        start,count,flags=struct.unpack_from('<3I',data,at+104)
        if count and start<0x1000000 and count<0x1000000 and flags==0:
            records.append(dict(name=name.decode(),start=start,count=count,file_offset=at))
    return records


def _attach_nor_geometry(board, profile, ap_data, factory_bytes):
    """Attach only an exact AP-bound storage description, not runtime support."""
    from backend.src.instance_data import validate_nor_geometry
    geometry = board['geometry']
    # Imported p67 OTAs retain the same AP and writable-NOR layout but carry a
    # generated profile ID (for example @0@<sha>).  Restrict this derivation to
    # the verified p67 AP identity while accepting those imported IDs; matching
    # only catalog IDs silently placed imported instances' nv records at the
    # factory boundary and the firmware disconnected before BindInfo.
    if (profile['model'] in ('miwear.watch.p67cn', 'miwear.watch.p67tc')
            and profile['ap']['sha256'] == '519307675665e4866d722a8119a98589c397b614ac3294cb87bfc86de45756ec'):
        # These two catalog profiles share the same AP. Its BES userdata
        # starts after the actual image, before the mode/nv MTD partitions.
        # A real NOR0 erase at 0xdea000 proves this area is guest-writable.
        # Keep every loaded AP byte protected and retain the existing tail.
        ap_end = geometry['ap-offset'] + len(ap_data)
        tail = (ap_end + 0x1fff) & ~0x1fff
        if not ap_end <= tail < geometry['nv-offset']:
            raise ValueError('P67 AP does not leave its measured NOR data area')
        geometry['nv-offset'] = tail
        board['evidence']['bes_userdata'] = {
            'apEnd': ap_end, 'eraseAlignment': 0x2000, 'offset': tail,
            'source': 'P67 .043 real NOR0 command 0x20 at 0xdea000; AP USER_SEC_SIZE=0x2000',
        }
    descriptor = dict(profileID=profile['id'], apSHA256=profile['ap']['sha256'],
        apBytes=len(ap_data), apOffset=geometry['ap-offset'], flashBytes=geometry['flash-bytes'],
        offset=geometry['nv-offset'], bytes=geometry['flash-bytes']-geometry['nv-offset'],
        factoryOffset=geometry['factory-offset'])
    if geometry.get('config-bytes'):
        # Compact NOR image: writable tail first, then the disjoint pre-AP
        # configuration extent. AP and the intervening gap are never included.
        descriptor.update(configOffset=geometry['config-offset'], configBytes=geometry['config-bytes'])
        descriptor['bytes'] += geometry['config-bytes']
    board['nor_geometry'] = descriptor
    board['factory_bytes'] = factory_bytes
    validate_nor_geometry(board, ap_sha256=profile['ap']['sha256'], ap_bytes=profile['ap']['size'])
    return board


def board_profile(profile, resource_bytes, storage_kind='romfs'):
    ap=profile['ap']
    if profile['model'] == 'miwear.watch.q65':
        from backend.src.q65_runner import board_profile as q65_board
        return q65_board(profile)
    if ap['chip']!='best1503':
        raise ValueError(ap['backend_requirement'])
    ap_data = Path(profile['ap_path']).read_bytes()
    if len(ap_data) != ap['size']:
        raise ValueError('Board profile AP byte count differs from selected metadata')
    records=partition_records(ap_data)
    factory=[r for r in records if r['name'] in ('facmode','mode') and r['count']==256]
    nv=[r for r in records if r['name']=='nv' and r['count']==256]
    writable=[r for r in records if r['name'] in ('nvflash0','nv','facmode','mode')]
    legacy=storage_kind=='yaffs' and profile['model'].startswith(('miwear.watch.n66','miwear.watch.o66'))
    if legacy:
        factory=[r for r in records if r['name']=='factory' and r['count']==16]
        nv=[r for r in records if r['name']=='fds' and r['count']==32]
        writable=[r for r in records if r['name'] in ('fds','bootinfo','factory')]
    # Select a resource/data pair whose complete partition map fits the modeled
    # 512 MiB NAND. A 256 MiB variant is not interchangeable with that chip ID.
    data=[r for r in records if r['name']=='nand_data' and
          r['start']*2048>=resource_bytes and (r['start']+r['count'])*2048<=512*1024**2]
    if storage_kind=='yaffs' and profile['model']=='miwear.watch.n67cn':
        config=[r for r in records if r['name']=='flash_config']
        if len(data)!=1 or ap['flash_bytes']!=8*1024**2:
            raise ValueError('Ambiguous N67 storage layout')
        if len(config)!=1 or (config[0]['start']+config[0]['count'])*512>ap['ap_offset']:
            raise ValueError('Invalid N67 configuration partition')
        # Header USER_SEC_SIZE and FACT_SEC_SIZE are each 8 KiB. The .187
        # nvrec_dev_data_open literals at file 0x49c094/180/258 identify the
        # factory sector at 0x287fe000. Keep the remainder of NOR read-only.
        reserved=int(ap['build']['USER_SEC_SIZE'],0)+int(ap['build']['FACT_SEC_SIZE'],0)
        board = dict(chip=ap['chip'],profile_id=profile['id'],ap_sha256=ap['sha256'],
            local_setup_bypass=False,
            geometry={'ap-offset':ap['ap_offset'],'reset-sp':ap['reset_sp'],
                'flash-bytes':ap['flash_bytes'],'factory-offset':ap['flash_bytes']-0x2000,
                'nv-offset':ap['flash_bytes']-reserved,'nand-data-offset':data[0]['start']*2048,
                'nand-data-bytes':data[0]['count']*2048,
                'config-offset':config[0]['start']*512,'config-bytes':config[0]['count']*512,
                'resource-yaffs':1,'pmu-calibration':1,
                'panel-power0':0,'panel-power1':(1<<14)|(1<<24)|(1<<27),
                'panel-power2':1<<3,'panel-id':0x270120,'panel-te-pin':47,
                'touch-i2c':5,'touch-power-pin':52,'touch-reset-pin':77,'touch-irq-pin':76,
                # .187 bespower_init opens up_i2cinitialize(3) (0x40061000) and
                # probes SY6103X, HP4560, SC7061 at 0x07; hp4560_initialize
                # (0x0c0c957a) accepts identity 0xc6 at register 0x60.
                # 0x48 on I2C5 is the ambient light sensor.
                'charger-i2c':0x0307,'charger-id':0xc6,'charger-map':1,
                # [lc ntc] shell/skin thermistors on PMU ADC 2/5 (0x0c0e2dd0,
                # 0x0c0e2e84); battery voltage is ADC 4 x 4.9 (0x0c0e2f00).
                # Their mV->C table (0x2c5e3e5c, index = C + 40) puts 25 C at
                # 250 mV, raw 5303 on the PMU calibration.
                'pmu-adc-ntc':(1<<2)|(1<<5),'pmu-adc-ntc-room':5303},
            evidence=dict(nand_data=data[0],config=config[0],nv='BES header USER_SEC_SIZE + FACT_SEC_SIZE',
                          touch='N67 .187 table at file 0x6777d4: IRQ76/reset77; power52 at 0x0c0caf84; runtime I2C5=0x40063000',
                          panel='N67 .187 power sequence 0x0c0caef0 and accepted ID in 0x0c0df760; TE47 requires runtime verification'))
        # The factory literal evidence above is specific to this AP. Other
        # N67 versions retain diagnostics but receive no persistence descriptor.
        if profile['id'] == 'miwear.watch.n67cn@3.1.187@6d33afe20089':
            return _attach_nor_geometry(board, profile, ap_data, int(ap['build']['FACT_SEC_SIZE'], 0))
        return board
    if len(factory)!=1 or len(nv)!=1 or not data:
        raise ValueError('Product needs a different factory/storage layout; not a Q66 geometry variant')
    # The persistent tail must contain exactly the writable records, without
    # absorbing unknown/read-only gaps into the transaction.
    end = min(r['start'] for r in writable) * 512
    for record in sorted(writable, key=lambda r:r['start']):
        if record['start'] * 512 != end:
            raise ValueError('NOR writable partition records overlap or leave an unsupported gap')
        end += record['count'] * 512
    if end != ap['flash_bytes']:
        raise ValueError('NOR writable records do not end at the original flash boundary')
    data=max(data,key=lambda r:r['start']+r['count'])
    board = dict(chip=ap['chip'],profile_id=profile['id'],ap_sha256=ap['sha256'],
        local_setup_bypass=not legacy,
        geometry={'ap-offset':ap['ap_offset'],'reset-sp':ap['reset_sp'],
                  'flash-bytes':ap['flash_bytes'],'factory-offset':factory[0]['start']*512,
                  'nv-offset':min(r['start'] for r in writable)*512,
                  'nand-data-offset':data['start']*2048,'nand-data-bytes':data['count']*2048},
        evidence=dict(factory=factory[0],nv=nv[0],writable=writable,nand_data=data))
    if legacy: board['geometry'].update({'resource-yaffs':1,'pmu-calibration':1})
    if profile['model'].startswith('miwear.watch.n66'):
        board['geometry']['touch-health-format']=1
        # get_bat_temp (.32 0x0c166a34) converts PMU ADC 3; battery is ADC 2.
        board['geometry']['pmu-adc-ntc']=1<<3
        board['evidence']['touch']='N66 .17/.32 esd_check 0x0c1586ec accepts exact D048 byte 0x20/0xa0'
    if profile['model'].startswith(('miwear.watch.o66', 'miwear.watch.q66')):
        # Charger on I2C0 at 0x07: hp4560x_checkid (O66 .28 0x0c39caf8)
        # accepts only identity 0xe0 at register 0x0a, else probes SC7061.
        board['geometry'].update({'charger-i2c':0x0007,'charger-id':0xe0})
        # Battery thermistor: O66 .28 get_bat_temp (0x0c11a8a4) reads ADC 3;
        # Q66 stops charging as too cold unless ADC 5 is warm.
        board['geometry']['pmu-adc-ntc']=1<<3 if profile['model'].startswith('miwear.watch.o66') else 1<<5
    if profile['model'].startswith('miwear.watch.o66'):
        # .28 read_chip_id at 0x0c10796e accepts module 0x03660014 or 0x03660012.
        # This is a virtual supported module identity, not a physical measurement.
        board['geometry']['touch-module-id']=0x03660014
        board['evidence']['touch']='O66 .28 read_chip_id 0x0c10796e; module literal at file 0x679b0'
    if profile['model'].startswith('miwear.watch.p67'):
        # Original property service at file 0xb4e194 reads /mode/app_nv.prop.
        board['local_property_file']='app_nv.prop'
        # P67 .043 original power sequence at 0x0c3dd0a8 uses GPIO
        # 46 (reset), 67, 56, 59. Panel dispatch at 0x0c3e2108 accepts 27:01:20.
        board['geometry'].update({'panel-power0':0,'panel-power1':(1<<14)|(1<<24)|(1<<27),
                                  'panel-power2':1<<3,'panel-id':0x270120,'panel-te-pin':47,
                                  'touch-i2c':5,'touch-power-pin':52,'touch-reset-pin':78,'touch-irq-pin':76,
                                  # .043 hp4570_initialize (0x0c4044d4): I2C3
                                  # 0x07, identity 0xc6 at register 0x60.
                                  'charger-i2c':0x0307,'charger-id':0xc6,'charger-map':1,
                                  # [lc ntc] shell/skin thermistors as on N67
                                  'pmu-adc-ntc':(1<<2)|(1<<5),'pmu-adc-ntc-room':5303})
        board['evidence']['touch']='P67 .043 Hynitron setup 0x0c3f87e4; file 0xc460a0 config; GPIO IRQ 76/reset 78/power 52'
        board['evidence']['panel']='P67 .043 power sequence 0x0c3dd0a8 and panel dispatch 0x0c3e2108; other P67 versions require runtime verification'
    return _attach_nor_geometry(board, profile, ap_data, factory[0]['count'] * 512)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device',default='q66@4.100.177',help='Exact model@version or catalog ID')
    parser.add_argument('--list',action='store_true')
    parser.add_argument('--nor-layout',action='store_true',
        help='Print the AP-derived persistent NOR layout of --device and exit')
    parser.add_argument('--storage-kind',choices=('romfs','yaffs'),default='romfs')
    parser.add_argument('--headless',action='store_true',help='Diagnostics only; no Web server')
    parser.add_argument('--seconds',type=float)
    parser.add_argument('--trace',action='store_true',help='Save instruction/exception diagnostics')
    parser.add_argument('--verbose-logs',action='store_true',help='Enable verbose QEMU guest error/HCI diagnostics')
    parser.add_argument('--log-dir',type=Path)
    parser.add_argument('--instance-data-image',type=Path)
    parser.add_argument('--instance-spare-image',type=Path)
    parser.add_argument('--instance-data-sha256')
    parser.add_argument('--instance-spare-sha256')
    parser.add_argument('--instance-nor-image', type=Path)
    parser.add_argument('--instance-nor-sha256')
    parser.add_argument('--instance-nor-initialize', action='store_true')
    parser.add_argument('--instance-settings', type=Path)
    parser.add_argument('--skip-oobe', action=argparse.BooleanOptionalAction, default=True,
        help='Complete first-run setup locally (default); --no-skip-oobe preserves the firmware setup flow. Existing instance settings are kept.')
    parser.add_argument('--virtual-peer-state',type=Path)
    parser.add_argument('--virtual-peer-state-sha256')
    parser.add_argument('--virtual-peer-output',type=Path)
    parser.add_argument('--gdb',type=int,nargs='?',const=0,metavar='PORT',
        help='GDB server on 127.0.0.1 (free port when omitted/0); works with the virtual peer')
    parser.add_argument('--gdb-wait',action='store_true',help='With --gdb: hold the CPU at reset until the debugger continues')
    args=parser.parse_args()
    virtual_requested=any((args.virtual_peer_state,args.virtual_peer_state_sha256,args.virtual_peer_output))
    if virtual_requested and (not all((args.virtual_peer_state,args.virtual_peer_state_sha256,args.virtual_peer_output)) or args.trace):
        parser.error('Virtual peer requires all private state arguments and an exact supported profile; instruction traces are disabled')
    instance_requested = any((args.instance_data_image, args.instance_spare_image,
                              args.instance_data_sha256, args.instance_spare_sha256))
    instance_storage = None
    instance_nor = None
    if args.list:
        for p in load_catalog()['profiles']: print(p['id'],p['ap']['chip'])
        return 0
    if args.nor_layout:
        try:
            layout=dict(board_profile(select(args.device),0,args.storage_kind)['nor_geometry'])
        except (ValueError,OSError,KeyError) as error:
            parser.error(str(error))
        layout.pop('profileID',None)
        print(json.dumps(layout,sort_keys=True))
        return 0
    try:
        profile=select(args.device)
        if profile['model'] == 'miwear.watch.q65':
            from backend.src.q65_runner import run
            return run(profile, args)
        if profile['ap']['chip']!='best1503': raise ValueError(profile['ap']['backend_requirement'])
        qemu_sha256 = None  # No startup full-core hash; release manifest owns provenance.
        resource,resource_sha,storage_kind=prepare_storage(profile)
        board=board_profile(profile,resource.stat().st_size,storage_kind)
        board['skip_oobe_requested'] = args.skip_oobe
        board['local_setup_bypass'] = board['local_setup_bypass'] and args.skip_oobe
        board['qemu_sha256'] = qemu_sha256
        data_image=None
        data_sha=None
        if storage_kind=='yaffs' and args.skip_oobe and (not instance_requested or args.instance_nor_initialize):
            from legacy_ui_seed import prepare_ui_seed
            data_image,data_sha,data_proof=prepare_ui_seed(profile,board['geometry']['nand-data-bytes'])
            if data_proof['partition']['offset']!=board['geometry']['nand-data-offset']:
                raise ValueError('Local UI data offset differs from original partition')
            board['data_image_sha256']=data_sha
            board['local_ui_seed']=data_proof
        if instance_requested:
            from backend.src.instance_data import validate_instance_images
            instance_storage = validate_instance_images(
                args.instance_data_image, args.instance_spare_image,
                args.instance_data_sha256, args.instance_spare_sha256, board['geometry'])
            board['instance_storage'] = instance_storage
        if args.instance_nor_image or args.instance_nor_sha256 or args.instance_nor_initialize:
            from backend.src.instance_data import validate_instance_nor
            instance_nor = validate_instance_nor(args.instance_nor_image, args.instance_nor_sha256,
                                                 args.instance_nor_initialize, board, instance_storage)
            board['instance_nor'] = instance_nor
    except (ValueError,OSError) as error:
        parser.error(str(error))
    logdir=args.log_dir or ROOT/'results/devices'/profile['id']
    logdir.mkdir(parents=True,exist_ok=True)
    if virtual_requested: board['virtual_peer_state_sha256']=args.virtual_peer_state_sha256
    board_path=logdir/'board.json'
    board_path.write_text(json.dumps(board,indent=2)+'\n')
    from runtime_fingerprint import runtime_fingerprint
    runtime_identity=runtime_fingerprint(ROOT, hash_contents=False)
    identity = dict(profile_id=profile['id'],
        hash_verification='not-required-for-startup', ap_sha256_source='catalog',
        ota_sha256=profile['ota_sha256'],
        ap_sha256=profile['ap']['sha256'],resource_sha256=resource_sha,data_sha256=data_sha,
        instance_data_sha256=args.instance_data_sha256,
        instance_spare_sha256=args.instance_spare_sha256,
        runtime_sha256=runtime_identity['sha256'],runtime_files=runtime_identity['files'],
        qemu_sha256=qemu_sha256)
    if instance_nor:
        identity.update(instance_nor_sha256=args.instance_nor_sha256,
                        instance_nor_initialize=args.instance_nor_initialize)
    if virtual_requested: identity['virtual_peer_state_sha256']=args.virtual_peer_state_sha256
    (logdir/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    sys.path.insert(0,str(BACKEND))
    from src.qmp import QMP
    from src import runner
    runner.QMP=lambda path:QMP(path,timeout=2)
    sys.argv=[str(BACKEND/'run.py'),'--bluetooth','virtual' if virtual_requested else 'reject',
        '--firmware',profile['ap_path'],'--resource',str(resource),'--board-profile',str(board_path),
        '--qemu',str(BACKEND/'build/qemu-system-arm'),'--log-dir',str(logdir)]
    sys.argv.append('--skip-oobe' if args.skip_oobe else '--no-skip-oobe')
    if board['local_setup_bypass']:sys.argv+=['--skip-binding']
    if instance_storage:
        sys.argv += ['--instance-data-image', instance_storage['dataImage'],
                     '--instance-spare-image', instance_storage['spareImage'],
                     '--instance-data-sha256', args.instance_data_sha256,
                     '--instance-spare-sha256', args.instance_spare_sha256]
        if data_image and instance_nor and args.instance_nor_initialize:
            sys.argv += ['--instance-data-seed', str(data_image)]
    elif data_image:
        sys.argv += ['--data-image',str(data_image)]
    if args.instance_settings:
        if not instance_storage:
            parser.error('Instance settings require instance storage')
        sys.argv += ['--instance-settings', str(args.instance_settings)]
    if instance_nor:
        sys.argv += ['--instance-nor-image', instance_nor['norImage'],
                     '--instance-nor-sha256', args.instance_nor_sha256]
        if args.instance_nor_initialize:
            sys.argv.append('--instance-nor-initialize')
    if virtual_requested:
        sys.argv += ['--virtual-peer-state',str(args.virtual_peer_state),'--virtual-peer-state-sha256',args.virtual_peer_state_sha256,'--virtual-peer-output',str(args.virtual_peer_output)]
    if not args.headless: sys.argv+=['--window']
    if args.trace: sys.argv+=['--trace']
    if args.verbose_logs: sys.argv+=['--verbose-logs']
    if args.seconds is not None: sys.argv+=['--seconds',str(args.seconds)]
    if args.gdb is not None: sys.argv+=['--gdb',str(args.gdb)]+(['--gdb-wait'] if args.gdb_wait else [])
    return runner.main()


if __name__=='__main__':
    raise SystemExit(main())
