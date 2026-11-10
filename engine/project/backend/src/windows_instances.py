"""Private, transactional Windows instance storage.

Accepted revisions are immutable. A core receives only session working copies;
NAND, OOB, compact NOR and virtual identity state advance together after an
explicit, healthy owned-process completion. This module never starts a core,
accesses a radio, or decides that a guest filesystem was cleanly unmounted.
"""
from __future__ import annotations

from contextlib import ExitStack, contextmanager
import copy
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import time
import uuid


SCHEMA = 1
MAX_JSON = 128 * 1024
MAX_IMAGE = 512 * 1024 * 1024
BLOCK = 131072
REVISION = re.compile(r"r-[0-9a-f]{32}\Z")
BATCH = re.compile(r"i-[0-9a-f]{32}\Z")
SESSION = re.compile(r"session-[0-9a-f]{32}\Z")
HASH = re.compile(r"[0-9a-f]{64}\Z")
MODEL = re.compile(r"[A-Za-z0-9_.@-]{1,128}\Z")
INPUT_LIMITS = {"firmware.bin": 768 * 1024**2, "original-ota.bin": 768 * 1024**2,
                "ap.bin": 64 * 1024**2, "resource.bin": MAX_IMAGE,
                "audio.bin": 16 * 1024**2, "sensor.bin": 16 * 1024**2}
FILE_LABELS = {"nand-data.bin", "nand-spare.bin", "nor-config.bin",
               "guest-state/guest-nor.bin", "guest-state/virtual-peer-state.json",
               "guest-state/guest-state.json", "raw-manufacturing.json"}
STATE = "guest-state/virtual-peer-state.json"
STATE_MANIFEST = "guest-state/guest-state.json"
STATE_NOR = "guest-state/guest-nor.bin"
HEALTH_FIELDS = {"ownedChildClosed", "exitCode", "cleanupComplete", "forced",
                 "guestFault", "qmpHealthy", "storageHealthy", "peerHealthy"}


class InstanceStorageError(RuntimeError):
    def __init__(self, code, message):
        self.code = code
        super().__init__(message)


def _uuid(value):
    if type(value) is not str:
        raise ValueError("Canonical instance UUID required")
    try:
        valid = str(uuid.UUID(value)) == value
    except ValueError:
        valid = False
    if not valid:
        raise ValueError("Canonical lowercase instance UUID required")
    return value


def _pairs(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError("Duplicate instance metadata field")
        value[key] = item
    return value


def _decode(raw):
    return json.loads(raw.decode("utf-8"), object_pairs_hook=_pairs,
                      parse_constant=lambda _: (_ for _ in ()).throw(ValueError("Nonfinite instance metadata")))


def _encode(value):
    raw = (json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True,
                      allow_nan=False) + "\n").encode("ascii")
    if len(raw) > MAX_JSON:
        raise ValueError("Instance metadata exceeds its bound")
    return raw


def _sha(raw):
    return hashlib.sha256(raw).hexdigest()


def _text(value, limit):
    return type(value) is str and 0 < len(value) <= limit and not any(ord(x) < 32 for x in value)


def _label(value, allowed):
    if (type(value) is not str or value not in allowed or
            PurePosixPath(value).is_absolute() or ".." in PurePosixPath(value).parts):
        raise ValueError("Unsupported instance file label")
    return value


