"""Transparent P65 local-setup AP copies; never mutate an original AP.

Only skipOOBE=True uses the already existing p65_setup transformation. Keeping
OOBE while injecting identity needs a distinct reviewed recipe and is rejected.
The file transaction uses protected CREATE_NEW Win32 output, source write/delete
sharing denial, exact hashes and structural readback. No guest code is executed.
"""
from __future__ import annotations

import ctypes
import hashlib
import os
from pathlib import Path
import re

import p65_setup
from .windows_runtime_files import WindowsFiles

MAX_AP_BYTES = 64 * 1024 * 1024
IDENTITY_KEYS = {'ro.factory.psn', 'ro.factory.did'}


def _hash(data):
    return hashlib.sha256(data).hexdigest()


def _contract(model, skip_oobe, identity_properties):
    if model != 'miwear.watch.p65':
        raise ValueError('This boot-copy recipe is only for miwear.watch.p65')
    if type(skip_oobe) is not bool:
        raise ValueError('skipOOBE must be boolean')
    if not skip_oobe:
        raise ValueError('P65 boot-copy requires skipOOBE=True; OOBE-preserving identity injection is unsupported')
    if identity_properties is None:
        return None
    if type(identity_properties) is not dict or set(identity_properties) != IDENTITY_KEYS:
        raise ValueError('Only the complete synthetic P65 serial/DID pair may be injected')
    serial, did = identity_properties['ro.factory.psn'], identity_properties['ro.factory.did']
    if (type(serial) is not str or not re.fullmatch(r'AZKI-P65-[0-9A-F]{12}', serial) or
            type(did) is not str or not re.fullmatch(r'99[0-9]{11}', did)):
        raise ValueError('Synthetic P65 instance identity required')
    return dict(identity_properties)


def _p65_header(original):
    match = re.search(rb'(?<![A-Z_])CHIP=([^\r\n\x00 ]+)', original)
    if match is None:
        raise ValueError('P65 AP requires its BES build header')
    block = original[match.start():match.start() + 900].split(b'\0', 1)[0]
    fields = dict(re.findall(rb'(?:^|\n)([A-Z_0-9]+)=([^\r\n]+)', block))
    try:
        valid = (fields[b'CHIP'] == b'best1502p' and fields[b'KERNEL'] == b'NUTTX' and
                 int(fields[b'OTA_CODE_OFFSET'], 0) == 0xc0000 and
                 int(fields[b'FLASH_SIZE'], 0) == 0x1000000)
    except (KeyError, ValueError):
        valid = False
    if not valid:
        raise ValueError('P65 boot-copy requires the BEST1502P 0xc0000/16MiB AP layout')


def _property_content(image, base, inode):
    return image[base + inode['data']:base + inode['data'] + inode['length']]


def build_p65_boot_copy(original, *, expected_source_sha256, model, skip_oobe,
                        identity_properties=None):
    """Pure transformation, testable on synthetic AP/ROMFS bytes on any OS.

    originalAPUnchanged remains unknown here: a bytes operation cannot prove a
    separate file's state. write_windows_boot_copy verifies that file explicitly.
    Identity values and original property contents are never included in receipt.
    """
    properties = _contract(model, skip_oobe, identity_properties)
    if (type(original) is not bytes or not 512 <= len(original) <= MAX_AP_BYTES or
            type(expected_source_sha256) is not str or
            not re.fullmatch('[0-9a-f]{64}', expected_source_sha256)):
        raise ValueError('Immutable bounded AP bytes and exact source SHA256 required')
    source_sha = _hash(original)
    if source_sha != expected_source_sha256:
        raise ValueError('Source AP differs from selected firmware hash')
    _p65_header(original)
    base, old_size, old_build, old_content = p65_setup._locate_romfs(original)
    if b'ro.product.device.devicetype=watch' not in _property_content(original, base, old_content).splitlines():
        raise ValueError('P65 ROMFS build.prop is not a watch image')
    output = p65_setup.local_boot_image(original, properties)
    if type(output) is not bytes or len(output) != len(original):
        raise ValueError('Boot copy changed AP byte geometry')
    new_base, new_size, new_build, new_content = p65_setup._locate_romfs(output)
    if (new_base != base or new_size <= old_size or new_build['offset'] != old_build['offset'] or
            new_build['kind'] != 0 or new_content['kind'] != 2 or new_content['offset'] < old_size):
        raise ValueError('Boot-copy ROMFS relink/readback is invalid')
    parsed = {}
    for line in _property_content(output, base, new_content).splitlines():
        if b'=' in line:
            key, value = line.split(b'=', 1)
            parsed[key] = value
    required = dict(p65_setup.LOCAL_PROPERTIES)
    required.update(properties or {})
    if any(parsed.get(key.encode('ascii')) != value.encode('ascii') for key, value in required.items()):
        raise ValueError('Boot-copy properties failed structural readback')
    appended_begin = base + new_content['offset']
    appended_end = base + ((new_content['data'] + new_content['length'] + 15) & ~15)
    if any(original[appended_begin:appended_end]):
        raise ValueError('Boot-copy appended inode overwrote nonzero original data')
    allowed = sorted([(base + 8, base + 16),
                      (base + old_build['offset'], base + old_build['offset'] + 16),
                      (appended_begin, appended_end)])
    cursor = 0
    for begin, end in allowed:
        if begin < cursor or not begin <= end <= len(original) or original[cursor:begin] != output[cursor:begin]:
            raise ValueError('Boot copy modified bytes outside the approved ROMFS recipe')
        cursor = end
    if original[cursor:] != output[cursor:] or _hash(original) != source_sha:
        raise ValueError('Boot copy changed non-recipe AP bytes')
    boot_sha = _hash(output)
    receipt = {'schema': 'azki.p65.local-setup-boot-copy.v1', 'model': model,
               'sourceAPHash': source_sha, 'sourceAPBytes': len(original),
               'bootCopySHA': boot_sha, 'bootCopyBytes': len(output),
               'bootCopyModified': boot_sha != source_sha, 'sourceBytesUnchanged': True,
               'originalAPUnchanged': None, 'skipOOBE': True,
               'identityPropertyNames': sorted(properties or {}),
               'localPropertyNames': sorted(p65_setup.LOCAL_PROPERTIES),
               'romfs': {'offset': base, 'originalBytes': old_size, 'bootBytes': new_size,
                         'changedRanges': [list(pair) for pair in allowed]},
               'structuralReadbackVerified': True, 'guestExecutionVerified': False}
    return output, receipt


