#!/usr/bin/env python3
"""Build the macOS arm64 engine from source, without firmware or Developer ID.

Requires Xcode command line tools and Homebrew build dependencies documented in
README. All outputs live under --work-dir / --output. No guest is executed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
INPUT = ROOT / 'engine'
QEMU_VERSION = '10.1.0'
PYTHON_VERSION = '3.12.12'
PACKAGES = {'pillow': '12.3.0', 'littlefs-python': '0.19.0'}
MAGIC = {bytes.fromhex(v) for v in ('feedface','cefaedfe','feedfacf','cffaedfe','cafebabe','bebafeca','cafebabf','bfbafeca')}


def run(args, **kwargs):
    print('+', ' '.join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def output(args):
    return subprocess.check_output(list(map(str, args)), text=True).strip()


def download(url, target):
    if not target.exists():
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_name(target.name + '.part')
        run(['curl', '--fail', '--location', '--retry', '3', '--output', temporary, url])
        temporary.rename(target)
    return target


def unpack(archive, parent):
    parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as tar:
        for item in tar.getmembers():
            p = Path(item.name)
            if p.is_absolute() or '..' in p.parts:
                raise ValueError('Unsafe source archive path')
        def source_filter(member, destination):
            # Upstream QEMU's unused EDK2 X11IncludeHack points to /usr/include.
            # Do not materialize absolute links from any source archive.
            if member.issym() and Path(member.linkname).is_absolute(): return None
            return tarfile.data_filter(member, destination)
        tar.extractall(parent, filter=source_filter)


def copytree(source, target):
    shutil.copytree(source, target, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns('__pycache__', '*.pyc', '.DS_Store'))


def macho(path):
    if not path.is_file():
        return False
    with path.open('rb') as f:
        return f.read(4) in MAGIC


def load_commands(path):
    text = output(['otool', '-l', path])
    loads, rpaths, identity = [], [], None
    command = ''
    for line in text.splitlines():
        words = line.strip().split()
        if words[:1] == ['cmd']:
            command = words[1]
        elif words[:1] == ['name'] and command in ('LC_LOAD_DYLIB','LC_LOAD_WEAK_DYLIB','LC_REEXPORT_DYLIB','LC_ID_DYLIB'):
            name = line.strip()[5:].split(' (offset')[0]
            if command == 'LC_ID_DYLIB': identity = name
            else: loads.append(name)
        elif words[:1] == ['path'] and command == 'LC_RPATH':
            rpaths.append(line.strip()[5:].split(' (offset')[0])
    return loads, rpaths, identity


def relocate(engine, brew, source_delivery):
    """Copy the actual non-system Mach-O closure and make every load relative."""
    pending = [(p, p) for p in engine.rglob('*') if macho(p)]
    visited, original_to_copy, formulas = set(), {}, set()
    lib = engine / 'lib'; lib.mkdir(exist_ok=True)
    while pending:
        original, target = pending.pop()
        if target in visited: continue
        visited.add(target)
        loads, rpaths, identity = load_commands(original)
        args = []
        if identity: args += ['-id', '@loader_path/' + target.name]
        for dependency in loads:
            if dependency.startswith(('/usr/lib/', '/System/Library/')): continue
            def expand(name):
                return name.replace('@loader_path', str(original.parent)).replace('@executable_path', str(engine/'python/bin'))
            if dependency.startswith('@rpath/'):
                candidates = [Path(expand(r))/dependency[7:] for r in rpaths]
                candidates += [engine/'python/lib'/dependency[7:], original.parent/dependency[7:]]
                resolved = next((p.resolve() for p in candidates if p.is_file()), None)
            else:
                candidate = Path(expand(dependency))
                resolved = candidate.resolve() if candidate.is_file() else None
            if resolved is None: raise RuntimeError(f'Unresolved library {dependency} in {original}')
            if resolved.is_relative_to(engine): destination = resolved
            elif resolved in original_to_copy: destination = original_to_copy[resolved]
            else:
                destination = lib/resolved.name
                if destination.exists(): raise RuntimeError('Library basename collision: ' + resolved.name)
                shutil.copy2(resolved, destination)
                original_to_copy[resolved] = destination
                pending.append((resolved, destination))
                parts = resolved.parts
                if 'Cellar' in parts: formulas.add(parts[parts.index('Cellar')+1])
                else: raise RuntimeError('No source provenance for external library: ' + str(resolved))
            args += ['-change', dependency, '@loader_path/' + os.path.relpath(destination, target.parent)]
        for rpath in rpaths:
            if rpath.startswith('/') or rpath.startswith('@'):
                args += ['-delete_rpath', rpath]
        if args: run(['install_name_tool', *args, target])
    # Deliver exact source versions for the libraries actually copied, not every
    # formula installed on the runner. Formula metadata includes original terms.
    for formula in sorted(formulas):
        kegs = {Path(*p.parts[:p.parts.index('Cellar')+3]) for p in original_to_copy if 'Cellar' in p.parts and p.parts[p.parts.index('Cellar')+1] == formula}
        if len(kegs)!=1:raise RuntimeError('Mixed installed versions of '+formula)
        keg=kegs.pop()
        recipe=keg/'.brew'/f'{formula}.rb'
        if not recipe.is_file():raise RuntimeError('Installed source recipe missing: '+str(recipe))
        recipe_text=recipe.read_text()
        urls=re.findall(r'^\s*url\s+[\"\']([^\"\']+)[\"\']',recipe_text,re.M)
        if not urls or '#{' in urls[0]:raise RuntimeError('Cannot resolve installed source URL: '+str(recipe))
        # The recipe inside this installed keg, rather than today's formula API,
        # records the exact library version used by this binary.
        d = source_delivery/'native-libraries'/formula; d.mkdir(parents=True, exist_ok=True)
        shutil.copy2(recipe,d/'formula.rb')
        shutil.copy2(keg/'INSTALL_RECEIPT.json',d/'INSTALL_RECEIPT.json')
        url = urls[0]
        archive = download(url, d/Path(url.split('?')[0]).name)
        # Source package itself preserves complete license notices. Also expose
        # root license/copyright files inside Engine for binary recipients.
        notice_dir = engine/'licenses'/formula; notice_dir.mkdir(parents=True,exist_ok=True)
        with tarfile.open(archive) as tar:
            for member in tar.getmembers():
                if member.isfile() and re.search(r'(license|licence|copying|copyright|notice)',Path(member.name).name,re.I):
                    stream=tar.extractfile(member)
                    if stream:
                        name=member.name.replace('/','__')
                        (notice_dir/name).write_bytes(stream.read())
        for patch_url in re.findall(r'patch\s+do\s+url\s+[\"\']([^\"\']+)[\"\']',recipe_text):
            download(patch_url,d/Path(patch_url).name)
    for p in visited:
        for dependency in load_commands(p)[0]:
            if not dependency.startswith(('/usr/lib/','/System/Library/','@loader_path/')):
                raise RuntimeError('Unrelocated dependency: '+dependency)
        run(['codesign','--force','--sign','-',p])
    return sorted(formulas)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work-dir',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--source-output',type=Path,required=True)
    parser.add_argument('--jobs',type=int,default=min(os.cpu_count() or 2,8))
    parser.add_argument('--build-python',type=Path,help='Existing CPython 3.12 with pip; default: Homebrew python@3.12')
    parser.add_argument('--tools-dir',type=Path,help='Optional existing directory containing ninja/pkg-config')
    args=parser.parse_args()
    if platform.system()!='Darwin' or platform.machine()!='arm64': parser.error('macOS arm64 build host required')
    work=args.work_dir.resolve(); engine=args.output.resolve(); source_output=args.source_output.resolve()
    if engine.exists(): parser.error('--output must be a new path')
    work.mkdir(parents=True,exist_ok=True)
    brew=shutil.which('brew')
    if not brew: parser.error('Homebrew build dependencies are required; see README')
    builder=args.build_python or Path(output([brew,'--prefix','python@3.12']))/'bin/python3.12'
    if not builder.is_file(): parser.error('Install python@3.12 for building wheels')
    env=os.environ.copy(); env['MACOSX_DEPLOYMENT_TARGET']='15.0'
    # Some portable builder Pythons record their original, now absent compiler
    # paths in sysconfig. Use the selected Xcode tools for native extensions.
    for key,tool in (('CC','clang'),('CXX','clang++'),('AR','ar'),('RANLIB','ranlib')):
        env[key]=output(['xcrun','--find',tool])
    sdk=output(['xcrun','--show-sdk-path']);env['SDKROOT']=sdk
    for key in ('CFLAGS','CXXFLAGS','CPPFLAGS','LDFLAGS'):
        env[key]=env.get(key,'')+' -isysroot '+sdk+' -mmacosx-version-min=15.0'
    env['PATH']=str(builder.parent)+os.pathsep+env['PATH']
    if args.tools_dir:env['PATH']=str(args.tools_dir)+os.pathsep+env['PATH']
    prefix=Path(output([brew,'--prefix']))
    env['PKG_CONFIG_PATH']=os.pathsep.join(str(prefix/'opt'/f/'lib/pkgconfig') for f in ('glib','libffi','pcre2','zstd','dtc'))
    env['PKG_CONFIG_PATH']+=os.pathsep+sdk+'/usr/lib/pkgconfig'
    env['CPATH']=str(prefix/'include'); env['LIBRARY_PATH']=str(prefix/'lib')
    downloads=work/'downloads'; sources=work/'source'; sources.mkdir(exist_ok=True)
    delivery=work/'corresponding-source';delivery.mkdir(exist_ok=True)
    q_archive=download(f'https://download.qemu.org/qemu-{QEMU_VERSION}.tar.xz',downloads/f'qemu-{QEMU_VERSION}.tar.xz')
    p_archive=download(f'https://www.python.org/ftp/python/{PYTHON_VERSION}/Python-{PYTHON_VERSION}.tar.xz',downloads/f'Python-{PYTHON_VERSION}.tar.xz')
    qsource=sources/f'qemu-{QEMU_VERSION}'; psource=sources/f'Python-{PYTHON_VERSION}'
    for archive,source in ((q_archive,qsource),(p_archive,psource)):
        if not (source/'.azki-extracted').exists():
            unpack(archive,sources)
            (source/'.azki-extracted').touch()
    copytree(INPUT/'qemu-overlay',qsource)
    qbuild=work/'qemu-build';qbuild.mkdir(exist_ok=True)
    if not (qbuild/'build.ninja').exists():
        run([qsource/'configure','--target-list=arm-softmmu','--with-devices-arm=mi-emu','--without-default-devices',
             '--disable-docs','--disable-tools','--disable-guest-agent','--disable-gtk','--disable-sdl','--disable-vnc',
             '--disable-opengl','--disable-werror','--disable-download','--audio-drv-list=','--disable-slirp',
             '--python='+str(builder)],cwd=qbuild,env=env)
    run(['ninja','-C',qbuild,'-j',args.jobs,'qemu-system-arm'],env=env)
    # Build a non-framework Python with its normal prefix-relative layout.
    # No user Python, Homebrew, pip or network is needed on the target Mac.
    pbuild=work/'python-build';pbuild.mkdir(exist_ok=True)
    pyinstall=work/'python-install'
    if not (pbuild/'Makefile').exists():
        run([psource/'configure','--prefix='+str(pyinstall),'--with-ensurepip=no','--disable-test-modules',
             '--with-openssl='+output([brew,'--prefix','openssl@3'])],cwd=pbuild,env=env)
    run(['make','-j',args.jobs],cwd=pbuild,env=env)
    run(['make','install'],cwd=pbuild,env=env)
    wheels=work/'wheels';wheels.mkdir(exist_ok=True)
    package_sources=delivery/'python-packages';package_sources.mkdir(exist_ok=True)
    for package,version in PACKAGES.items():
        metadata=json.loads(urllib.request.urlopen(f'https://pypi.org/pypi/{package}/{version}/json').read())
        item=next(x for x in metadata['urls'] if x['packagetype']=='sdist')
        source=download(item['url'],package_sources/item['filename'])
        command=[builder,'-m','pip','wheel','--no-deps','--wheel-dir',wheels,source]
        if package=='pillow':
            for feature in ('jpeg','tiff','freetype','lcms','webp','jpeg2000','raqm','imagequant','xcb','avif'):
                command+=['--config-settings',feature+'=disable']
        run(command,env=env)
    engine.mkdir(parents=True)
    copytree(INPUT/'project',engine/'project')
    copytree(pyinstall/'lib/python3.12',engine/'python/lib/python3.12')
    (engine/'python/bin').mkdir(parents=True)
    shutil.copy2(pyinstall/'bin/python3.12',engine/'python/bin/python3.12')
    site=engine/'python/lib/python3.12/site-packages';site.mkdir(exist_ok=True)
    # Wheels were compiled for CPython 3.12 arm64 above; expand their runtime
    # files, excluding installation scripts and packaging tools.
    for wheel in wheels.glob('*.whl'):
        with zipfile.ZipFile(wheel) as z:
            for entry in z.infolist():
                if entry.is_dir(): continue
                path=Path(entry.filename)
                if path.is_absolute() or '..' in path.parts: raise ValueError('Unsafe wheel path')
                target=site/path;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(z.read(entry))
    for pattern in ('__pycache__','test','tests','idlelib','tkinter','ensurepip'):
        for p in sorted((engine/'python').rglob(pattern),reverse=True):
            if p.is_dir(): shutil.rmtree(p)
    for p in (engine/'python').rglob('*.pyc'):p.unlink()
    build=engine/'project/backend/build';build.mkdir(parents=True,exist_ok=True)
    shutil.copy2(qbuild/'qemu-system-arm',build/'qemu-system-arm')
    y=INPUT/'third-party/yaffs2';u=INPUT/'third-party/unqlite'
    definitions=('CONFIG_YAFFS_DIRECT','CONFIG_YAFFS_YAFFS2','CONFIG_YAFFS_DEFINES_TYPES','CONFIG_YAFFS_PROVIDE_DEFS','CONFIG_YAFFSFS_PROVIDE_VALUES','CONFIG_YAFFS_USE_32_BIT_TIME_T')
    run(['cc','-O2',*['-D'+x for x in definitions],'-Dloff_t=int64_t','-I'+str(y/'core'),'-I'+str(y/'direct'),
         INPUT/'project/backend/src/yaffs_image.c',*sorted((y/'core').glob('*.c')),
         *[y/'direct'/n for n in ('yaffsfs.c','yaffs_attribs.c','yaffs_error.c','yaffs_hweight.c')],'-o',build/'yaffs-image'],env=env)
    run(['cc','-O2','-I'+str(u),INPUT/'project/backend/src/legacy_ui_db.c',u/'unqlite.c','-o',build/'legacy-ui-db'],env=env)
    # Reads/updates persist.db inside an instance YAFFS image (settings and boot clock).
    run(['cc','-O2',*['-D'+x for x in definitions],'-Dloff_t=int64_t','-I'+str(y/'core'),'-I'+str(y/'direct'),'-I'+str(u),
         INPUT/'project/backend/src/instance_props.c',*sorted((y/'core').glob('*.c')),
         *[y/'direct'/n for n in ('yaffsfs.c','yaffs_attribs.c','yaffs_error.c','yaffs_hweight.c')],u/'unqlite.c',
         '-o',build/'instance-props'],env=env)
    shutil.copy2(INPUT/'bootstrap.py',engine/'bootstrap.py')
    copytree(INPUT/'licenses',engine/'licenses')
    shutil.copy2(INPUT/'THIRD_PARTY.md',engine/'licenses/THIRD_PARTY.md')
    for name,path in [('python',psource/'LICENSE'),('yaffs',y/'LICENSE'),('unqlite',u/'LICENSE')]:
        d=engine/'licenses'/name;d.mkdir(exist_ok=True);shutil.copy2(path,d/'LICENSE')
    formulas=relocate(engine,brew,delivery)
    # Import smoke checks exercise newly built host libraries, never firmware.
    smoke="import sys; from pathlib import Path; assert Path(sys.prefix).resolve()==Path(sys.executable).resolve().parent.parent; sys.path.insert(0,sys.argv[1]); import PIL.Image, littlefs, ctypes, socket, zlib; print('Engine Python imports ready')"
    run([engine/'python/bin/python3.12','-I','-S','-B','-c',smoke,site],env={**env,'PYTHONDONTWRITEBYTECODE':'1'})
    run([build/'qemu-system-arm','--version'],env=env)
    for p in engine.rglob('*'):
        if p.is_file():p.chmod(p.stat().st_mode & ~0o022)
    files=[{'path':p.relative_to(engine).as_posix(),'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'executable':bool(p.stat().st_mode&0o111)} for p in sorted(engine.rglob('*')) if p.is_file()]
    # These are build records used by the existing packager, not startup gates.
    capabilities=json.loads((INPUT/'capabilities.json').read_text())
    qemu_record=next(item for item in files if item['path']=='project/backend/build/qemu-system-arm')
    for capability in capabilities.values():
        capability['qemuSHA256']=qemu_record['sha256']
    manifest={'schema':'azki.simulator.engine.v1','version':1,'profile':'local-assets',
      'entrypoints':{'python':'python/bin/python3.12','qemu':'project/backend/build/qemu-system-arm','project':'project','bootstrap':'bootstrap.py'},
      'python':{'version':'3.12','home':'python','stdlib':'python/lib/python3.12','sitePackages':'python/lib/python3.12/site-packages','startupFlags':['-I','-S','-B']},
      'files':files,'payloadSha256':hashlib.sha256(json.dumps(files,sort_keys=True,separators=(',',':'),ensure_ascii=True).encode()).hexdigest(),
      'distribution':{'mode':'runtime-only','firmwareIncluded':False,'publicReady':True},
      'signing':{'developerIDSigned':False,'notarized':False,'adHoc':True},'executionVerified':False,
      'build':{'qemu':QEMU_VERSION,'python':PYTHON_VERSION,'packages':PACKAGES,'nativeLibraries':formulas},
      **capabilities}
    (engine/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    copytree(INPUT,delivery/'engine')
    (delivery/'scripts').mkdir(exist_ok=True)
    shutil.copy2(Path(__file__),delivery/'scripts/build-engine.py')
    shutil.copy2(ROOT/'LICENSE',delivery/'LICENSE')
    # Keep original tarballs plus overlay rather than generated build trees.
    shutil.copy2(q_archive,delivery/q_archive.name);shutil.copy2(p_archive,delivery/p_archive.name)
    (delivery/'BUILD.json').write_text(json.dumps(manifest['build'],indent=2)+'\n')
    source_output.parent.mkdir(parents=True,exist_ok=True)
    with tarfile.open(source_output,'w:gz') as tar:tar.add(delivery,arcname='Engine-sources')
    print(json.dumps({'engine':str(engine),'correspondingSource':str(source_output),'firmwareExecuted':False}))


if __name__=='__main__':main()