def validate_binding(binding, *, model=None):
    """Validate the data-only adapter contract; do not derive a hardware layout."""
    value = _decode(_encode(binding))
    required = {"schema", "model", "profileID", "executionProfileID", "apSHA256",
                "boardGeometry", "firstRunSkipOOBE", "persistence", "expectedFiles"}
    if (type(value) is not dict or not required <= set(value) or value["schema"] != SCHEMA
            or type(value["schema"]) is not int or type(value["model"]) is not str or not MODEL.fullmatch(value["model"])
            or (model is not None and value["model"] != model)
            or not _text(value["profileID"], 512) or not _text(value["executionProfileID"], 512)
            or type(value["apSHA256"]) is not str or not HASH.fullmatch(value["apSHA256"])
            or type(value["boardGeometry"]) is not dict or type(value["firstRunSkipOOBE"]) is not bool):
        raise ValueError("Invalid exact-firmware instance binding")
    capability = value["persistence"]
    capability_keys = {"nand", "nor", "identity", "bindings", "volatileAreas"}
    if (type(capability) is not dict or not capability_keys <= set(capability)
            or set(capability) - capability_keys - {"manufacturingIdentity", "identityKind"}
            or any(type(capability[k]) is not bool for k in ("nand", "nor", "identity", "bindings"))
            or type(capability["volatileAreas"]) is not list or len(capability["volatileAreas"]) > 32
            or any(not _text(x, 128) for x in capability["volatileAreas"])
            or (capability["identity"] and not capability["nor"])
            or (capability["bindings"] and not capability["nor"])
            or ("manufacturingIdentity" in capability and type(capability["manufacturingIdentity"]) is not bool)
            or ("identityKind" in capability and capability["identityKind"] not in ("virtual", "manufacturing-only", "none"))):
        raise ValueError("Invalid persistence capabilities")
    domain = value.get("transportDomain")
    if domain is not None and domain not in ("virtual", "raw-usb"):
        raise ValueError("Unknown controller transport domain")
    if domain == "raw-usb" and capability["identity"]:
        raise ValueError("Raw controllers must not reuse virtual identity/SMP state")
    for key in ("controllerInstanceID", "addressSource"):
        if value.get(key) is not None and not _text(value[key], 512 if key == "controllerInstanceID" else 128):
            raise ValueError("Invalid controller identity metadata")
    if value.get("controllerAddress") is not None and (type(value["controllerAddress"]) is not str or
            not re.fullmatch(r"[0-9A-F]{2}(:[0-9A-F]{2}){5}", value["controllerAddress"])):
        raise ValueError("Invalid observed controller address")
    expected = value["expectedFiles"]
    if type(expected) is not dict or not 1 <= len(expected) <= len(FILE_LABELS):
        raise ValueError("Explicit writable file extents required")
    for name, extent in expected.items():
        _label(name, FILE_LABELS)
        if type(extent) is not dict or set(extent) not in ({"bytes"}, {"maxBytes"}):
            raise ValueError("Each file needs exact bytes or a bounded JSON maximum")
        size = next(iter(extent.values()))
        if type(size) is not int or not 0 < size <= MAX_IMAGE:
            raise ValueError("Invalid writable file size")
        if "maxBytes" in extent and (name not in (STATE, STATE_MANIFEST, "raw-manufacturing.json") or size > (4096 if name == STATE else 8192)):
            raise ValueError("Variable extents are limited to bounded guest-state JSON")
    geometry = value["boardGeometry"]
    nand = {"nand-data.bin", "nand-spare.bin"}
    if capability["nand"]:
        offset, size = geometry.get("nand-data-offset"), geometry.get("nand-data-bytes")
        if (type(offset) is not int or type(size) is not int or offset < 0 or size <= 0
                or (offset | size) & (BLOCK - 1) or offset + size > MAX_IMAGE
                or expected.get("nand-data.bin") != {"bytes": size}
                or expected.get("nand-spare.bin") != {"bytes": size // 2048 * 64}):
            raise ValueError("NAND/OOB files differ from exact board geometry")
    elif set(expected) & nand:
        raise ValueError("RAM-only NAND must not be advertised through saved dummy files")
    nor_labels = {"nor-config.bin", STATE_NOR} & set(expected)
    if capability["nor"]:
        layout = value.get("norGeometry")
        if (type(layout) is not dict or len(nor_labels) != 1 or
                layout.get("profileID") != value["executionProfileID"] or layout.get("apSHA256") != value["apSHA256"]):
            raise ValueError("Exact-profile NOR descriptor required")
        size = layout.get("bytes")
        if type(size) is not int or not 0 < size <= 16 * 1024**2 or expected[next(iter(nor_labels))] != {"bytes": size}:
            raise ValueError("Compact NOR file differs from its geometry")
    elif nor_labels:
        raise ValueError("NOR file supplied for an unsupported profile")
    identities = {STATE, STATE_MANIFEST}
    if capability["identity"]:
        if not identities | {STATE_NOR} <= set(expected):
            raise ValueError("Identity requires the complete guest-state/NOR triplet")
    elif set(expected) & identities:
        raise ValueError("Unadvertised private identity state")
    if "raw-manufacturing.json" in expected and (domain != "raw-usb" or capability["identity"]):
        raise ValueError("Manufacturing-only metadata belongs to the raw controller domain")
    if not capability["nand"] and not capability["nor"]:
        raise ValueError("This profile has no writable persistence backend")
    return value


def validate_health(health):
    if (type(health) is not dict or set(health) != HEALTH_FIELDS
            or any(type(health[k]) is not bool for k in HEALTH_FIELDS - {"exitCode"})
            or (health["exitCode"] is not None and type(health["exitCode"]) is not int)):
        raise ValueError("Exact trusted runtime completion fields required")
    return (health["ownedChildClosed"] and health["exitCode"] == 0 and health["cleanupComplete"]
            and not health["forced"] and not health["guestFault"] and health["qmpHealthy"]
            and health["storageHealthy"] and health["peerHealthy"])


def _immutable_state(raw, *, instance_id, binding):
    from .virtual_peer.state import validate_state, encode_state
    value = validate_state(_decode(raw))
    if (value["instanceID"] != instance_id or value["profileID"] != binding["executionProfileID"]
            or value["apSHA256"] != binding["apSHA256"] or value["identity"]["model"] != binding["model"]):
        raise ValueError("Private identity differs from this instance/firmware")
    value = copy.deepcopy(value)
    value["binding"] = None
    value.pop("guestLinkBond", None)
    return _sha(encode_state(value))


class _Overlapped(c.Structure):
    _fields_ = [("internal", c.c_size_t), ("high", c.c_size_t),
                ("offset", w.DWORD), ("offsetHigh", w.DWORD), ("event", w.HANDLE)]


class _PrivateFiles:
    def __init__(self, api=None):
        if os.name != "nt":
            raise InstanceStorageError("windows_required", "Persistent instances require real Windows APIs")
        from .windows_runtime_files import WindowsFiles
        self.api = api or WindowsFiles()
        self.api.initialize_private_creator()
        self.k = self.api.kernel
        self.k.ReadFile.argtypes = [w.HANDLE, w.LPVOID, w.DWORD, c.POINTER(w.DWORD), w.LPVOID]
        self.k.ReadFile.restype = w.BOOL
        self.k.WriteFile.argtypes = [w.HANDLE, w.LPVOID, w.DWORD, c.POINTER(w.DWORD), w.LPVOID]
        self.k.WriteFile.restype = w.BOOL
        self.k.FlushFileBuffers.argtypes = [w.HANDLE]
        self.k.FlushFileBuffers.restype = w.BOOL
        self.k.SetFilePointerEx.argtypes = [w.HANDLE, c.c_longlong, c.POINTER(c.c_longlong), w.DWORD]
        self.k.SetFilePointerEx.restype = w.BOOL
        self.k.LockFileEx.argtypes = [w.HANDLE, w.DWORD, w.DWORD, w.DWORD, w.DWORD, c.POINTER(_Overlapped)]
        self.k.LockFileEx.restype = w.BOOL
        self.k.MoveFileExW.argtypes = [w.LPCWSTR, w.LPCWSTR, w.DWORD]
        self.k.MoveFileExW.restype = w.BOOL
        self.k.DeleteFileW.argtypes = [w.LPCWSTR]
        self.k.DeleteFileW.restype = w.BOOL

    def close(self, handle):
        if handle is not None and not self.k.CloseHandle(handle):
            raise self.api.error("Close private instance handle")

    def close_all(self, handles):
        errors = []
        for handle in reversed(handles):
            try:
                self.close(handle)
            except OSError as error:
                errors.append(error)
        if errors:
            raise errors[0]

    @contextmanager
    def parents(self, path):
        path = self.api.local_path(path)
        handles = []
        try:
            current = Path(path.anchor)
            handles.append(self.api.open_read(current, directory=True, require_private=False))
            for part in path.parent.parts[1:]:
                current /= part
                handles.append(self.api.open_read(current, directory=True, require_private=False))
            yield path
        finally:
            self.close_all(handles)

    @contextmanager
    def source(self, path, *, maximum, exact=None, private=True, locked=False):
        with self.parents(path) as path:
            handle = self.api.open_read(path, require_private=private, share_write=False)
            try:
                info = self.api.information(handle)
                if not 0 < info[2] <= maximum or (exact is not None and info[2] != exact):
                    raise ValueError("Instance file differs from its exact extent")
                overlap = _Overlapped()
                if locked and not self.k.LockFileEx(handle, 3, 0, 0xffffffff, 0xffffffff, c.byref(overlap)):
                    raise self.api.error("Instance file is still in use")
                yield handle, info
                if self.api.information(handle) != info:
                    raise ValueError("Instance file changed during its held read")
            finally:
                self.close(handle)

    def seek(self, handle):
        actual = c.c_longlong()
        if not self.k.SetFilePointerEx(handle, 0, c.byref(actual), 0) or actual.value != 0:
            raise self.api.error("Seek private instance input")

    def chunks(self, handle, size):
        self.seek(handle)
        remaining = size
        while remaining:
            length = min(remaining, 1024 * 1024)
            buffer = c.create_string_buffer(length)
            count = w.DWORD()
            if not self.k.ReadFile(handle, buffer, length, c.byref(count), None):
                raise self.api.error("Read private instance input")
            if not 0 < count.value <= length:
                raise OSError("Short private instance read")
            remaining -= count.value
            yield buffer.raw[:count.value]

    def read(self, path, maximum=MAX_JSON):
        with self.source(path, maximum=maximum) as (handle, info):
            return b"".join(self.chunks(handle, info[2]))

    def new_handle(self, path):
        attrs, descriptor = self.api.security_attributes()
        try:
            handle = self.k.CreateFileW(str(path), 0xc0020000, 0, c.byref(attrs), 1, 0x00200000, None)
        finally:
            self.k.LocalFree(descriptor)
        if handle == self.api.INVALID:
            raise self.api.error("Create exclusive private instance file")
        try:
            self.api.check_private(handle)
            return handle
        except BaseException:
            self.close(handle)
            raise

    def write(self, handle, data):
        offset = 0
        while offset < len(data):
            part = data[offset:offset + 1024 * 1024]
            buffer = c.create_string_buffer(part)
            count = w.DWORD()
            if not self.k.WriteFile(handle, buffer, len(part), c.byref(count), None):
                raise self.api.error("Write private instance file")
            if not 0 < count.value <= len(part):
                raise OSError("Short private instance write")
            offset += count.value

    def write_new(self, path, raw):
        self.api.private_directory(Path(path).parent, exist_ok=True)
        with self.parents(path) as path:
            handle = self.new_handle(path)
            try:
                self.write(handle, raw)
                if not self.k.FlushFileBuffers(handle):
                    raise self.api.error("Flush private instance file")
            finally:
                self.close(handle)

    def move(self, source, target, *, replace=False):
        source, target = self.api.local_path(source), self.api.local_path(target)
        if source.drive.lower() != target.drive.lower():
            raise ValueError("Instance publication must stay on one local volume")
        # Do not allow cross-volume copy/delete fallback. The instance lock is
        # outside the movable directory; no own no-delete handle remains here.
        if not self.k.MoveFileExW(str(source), str(target), 8 | (1 if replace else 0)):
            raise self.api.error("Atomically publish private instance entry")

    def atomic_json(self, path, value):
        path = self.api.local_path(path)
        if path.exists():
            self.read(path)
        temporary = path.parent / (".metadata-" + uuid.uuid4().hex + ".json")
        raw = _encode(value)
        self.write_new(temporary, raw)
        self.move(temporary, path, replace=path.exists())
        try:
            if self.read(path) != raw:
                raise ValueError("Published instance metadata readback mismatch")
        except (OSError, ValueError) as error:
            raise InstanceStorageError("metadata_publication_unconfirmed",
                "Metadata entry was replaced but its readback was not confirmed") from error

    def json(self, path):
        return _decode(self.read(path))

    def unlink(self, path):
        path = self.api.local_path(path)
        if not self.k.DeleteFileW(str(path)):
            raise self.api.error("Remove settled private instance marker")

    def copy_handle(self, source, information, target, *, expected_sha=None):
        self.api.private_directory(Path(target).parent, exist_ok=True)
        digest = hashlib.sha256()
        with self.parents(target) as target:
            destination = self.new_handle(target)
            try:
                for chunk in self.chunks(source, information[2]):
                    digest.update(chunk)
                    self.write(destination, chunk)
                if not self.k.FlushFileBuffers(destination):
                    raise self.api.error("Flush copied instance image")
            finally:
                self.close(destination)
        if self.api.information(source) != information:
            raise ValueError("Private source identity changed while copying")
        identity = digest.hexdigest()
        if expected_sha is not None and identity != expected_sha:
            raise ValueError("Accepted instance contents differ from their recorded hash")
        with self.source(target, maximum=information[2], exact=information[2]) as (handle, info):
            actual = hashlib.sha256()
            for chunk in self.chunks(handle, info[2]):
                actual.update(chunk)
            if actual.hexdigest() != identity:
                raise ValueError("Instance copy disk readback mismatch")
        return {"bytes": information[2], "sha256": identity}

    def copy(self, source, target, *, maximum, exact=None, expected_sha=None, private=True):
        with self.source(source, maximum=maximum, exact=exact, private=private) as (handle, info):
            return self.copy_handle(handle, info, target, expected_sha=expected_sha)

    def tree_idle(self, root):
        """Validate, pin and release one bounded private tree; never follow links."""
        if not root.exists():
            return
        count = 0
        with ExitStack() as stack:
            def visit(directory, depth):
                nonlocal count
                if depth > 8:
                    raise ValueError("Unexpected instance directory depth")
                stack.callback(self.close, self.api.open_read(directory, directory=True))
                with os.scandir(directory) as entries:
                    for entry in entries:
                        count += 1
                        if count > 256:
                            raise ValueError("Unexpected instance tree size")
                        path = self.api.local_path(Path(entry.path))
                        attrs = self.k.GetFileAttributesW(str(path))
                        if attrs == 0xffffffff or attrs & 0x400:
                            raise PermissionError("Instance tree contains an inaccessible or reparse entry")
                        if attrs & 0x10:
                            visit(path, depth + 1)
                        else:
                            handle = self.api.open_read(path, share_write=False)
                            stack.callback(self.close, handle)
                            overlap = _Overlapped()
                            if not self.k.LockFileEx(handle, 3, 0, 0xffffffff, 0xffffffff, c.byref(overlap)):
                                raise self.api.error("Private instance tree is still in use")
            visit(root, 0)


class WindowsInstanceStore:
    """Lazy native store. Importing/constructing it is safe on a non-Windows host."""
    def __init__(self, workspace, api=None):
        self.workspace = Path(workspace).absolute()
        self.root = self.workspace / "instances"
        self._api = api
        self._files = None

    @property
    def fs(self):
        if self._files is None:
            self._files = _PrivateFiles(self._api)
        return self._files

    def _ensure_root(self, *, create=False):
        fs = self.fs
        fs.api.local_path(self.workspace)
        if not self.root.exists():
            if not create:
                return False
            fs.api.private_directory(self.workspace, exist_ok=True)
            fs.api.private_directory(self.root)
            fs.write_new(self.root / ".store.json", _encode({"schema": SCHEMA, "ownerSID": fs.api.sid}))
            fs.api.private_directory(self.root / ".locks")
            fs.api.private_directory(self.root / ".trash")
        handle = fs.api.open_read(self.root, directory=True)
        fs.close(handle)
        if fs.json(self.root / ".store.json") != {"schema": SCHEMA, "ownerSID": fs.api.sid}:
            raise InstanceStorageError("store_owner", "Instance store belongs to another owner")
        for name in (".locks", ".trash"):
            fs.api.private_directory(self.root / name, exist_ok=True)
        return True

    def create(self, profile_id, name, model):
        if not _text(profile_id, 512) or not _text(name, 80) or type(model) is not str or not MODEL.fullmatch(model):
            raise ValueError("A bounded instance name, profile and model are required")
        self._ensure_root(create=True)
        identity = str(uuid.uuid4())
        root = self.fs.api.private_directory(self.root / identity)
        info = {"schema": SCHEMA, "instanceID": identity, "ownerSID": self.fs.api.sid,
                "name": name, "model": model, "profileID": profile_id, "createdUTC": time.time()}
        self.fs.write_new(root / "instance.json", _encode(info))
        for directory in ("revisions", "inputs", ".quarantine", "receipts"):
            self.fs.api.private_directory(root / directory)
        return {"instanceID": identity, "created": True, "initialized": False, "name": name, "model": model}

    def lease(self, instance_id):
        self._ensure_root()
        return WindowsInstanceLease(self, _uuid(instance_id))

    def list(self):
        if not self._ensure_root():
            return {"instances": []}
        values = []
        for entry in sorted(self.root.iterdir(), key=lambda p: p.name):
            try:
                _uuid(entry.name)
            except ValueError:
                continue
            try:
                with self.lease(entry.name) as lease:
                    values.append(lease.public_info())
            except (OSError, ValueError) as error:
                # Do not inspect a live instance's mutable state without its lock.
                busy = getattr(error, "winerror", None) in (32, 33)
                public = {"instanceID": entry.name, "status": "inUse" if busy else "invalid", "initialized": None}
                if busy:
                    try:
                        info = self.fs.json(entry / "instance.json")
                        if (info.get("instanceID") == entry.name and info.get("ownerSID") == self.fs.api.sid
                                and _text(info.get("name"), 80) and type(info.get("model")) is str
                                and MODEL.fullmatch(info["model"])):
                            public.update(name=info["name"], model=info["model"], profileID=info.get("profileID"))
                    except (OSError, ValueError):
                        pass
                values.append(public)
        return {"instances": values}

    def delete(self, instance_id, expected_revision=None):
        with self.lease(instance_id) as lease:
            if lease.pending_path.exists():
                raise InstanceStorageError("recovery_required", "Recover the unpublished instance run before deleting it")
            head = lease.head()
            revision = head["revision"] if head else None
            if expected_revision is not None and expected_revision != revision:
                raise InstanceStorageError("revision_changed", "Instance revision changed before deletion")
            self.fs.tree_idle(lease.root)
            lease.release_directory_pin()
            target = self.root / ".trash" / (instance_id + "-" + uuid.uuid4().hex)
            self.fs.move(lease.root, target)
            return {"instanceID": instance_id, "deleted": True, "tombstone": target.name, "revision": revision}

    def recover(self, instance_id, operation="discardUnpublished"):
        if operation != "discardUnpublished":
            raise ValueError("Only explicit unpublished-work quarantine is supported")
        with self.lease(instance_id) as lease:
            return lease.recover()


class WindowsInstanceLease:
    def __init__(self, store, instance_id):
        self.store, self.fs, self.instance_id = store, store.fs, instance_id
        self.root = store.root / instance_id
        self.pending_path = self.root / "pending.json"
        self.lock = None
        self.directory_pin = None
        self.closed = False
        try:
            from .windows_runtime_files import WindowsCacheLock
            self.lock = WindowsCacheLock(store.root / ".locks" / (instance_id + ".lock"))
            self.lock.__enter__()
            self.directory_pin = self.fs.api.open_read(self.root, directory=True)
            self.info = self.fs.json(self.root / "instance.json")
            if (type(self.info) is not dict or type(self.info.get("schema")) is not int or self.info.get("schema") != SCHEMA
                    or self.info.get("instanceID") != instance_id or self.info.get("ownerSID") != self.fs.api.sid
                    or not MODEL.fullmatch(self.info.get("model", ""))
                    or not _text(self.info.get("name"), 80)):
                raise ValueError("Invalid private instance identity")
        except BaseException:
            self.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def _open(self):
        if self.closed:
            raise InstanceStorageError("instance_closed", "Instance lease is closed")

    def release_directory_pin(self):
        if self.directory_pin is not None:
            handle, self.directory_pin = self.directory_pin, None
            self.fs.close(handle)

    def close(self):
        errors = []
        try:
            self.release_directory_pin()
        except OSError as error:
            errors.append(error)
        if self.lock is not None and self.lock.handle is not None:
            handle, self.lock.handle = self.lock.handle, None
            try:
                self.fs.close(handle)
            except OSError as error:
                errors.append(error)
        self.closed = True
        if errors:
            raise errors[0]

    def _revision(self, value):
        if type(value) is not str or not REVISION.fullmatch(value):
            raise ValueError("Invalid instance revision")
        return self.root / "revisions" / value

    def _record(self):
        self._open()
        head_path = self.root / "head.json"
        if not head_path.exists():
            return None
        head = self.fs.json(head_path)
        if (type(head) is not dict or set(head) != {"schema", "revision", "manifestSHA256"}
                or type(head["schema"]) is not int or head["schema"] != SCHEMA):
            raise ValueError("Invalid instance HEAD")
        folder = self._revision(head["revision"])
        raw = self.fs.read(folder / "revision.json")
        if not HASH.fullmatch(str(head["manifestSHA256"])) or _sha(raw) != head["manifestSHA256"]:
            raise ValueError("Instance HEAD manifest hash mismatch")
        record = _decode(raw)
        if (type(record) is not dict or type(record.get("schema")) is not int or record.get("schema") != SCHEMA
                or record.get("instanceID") != self.instance_id or record.get("revision") != head["revision"]
                or not BATCH.fullmatch(record.get("inputBatch", ""))):
            raise ValueError("Revision does not belong to this instance")
        binding = validate_binding(record["binding"], model=self.info["model"])
        if set(record.get("files", {})) != set(binding["expectedFiles"]) or not 1 <= len(record.get("inputs", {})) <= len(INPUT_LIMITS):
            raise ValueError("Revision file inventory differs from its binding")
        for name, pin in record["inputs"].items():
            _label(name, INPUT_LIMITS)
            self._validate_pin(pin, INPUT_LIMITS[name])
            with self.fs.source(self.root / "inputs" / record["inputBatch"] / name,
                                maximum=pin["bytes"], exact=pin["bytes"]):
                pass
        for name, pin in record["files"].items():
            extent = binding["expectedFiles"][name]
            self._validate_pin(pin, extent.get("bytes", extent.get("maxBytes")), exact=extent.get("bytes"))
            with self.fs.source(folder / "files" / name, maximum=pin["bytes"], exact=pin["bytes"]):
                pass
        return record

    @staticmethod
    def _validate_pin(pin, maximum, *, exact=None):
        if (type(pin) is not dict or set(pin) - {"immutableSHA256"} != {"bytes", "sha256"}
                or type(pin["bytes"]) is not int or not 0 < pin["bytes"] <= maximum
                or (exact is not None and pin["bytes"] != exact)
                or type(pin["sha256"]) is not str or not HASH.fullmatch(pin["sha256"])
                or ("immutableSHA256" in pin and not HASH.fullmatch(str(pin["immutableSHA256"])) )):
            raise ValueError("Invalid accepted file pin")

    def head(self):
        record = self._record()
        if record is None:
            return None
        folder = self._revision(record["revision"])
        batch = self.root / "inputs" / record["inputBatch"]
        return {"revision": record["revision"], "binding": copy.deepcopy(record["binding"]),
                "inputs": {name: {**pin, "path": str(batch / name)} for name, pin in record["inputs"].items()},
                "files": {name: {**pin, "path": str(folder / "files" / name)} for name, pin in record["files"].items()}}

    def public_info(self):
        head = self.head()
        return {"instanceID": self.instance_id, "name": self.info["name"], "model": self.info["model"],
                "profileID": head["binding"]["profileID"] if head else self.info["profileID"],
                "initialized": head is not None, "headRevision": head["revision"] if head else None,
                "status": "pending" if self.pending_path.exists() else "ready" if head else "empty",
                "persistenceCapabilities": copy.deepcopy(head["binding"]["persistence"]) if head else None}

    def _no_pending(self):
        self._open()
        if self.pending_path.exists():
            raise InstanceStorageError("recovery_required", "An unpublished run is preserved; explicitly recover it before another run")

    def _content_pin(self, name, path, pin, binding, previous=None):
        if name in (STATE, STATE_MANIFEST, "raw-manufacturing.json"):
            raw = self.fs.read(path, 4096 if name == STATE else 8192)
            if type(_decode(raw)) is not dict:
                raise ValueError("Private guest state must be a JSON object")
            if name == STATE:
                immutable = _immutable_state(raw, instance_id=self.instance_id, binding=binding)
                if previous is not None and immutable != previous.get("immutableSHA256"):
                    raise ValueError("A bond update changed immutable private identity")
                pin["immutableSHA256"] = immutable
            else:
                if name == "raw-manufacturing.json":
                    value = _decode(raw)
                    allowed = {"schema", "instanceID", "model", "serial", "psn", "did", "factoryMAC",
                               "identityKind", "addressSource", "controllerInstanceID", "controllerAddress"}
                    if (set(value) - allowed or type(value.get("schema")) is not int or value["schema"] != SCHEMA
                            or value.get("instanceID") != self.instance_id or value.get("model") != binding["model"]
                            or any(item is not None and type(item) not in (str, int) for item in value.values())):
                        raise ValueError("Raw manufacturing metadata must not contain virtual keys or bonds")
                if previous is not None and pin["sha256"] != previous["sha256"]:
                    raise ValueError("Immutable initialization metadata changed")
        return pin

    def _verify_accepted(self, record):
        """Resolve uncertain post-publication retries from actual accepted bytes."""
        groups = ((self._revision(record["revision"]) / "files", record["files"]),
                  (self.root / "inputs" / record["inputBatch"], record["inputs"]))
        for folder, pins in groups:
            for name, expected in pins.items():
                with self.fs.source(folder / name, maximum=expected["bytes"], exact=expected["bytes"], locked=True) as (handle, info):
                    digest = hashlib.sha256()
                    for chunk in self.fs.chunks(handle, info[2]):
                        digest.update(chunk)
                    if digest.hexdigest() != expected["sha256"]:
                        raise ValueError("Published endpoint bytes differ from the revision receipt")

    def _publish(self, record):
        previous = self._record()
        folder = self._revision(record["revision"])
        raw = _encode(record)
        self.fs.write_new(folder / "revision.json", raw)
        try:
            self.fs.atomic_json(self.root / "head.json", {"schema": SCHEMA, "revision": record["revision"],
                                                           "manifestSHA256": _sha(raw)})
        except (OSError, ValueError, InstanceStorageError) as error:
            try:
                actual = self._record()
                if actual == record:
                    self._verify_accepted(actual)
                    return ["HEAD publication reported an error; the new accepted endpoint and all file hashes were independently verified"]
                if actual == previous and getattr(error, "code", None) != "metadata_publication_unconfirmed":
                    raise error
            except (OSError, ValueError, InstanceStorageError) as verification:
                if verification is error:
                    raise
                raise InstanceStorageError("commit_outcome_unknown",
                    "HEAD publication outcome is unknown; pending evidence and both revisions were preserved") from error
            raise InstanceStorageError("commit_outcome_unknown",
                "HEAD changed or was replaced without a verified endpoint; no rollback attempted") from error
        try:
            actual = self._record()
            if actual != record:
                raise ValueError("Published HEAD differs from the complete revision")
        except (OSError, ValueError, InstanceStorageError) as error:
            raise InstanceStorageError("commit_outcome_unknown", "Published HEAD could not be verified; no rollback attempted") from error
        return []

    def initialize(self, binding, inputs, initial_files):
        self._no_pending()
        if self._record() is not None:
            raise InstanceStorageError("already_initialized", "Existing accepted instance data is never reseeded")
        binding = validate_binding(binding, model=self.info["model"])
        if type(inputs) is not dict or not 1 <= len(inputs) <= len(INPUT_LIMITS) or type(initial_files) is not dict or set(initial_files) != set(binding["expectedFiles"]):
            raise ValueError("Complete immutable input and seed inventories required")
        for name in inputs:
            _label(name, INPUT_LIMITS)
        revision, batch = "r-" + uuid.uuid4().hex, "i-" + uuid.uuid4().hex
        pending = {"schema": SCHEMA, "kind": "initialize", "instanceID": self.instance_id,
                   "transactionID": revision, "baseRevision": None, "inputBatch": batch, "phase": "preparing"}
        self.fs.atomic_json(self.pending_path, pending)
        folder = self.fs.api.private_directory(self._revision(revision))
        input_root = self.fs.api.private_directory(self.root / "inputs" / batch)
        pins = {name: self.fs.copy(path, input_root / name, maximum=INPUT_LIMITS[name], private=False)
                for name, path in inputs.items()}
        files = {}
        for name, source in initial_files.items():
            extent = binding["expectedFiles"][name]
            target = folder / "files" / name
            pin = self.fs.copy(source, target, maximum=extent.get("bytes", extent.get("maxBytes")), exact=extent.get("bytes"))
            files[name] = self._content_pin(name, target, pin, binding)
        record = {"schema": SCHEMA, "instanceID": self.instance_id, "revision": revision, "previousRevision": None,
                  "inputBatch": batch, "inputs": pins, "files": files, "binding": binding, "origin": "initial-seed",
                  "consistency": "crash-consistent", "guestFilesystemClean": False}
        warnings = self._publish(record)
        if not warnings:
            self.fs.unlink(self.pending_path)
        result = self.head()
        if warnings:
            result.update(warnings=warnings, pendingPreserved=True)
        return result

    def begin_work(self, session):
        self._no_pending()
        record = self._record()
        if record is None:
            raise InstanceStorageError("not_initialized", "Initialize this instance once before starting it")
        session = self.fs.api.local_path(session)
        if session.parent != self.store.workspace or not SESSION.fullmatch(session.name):
            raise ValueError("Work must be in a fresh session directly inside this workspace")
        pin = self.fs.api.open_read(session, directory=True)
        self.fs.close(pin)
        root = session / "instance-data"
        if root.exists() or (session / "instance-work.json").exists():
            raise FileExistsError("Session already contains instance work")
        transaction = "r-" + uuid.uuid4().hex
        pending = {"schema": SCHEMA, "kind": "work", "instanceID": self.instance_id,
                   "transactionID": transaction, "baseRevision": record["revision"], "phase": "preparing",
                   "sessionID": session.name, "sessionRoot": str(session), "bindingSHA256": _sha(_encode(record["binding"]))}
        self.fs.atomic_json(self.pending_path, pending)
        self.fs.api.private_directory(root)
        files = {}
        for name, original in record["files"].items():
            source = self._revision(record["revision"]) / "files" / name
            copied = self.fs.copy(source, root / name, maximum=original["bytes"], exact=original["bytes"], expected_sha=original["sha256"])
            files[name] = {**original, **copied, "path": str(root / name)}
        work = {"schema": SCHEMA, "instanceID": self.instance_id, "transactionID": transaction,
                "baseRevision": record["revision"], "sessionID": session.name, "sessionRoot": str(session),
                "binding": copy.deepcopy(record["binding"]), "inputs": self.head()["inputs"], "files": files}
        self.fs.write_new(session / "instance-work.json", _encode(work))
        self.fs.atomic_json(self.pending_path, {**pending, "phase": "guest"})
        return work

    def _work(self, work):
        self._open()
        if (type(work) is not dict or work.get("schema") != SCHEMA or work.get("instanceID") != self.instance_id
                or not REVISION.fullmatch(work.get("transactionID", ""))
                or not REVISION.fullmatch(work.get("baseRevision", "")) or not SESSION.fullmatch(work.get("sessionID", ""))):
            raise ValueError("Invalid owned work transaction")
        session = self.fs.api.local_path(work["sessionRoot"])
        if session != self.store.workspace / work["sessionID"] or self.fs.json(session / "instance-work.json") != work:
            raise ValueError("Work does not match its private session receipt")
        return session

    def _receipt(self, work, committed, reason):
        return {"instanceID": self.instance_id, "transactionID": work["transactionID"],
                "sessionID": work["sessionID"], "committed": committed,
                "previousRevision": work["baseRevision"],
                "revision": work["transactionID"] if committed else work["baseRevision"],
                "consistency": "crash-consistent" if committed else "previous-snapshot-preserved",
                "guestFilesystemClean": False, "reason": reason}

    def commit_work(self, work, health):
        session = self._work(work)
        healthy = validate_health(health)
        record = self._record()
        if record and record["revision"] == work["transactionID"]:
            if record.get("sessionID") != work["sessionID"] or record.get("previousRevision") != work["baseRevision"]:
                raise ValueError("Published revision belongs to another transaction")
            self._verify_accepted(record)
            return self._receipt(work, True, "already-published")
        if record is None or record["revision"] != work["baseRevision"]:
            raise InstanceStorageError("revision_changed", "Accepted revision changed; no commit attempted")
        pending = self.fs.json(self.pending_path)
        if (pending.get("kind") != "work" or pending.get("instanceID") != self.instance_id
                or pending.get("transactionID") != work["transactionID"]
                or pending.get("baseRevision") != work["baseRevision"] or pending.get("sessionID") != work["sessionID"]
                or pending.get("sessionRoot") != str(session)
                or pending.get("bindingSHA256") != _sha(_encode(record["binding"]))
                or work["binding"] != record["binding"] or pending.get("phase") not in ("guest", "stopped")):
            raise ValueError("Pending storage transaction does not match this work")
        if not healthy:
            self.fs.atomic_json(self.pending_path, {**pending,
                "phase": "stopped" if health["ownedChildClosed"] else "guest", "health": health})
            return self._receipt(work, False, "owned-exit-or-health-not-confirmed")
        folder = self.fs.api.private_directory(self._revision(work["transactionID"]))
        files = {}
        with ExitStack() as stack:
            sources = {}
            for name, extent in record["binding"]["expectedFiles"].items():
                sources[name] = stack.enter_context(self.fs.source(session / "instance-data" / name,
                    maximum=extent.get("bytes", extent.get("maxBytes")), exact=extent.get("bytes"), locked=True))
            for name, (handle, info) in sources.items():
                target = folder / "files" / name
                pin = self.fs.copy_handle(handle, info, target)
                files[name] = self._content_pin(name, target, pin, record["binding"], record["files"][name])
        accepted = {**record, "revision": work["transactionID"], "previousRevision": record["revision"],
                    "origin": "healthy-owned-run", "sessionID": work["sessionID"], "files": files, "completion": health}
        publication_warnings = self._publish(accepted)
        receipt = self._receipt(work, True, "owned-core-exited-and-synchronous-storage-complete")
        # HEAD is the commit point. Failure to persist a redundant receipt must
        # not roll back or misreport an already verified accepted revision.
        warnings = list(publication_warnings)
        if publication_warnings:
            receipt.update(warnings=warnings, pendingPreserved=True)
        try:
            self.fs.write_new(self.root / "receipts" / (work["transactionID"] + ".json"), _encode(receipt))
        except (OSError, ValueError) as error:
            warnings.append("receipt: " + str(error)[:256])
        if not publication_warnings:
            try:
                self.fs.unlink(self.pending_path)
            except OSError as error:
                warnings.append("pending: " + str(error)[:256])
        if warnings:
            receipt["warnings"] = warnings
        return receipt

    def recover(self):
        """Quarantine unpublished work only after real Win32 idle checks."""
        self._open()
        if not self.pending_path.exists():
            return {"instanceID": self.instance_id, "recovered": False, "reason": "no-pending-run"}
        pending = self.fs.json(self.pending_path)
        if (type(pending) is not dict or pending.get("schema") != SCHEMA or pending.get("instanceID") != self.instance_id
                or pending.get("kind") not in ("initialize", "work")
                or not REVISION.fullmatch(pending.get("transactionID", ""))
                or pending.get("phase") not in ("preparing", "guest", "stopped")):
            raise ValueError("Invalid pending instance transaction")
        record = self._record()
        if record and record["revision"] == pending["transactionID"]:
            self._verify_accepted(record)
            self.fs.unlink(self.pending_path)
            return {"instanceID": self.instance_id, "recovered": True, "committed": True,
                    "revision": record["revision"], "reason": "already-published"}
        if (record["revision"] if record else None) != pending.get("baseRevision"):
            raise ValueError("Pending base does not match accepted HEAD")
        entries = [(self._revision(pending["transactionID"]), "revision")]
        if pending["kind"] == "initialize":
            if record is not None or not BATCH.fullmatch(pending.get("inputBatch", "")):
                raise ValueError("Invalid unpublished initialization")
            entries.append((self.root / "inputs" / pending["inputBatch"], "inputs"))
        else:
            session_id = pending.get("sessionID", "")
            if not SESSION.fullmatch(session_id) or pending.get("sessionRoot") != str(self.store.workspace / session_id):
                raise ValueError("Pending work is outside its owned session")
            receipt_path = self.store.workspace / session_id / "instance-work.json"
            if pending.get("phase") != "preparing" or receipt_path.exists():
                receipt = self.fs.json(receipt_path)
                for key in ("instanceID", "transactionID", "baseRevision", "sessionID", "sessionRoot"):
                    if receipt.get(key) != pending.get(key):
                        raise ValueError("Pending recovery does not own this session's work")
            entries.append((self.store.workspace / session_id / "instance-data", "work"))
        recovery = pending.get("recovery")
        if recovery is None:
            recovery = "q-" + uuid.uuid4().hex
            self.fs.atomic_json(self.pending_path, {**pending, "recovery": recovery})
        elif type(recovery) is not str or not re.fullmatch(r"q-[0-9a-f]{32}", recovery):
            raise ValueError("Invalid recovery marker")
        quarantine = self.root / ".quarantine" / recovery
        self.fs.api.private_directory(quarantine, exist_ok=True)
        for source, name in entries:
            target = quarantine / name
            if source.exists() and target.exists():
                raise ValueError("Ambiguous recovery sources; both preserved")
            if source.exists():
                self.fs.tree_idle(source)
            elif target.exists():
                self.fs.tree_idle(target)
        for source, name in entries:
            if source.exists():
                self.fs.move(source, quarantine / name)
        recovered = {"schema": SCHEMA, "instanceID": self.instance_id,
                     "transactionID": pending["transactionID"], "baseRevision": pending.get("baseRevision")}
        receipt_path = quarantine / "recovered.json"
        if receipt_path.exists():
            if self.fs.json(receipt_path) != recovered:
                raise ValueError("Recovery receipt differs; preserved all entries")
        else:
            self.fs.write_new(receipt_path, _encode(recovered))
        self.fs.unlink(self.pending_path)
        return {"instanceID": self.instance_id, "recovered": True, "committed": False,
                "revision": pending.get("baseRevision"), "quarantine": recovery, "reason": "unpublished-work-quarantined"}
