#!/usr/bin/env python3
"""Build the simulator engine from source, without firmware or Developer ID.

Hosts: macOS arm64 (Xcode + Homebrew) and Linux (gcc + pkg-config system libs).
Final outputs live in dist; temporary work is cleaned. No guest is executed.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import signal
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
INPUT = ROOT / 'engine'
QEMU_VERSION = '10.1.0'
PYTHON_VERSION = '3.12.12'
PACKAGES = {'pillow': '12.3.0', 'littlefs-python': '0.19.0'}
MAGIC = {bytes.fromhex(v) for v in ('feedface','cefaedfe','feedfacf','cffaedfe','cafebabe','bebafeca','cafebabf','bfbafeca')}
ELF_MAGIC = bytes.fromhex('7f454c46')
HOST_OS = platform.system()
HOST_ARCH = platform.machine()
# Libraries that a Linux engine build is allowed to resolve outside the tree.
# Everything else must be vendored or the build fails; glibc-family loader
# pseudo-entries (vdso, ld.so) are skipped by name prefix in linux_verify().
LINUX_SYSTEM_LIB_ROOTS = ('/usr/lib/', '/lib/')


def run(args, **kwargs):
    print('+', ' '.join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def output(args):
    return subprocess.check_output(list(map(str, args)), text=True).strip()


class RangeDownloadUnavailable(Exception):
    """The server advertised ranges but did not honor the request."""


class DownloadRateLimited(Exception):
    """The source temporarily rejected a request; the caller may fall back."""


class DownloadTransientFailure(Exception):
    """A retryable transport failure exhausted one range request."""


def response_headers(text):
    # curl includes proxy and redirect headers; only the final response counts.
    for block in reversed(re.split(r'\r?\n\r?\n', text.strip())):
        lines=block.splitlines()
        match=re.match(r'HTTP/\S+\s+(\d{3})\b',lines[0]) if lines else None
        if match:
            headers={}
            for line in lines[1:]:
                name,separator,value=line.partition(':')
                if separator:headers[name.lower().strip()]=value.strip()
            return int(match.group(1)),headers
    return 0,{}


def parallel_download(url, temporary, size, metadata):
    # Every segment stays inside the same disposable build work directory.
    with tempfile.TemporaryDirectory(prefix=temporary.name+'.segments-',dir=temporary.parent) as directory:
        directory=Path(directory)
        ranges=[(size*i//8,size*(i+1)//8-1) for i in range(8)]
        validator=metadata.get('etag','')
        if not validator or validator.startswith('W/'):validator=metadata.get('last-modified','')

        def fetch(item):
            index,(start,end)=item
            part=directory/str(index);header=directory/(str(index)+'.headers')
            for attempt in range(1, 6):
                header.unlink(missing_ok=True)
                part.unlink(missing_ok=True)
                command=['curl','--fail','--location','--silent','--show-error',
                         '--connect-timeout','15','--max-time','900',
                         '--header','Accept-Encoding: identity',
                         '--range',f'{start}-{end}','--max-filesize',str(end-start+1),
                         '--dump-header',str(header),'--output',str(part)]
                if validator:command+=['--header','If-Range: '+validator]
                result=subprocess.run([*command,url],capture_output=True,text=True)
                status,headers=response_headers(header.read_text() if header.exists() else '')
                if status in (200,405,416):
                    raise RangeDownloadUnavailable('Server did not honor byte ranges')
                if status == 429 or 500 <= status <= 599:
                    if attempt == 5:
                        raise DownloadRateLimited(f'HTTP {status} after {attempt} attempts')
                    # CDN rate limits are commonly per-client rather than per
                    # URL. Back off before retrying the same segment, honoring
                    # Retry-After when it is a bounded number of seconds.
                    retry_after=headers.get('retry-after','')
                    try:
                        delay=min(60.0, max(1.0, float(retry_after)))
                    except ValueError:
                        delay=min(60.0, 2.0 ** (attempt - 1))
                    time.sleep(delay)
                    continue
                if result.returncode:
                    # curl 6/7/18/28/35/47/52/55/56 are connection or
                    # transfer failures seen with public CDNs. Retry the
                    # segment, then abandon ranges for a single request.
                    transient = result.returncode in (6, 7, 18, 28, 35, 47, 52, 55, 56)
                    if transient and attempt < 5:
                        time.sleep(min(60.0, 2.0 ** (attempt - 1)))
                        continue
                    if transient:
                        raise DownloadTransientFailure(
                            f'Range download failed ({result.returncode}) after {attempt} attempts: {result.stderr.strip()}')
                    raise RuntimeError(f'Range download failed ({result.returncode}): {result.stderr.strip()}')
                content_range=re.fullmatch(r'bytes (\d+)-(\d+)/(\d+)',headers.get('content-range',''))
                if status!=206 or not content_range or tuple(map(int,content_range.groups()))!=(start,end,size):
                    raise RangeDownloadUnavailable('Missing or incorrect Content-Range')
                if headers.get('content-encoding','identity').lower()!='identity':
                    raise RuntimeError('Unexpected encoding in range response')
                if part.stat().st_size!=end-start+1:
                    raise RuntimeError('Range download has an incorrect length')
                return part

        print(f'Downloading {temporary.name.removesuffix(".part")} in 8 verified byte ranges',flush=True)
        # Eight simultaneous requests can trigger 429 responses on otherwise
        # range-capable public mirrors. Keep the default conservative; callers
        # may raise it explicitly for an internal mirror.
        workers=max(1, min(8, int(os.environ.get('AZKI_DOWNLOAD_JOBS','2'))))
        with ThreadPoolExecutor(max_workers=workers) as executor:
            parts=list(executor.map(fetch,enumerate(ranges)))
        with temporary.open('wb') as joined:
            for part in parts:
                with part.open('rb') as source:shutil.copyfileobj(source,joined)
        if temporary.stat().st_size!=size:
            raise RuntimeError('Combined download has an incorrect length')


def _range_default(target):
    # Public GNU mirrors commonly rate-limit concurrent ranges.  Keep range
    # requests for the two large, stable upstream archives only; small Homebrew
    # formula sources are more reliable as one retried request.
    if os.environ.get('AZKI_DOWNLOAD_RANGES') == '1':
        return True
    return target.name in {f'qemu-{QEMU_VERSION}.tar.xz',
                           f'Python-{PYTHON_VERSION}.tar.xz'}


def _download_one(url, target, *, allow_ranges=None):
    """Download one URL into target, leaving no partial file on failure."""
    if target.exists():return target
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary=target.with_name(target.name+'.part')
    try:
        head=subprocess.run(['curl','--fail','--location','--silent','--show-error',
                             '--connect-timeout','15','--max-time','30','--head',
                             '--header','Accept-Encoding: identity',url],capture_output=True,text=True)
        status,metadata=response_headers(head.stdout)
        length=metadata.get('content-length','')
        size=int(length) if length.isdecimal() else 0
        ranged=(allow_ranges is not False and (allow_ranges is True or _range_default(target))
                and head.returncode==0 and status==200 and size>16*1024*1024
                and metadata.get('accept-ranges','').lower()=='bytes'
                and metadata.get('content-encoding','identity').lower()=='identity')
        if ranged:
            try:
                parallel_download(url,temporary,size,metadata)
            except (RangeDownloadUnavailable, DownloadRateLimited, DownloadTransientFailure) as error:
                print(f'{error}; falling back to a single download',flush=True)
                ranged=False
        if not ranged:
            # A single request is often accepted after the mirror's rate-limit
            # window has reset. Retry transient 429/5xx responses with a bounded
            # backoff; curl's default retry list is not portable across versions.
            for attempt in range(1, 6):
                result=subprocess.run(['curl','--fail','--location','--silent','--show-error',
                                       '--connect-timeout','15','--max-time','1800',
                                       '--speed-limit','20480','--speed-time','60',
                                       '--dump-header',str(temporary)+'.headers',
                                       '--output',temporary,url],capture_output=True,text=True)
                status,metadata=response_headers(Path(str(temporary)+'.headers').read_text()
                                                if Path(str(temporary)+'.headers').exists() else '')
                Path(str(temporary)+'.headers').unlink(missing_ok=True)
                if result.returncode == 0:
                    break
                if status == 429 or 500 <= status <= 599:
                    if attempt == 5:
                        raise RuntimeError(f'Source download failed with HTTP {status} after {attempt} attempts')
                    time.sleep(min(60.0, 2.0 ** (attempt - 1)))
                    continue
                raise RuntimeError(f'Source download failed ({result.returncode}): {result.stderr.strip()}')
            else:
                raise RuntimeError('Source download failed')
        temporary.rename(target)
        return target
    finally:
        temporary.unlink(missing_ok=True)


def _sha256_file(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


# PyPI replicates its artifact tree byte for byte, and the download is checked
# against the digest from the canonical metadata, so a throttled or unreachable
# files.pythonhosted.org does not stall the build.
PYPI_ARTIFACT_HOSTS = ('https://files.pythonhosted.org', 'https://mirrors.cloud.tencent.com/pypi',
                       'https://mirrors.aliyun.com/pypi', 'https://mirror.nju.edu.cn/pypi/web')


def pypi_artifact_urls(url):
    if not url.startswith(PYPI_ARTIFACT_HOSTS[0] + '/'):
        return [url]
    path = url[len(PYPI_ARTIFACT_HOSTS[0]):]
    return [host + path for host in PYPI_ARTIFACT_HOSTS]


def _normalize_distribution(name):
    """PEP 503 distribution name: runs of -_. collapse to a single -."""
    return re.sub(r'[-_.]+', '-', name).lower()


def find_cached_wheel(wheels, package, version):
    """Locate a cached wheel for exactly this distribution and version.

    Wheel files spell the distribution with underscores (littlefs_python) while
    PyPI metadata spells it with dashes (littlefs-python), so the two are
    compared through the normalized form instead of a raw prefix test. The
    version is compared exactly: a substring test would also match a wheel
    built for 0.19.0 when 0.19.10 was requested.
    """
    wanted=_normalize_distribution(package)
    for path in sorted(wheels.glob('*.whl')):
        parts=path.name[:-len('.whl')].split('-')
        if len(parts)<5: continue
        if _normalize_distribution(parts[0])!=wanted: continue
        if parts[1]!=version: continue
        return path
    return None


def download(url, target, alternates=(), sha256=None):
    """Download from URL candidates, preserving the first working artifact."""
    urls = [url] if isinstance(url, str) else list(url)
    urls += list(alternates)
    seen = set()
    last_error = None
    for candidate in urls:
        if not candidate or candidate in seen:
            continue
        seen.add(candidate)
        try:
            downloaded = _download_one(candidate, target)
            if sha256 and _sha256_file(downloaded) != sha256:
                downloaded.unlink()
                raise RuntimeError(f'Source digest mismatch from {candidate}')
            return downloaded
        except Exception as error:
            last_error = error
            print(f'Download failed from {candidate}: {error}; trying next mirror', flush=True)
            target.with_name(target.name + '.part').unlink(missing_ok=True)
    if last_error is not None:
        raise last_error
    raise ValueError('No source URL was provided')


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


def elf(path):
    """True for a loadable ELF. Relocatable objects (CPython's config-3.12/python.o)
    share the magic but have no dependency closure, and ldd refuses them."""
    if not path.is_file():
        return False
    with path.open('rb') as f:
        header = f.read(18)
    return header[:4] == ELF_MAGIC and int.from_bytes(header[16:18], 'little') in {2, 3}


def ldd_entries(path):
    """Parse `ldd` output into (soname, resolved path) pairs, failing on gaps."""
    result=subprocess.run(['ldd',str(path)],capture_output=True,text=True)
    if result.returncode and 'not a dynamic executable' in result.stdout+result.stderr:
        return []
    if result.returncode:
        raise RuntimeError(f'ldd failed for {path}: {(result.stdout+result.stderr).strip()}')
    entries = []
    for line in result.stdout.splitlines():
        words = line.split()
        if not words:
            continue
        if 'not found' in line:
            raise RuntimeError(f'Unresolved dynamic dependency in {path}: {line.strip()}')
        if len(words) >= 3 and words[1] == '=>' and words[2].startswith('/'):
            entries.append((words[0], words[2]))
        elif len(words) >= 2 and words[0].startswith('/'):
            # Loader pseudo-entry: /lib64/ld-linux-x86-64.so.2 (0x7f...).
            entries.append((Path(words[0]).name, words[0]))
        # Remaining forms are kernel-provided pseudo-libraries (linux-vdso and
        # friends) and carry no filesystem path to check.
    return entries


def dynamic_strings(path):
    text = output(['readelf', '-dW', path])
    values = []
    for line in text.splitlines():
        match = re.search(r'\((RUNPATH|RPATH)\)\s+Library (?:rpath|runpath):', line)
        if match:
            quoted = re.search(r"\[(.*)\]", line)
            if quoted:
                values.append(quoted.group(1))
    return values


def linux_verify(engine):
    """Fail closed unless every ELF dependency resolves to the engine tree or
    the host's system library roots, and no ELF carries a build-time rpath.

    Linux engines deliberately link against the host glibc/OpenSSL/etc. instead
    of vendoring a closure: the build host is the run host (Arch target), and
    packaging would duplicate what pkg-config already resolves. The returned
    SONAME table documents the runtime ABI floor."""
    system, seen = set(), set()
    for path in engine.rglob('*'):
        if not elf(path):
            continue
        for value in dynamic_strings(path):
            for entry in value.split(':'):
                if not entry:
                    continue
                resolved = str((path.parent / entry).resolve()) if entry.startswith('$ORIGIN') else str(Path(entry).resolve())
                if not (Path(resolved).is_relative_to(engine) or any(resolved.startswith(r) for r in LINUX_SYSTEM_LIB_ROOTS)):
                    raise RuntimeError(f'Build-time dynamic path in {path}: {entry}')
        for soname, resolved in ldd_entries(path):
            # ldd reports the loader itself with an absolute first column, and some
            # distributions resolve system libraries through a /usr/lib64 symlink.
            if Path(soname).name.startswith(('linux-vdso', 'linux-gate', 'ld-linux')):
                continue
            location = Path(resolved).resolve()
            if location.is_relative_to(engine):
                continue
            if not any(str(location).startswith(root) for root in LINUX_SYSTEM_LIB_ROOTS):
                raise RuntimeError(f'External dependency outside system roots: {soname} -> {resolved} in {path}')
            seen.add((str(location.parent), soname))
            system.add(soname)
    return sorted(system), sorted(({'soname': s, 'directory': d} for d, s in seen),
                                  key=lambda entry:(entry['directory'], entry['soname']))


def c_tool_smoke(build, env):
    """Run the three host C tools against a synthetic NAND, without firmware.

    legacy-ui-db and yaffs-image verify their own output, but what the runtime
    depends on is the pairing: instance-props must mount a yaffs-image image,
    read the seeded persist.db back, edit it, and survive a cold remount. A
    loff_t/time_t or in-band-tag difference breaks silently at exactly that
    seam, and a compile-only acceptance never sees it.
    """
    data_bytes = 8 * 1024 * 1024
    with tempfile.TemporaryDirectory(prefix='azki-c-tools-') as name:
        work = Path(name)
        tree = work / 'nand'
        tree.mkdir()
        run([build / 'legacy-ui-db', tree / 'persist.db'], env=env)
        data = work / 'nand-data.bin'
        run([build / 'yaffs-image', data, '--bytes', data_bytes, '--tree', tree], env=env)
        # instance-props only opens private, single-link images.
        data.chmod(0o600)
        spare = work / 'nand-spare.bin'
        with spare.open('xb') as handle:
            handle.write(b'\xff' * (data_bytes // 2048 * 64))
        spare.chmod(0o600)

        def props(*args):
            return subprocess.run([build / 'instance-props', data, spare, *args], check=True,
                                  capture_output=True, text=True, env={**env, 'TMPDIR': str(work)}).stdout

        copy = work / 'persist-copy.db'
        props('extract', '/nand/persist.db', str(copy))
        run([build / 'legacy-ui-db', '--verify', copy], env=env)
        seed = json.loads(props('get'))
        if seed.get('persist.locale') != 'zh_CN' or seed.get('persist.setup_wizard') != '1':
            raise RuntimeError(f'instance-props read-back mismatch: {seed}')
        changes = work / 'changes.txt'
        changes.write_bytes(b'S\tpersist.locale\tfr_FR\nD\tpersist.setup_wizard\n')
        changes.chmod(0o600)
        props('set', changes)
        after = json.loads(props('get'))
        if after.get('persist.locale') != 'fr_FR' or 'persist.setup_wizard' in after:
            raise RuntimeError(f'instance-props set/get mismatch: {after}')


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
        urls=re.findall(r'^\s*(?:url|mirror)\s+[\"\']([^\"\']+)[\"\']',recipe_text,re.M)
        if not urls or '#{' in urls[0]:raise RuntimeError('Cannot resolve installed source URL: '+str(recipe))
        # The recipe inside this installed keg, rather than today's formula API,
        # records the exact library version used by this binary.
        d = source_delivery/'native-libraries'/formula; d.mkdir(parents=True, exist_ok=True)
        shutil.copy2(recipe,d/'formula.rb')
        shutil.copy2(keg/'INSTALL_RECEIPT.json',d/'INSTALL_RECEIPT.json')
        url = urls[0]
        archive = download(url, d/Path(url.split('?')[0]).name, urls[1:])
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
    parser.add_argument('--output',type=Path,default=ROOT/'dist/Engine')
    parser.add_argument('--source-output',type=Path,default=ROOT/'dist/Engine-sources.tar.gz')
    parser.add_argument('--jobs',type=int,default=min(os.cpu_count() or 2,8))
    parser.add_argument('--build-python',type=Path,help='Existing CPython 3.12 with pip; default: Homebrew python@3.12')
    parser.add_argument('--tools-dir',type=Path,help='Optional existing directory containing ninja/pkg-config')
    args=parser.parse_args()
    if HOST_OS not in ('Darwin','Linux') or (HOST_OS=='Darwin' and HOST_ARCH!='arm64') \
            or (HOST_OS=='Linux' and HOST_ARCH not in ('x86_64','aarch64')):
        parser.error('build host must be macOS arm64 or Linux x86_64/aarch64')
    dist=ROOT/'dist'
    engine=args.output.resolve(); source_output=args.source_output.resolve()
    if dist.is_symlink() or engine.parent != dist or source_output.parent != dist:
        parser.error('Final engine and source outputs must be direct children of the repository dist directory')
    if engine == source_output or engine.exists() or source_output.exists():
        parser.error('Engine and source outputs must be distinct new paths')
    dist.mkdir(exist_ok=True)
    def terminate(signum, frame):
        raise SystemExit(128 + signum)
    previous_handlers={}
    try:
        for signum in (signal.SIGTERM,signal.SIGHUP):
            previous_handlers[signum]=signal.signal(signum,terminate)
        with tempfile.TemporaryDirectory(prefix='azki-engine-',
                                        dir='/private/tmp' if HOST_OS=='Darwin' else None) as temporary:
            work=Path(temporary)
            build_engine(work,work/'Engine',work/'Engine-sources.tar.gz',args.jobs,args.build_python,args.tools_dir)
            (work/'Engine').rename(engine)
            (work/'Engine-sources.tar.gz').rename(source_output)
    finally:
        for signum,handler in previous_handlers.items():
            signal.signal(signum,handler)


def build_engine(work, engine, source_output, jobs=None, build_python=None, tools_dir=None):
    """Internal staging API; callers own and clean the temporary work tree."""
    if HOST_OS not in ('Darwin','Linux') or (HOST_OS=='Darwin' and HOST_ARCH!='arm64') \
            or (HOST_OS=='Linux' and HOST_ARCH not in ('x86_64','aarch64')):
        raise RuntimeError('build host must be macOS arm64 or Linux x86_64/aarch64')
    args=argparse.Namespace(jobs=jobs or min(os.cpu_count() or 2,8),build_python=build_python,tools_dir=tools_dir)
    # Persistent dependency cache under the repository build/ directory so that
    # downloaded tarballs, extracted upstream sources, configured build trees and
    # compiled wheels survive across runs. Only the final engine staging dirs
    # (engine/source_output) stay ephemeral. Override with AZKI_ENGINE_CACHE.
    cache = Path(os.environ.get('AZKI_ENGINE_CACHE') or (ROOT / 'build')).resolve()
    cache.mkdir(parents=True, exist_ok=True)
    for name in ('downloads', 'source', 'qemu-build', 'python-build',
                 'python-install', 'wheels', 'python-packages',
                 'corresponding-source', 'pip-cache', 'tmp'):
        (cache / name).mkdir(parents=True, exist_ok=True)
    brew=shutil.which('brew') if HOST_OS=='Darwin' else None
    if HOST_OS=='Darwin':
        if not brew: raise RuntimeError('Homebrew build dependencies are required; see README')
        builder=args.build_python or Path(output([brew,'--prefix','python@3.12']))/'bin/python3.12'
        if not builder.is_file(): raise RuntimeError('Install python@3.12 for building wheels')
        env=os.environ.copy(); env['MACOSX_DEPLOYMENT_TARGET']='15.0'
    else:
        builder=args.build_python or Path(shutil.which('python3') or '')
        if not builder.is_file(): raise RuntimeError('A python3 build host with ensurepip is required; see docs/BUILDING-LINUX.md')
        env=os.environ.copy()
    temporary=cache/'tmp'
    env['TMPDIR']=str(temporary);env['PIP_CACHE_DIR']=str(cache/'pip-cache')
    # A host's global pip.conf can name an index that refuses this build, where a 403
    # surfaces as "from versions: none". PEP 517 build backends install their own
    # dependencies from that index, so the index is pinned rather than inherited.
    env['PIP_INDEX_URL']=os.environ.get('AZKI_PIP_INDEX_URL','https://pypi.org/simple')
    env['PIP_EXTRA_INDEX_URL']=''
    # Build backends fetch Cython/meson-python over the same throttled link, where a
    # single incomplete read aborts the whole wheel build.
    env.setdefault('PIP_RETRIES','10')
    if HOST_OS=='Darwin':
        # Some portable builder Pythons record their original, now absent compiler
        # paths in sysconfig. Use the selected Xcode tools for native extensions.
        for key,tool in (('CC','clang'),('CXX','clang++'),('AR','ar'),('RANLIB','ranlib')):
            env[key]=output(['xcrun','--find',tool])
        sdk=output(['xcrun','--show-sdk-path']);env['SDKROOT']=sdk
        for key in ('CFLAGS','CXXFLAGS','CPPFLAGS','LDFLAGS'):
            env[key]=env.get(key,'')+' -isysroot '+sdk+' -mmacosx-version-min=15.0'
    env['PATH']=str(builder.parent)+os.pathsep+env['PATH']
    if args.tools_dir:env['PATH']=str(args.tools_dir)+os.pathsep+env['PATH']
    if HOST_OS=='Darwin':
        prefix=Path(output([brew,'--prefix']))
        env['PKG_CONFIG_PATH']=os.pathsep.join(str(prefix/'opt'/f/'lib/pkgconfig') for f in ('glib','libffi','pcre2','zstd','dtc'))
        env['PKG_CONFIG_PATH']+=os.pathsep+sdk+'/usr/lib/pkgconfig'
        env['CPATH']=str(prefix/'include'); env['LIBRARY_PATH']=str(prefix/'lib')
    downloads=cache/'downloads'; sources=cache/'source'; sources.mkdir(exist_ok=True)
    delivery=cache/'corresponding-source';delivery.mkdir(exist_ok=True)
    if HOST_OS=='Linux':
        # QEMU's configure imports PyYAML. Keep it out of the host interpreter by
        # building a throwaway venv; wheel bootstrapping uses the engine python.
        try:
            run([builder,'-c','import yaml'],stderr=subprocess.DEVNULL)
        except subprocess.CalledProcessError:
            qemu_venv=cache/'qemu-venv'
            if not (qemu_venv/'bin/python').exists():
                run([builder,'-m','venv',str(qemu_venv)])
                run([qemu_venv/'bin/python','-m','pip','install','--quiet','PyYAML'],env=env)
            builder_py=qemu_venv/'bin/python'
        else:
            builder_py=builder
    else:
        builder_py=builder
    q_archive=download(f'https://download.qemu.org/qemu-{QEMU_VERSION}.tar.xz',downloads/f'qemu-{QEMU_VERSION}.tar.xz')
    p_archive=download(f'https://www.python.org/ftp/python/{PYTHON_VERSION}/Python-{PYTHON_VERSION}.tar.xz',downloads/f'Python-{PYTHON_VERSION}.tar.xz')
    qsource=sources/f'qemu-{QEMU_VERSION}'; psource=sources/f'Python-{PYTHON_VERSION}'
    for archive,source in ((q_archive,qsource),(p_archive,psource)):
        if not (source/'.azki-extracted').exists():
            unpack(archive,sources)
            (source/'.azki-extracted').touch()
    copytree(INPUT/'qemu-overlay',qsource)
    qbuild=cache/'qemu-build';qbuild.mkdir(exist_ok=True)
    if not (qbuild/'build.ninja').exists():
        run([qsource/'configure','--target-list=arm-softmmu','--with-devices-arm=mi-emu','--without-default-devices',
             '--disable-docs','--disable-tools','--disable-guest-agent','--disable-gtk','--disable-sdl','--disable-vnc',
             '--disable-opengl','--disable-werror','--disable-download','--audio-drv-list=','--disable-slirp',
             '--python='+str(builder_py)],cwd=qbuild,env=env)
    run(['ninja','-C',qbuild,'-j',args.jobs,'qemu-system-arm'],env=env)
    # Build a non-framework Python with its normal prefix-relative layout.
    # No user Python, Homebrew, pip or network is needed on the target Mac.
    pbuild=cache/'python-build';pbuild.mkdir(exist_ok=True)
    pyinstall=cache/'python-install'
    if HOST_OS=='Darwin':
        python_flags=[
            '--prefix='+str(pyinstall),'--with-ensurepip=no','--disable-test-modules',
            '--with-openssl='+output([brew,'--prefix','openssl@3']),
            # SDK unistd.h can advertise pipe2 while libsystem_kernel only
            # exports pipe; the resulting binary SIGSEGV at the first os.pipe().
            'ac_cv_func_pipe2=no',
        ]
    else:
        # glibc has a real pipe2. ensurepip lets the freshly installed 3.12
        # build its own wheels (cp312 ABI match); pip is pruned from the
        # delivered engine copy below.
        python_flags=[
            '--prefix='+str(pyinstall),'--with-ensurepip=install','--disable-test-modules',
            '--with-openssl=/usr',
        ]
    stamp=pbuild/'.azki-configure'
    needed='\n'.join(python_flags)+'\n'
    if not stamp.exists() or stamp.read_text()!=needed:
        run([psource/'configure',*python_flags],cwd=pbuild,env=env)
        stamp.write_text(needed)
    run(['make','-j',args.jobs],cwd=pbuild,env=env)
    run(['make','install'],cwd=pbuild,env=env)
    wheels=cache/'wheels';wheels.mkdir(exist_ok=True)
    package_sources=delivery/'python-packages';package_sources.mkdir(exist_ok=True)
    # On Linux wheels must be compiled by the same 3.12 that ships in the
    # engine, not by the (possibly newer) host python3 used to bootstrap.
    wheel_builder=pyinstall/'bin/python3.12' if HOST_OS=='Linux' else builder
    for package,version in PACKAGES.items():
        # Reuse a previously built wheel for this exact package/version so the
        # PyPI metadata fetch, sdist download and native build are all skipped.
        cached=find_cached_wheel(wheels,package,version)
        if cached:
            print(f'Reusing cached wheel {cached.name}', flush=True)
            continue
        # Always deliver the pinned source and build it with this recipe.  A
        # cached wheel may use different optional features or dependencies and
        # is not a substitute for its corresponding source distribution.
        metadata=json.loads(urllib.request.urlopen(f'https://pypi.org/pypi/{package}/{version}/json').read())
        item=next(x for x in metadata['urls'] if x['packagetype']=='sdist')
        source=download(pypi_artifact_urls(item['url']),package_sources/item['filename'],
                        sha256=(item.get('digests') or {}).get('sha256'))
        command=[wheel_builder,'-m','pip','wheel','--no-deps','--wheel-dir',wheels,source]
        if package=='pillow':
            for feature in ('jpeg','tiff','freetype','lcms','webp','jpeg2000','raqm','imagequant','xcb','avif'):
                command+=['--config-settings',feature+'=disable']
        run(command,env=env)
    engine.mkdir(parents=True)
    copytree(INPUT/'project',engine/'project')
    copytree(pyinstall/'lib/python3.12',engine/'python/lib/python3.12')
    (engine/'python/bin').mkdir(parents=True)
    shutil.copy2(pyinstall/'bin/python3.12',engine/'python/bin/python3.12')
    if HOST_OS=='Linux':
        # ensurepip was enabled only so this interpreter could build its own
        # wheels; the delivered engine must not contain pip tooling.
        doomed=[p for p in (engine/'python').rglob('*') if p.name in ('pip','setuptools','wheel') and p.is_dir()]
        for p in doomed: shutil.rmtree(p, ignore_errors=True)
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
    if HOST_OS=='Linux':
        # glibc already typedefs loff_t (64-bit off_t) and time_t is 64-bit;
        # the Darwin-only overrides would collide or truncate on the timestamp
        # the guest's boot clock reads from persist.db.
        definitions=tuple(d for d in definitions if d!='CONFIG_YAFFS_USE_32_BIT_TIME_T')
        yaffs_extra=()
    else:
        yaffs_extra=('-Dloff_t=int64_t',)
    run(['cc','-O2',*['-D'+x for x in definitions],*yaffs_extra,'-I'+str(y/'core'),'-I'+str(y/'direct'),
         INPUT/'project/backend/src/yaffs_image.c',*sorted((y/'core').glob('*.c')),
         *[y/'direct'/n for n in ('yaffsfs.c','yaffs_attribs.c','yaffs_error.c','yaffs_hweight.c')],'-o',build/'yaffs-image'],env=env)
    run(['cc','-O2','-I'+str(u),INPUT/'project/backend/src/legacy_ui_db.c',u/'unqlite.c','-o',build/'legacy-ui-db'],env=env)
    # Reads/updates persist.db inside an instance YAFFS image (settings and boot clock).
    run(['cc','-O2',*['-D'+x for x in definitions],*yaffs_extra,'-I'+str(y/'core'),'-I'+str(y/'direct'),'-I'+str(u),
         INPUT/'project/backend/src/instance_props.c',*sorted((y/'core').glob('*.c')),
         *[y/'direct'/n for n in ('yaffsfs.c','yaffs_attribs.c','yaffs_error.c','yaffs_hweight.c')],u/'unqlite.c',
         '-o',build/'instance-props'],env=env)
    shutil.copy2(INPUT/'bootstrap.py',engine/'bootstrap.py')
    copytree(INPUT/'licenses',engine/'licenses')
    shutil.copy2(INPUT/'THIRD_PARTY.md',engine/'licenses/THIRD_PARTY.md')
    for name,path in [('python',psource/'LICENSE'),('yaffs',y/'LICENSE'),('unqlite',u/'LICENSE')]:
        d=engine/'licenses'/name;d.mkdir(exist_ok=True);shutil.copy2(path,d/'LICENSE')
    system_libraries=[]
    if HOST_OS=='Darwin':
        formulas=relocate(engine,brew,delivery)
    else:
        formulas=[]
        system_libraries,_library_table=linux_verify(engine)
        # System libraries are host-provided on Linux; record the exact
        # versions this artifact was linked against for the source offer and
        # the ABI floor, and keep upstream license text inside Engine/licenses.
        upstream={'glib':('https://gitlab.gnome.org/GNOME/glib','LGPL-2.1-or-later'),
                  'pixman':('https://gitlab.freedesktop.org/pixman/pixman','MIT'),
                  'zlib':('https://zlib.net','Zlib'),
                  'libffi':('https://github.com/libffi/libffi','MIT'),
                  'pcre2':('https://github.com/PCRE2Project/pcre2','BSD-3-Clause'),
                  'zstd':('https://github.com/facebook/zstd','BSD-2-Clause'),
                  'openssl':('https://www.openssl.org','Apache-2.0'),
                  'dtc':('https://gitlab.com/libfdt/dtc','GPL-2.0-or-later')}
        records=[]
        for name,pc in tuple(upstream.items()):
            try: version=output(['pkg-config','--modversion',pc])
            except Exception: version=None
            records.append({'name':name,'version':version,'upstream':upstream[name][0],'license':upstream[name][1]})
        (delivery/'DEPENDENCIES.json').write_text(json.dumps(
            {'host':{'os':'linux','arch':HOST_ARCH},
             'note':'Dynamic links resolve against the build host system libraries; obtain exact source from the distribution of the build host (dev/CI distribution, not a cross-distro package).',
             'libraries':records,'runtimeSonames':system_libraries},indent=2)+'\n')
    # Import smoke checks exercise newly built host libraries, never firmware.
    smoke="import sys; from pathlib import Path; assert Path(sys.prefix).resolve()==Path(sys.executable).resolve().parent.parent; sys.path.insert(0,sys.argv[1]); import PIL.Image, littlefs, ctypes, socket, zlib; print('Engine Python imports ready')"
    run([engine/'python/bin/python3.12','-I','-S','-B','-c',smoke,site],env={**env,'PYTHONDONTWRITEBYTECODE':'1'})
    run([build/'qemu-system-arm','--version'],env=env)
    if HOST_OS=='Linux':
        # The Darwin pipeline keeps its historical compile-only acceptance; the
        # Linux toolchain is what the loff_t/time_t break would first hit.
        c_tool_smoke(build,env)
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
      'signing':({'developerIDSigned':False,'notarized':False,'adHoc':True} if HOST_OS=='Darwin'
                 else {'developerIDSigned':False,'notarized':False,'adHoc':False,'scheme':'manifest-sha256'}),
      'executionVerified':False,
      'build':{'qemu':QEMU_VERSION,'python':PYTHON_VERSION,'packages':PACKAGES,'nativeLibraries':formulas},
      **capabilities}
    if HOST_OS=='Linux':
        manifest['host']={'os':'linux','arch':HOST_ARCH}
        manifest['distribution']['runtimeLibraries']=system_libraries
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