def write_windows_boot_copy(source, destination, *, expected_source_sha256,
                            model, skip_oobe, identity_properties=None, api=None):
    """Write only a new private AP boot copy, returning transparent provenance.

    A failure preserves any private partial output for diagnosis. Existing
    destinations and source aliases are never truncated, replaced or deleted.
    """
    _contract(model, skip_oobe, identity_properties)
    import msvcrt
    api = api or WindowsFiles()
    source, destination = api.local_path(source), api.local_path(destination)
    if os.path.normcase(str(source)) == os.path.normcase(str(destination)):
        raise ValueError('Boot copy must differ from original AP')
    parent_handles, native_source, source_fd = [], None, None
    try:
        for parent in dict.fromkeys([source.parent, destination.parent]):
            parent_handles.append(api.open_read(parent, directory=True))
        native_source = api.open_read(source, share_write=False)
        source_identity = api.information(native_source)
        if not 512 <= source_identity[2] <= MAX_AP_BYTES:
            raise ValueError('Source AP size is outside the supported bound')
        source_fd = msvcrt.open_osfhandle(native_source, os.O_RDONLY | os.O_BINARY | os.O_NOINHERIT)
        native_source = None
        with os.fdopen(source_fd, 'rb') as stream:
            source_fd = None
            original = stream.read(MAX_AP_BYTES + 1)
            if len(original) != source_identity[2]:
                raise ValueError('Source AP byte count changed')
            payload, receipt = build_p65_boot_copy(original,
                expected_source_sha256=expected_source_sha256, model=model,
                skip_oobe=skip_oobe, identity_properties=identity_properties)
            attributes, descriptor = api.security_attributes()
            try:
                target = api.kernel.CreateFileW(str(destination), 0xc0020000, 0,
                    ctypes.byref(attributes), 1, 0x00200000, None)  # CREATE_NEW/OPEN_REPARSE_POINT
            finally:
                api.kernel.LocalFree(descriptor)
            if target == api.INVALID:
                raise api.error('Create new private P65 boot copy')
            try:
                api.check_private(target)
                target_fd = msvcrt.open_osfhandle(target, os.O_RDWR | os.O_BINARY | os.O_NOINHERIT)
            except BaseException:
                api.close(target)
                raise
            with os.fdopen(target_fd, 'w+b') as out:
                if out.write(payload) != len(payload):
                    raise OSError('Incomplete P65 boot-copy write')
                out.flush(); os.fsync(out.fileno()); out.seek(0)
                if _hash(out.read(MAX_AP_BYTES + 1)) != receipt['bootCopySHA']:
                    raise ValueError('P65 boot-copy disk readback mismatch')
                if api.information(msvcrt.get_osfhandle(out.fileno()))[2] != len(payload):
                    raise ValueError('P65 boot-copy disk size mismatch')
            stream.seek(0)
            if (_hash(stream.read(MAX_AP_BYTES + 1)) != receipt['sourceAPHash'] or
                    api.information(msvcrt.get_osfhandle(stream.fileno())) != source_identity):
                raise ValueError('Original P65 AP changed during preparation')
        receipt.update(sourceAPPath=str(source), bootCopyPath=str(destination),
                       originalAPUnchanged=True, diskReadbackVerified=True)
        return receipt
    finally:
        if source_fd is not None:
            os.close(source_fd)
        api.close(native_source)
        for handle in reversed(parent_handles):
            api.close(handle)
