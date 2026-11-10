"""Private synthetic guest identity and fresh NOR seed, using real Win32 files.

A seed permits an original guest attempt. It does not prove guest acceptance,
application authentication, raw radio control, or a hardware family's HCI support.
Original AP bytes are never edited here. P65 requires a separate boot-copy
build.prop identity overlay; callers must perform and disclose that step.
"""
from __future__ import annotations

import copy
import ctypes
from ctypes import wintypes as w
import hashlib
import json
from pathlib import Path
import threading
import uuid

from .instance_data import validate_nor_geometry
from .windows_runtime_files import FILE_INFORMATION, WindowsFiles
from .virtual_peer.state import (boot_properties, canonical_uuid, config_image,
                                 create_state, descriptor_for, encode_state,
                                 factory_image, strict_json, validate_state)


def _sha(data):
    return hashlib.sha256(data).hexdigest()


def _immutable(state):
    value = copy.deepcopy(validate_state(state))
    value['binding'] = None
    value.pop('guestLinkBond', None)
    return _sha(encode_state(value))


class WindowsGuestState:
    """Lifetime owner of an exclusive private state transaction, never repr secrets."""
    def __init__(self, recipe, board, private_root, *, instance_id=None, skip_oobe=True, api=None):
        if type(skip_oobe) is not bool:
            raise ValueError('skip_oobe must be boolean')
        if type(recipe) is not dict or type(recipe.get('ap')) is not dict:
            raise ValueError('Exact catalog recipe required')
        self.api = api or WindowsFiles()
        self.lock = threading.RLock()
        self.handles = []
        self.closed = False
        self.profile_id = recipe['id']
        self.ap_sha256 = recipe['ap']['sha256']
        self.descriptor = descriptor_for(self.profile_id, self.ap_sha256)
        self.geometry = validate_nor_geometry(board, ap_sha256=self.ap_sha256)
        if (self.descriptor['apSHA256'] != self.ap_sha256 or self.geometry['profileID'] != self.profile_id or self.geometry['apSHA256'] != self.ap_sha256 or
                board['factory_bytes'] != self.descriptor['factoryBytes'] or
                self.geometry.get('configBytes', 0) != self.descriptor['configBytes']):
            raise ValueError('Factory descriptor differs from exact board/profile geometry')
        self.root = self.api.local_path(private_root)
        self.state_path = self.root / 'virtual-peer-state.json'
        self.nor_path = self.root / 'guest-nor.bin'
        self.manifest_path = self.root / 'guest-state.json'
        self._bind_api()
        try:
            # Pin existing ancestors against replacement, then create/pin our directory.
            if not self.root.parent.is_dir():
                raise ValueError('Guest-state parent must already exist')
            current = Path(self.root.anchor)
            self.handles.append(self.api.open_read(current, directory=True, require_private=False))
            for part in self.root.parent.parts[1:]:
                current /= part
                self.handles.append(self.api.open_read(current, directory=True, require_private=False))
            self.api.private_directory(self.root, exist_ok=True)
            self.handles.append(self.api.open_read(self.root, directory=True))
            self._lock_file()
            found = [p.exists() for p in (self.state_path, self.nor_path, self.manifest_path)]
            if any(found) and not all(found):
                raise ValueError('Incomplete guest-state transaction; existing files were preserved')
            if not any(found):
                if set(p.name for p in self.root.iterdir()) != {'guest-state.lock'}:
                    raise ValueError('Fresh guest-state directory contains unexpected files')
                selected = str(uuid.uuid4()) if instance_id is None else instance_id
                if not canonical_uuid(selected):
                    raise ValueError('Canonical instance UUID required')
                self._state = create_state(selected, profile_id=self.profile_id, ap_sha256=self.ap_sha256)
                factory = factory_image(self._state, skip_oobe=skip_oobe)
                config = config_image(self._state) if self.descriptor['configBytes'] else b''
                size = self.geometry['bytes']
                tail = size - self.geometry.get('configBytes', 0)
                offset = self.geometry['factoryOffset'] - self.geometry['offset']
                if len(factory) != self.descriptor['factoryBytes'] or not 0 <= offset < offset + len(factory) <= tail:
                    raise ValueError('Factory seed does not fit compact NOR tail')
                if len(config) != size - tail:
                    raise ValueError('Config seed differs from compact NOR extent')
                nor = bytearray(b'\xff' * size)
                nor[offset:offset + len(factory)] = factory
                nor[tail:] = config
                self.factory_seed_sha256 = _sha(factory)
                self._manifest = {'schema': 1, 'instanceID': selected, 'profileID': self.profile_id,
                                  'apSHA256': self.ap_sha256, 'norGeometry': self.geometry,
                                  'immutableStateSHA256': _immutable(self._state),
                                  'initialNorSHA256': _sha(nor),
                                  'factorySeedSHA256': self.factory_seed_sha256, 'skipOOBE': skip_oobe,
                                  'guestAcceptanceVerified': False}
                self._create(self.state_path, encode_state(self._state))
                self._create(self.nor_path, nor)
                self._create(self.manifest_path, json.dumps(self._manifest, sort_keys=True, separators=(',', ':')).encode('ascii'))
            else:
                manifest = strict_json(self._read(self.manifest_path, 8192))
                state = validate_state(strict_json(self._read(self.state_path, 4096)))
                required = {'schema', 'instanceID', 'profileID', 'apSHA256', 'norGeometry',
                            'immutableStateSHA256', 'initialNorSHA256', 'factorySeedSHA256',
                            'skipOOBE', 'guestAcceptanceVerified'}
                if (type(manifest) is not dict or set(manifest) != required or
                        type(manifest['schema']) is not int or manifest['schema'] != 1 or
                        type(manifest['skipOOBE']) is not bool or manifest['skipOOBE'] != skip_oobe or
                        manifest['guestAcceptanceVerified'] is not False or
                        manifest['profileID'] != self.profile_id or manifest['apSHA256'] != self.ap_sha256 or
                        manifest['norGeometry'] != self.geometry or
                        manifest['instanceID'] != state['instanceID'] or
                        state['profileID'] != self.profile_id or state['apSHA256'] != self.ap_sha256 or
                        (instance_id is not None and instance_id != state['instanceID']) or
                        manifest['immutableStateSHA256'] != _immutable(state)):
                    raise ValueError('Private identity/manifest does not match exact selected firmware')
                for key in ('initialNorSHA256', 'factorySeedSHA256'):
                    value = manifest[key]
                    if type(value) is not str or len(value) != 64 or any(c not in '0123456789abcdef' for c in value):
                        raise ValueError('Invalid immutable seed digest')
                h = self.api.open_read(self.nor_path, share_write=False)
                try:
                    if self.api.information(h)[2] != self.geometry['bytes']:
                        raise ValueError('Existing NOR geometry mismatch; no reseed attempted')
                finally:
                    self.api.close(h)
                self._state, self._manifest = state, manifest
                self.factory_seed_sha256 = manifest['factorySeedSHA256']
            self.state_sha256 = _sha(self._read(self.state_path, 4096))
            self.instance_id = self._state['instanceID']
            self.boot_properties = boot_properties(self._state)
            self.requires_boot_property_overlay = self.descriptor['family'] == 'p65'
        except BaseException:
            self.close()
            raise

    def _bind_api(self):
        k = self.api.kernel
        k.ReadFile.argtypes = [w.HANDLE, w.LPVOID, w.DWORD, ctypes.POINTER(w.DWORD), w.LPVOID]
        k.ReadFile.restype = w.BOOL
        k.WriteFile.argtypes = [w.HANDLE, w.LPVOID, w.DWORD, ctypes.POINTER(w.DWORD), w.LPVOID]
        k.WriteFile.restype = w.BOOL
        k.FlushFileBuffers.argtypes = [w.HANDLE]
        k.FlushFileBuffers.restype = w.BOOL
        k.ReplaceFileW.argtypes = [w.LPCWSTR, w.LPCWSTR, w.LPCWSTR, w.DWORD, w.LPVOID, w.LPVOID]
        k.ReplaceFileW.restype = w.BOOL

    def _lock_file(self):
        path = self.root / 'guest-state.lock'
        attrs, sd = self.api.security_attributes()
        try:
            h = self.api.kernel.CreateFileW(str(path), 0xc0020000, 0, ctypes.byref(attrs), 4, 0x00200000, None)
        finally:
            self.api.kernel.LocalFree(sd)
        if h == self.api.INVALID:
            raise self.api.error('Guest state is locked or inaccessible')
        try:
            info = FILE_INFORMATION()
            if not self.api.kernel.GetFileInformationByHandle(h, ctypes.byref(info)):
                raise self.api.error('Inspect guest-state lock')
            if info.attributes & (0x10 | 0x400) or info.links != 1 or self.api.kernel.GetFileType(h) != 1:
                raise PermissionError('Unsafe guest-state lock')
            self.api.check_private(h)
            self.handles.append(h)
        except BaseException:
            self.api.close(h)
            raise

    def _read(self, path, maximum):
        h = self.api.open_read(path, share_write=False)
        try:
            size = self.api.information(h)[2]
            if not 1 <= size <= maximum:
                raise ValueError('Private guest-state byte bound')
            buffer = ctypes.create_string_buffer(size + 1)
            count = w.DWORD()
            if not self.api.kernel.ReadFile(h, buffer, size + 1, ctypes.byref(count), None):
                raise self.api.error('Read private guest state')
            if count.value != size:
                raise ValueError('Private guest-state size changed')
            return buffer.raw[:size]
        finally:
            self.api.close(h)

    def _create(self, path, data):
        attrs, sd = self.api.security_attributes()
        try:
            h = self.api.kernel.CreateFileW(str(path), 0xc0020000, 0, ctypes.byref(attrs), 1, 0x00200000, None)
        finally:
            self.api.kernel.LocalFree(sd)
        if h == self.api.INVALID:
            raise self.api.error('Create private guest-state file')
        try:
            self.api.check_private(h)
            buffer = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
            count = w.DWORD()
            if not self.api.kernel.WriteFile(h, buffer, len(data), ctypes.byref(count), None) or count.value != len(data):
                raise self.api.error('Write private guest-state file')
            if not self.api.kernel.FlushFileBuffers(h):
                raise self.api.error('Flush private guest-state file')
        finally:
            self.api.close(h)

    def _write_bond(self, candidate):
        with self.lock:
            if self.closed:
                raise RuntimeError('Private guest state is closed')
            value = validate_state(copy.deepcopy(candidate))
            if _immutable(value) != self._manifest['immutableStateSHA256']:
                raise ValueError('Bond update attempted to replace private guest identity')
            current = self._read(self.state_path, 4096)
            if _sha(current) != self.state_sha256:
                raise ValueError('Private state changed outside this owner; no overwrite attempted')
            data = encode_state(value)
            temporary = self.root / ('state-pending-' + uuid.uuid4().hex + '.json')
            self._create(temporary, data)
            if not self.api.kernel.ReplaceFileW(str(self.state_path), str(temporary), None, 0, None, None):
                raise self.api.error('Replace private guest bond; pending private file preserved')
            readback = self._read(self.state_path, 4096)
            if readback != data:
                raise ValueError('Private bond readback mismatch')
            self._state = value
            self.state_sha256 = _sha(data)
            return self.state_sha256

    def new_peer(self, *, run_directory=None):
        with self.lock:
            if self.closed:
                raise RuntimeError('Private guest state is closed')
            from .windows_guest_peer import WindowsGuestPeer
            return WindowsGuestPeer(copy.deepcopy(self._state), api=self.api, run_directory=run_directory,
                                    bond_writer=self._write_bond)

    def close(self):
        with self.lock:
            if not self.closed:
                for handle in reversed(self.handles):
                    self.api.close(handle)
                self.handles.clear()
                self.closed = True


def create_windows_guest_state(recipe, board, private_root, *, instance_id=None, skip_oobe=True, api=None):
    return WindowsGuestState(recipe, board, private_root, instance_id=instance_id, skip_oobe=skip_oobe, api=api)
