"""Instance NAND snapshot transactions and native-session integration.

Derived from the self-authored experiments/instance-storage/instance_storage.py
SHA-256 0072642df0d209ea8e6dac18e9b4a15bca9163a2ac4368995412ce81752dccd9.

Immutable accepted revisions are snapshots. A guest receives a private copy;
only a successful, stopped guest may publish it. Thus upgrade failures/crashes
leave HEAD untouched. Unknown/different layouts never reinterpret saved bytes.
Only an explicitly declared exact-profile NOR layout may join the same snapshot.
Synthetic virtual identity/authentication state is a separate revision object.
No imported factory/authkey, AP or resource image is accepted by this API.
"""
from __future__ import annotations
from dataclasses import asdict, dataclass
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import uuid
import struct
import sys
import types

from .minative import (profile_in_scope, _directory_fd, _open_file, _path, _pin, _read, _decode,
                      _json as _bounded_json, MError)

BLOCK = 131072
MAX_DATA = 512 * 1024 * 1024
MAX_REVISIONS = 8
ID = re.compile(r"[a-zA-Z0-9_.@-]{1,128}\Z")
NOR_PROFILE = "q66@4.100.177"
NOR_AP_SHA256 = "ff74c6f467529963b6afd40669437d8ec39a02e48c10d0893739847b3ba3dc2b"
NOR_LAYOUT = {"apSHA256": NOR_AP_SHA256, "apBytes": 12292708, "apOffset": 0xC0000,
              "flashBytes": 0x1000000, "offset": 0xFB8000,
              "bytes": 0x48000, "factoryOffset": 0xFC0000}
LEGACY_NOR_LAYOUT = {"apSHA256": NOR_AP_SHA256, "offset": 0xFB8000,
                     "bytes": 0x48000, "factoryOffset": 0xFC0000}
VIRTUAL_PROFILES = {
    "q66@4.100.177": (NOR_AP_SHA256, "miwear.watch.q66nfc"),
    "miwear.watch.q66glt@4.200.078@754da125b286":
        ("bf36cc88905271e7f596b1de9b8c314e6a9737d5e36f8d67f60535f003ee229c", "miwear.watch.q66glt"),
    "miwear.watch.q66tc@4.100.155@f1c824fef81d":
        ("ea0bdf1920cb30223d616432af00565ca67622e6468328f5eab155f8cdc2fb9f", "miwear.watch.q66tc"),
    "miwear.watch.q66u@4.100.108@6ef691453a86":
        ("9315ca353f624cec25dfcfc98a95ba959e2d7b24573bf1d6adf16ea10341bd99", "miwear.watch.q66u"),
    "miwear.watch.n66cn@3.1.32@489f20502ae2":
        ("9c02dab4020b2cc9666ee7d34cf27d311b76aadcec519a38361bbcbd94c53264", "miwear.watch.n66cn"),
    "miwear.watch.n66nfc@3.1.32@489f20502ae2":
        ("9c02dab4020b2cc9666ee7d34cf27d311b76aadcec519a38361bbcbd94c53264", "miwear.watch.n66nfc"),
    "miwear.watch.n66tc@3.1.32@489f20502ae2":
        ("9c02dab4020b2cc9666ee7d34cf27d311b76aadcec519a38361bbcbd94c53264", "miwear.watch.n66tc"),
    "miwear.watch.n67cn@3.1.187@6d33afe20089":
        ("acb9b0e867140f09c90747c0ada98f340e2edb540eb69b999c4b78adbc102595", "miwear.watch.n67cn"),
    "miwear.watch.o66cn@3.1.28@03ac3e2b8325":
        ("fb726472416e99ba11d1c20ce8f0d4f0831a6e9a673d63bb5918e2c3921d8845", "miwear.watch.o66cn"),
    "miwear.watch.o66lj@3.1.26@d4e61778eee0":
        ("185fc5737fa203ff3f109a282feca89a09c12d84bccd44fd7b61d74853f7fa49", "miwear.watch.o66lj"),
    "miwear.watch.o66nfc@3.1.28@03ac3e2b8325":
        ("fb726472416e99ba11d1c20ce8f0d4f0831a6e9a673d63bb5918e2c3921d8845", "miwear.watch.o66nfc"),
    "miwear.watch.o66tc@3.1.28@03ac3e2b8325":
        ("fb726472416e99ba11d1c20ce8f0d4f0831a6e9a673d63bb5918e2c3921d8845", "miwear.watch.o66tc"),
    "miwear.watch.p67cn@3.101.043@a4ce85647786":
        ("519307675665e4866d722a8119a98589c397b614ac3294cb87bfc86de45756ec", "miwear.watch.p67cn"),
    "miwear.watch.p67tc@3.101.043@a4ce85647786":
        ("519307675665e4866d722a8119a98589c397b614ac3294cb87bfc86de45756ec", "miwear.watch.p67tc"),
}

# Families whose synthetic identity format the virtual peer knows. The format
# follows the hardware family and SKU, not one firmware build: any firmware of
# these presets (catalog or imported) gets the virtual connection.
VIRTUAL_FAMILIES = ("n66", "n67", "o66", "p67", "q65", "q66")


def derived_nor_layout(project, profile_id, storage_kind):
    """NOR layout the runner derives from this profile's own AP, or None.

    The packaged capability lists the layouts measured for the bundled
    profiles; any other firmware of a supported preset (another catalog version
    or an imported OTA) gets the same AP-derived description the runner checks
    its board against before it gives NOR storage to QEMU."""
    import subprocess
    import sys
    if storage_kind not in ("yaffs", "romfs"):
        return None
    try:
        # -I keeps the script directory off sys.path; add the project itself.
        stub = ("import runpy, sys; sys.path.insert(0, sys.argv[1]); sys.argv = sys.argv[1:]; "
                "sys.argv[0] = sys.argv[0] + '/multi_device.py'; "
                "runpy.run_path(sys.argv[0], run_name='__main__')")
        done = subprocess.run([sys.executable, "-I", "-S", "-B", "-c", stub, str(project),
                               "--device", profile_id, "--nor-layout", "--storage-kind", storage_kind],
                              cwd=str(project), capture_output=True, timeout=120,
                              env={"PATH": "/usr/bin:/bin", "LANG": "C", "LC_ALL": "C"})
    except (OSError, subprocess.SubprocessError):
        return None
    if done.returncode != 0:
        return None
    try:
        layout = json.loads(done.stdout.decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        return None
    return layout if type(layout) is dict else None


def virtual_profile(profile_id, ap_sha256=None):
    """(AP SHA-256, identity model) for a profile's virtual peer, or None."""
    if profile_id in VIRTUAL_PROFILES:
        return VIRTUAL_PROFILES[profile_id]
    model = profile_id.split("@", 1)[0] if type(profile_id) is str else ""
    if (not profile_in_scope(profile_id) or not model.startswith("miwear.watch.")
            or model[len("miwear.watch."):][:3] not in VIRTUAL_FAMILIES):
        return None
    return (ap_sha256 if _hex(ap_sha256, 64) else None), model

VIRTUAL_STATE = "virtual-peer-state.json"
VIRTUAL_OUTPUT = "virtual-peer-state.pending.json"
VIRTUAL_SHA = "virtualPeerStateSHA256"
VIRTUAL_FIELDS = ("virtualPeerStatePath", VIRTUAL_SHA, "virtualPeerStateSchema")
# Only these bundled helper modules are loaded, from bounded held byte strings.
# Their build SHA values are descriptive; code changes are not an execution gate.
VIRTUAL_SOURCE_MODULES = ("crypto", "auth_crypto", "state")


def _hex(value, length):
    return type(value) is str and re.fullmatch(r"[0-9a-f]{" + str(length) + r"}", value) is not None


def _virtual_value(value, *, instance_id=None):
    """Data-only schema check, usable even with an older Engine selected."""
    fields = {"schema", "origin", "instanceID", "profileID", "apSHA256", "identityVersion",
              "identity", "psk", "p256Private", "binding"}
    if type(value) is not dict or set(value) - {"guestLinkBond"} != fields:
        raise ValueError("Invalid synthetic virtual state schema")
    _uuid(value["instanceID"])
    descriptor = virtual_profile(value["profileID"], value["apSHA256"])
    if (descriptor is None or type(value["schema"]) is not int or value["schema"] != 1
            or value["origin"] != "synthetic-virtual-instance"
            or not _hex(value["apSHA256"], 64)
            or (instance_id is not None and value["instanceID"] != instance_id)
            or type(value["identityVersion"]) is not int or not 1 <= value["identityVersion"] <= 0xffffffff):
        raise ValueError("Synthetic virtual state identity mismatch")
    identity = value["identity"]
    if type(identity) is not dict or set(identity) != {"serial", "address", "did", "model"}:
        raise ValueError("Invalid synthetic virtual identity")
    did_digits = 9 if identity["model"].startswith("miwear.watch.n66") else 11 if identity["model"].startswith("miwear.watch.o66") else 13
    serial_limit = 21 if identity["model"].startswith("miwear.watch.n67") else 24 if identity["model"].startswith(("miwear.watch.n66", "miwear.watch.o66")) else 31
    if (type(identity["serial"]) is not str or not 1 <= len(identity["serial"]) <= serial_limit
            or any(not 33 <= ord(char) <= 126 for char in identity["serial"])
            or type(identity["did"]) is not str or not re.fullmatch(r"99[0-9]{" + str(did_digits - 2) + r"}", identity["did"])
            or identity["model"] != descriptor[1] or type(identity["address"]) is not str
            or not re.fullmatch(r"[0-9A-F]{2}(:[0-9A-F]{2}){5}", identity["address"])):
        raise ValueError("Identity must be a synthetic virtual device")
    if not _hex(value["psk"], 32) or not _hex(value["p256Private"], 194) or not value["p256Private"].startswith("04"):
        raise ValueError("Invalid synthetic private material")
    binding = value["binding"]
    bond = value.get("guestLinkBond")
    if bond is not None and (type(bond) is not dict or set(bond) != {"ltk", "random", "ediv"}
            or not _hex(bond["ltk"], 32) or not _hex(bond["random"], 16)
            or type(bond["ediv"]) is not int or not 0 <= bond["ediv"] <= 65535):
        raise ValueError("Invalid guest-confirmed external link bond")
    if binding is not None and (type(binding) is not dict or set(binding) != {"authKey", "ltk", "random", "ediv"}
            or not _hex(binding["authKey"], 32) or not _hex(binding["ltk"], 32) or not _hex(binding["random"], 16)
            or type(binding["ediv"]) is not int or not 0 <= binding["ediv"] <= 65535):
        raise ValueError("Invalid guest-confirmed synthetic binding")
    return value


def _virtual_read(path, expected_sha=None, *, instance_id=None):
    raw = _read(path, 4096, private=True)
    digest = hashlib.sha256(raw).hexdigest()
    if expected_sha is not None and (not _hex(expected_sha, 64) or digest != expected_sha):
        raise ValueError("Virtual state hash differs from storage transaction")
    return _virtual_value(_decode(raw), instance_id=instance_id), raw, digest


def _virtual_encode(value):
    raw = (json.dumps(_virtual_value(value), sort_keys=True, separators=(",", ":")) + "\n").encode("ascii")
    if len(raw) > 4096:
        raise ValueError("Virtual state exceeds private file bound")
    return raw


def _virtual_write(path, raw):
    """Create only a new private file; callers publish via the revision HEAD."""
    _virtual_value(_decode(raw))
    if not 1 <= len(raw) <= 4096:
        raise ValueError("Virtual state exceeds private file bound")
    directory = _directory_fd(path.parent, private=True)
    try:
        fd = os.open(path.name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=directory)
        with os.fdopen(fd, "wb") as stream:
            stream.write(raw); stream.flush(); os.fsync(stream.fileno())
    finally:
        os.close(directory)


def _create_virtual_state(project, instance_id, *, profile_id=NOR_PROFILE, ap_sha256=None,
                          virtual_key_file=None, virtual_key_sha256=None):
    private_data = None
    if virtual_key_file is not None or virtual_key_sha256 is not None:
        path = _path(virtual_key_file) if virtual_key_file is not None else None
        if path is None or path.name != "native-p256.bin" or not _hex(virtual_key_sha256, 64):
            raise ValueError("Native synthetic key input contract is invalid")
        private_data = _read(path, 97, private=True)
        if (len(private_data) != 97 or private_data[0] != 4
                or hashlib.sha256(private_data).hexdigest() != virtual_key_sha256):
            raise ValueError("Native synthetic key identity differs")
    folder = _path(project) / "backend/src/virtual_peer"
    sources = {name: _read(folder / (name + ".py"), 32768) for name in VIRTUAL_SOURCE_MODULES}
    # A unique private package prevents resolution through unverified __init__
    # files or unrelated sys.path entries. Only the held local byte strings run.
    package_name = "_azki_synthetic_" + uuid.uuid4().hex
    package = types.ModuleType(package_name); package.__path__ = []
    modules = {package_name: package}
    try:
        sys.modules[package_name] = package
        for name, raw in sources.items():
            qualified = package_name + "." + name
            module = types.ModuleType(qualified); module.__package__ = package_name
            modules[qualified] = module; sys.modules[qualified] = module
            exec(compile(raw, str(folder / (name + ".py")), "exec"), module.__dict__)
        value = modules[package_name + ".state"].create_state(
            instance_id, private_data=private_data, profile_id=profile_id,
            **({"ap_sha256": ap_sha256} if ap_sha256 else {}))
        return _virtual_value(value, instance_id=instance_id)
    finally:
        for name in modules:
            sys.modules.pop(name, None)


def _capability_profile_id(profiles, profile_id, ap_sha256):
    """Exact capability key for a profile, or the verified entry pinning its AP.

    An imported OTA runs as a generated profile ID, so a table keyed by
    catalog profile IDs never matches it. Its AP image, however, is the exact
    bytes a verified entry pinned: when this profile's model has an entry
    whose apSHA256 equals the selected AP's digest, that entry's verified
    layout applies to this firmware too.
    """
    if type(profile_id) is str and profile_id in profiles:
        return profile_id
    if type(profile_id) is not str or not _hex(ap_sha256, 64):
        return None
    model = profile_id.split("@", 1)[0]
    for key, entry in profiles.items():
        if (type(key) is str and key.split("@", 1)[0] == model
                and type(entry) is dict and entry.get("apSHA256") == ap_sha256):
            return key
    return None


def _nor_capability(capability, profile_id, ap_sha256, qemu_sha256):
    if type(profile_id) is not str or not profile_in_scope(profile_id):
        raise ValueError("NOR capability profile is outside the Mi Band scope")
    if (not isinstance(capability, dict) or set(capability) != {"schema", "runnerProtocol", "qemuSHA256", "profiles"}
            or type(capability.get("schema")) is not int or capability["schema"] != 1
            or type(capability.get("runnerProtocol")) is not int or capability["runnerProtocol"] != 1
            or not _hex(capability.get("qemuSHA256"), 64)):
        raise ValueError("NOR capability identity or runner protocol mismatch")
    profiles = capability.get("profiles")
    if type(profiles) is not dict or not 1 <= len(profiles) <= 64 or profile_id not in profiles:
        raise ValueError("NOR capability lacks this exact firmware profile")
    layout = profiles[profile_id]
    base = {"apSHA256", "apBytes", "apOffset", "flashBytes", "offset", "bytes", "factoryOffset"}
    config = {"configOffset", "configBytes"}
    legacy = {"apSHA256", "offset", "bytes", "factoryOffset"}
    if type(layout) is not dict or (set(layout) not in (base, base | config, legacy)):
        raise ValueError("NOR profile descriptor schema mismatch")
    if set(layout) == legacy:
        if profile_id != NOR_PROFILE or {k:v for k,v in layout.items() if k != "apSHA256"} != {k:v for k,v in LEGACY_NOR_LAYOUT.items() if k != "apSHA256"}:
            raise ValueError("Legacy NOR descriptors are limited to the previously verified Q66 layout")
        layout = dict(NOR_LAYOUT)
    if (not _hex(layout["apSHA256"], 64)
            or any(type(layout[k]) is not int for k in set(layout) - {"apSHA256"})):
        raise ValueError("NOR profile AP/layout values are invalid")
    if (layout["apBytes"] <= 0 or layout["apOffset"] < 0 or layout["flashBytes"] <= 0
            or layout["offset"] < 0 or layout["bytes"] <= 0 or layout["factoryOffset"] < 0
            or layout["factoryOffset"] >= layout["flashBytes"]):
        raise ValueError("NOR profile geometry is outside flash")
    config_offset, config_bytes = layout.get("configOffset", 0), layout.get("configBytes", 0)
    if bool(config_offset) != bool(config_bytes):
        raise ValueError("Split NOR config geometry must be a complete pair")
    if config_bytes < 0:
        raise ValueError("NOR config range has an invalid size")
    tail_bytes = layout["bytes"] - config_bytes
    ap_start, ap_end = layout["apOffset"], layout["apOffset"] + layout["apBytes"]
    tail_end, config_end = layout["offset"] + tail_bytes, config_offset + config_bytes
    if (tail_bytes <= 0 or tail_end != layout["flashBytes"]
            or ap_end > layout["offset"] or ap_start >= ap_end
            or not layout["offset"] <= layout["factoryOffset"] < tail_end
            or (config_bytes and (config_offset < 0 or config_end > layout["flashBytes"]
                 or (config_offset < tail_end and layout["offset"] < config_end)
                 or (config_offset < ap_end and ap_start < config_end)))
            or any(x % 512 for x in (layout["offset"], tail_bytes, config_offset, config_bytes))
            or (config_bytes and (config_offset % 8192 or config_bytes % 8192))):
        raise ValueError("NOR writable extents overlap AP or exceed the exact flash geometry")
    return layout



def _private(path: Path, *, directory=False, size=None):
    """Require an object this app owns, narrowing a wider mode back.

    Modes change when a library is restored from a backup or copied by a file
    manager; that must not brick an instance. Ownership, link count and image
    size are still enforced, and a mode is only ever narrowed, never widened.
    """
    s = path.lstat()
    good = stat.S_ISDIR(s.st_mode) if directory else stat.S_ISREG(s.st_mode)
    if not good or s.st_uid != os.geteuid():
        raise ValueError("Expected owned private directory/file")
    if not directory and (s.st_nlink != 1 or (size is not None and s.st_size != size)):
        raise ValueError("Unsafe link or wrong image size")
    if stat.S_IMODE(s.st_mode) != (0o700 if directory else 0o600):
        try:
            path.chmod(0o700 if directory else 0o600)
        except OSError as error:
            raise ValueError("Expected owned private directory/file") from error


def _json(path: Path):
    _private(path)
    value = _decode(_read(path, 16_384, private=True))
    if not isinstance(value, dict): raise ValueError("Expected metadata object")
    return value


def _sync_dir(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try: os.fsync(fd)
    finally: os.close(fd)


def _atomic_json(path: Path, value):
    tmp = path.parent / (".json-" + uuid.uuid4().hex)
    data = (json.dumps(value, sort_keys=True, indent=2) + "\n").encode()
    if len(data) > 16384: raise ValueError("Oversized metadata")
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data); f.flush(); os.fsync(f.fileno())
        os.replace(tmp, path); _sync_dir(path.parent)
    finally:
        if tmp.exists(): tmp.unlink()


def _resize(path, size):
    """Truncate or extend with erased (0xFF) bytes to the selected geometry."""
    current = path.stat().st_size
    if current > size:
        os.truncate(path, size)
    elif current < size:
        with path.open("ab") as stream:
            remaining = size - current
            while remaining:
                amount = min(remaining, BLOCK)
                stream.write(b"\xff" * amount); remaining -= amount
            stream.flush(); os.fsync(stream.fileno())


def _blank(path, size):
    with path.open("xb") as f:
        path.chmod(0o600)
        chunk = b"\xff" * BLOCK
        while size:
            amount = min(size, len(chunk)); f.write(chunk[:amount]); size -= amount


def _copy(source, target, *, erased_prefix=0, preserved_prefix=0):
    source_fd, before = _open_file(source, MAX_DATA)
    directory = _directory_fd(target.parent, private=True)
    target_fd = None
    try:
        if (type(erased_prefix) is not int or type(preserved_prefix) is not int
                or not 0 <= erased_prefix <= MAX_DATA - before.st_size
                or not 0 <= preserved_prefix <= before.st_size):
            raise ValueError("Invalid erased NOR prefix")
        target_fd = os.open(target.name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=directory)
        def write_all(data):
            view = memoryview(data)
            while view:
                n = os.write(target_fd, view)
                if n <= 0: raise OSError("Short instance copy")
                view = view[n:]
        copied = 0
        while True:
            if copied == preserved_prefix and erased_prefix:
                # Split config stays at byte zero; only the NOR tail moves.
                while erased_prefix:
                    amount = min(erased_prefix, BLOCK)
                    write_all(b"\xff" * amount); erased_prefix -= amount
            amount = min(1024 * 1024, before.st_size + 1 - copied)
            if copied < preserved_prefix: amount = min(amount, preserved_prefix - copied)
            chunk = os.read(source_fd, amount)
            if not chunk: break
            copied += len(chunk)
            if copied > before.st_size: raise ValueError("Instance source grew during copy")
            write_all(chunk)
        after = os.fstat(source_fd)
        if copied != before.st_size or (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
                after.st_size, after.st_mtime_ns, after.st_ctime_ns):
            raise ValueError("Instance source changed during copy")
        os.fsync(target_fd)
    finally:
        os.close(source_fd); os.close(directory)
        if target_fd is not None: os.close(target_fd)


@dataclass(frozen=True)
class Firmware:
    model: str
    profile: str
    data_offset: int
    data_bytes: int
    data_format: str
    nor_offset: int = 0
    nor_bytes: int = 0
    nor_ap_sha256: str = ""
    nor_ap_bytes: int = 0
    nor_ap_offset: int = 0
    nor_flash_bytes: int = 0
    nor_factory_offset: int = 0
    nor_config_offset: int = 0
    nor_config_bytes: int = 0

    def validate(self):
        if not ID.fullmatch(self.model) or not ID.fullmatch(self.profile):
            raise ValueError("Invalid model/profile")
        if (type(self.data_offset) is not int or type(self.data_bytes) is not int
                or self.data_offset < 0 or self.data_bytes <= 0
                or (self.data_offset | self.data_bytes) % BLOCK
                or self.data_offset + self.data_bytes > MAX_DATA):
            raise ValueError("Unsupported NAND geometry")
        if self.data_format not in {"unknown", "yaffs2-inband-2048", "littlefs-nand-2048"}:
            raise ValueError("Unknown format tag")

        nor_fields = (self.nor_offset, self.nor_bytes, self.nor_ap_bytes, self.nor_ap_offset,
                      self.nor_flash_bytes, self.nor_factory_offset, self.nor_config_offset,
                      self.nor_config_bytes)
        if any(type(value) is not int for value in nor_fields):
            raise ValueError("NOR layout values must be strict integers")
        if self.nor_ap_sha256 and (not _hex(self.nor_ap_sha256, 64) or not self.nor_bytes):
            raise ValueError("NOR AP identity is malformed")
        if not self.nor_bytes:
            if any(nor_fields) or self.nor_ap_sha256:
                raise ValueError("Disabled NOR storage carries an unexpected layout")
        elif self.nor_ap_sha256:
            tail_bytes = self.nor_bytes - self.nor_config_bytes
            ap_end = self.nor_ap_offset + self.nor_ap_bytes
            tail_end = self.nor_offset + tail_bytes
            config_end = self.nor_config_offset + self.nor_config_bytes
            if (self.nor_ap_bytes <= 0 or self.nor_ap_offset < 0 or self.nor_flash_bytes <= 0
                    or self.nor_offset < 0 or tail_bytes <= 0 or tail_end > self.nor_flash_bytes
                    or ap_end > self.nor_offset or self.nor_ap_offset >= ap_end
                    or not self.nor_offset <= self.nor_factory_offset < tail_end
                    or (self.nor_config_bytes and (self.nor_config_offset <= 0
                         or config_end > self.nor_flash_bytes
                         or (self.nor_config_offset < tail_end and self.nor_offset < config_end)
                         or (self.nor_config_offset < ap_end and self.nor_ap_offset < config_end)))
                    or bool(self.nor_config_offset) != bool(self.nor_config_bytes)
                    or any(value % 512 for value in (self.nor_offset, tail_bytes,
                         self.nor_config_offset, self.nor_config_bytes))):
                raise ValueError("Unsupported NOR writable geometry")
        elif (self.profile != NOR_PROFILE or (self.nor_offset, self.nor_bytes) !=
              (LEGACY_NOR_LAYOUT["offset"], LEGACY_NOR_LAYOUT["bytes"])):
            raise ValueError("Legacy NOR storage is limited to the prior Q66 layout")

    @property
    def record(self):
        record = asdict(self)
        # Preserve pre-NOR pending/accepted records byte-for-byte in structure.
        if not self.nor_bytes:
            record.pop("nor_offset"); record.pop("nor_bytes")
            for key in ("nor_ap_sha256", "nor_ap_bytes", "nor_ap_offset", "nor_flash_bytes",
                        "nor_factory_offset", "nor_config_offset", "nor_config_bytes"):
                record.pop(key)
        elif not self.nor_ap_sha256:
            # Older accepted Q66 revisions remain readable and can be advanced
            # without rewriting the user's existing NOR image.
            for key in ("nor_ap_sha256", "nor_ap_bytes", "nor_ap_offset", "nor_flash_bytes",
                        "nor_factory_offset", "nor_config_offset", "nor_config_bytes"):
                record.pop(key)
        return record

    @property
    def images(self):
        images = [("nand-data.bin", self.data_bytes, "dataSHA256", "dataImage"),
                  ("nand-spare.bin", self.data_bytes // 2048 * 64, "spareSHA256", "spareImage")]
        if self.nor_bytes:
            images.append(("nor-config.bin", self.nor_bytes, "norSHA256", "norImage"))
        return images

    @property
    def layout(self):
        return self.data_offset, self.data_bytes, self.data_format

    @property
    def nor_layout(self):
        return (self.nor_offset, self.nor_bytes, self.nor_config_offset, self.nor_config_bytes)

    def nor_prefix_from(self, previous):
        """An erased prefix may extend the same firmware's existing NOR tail.

        Both layouts are already validated outside the AP. This preserves the
        factory/config addresses and never permits a firmware/layout reinterpretation.
        """
        unchanged = ("model", "profile", "nor_ap_sha256", "nor_ap_bytes", "nor_ap_offset",
                     "nor_flash_bytes", "nor_factory_offset", "nor_config_offset", "nor_config_bytes")
        prefix = previous.nor_offset - self.nor_offset
        if (not previous.nor_bytes or not previous.nor_ap_sha256 or prefix <= 0
                or any(getattr(self, name) != getattr(previous, name) for name in unchanged)
                or self.nor_bytes != previous.nor_bytes + prefix):
            return 0
        return prefix


_UNREAD_HEAD = object()


class InstanceStorage:
    def __init__(self, root: Path, *, max_revisions=MAX_REVISIONS):
        if type(max_revisions) is not int or not 1 <= max_revisions <= 32:
            raise ValueError("Snapshot retention limit must be 1..32")
        self.max_revisions = max_revisions
        self.root = Path(root).absolute()
        # Keep path traversal symlink-free. The production host also confines
        # the core to these two exact files in its default-deny sandbox.
        _private(self.root, directory=True)
        fd = _directory_fd(self.root, private=True)
        os.close(fd)
        self.info = _json(self.root / "instance.json")
        if self.info.get("schema") != 1 or not ID.fullmatch(self.info.get("model", "")):
            raise ValueError("实例数据的身份记录无效")
        # A restore that skipped hidden files or empty folders must not brick
        # the instance: the lock file and revision folder are recreated.
        self.lock = os.open(self.root / ".lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        try:
            (self.root / "revisions").mkdir(mode=0o700)
        except FileExistsError:
            pass
        except BaseException:
            os.close(self.lock); raise
        try:
            _private(self.root / ".lock")
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BaseException:
            os.close(self.lock); raise
        self.active = None

    @classmethod
    def create(cls, root: Path, model: str):
        if not ID.fullmatch(model): raise ValueError("Invalid model")
        root = Path(root).absolute()
        parent = _directory_fd(root.parent, private=True)
        try:
            os.mkdir(root.name, mode=0o700, dir_fd=parent)
        finally:
            os.close(parent)
        (root / "revisions").mkdir(mode=0o700)
        fd = os.open(root / ".lock", os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
        os.close(fd)
        _atomic_json(root / "instance.json", {"schema": 1, "model": model, "id": uuid.uuid4().hex})
        return cls(root)

    def close(self):
        if self.lock is not None:
            os.close(self.lock); self.lock = None

    def _ensure(self):
        if self.lock is None: raise ValueError("Instance is closed")

    def head(self):
        self._ensure()
        path = self.root / "head.json"
        if not path.exists(): return None
        head = _json(path)
        revision = head.get("revision", "")
        if not re.fullmatch(r"r-[0-9a-f]{32}", revision): raise ValueError("Invalid HEAD")
        folder = self.root / "revisions" / revision
        _private(folder, directory=True)
        manifest = _json(folder / "revision.json")
        fw = Firmware(**manifest["firmware"]); fw.validate()
        if fw.model != self.info["model"]: raise ValueError("Wrong model in revision")
        for name, size, key, _ in fw.images:
            # Owner, link count and exact size only. Re-hashing up to 512 MiB on
            # every start slowed launches and could only refuse to boot data
            # that is already on disk; commit records the digest for diagnosis.
            _private(folder / name, size=size)
        if VIRTUAL_SHA in manifest:
            if not _hex(manifest[VIRTUAL_SHA], 64):
                raise ValueError("Accepted virtual state has no valid hash")
            _virtual_read(folder / VIRTUAL_STATE, manifest[VIRTUAL_SHA])
        elif (folder / VIRTUAL_STATE).exists() or (folder / VIRTUAL_STATE).is_symlink():
            raise ValueError("Unrecorded virtual state in accepted revision")
        return folder, manifest, fw

    def begin(self, firmware: Firmware, *, synthetic_seed: Path | None = None,
              synthetic_seed_sha256: str | None = None, virtual_state=None, rebuild_identity=False,
              _previous_head=_UNREAD_HEAD):
        self._ensure(); firmware.validate()
        if firmware.model != self.info["model"]:
            raise ValueError("此实例的数据属于 " + self.info["model"] + "，不能用 " + firmware.model
                             + " 硬件预设启动；请换回原硬件预设，或新建实例")
        if self.active is not None or (self.root / "pending.json").exists():
            raise ValueError("此实例还有一次未完成的运行记录；请重置该实例后再启动")
        # prepare_session_storage already verified HEAD under this same held lock.
        old = self.head() if _previous_head is _UNREAD_HEAD else _previous_head
        if rebuild_identity and (old is None or virtual_state is None):
            raise ValueError("Identity rebuild requires a retained snapshot and fresh synthetic identity")
        # Firmware is never locked to an instance. Any version for the same
        # hardware preset runs on a private copy of the saved data; HEAD only
        # advances after a healthy stop, so a firmware that cannot use the data
        # fails at boot and leaves the committed snapshot intact. Only pieces
        # that genuinely cannot carry over are reinitialized, with a reason.
        carry = old is not None and not rebuild_identity
        notes = []
        nor_prefix = firmware.nor_prefix_from(old[2]) if carry else 0
        if carry and old[2].data_bytes != firmware.data_bytes:
            notes.append("此固件的数据分区大小与上次不同，已沿用原数据并按新大小" +
                         ("补齐空白区" if firmware.data_bytes > old[2].data_bytes else "截断末尾") +
                         "；上次的快照保留到本次正常停止为止")
        nor_carry = bool(carry and old[2].nor_bytes and firmware.nor_bytes
                         and (old[2].nor_layout == firmware.nor_layout or nor_prefix))
        # NOR saved by an earlier firmware travels along unused ("dormant")
        # through firmware without NOR, so switching back restores it.
        dormant = None
        if carry and old[2].nor_bytes:
            dormant = {"file": old[0] / "nor-config.bin", "layout": list(old[2].nor_layout), "bytes": old[2].nor_bytes}
        elif carry and isinstance(old[1].get("dormantNor"), dict):
            record = old[1]["dormantNor"]
            if (old[0] / "nor-config.dormant.bin").exists() and type(record.get("bytes")) is int:
                dormant = {"file": old[0] / "nor-config.dormant.bin", "layout": record.get("layout"),
                           "bytes": record["bytes"]}
        if not nor_carry and dormant and firmware.nor_bytes and dormant["layout"] == list(firmware.nor_layout):
            nor_carry, nor_source = True, dormant["file"]
            notes.append("已恢复此实例早先保存的 NOR 配置")
        else:
            nor_source = old[0] / "nor-config.bin" if nor_carry else None
        if carry and dormant and firmware.nor_bytes and not nor_carry:
            notes.append("此固件的 NOR 配置区布局与上次不同，NOR 配置区已重新初始化（原配置仍保存在旧快照中）；NAND 用户数据已沿用")
        if carry and dormant and not firmware.nor_bytes:
            notes.append("此固件不使用 NOR 配置区，上次的 NOR 配置原样保留，换回原固件时继续使用；NAND 用户数据已沿用")
        revisions = self.root / "revisions"
        _private(revisions, directory=True)
        self._make_room(protect={old[0].name} if old else set())
        if old and synthetic_seed is not None and not rebuild_identity:
            raise ValueError("Never reseed existing user data")
        if virtual_state is not None:
            _virtual_value(virtual_state)
            expected_version = 1
            if rebuild_identity and VIRTUAL_SHA in old[1]:
                previous_state, _, _ = _virtual_read(old[0] / VIRTUAL_STATE)
                expected_version = previous_state["identityVersion"] + 1
            if ((old and not rebuild_identity and VIRTUAL_SHA in old[1]) or not firmware.nor_bytes
                    or firmware.profile != virtual_state["profileID"]
                    or virtual_state["binding"] is not None or virtual_state["identityVersion"] != expected_version):
                raise ValueError("Fresh identity requires a new data revision without the old pairing")
        tx = "r-" + uuid.uuid4().hex
        folder = revisions / tx
        folder.mkdir(mode=0o700)
        marker = {"revision": tx, "previous": old[0].name if old else None,
                  "firmware": firmware.record, "phase": "preparing"}
        _atomic_json(self.root / "pending.json", marker)
        self.active = folder
        try:
            data, spare = folder / "nand-data.bin", folder / "nand-spare.bin"
            if carry:
                _copy(old[0] / data.name, data); _copy(old[0] / spare.name, spare)
                _resize(data, firmware.data_bytes)
                _resize(spare, firmware.data_bytes // 2048 * 64)
                origin = "accepted-instance-revision"
            else:
                if synthetic_seed is None:
                    _blank(data, firmware.data_bytes); origin = "blank-erased-nand"
                else:
                    # Only the host's independently verified synthetic UI seed
                    # may be passed here. Never accept factory/identity dumps.
                    _private(synthetic_seed, size=firmware.data_bytes)
                    with synthetic_seed.open("rb") as stream:
                        sha = hashlib.file_digest(stream, "sha256").hexdigest()
                    if sha != synthetic_seed_sha256:
                        raise ValueError("Synthetic seed hash mismatch")
                    _copy(synthetic_seed, data); origin = "host-verified-synthetic-ui-seed"
                _blank(spare, firmware.data_bytes // 2048 * 64)
            if firmware.nor_bytes:
                if nor_carry:
                    _copy(nor_source, folder / "nor-config.bin",
                          erased_prefix=nor_prefix, preserved_prefix=firmware.nor_config_bytes)
                else:
                    _blank(folder / "nor-config.bin", firmware.nor_bytes)
            dormant_kept = False
            if dormant and not firmware.nor_bytes:
                _copy(dormant["file"], folder / "nor-config.dormant.bin")
                dormant_kept = True
            if carry and VIRTUAL_SHA in old[1]:
                previous_state, raw, digest = _virtual_read(old[0] / VIRTUAL_STATE)
                target = virtual_profile(firmware.profile, firmware.nor_ap_sha256)
                if (previous_state["profileID"] != firmware.profile and target is not None
                        and target[1] == previous_state["identity"]["model"] and firmware.nor_bytes):
                    # Same device identity, keys and pairing; only the firmware
                    # build it is announced for follows the selected firmware.
                    raw = _virtual_encode({**previous_state, "profileID": firmware.profile, "apSHA256": target[0]})
                    digest = hashlib.sha256(raw).hexdigest()
                elif previous_state["profileID"] != firmware.profile:
                    notes.append("此固件没有已核验的虚拟通信支持，虚拟身份已原样保留但本次不启用")
                _virtual_write(folder / VIRTUAL_STATE, raw)
                marker[VIRTUAL_SHA] = digest
            elif virtual_state is not None:
                raw = _virtual_encode(virtual_state)
                _virtual_write(folder / VIRTUAL_STATE, raw)
                marker[VIRTUAL_SHA] = hashlib.sha256(raw).hexdigest()
            _atomic_json(folder / "revision.json", {**marker, "origin": origin,
                         "identityRebuilt": rebuild_identity,
                         **({"norExpandedFrom": list(old[2].nor_layout)} if nor_prefix else {}),
                         "carryOver": notes,
                         **({"dormantNor": {"layout": dormant["layout"], "bytes": dormant["bytes"]}}
                            if dormant_kept else {}),
                         "compatibility": "same-firmware-erased-nor-prefix" if nor_prefix else
                                          "data carried over; application migration unverified"})
            _atomic_json(self.root / "pending.json", {**marker, "phase": "guest"})
            return {"data_image": str(data), "data_spare_image": str(spare),
                    "data_writeback": True, "previous_revision": marker["previous"],
                    "nor_initialize": bool(firmware.nor_bytes and not nor_carry),
                    "notes": notes,
                    "compatibility": "data carried over; firmware decides at boot"}
        except BaseException:
            # Preparation only: no process has received the image paths yet.
            shutil.rmtree(folder); (self.root / "pending.json").unlink()
            self.active = None; raise

    def _make_room(self, *, protect=()):
        """Keep the snapshot count under the limit instead of refusing to start.

        Uncommitted leftovers go first, then the oldest committed history.
        HEAD and the snapshot right before it are always kept.
        """
        from .maintenance import _committed, _read_json, remove_tree
        revisions = self.root / "revisions"
        names = sorted(name for name in os.listdir(revisions) if re.fullmatch(r"r-[0-9a-f]{32}", name))
        if len(names) < self.max_revisions:
            return []
        head = (_read_json(self.root / "head.json") or {}).get("revision")
        chain, cursor = [], head
        while isinstance(cursor, str) and cursor in names and cursor not in chain:
            chain.append(cursor)
            cursor = (_read_json(revisions / cursor / "revision.json") or {}).get("previous")
        keep = set(chain[:2]) | set(protect)
        orphans = [name for name in names if name not in chain and name not in keep]
        orphans.sort(key=lambda name: (_committed(revisions / name), name))
        candidates = orphans + [name for name in reversed(chain) if name not in keep]
        removed = []
        for name in candidates:
            if len(names) - len(removed) < self.max_revisions:
                break
            try:
                remove_tree(str(revisions), name)
                removed.append(name)
            except OSError:
                continue
        if len(names) - len(removed) >= self.max_revisions:
            raise ValueError("实例快照目录无法整理出空间；请检查 " + str(revisions) + " 的权限")
        return removed

    def _idle_files(self, folder, firmware):
        held = []
        try:
            for name, size, _, _ in firmware.images:
                path = folder / name; _private(path, size=size)
                fd = os.open(path, os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC)
                held.append(fd); fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return held
        except BaseException:
            for fd in held: os.close(fd)
            raise

    def finish(self, *, success: bool):
        """Call only after owned core exit and guest/storage health evaluation."""
        self._ensure()
        if type(success) is not bool: raise ValueError("Explicit success boolean required")
        if self.active is None: raise ValueError("No transaction owned by this handle")
        pending = _json(self.root / "pending.json")
        firmware = Firmware(**pending["firmware"]); firmware.validate()
        folder = self.active
        fds = self._idle_files(folder, firmware)  # Refuse while core owns either disk.
        try:
            if success:
                for fd in fds: os.fsync(fd)
                metadata = _json(folder / "revision.json")
                metadata.update({key: _initial_pin(folder / name) for name, _, key, _ in firmware.images})
                if VIRTUAL_SHA in pending:
                    metadata[VIRTUAL_SHA] = _virtual_read(folder / VIRTUAL_STATE)[2]
                elif (folder / VIRTUAL_STATE).exists() or (folder / VIRTUAL_STATE).is_symlink():
                    raise ValueError("Unowned virtual state cannot enter a revision")
                _atomic_json(folder / "revision.json", metadata)
                _sync_dir(folder)
                _atomic_json(self.root / "head.json", {"revision": folder.name})
            else:
                # An earlier publish may have replaced HEAD before a directory
                # fsync failed. Restore the old pointer before deleting its
                # successor; a failed rollback must leave both revisions intact.
                current = self.head()
                if current and current[0] == folder:
                    previous = pending.get("previous")
                    if previous is None:
                        (self.root / "head.json").unlink(); _sync_dir(self.root)
                    elif re.fullmatch(r"r-[0-9a-f]{32}", previous):
                        _private(self.root / "revisions" / previous, directory=True)
                        _atomic_json(self.root / "head.json", {"revision": previous})
                    else:
                        raise ValueError("Invalid previous revision")
                shutil.rmtree(folder)
            (self.root / "pending.json").unlink(); _sync_dir(self.root)
            self.active = None
        finally:
            for fd in fds: os.close(fd)

    def recover(self):
        """Discard interrupted unpublished work once no guest holds its disk."""
        self._ensure()
        pending = _json(self.root / "pending.json")
        work_locks = []
        try:
            if pending.get("sessionRoot") is not None:
                session = _path(pending["sessionRoot"])
                fd = _directory_fd(session, private=True); os.close(fd)
                binding = _json(session / "instance-storage.json")
                firmware = Firmware(**pending["firmware"]); firmware.validate()
                if (binding.get("transactionID") != pending.get("revision")
                        or binding.get("sessionRoot") != str(session)
                        or binding.get("instanceID") != self.root.parent.name
                        or binding.get("model") != firmware.model or binding.get("profileID") != firmware.profile
                        or binding.get("dataImage") != str(session / "instance-data/nand-data.bin")
                        or binding.get("spareImage") != str(session / "instance-data/nand-spare.bin")
                        or (firmware.nor_bytes and binding.get("norImage") != str(session / "instance-data/nor-config.bin"))):
                    raise ValueError("Pending work disks do not belong to this instance transaction")
                broker = session / "native-broker"
                if (broker / "launch.json").exists():
                    state = _json(broker / "state.json")
                    if state.get("state") not in ("exited", "failed") or state.get("cleanup") != "reaped":
                        raise ValueError("Native launch is active or unconfirmed; recovery refused")
                work_locks = self._idle_files(session / "instance-data", firmware)
            return self._recover_local()
        finally:
            for fd in work_locks: os.close(fd)

    def _recover_local(self):
        self._ensure()
        pending = _json(self.root / "pending.json")
        revision = pending.get("revision", "")
        if not re.fullmatch(r"r-[0-9a-f]{32}", revision): raise ValueError("Invalid pending revision")
        folder = self.root / "revisions" / revision
        _private(folder, directory=True)
        # A crash after HEAD publication is already an accepted transaction.
        head = self.head()
        if head and head[0] == folder:
            (self.root / "pending.json").unlink(); _sync_dir(self.root)
            self.active = None
            return "already-published"
        if pending.get("phase") == "preparing":
            # Paths were never returned to a core; preparation may be partial.
            shutil.rmtree(folder); (self.root / "pending.json").unlink(); _sync_dir(self.root)
            self.active = None
            return "discarded-incomplete-preparation"
        if pending.get("phase") != "guest": raise ValueError("Unknown transaction phase")
        firmware = Firmware(**pending["firmware"]); firmware.validate()
        held = self._idle_files(folder, firmware)
        try:
            shutil.rmtree(folder); (self.root / "pending.json").unlink(); _sync_dir(self.root)
            self.active = None
            return "rolled-back-to-previous-head"
        finally:
            for fd in held: os.close(fd)


def _uuid(value):
    if not isinstance(value, str) or str(uuid.UUID(value)) != value:
        raise ValueError("实例 ID 无效，应为小写 UUID")
    return value


def checked_capabilities(manifest, qemu_sha256):
    """Validate declared runner capabilities and supported storage geometry."""
    result = {}
    for name in ("nandPersistence", "pmuAdcRaw", "norPersistence"):
        capability = manifest.get(name)
        if capability is None:
            continue
        if (not isinstance(capability, dict) or type(capability.get("schema")) is not int
                or capability["schema"] != 1 or type(capability.get("runnerProtocol")) is not int
                or capability["runnerProtocol"] != 1 or not _hex(capability.get("qemuSHA256"), 64)):
            raise ValueError("Core capability identity or runner protocol mismatch: " + name)
        if name == "norPersistence":
            profiles = capability.get("profiles")
            if type(profiles) is not dict or not 1 <= len(profiles) <= 64:
                raise ValueError("NOR capability profile table is invalid")
            for profile_id, layout in profiles.items():
                _nor_capability(capability, profile_id,
                                layout.get("apSHA256") if type(layout) is dict else None,
                                qemu_sha256)
        result[name] = (dict(capability) if name in ("nandPersistence", "norPersistence") else {
            "schemaVersion": 1, "controlProtocol": "pmu.adcRaw.v1", "channels": 8,
            "minimum": 0, "maximum": 65535, "qemuSHA256": qemu_sha256})
    if "virtualPeer" in manifest:
        from .virtual_peer import checked_capability
        result["virtualPeer"] = checked_capability(manifest["virtualPeer"], qemu_sha256)
    return result


def _firmware(plan, capability):
    identity = plan["identity"]
    profile = identity["profile"]
    board = identity.get("board")
    data = None
    if board is not None:
        geometry = board["geometry"]
        offset, size = geometry.get("nand-data-offset"), geometry.get("nand-data-bytes")
    else:
        # Read the pinned AP's descriptor bytes, not executable product code.
        data = _read(Path(identity["ap"]["path"]), 64 * 1024 * 1024)
        marker = b"nand_data" + bytes(95)
        found, start = [], 0
        resource_floor = max((p.get("size", 0) for p in profile.get("partitions", [])
                              if p.get("name") == "vela_resource.bin"), default=0)
        while (at := data.find(marker, start)) >= 0:
            start = at + 1
            if at % 4 or at + 116 > len(data):
                continue
            page, count, flags = struct.unpack_from("<3I", data, at + 104)
            if not flags and page and count and page * 2048 >= resource_floor and (page + count) * 2048 <= MAX_DATA:
                found.append((page * 2048, count * 2048))
        if not found:
            raise ValueError("Original NAND data partition was not found")
        furthest = max(offset + size for offset, size in found)
        candidates = set((offset, size) for offset, size in found if offset + size == furthest)
        if len(candidates) != 1:
            raise ValueError("Ambiguous original NAND data layout")
        offset, size = candidates.pop()
    formats = capability.get("dataFormats", {})
    if not isinstance(formats, dict) or len(formats) > 128:
        raise ValueError("Invalid declared NAND format table")
    nor_capability = identity.get("runtimeCapabilities", {}).get("norPersistence")
    nor = None
    nor_key = (_capability_profile_id(nor_capability["profiles"], plan["profileID"], identity["ap"]["sha256"])
               if nor_capability is not None else None)
    if nor_key is not None:
        nor = _nor_capability(nor_capability, nor_key, identity["ap"]["sha256"], identity["qemu"]["sha256"])
        ap = data if data is not None else _read(Path(identity["ap"]["path"]), 64 * 1024 * 1024)
        if len(ap) != nor["apBytes"]:
            raise ValueError("NOR AP declared size differs from layout")
        declared_ap_size = identity["ap"].get("size")
        if declared_ap_size is not None and (type(declared_ap_size) is not int or declared_ap_size != nor["apBytes"]):
            raise ValueError("NOR descriptor AP size differs from selected firmware")
        if board is not None:
            if type(board.get("geometry")) is not dict or type(board.get("nor_geometry")) is not dict:
                raise ValueError("Persistent NOR requires AP-derived exact board geometry")
            geometry = board["geometry"]
            pairs = (("ap-offset", "apOffset"), ("flash-bytes", "flashBytes"),
                     ("nv-offset", "offset"), ("factory-offset", "factoryOffset"))
            if any(geometry.get(key) != nor[field] for key, field in pairs):
                raise ValueError("NOR board geometry differs from exact capability")
            expected_config = (nor.get("configOffset", 0), nor.get("configBytes", 0))
            observed_config = (geometry.get("config-offset", 0), geometry.get("config-bytes", 0))
            if observed_config != expected_config:
                raise ValueError("NOR split-config geometry differs from exact capability")
            if ({k:v for k,v in board["nor_geometry"].items() if k != "apSHA256"}
                    != {k:v for k,v in {**nor, "profileID": plan["profileID"]}.items() if k != "apSHA256"}):
                raise ValueError("Runner's AP-derived NOR descriptor differs from exact core capability")
        # In generated mode the runner derives this same layout from the
        # selected AP before it gives any storage to QEMU. The manifest table
        # is tied to the exact AP and core; the host creates the empty snapshot
        # now so a new instance can boot directly into that normal path.
    firmware = Firmware(profile["model"], plan["profileID"], offset, size,
                        formats.get(plan["profileID"], "unknown"), nor["offset"] if nor else 0,
                        nor["bytes"] if nor else 0, nor["apSHA256"] if nor else "",
                        nor["apBytes"] if nor else 0, nor["apOffset"] if nor else 0,
                        nor["flashBytes"] if nor else 0, nor["factoryOffset"] if nor else 0,
                        nor.get("configOffset", 0) if nor else 0, nor.get("configBytes", 0) if nor else 0)
    firmware.validate()
    return firmware


def _instance_root(library_root, instance_id):
    return _path(library_root) / "instances" / _uuid(instance_id) / "storage"


def _open_or_create(root, model):
    """Open an instance store, rebuilding missing metadata in place.

    A storage folder without instance.json (interrupted creation, partial
    restore) is reinitialized when it has no committed HEAD; with a HEAD the
    model is recovered from that snapshot. Snapshot files are never touched.
    """
    if not root.exists():
        try:
            return InstanceStorage.create(root, model)
        except FileNotFoundError:
            # The host normally creates instances/<uuid>; create it if missing.
            from .firmware_catalog import _owned_directory
            os.close(_owned_directory(root.parent, create=True))
            return InstanceStorage.create(root, model)
    _private(root, directory=True)
    try:
        info = _json(root / "instance.json")
        healthy = info.get("schema") == 1 and type(info.get("model")) is str and ID.fullmatch(info["model"])
    except (FileNotFoundError, ValueError):
        healthy = False  # Unreadable metadata is set aside below, never deleted.
    if healthy:
        return InstanceStorage(root)
    from .maintenance import _read_json
    recovered = model
    head = (_read_json(root / "head.json") or {}).get("revision")
    if isinstance(head, str) and re.fullmatch(r"r-[0-9a-f]{32}", head):
        firmware = (_read_json(root / "revisions" / head / "revision.json") or {}).get("firmware") or {}
        if isinstance(firmware.get("model"), str) and ID.fullmatch(firmware["model"]):
            recovered = firmware["model"]
    if (root / "instance.json").exists() or (root / "instance.json").is_symlink():
        os.rename(root / "instance.json", root / ("instance.json.broken-" + uuid.uuid4().hex[:8]))
    _atomic_json(root / "instance.json", {"schema": 1, "model": recovered, "id": uuid.uuid4().hex})
    return InstanceStorage(root)


def _initial_pin(path):
    _private(path)
    fd, info = _open_file(path, MAX_DATA, private=True)
    try:
        digest = hashlib.sha256()
        while chunk := os.read(fd, 1024 * 1024):
            digest.update(chunk)
        after = os.fstat(fd)
        if (info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns) != (
                after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns):
            raise ValueError("Instance image changed while hashing")
        return digest.hexdigest()
    finally:
        os.close(fd)


def requested_virtual_identity(serial, address, model):
    if serial is None and address is None:
        return None
    limit = 21 if model.startswith("miwear.watch.n67") else 24 if model.startswith(("miwear.watch.n66", "miwear.watch.o66")) else 31
    if (type(serial) is not str or not 1 <= len(serial) <= limit or any(not 33 <= ord(char) <= 126 for char in serial)
            or type(address) is not str or re.fullmatch(r"[0-9A-F]{2}(:[0-9A-F]{2}){5}", address) is None
            ):
        raise ValueError("虚拟序列号或六组十六进制蓝牙地址格式无效")
    return {"serial": serial, "address": address}


def public_instance_identity(library_root, instance_id):
    """Read only public fields; never return synthetic keys or peer bindings."""
    try:
        root = _instance_root(library_root, _uuid(instance_id))
    except ValueError as error:
        raise ValueError("实例 ID 无效，应为小写 UUID") from error
    result = {"ok": True, "version": 1, "identity": None}
    if not root.exists():
        return result
    # Display only: a damaged snapshot reports no identity instead of failing
    # the instance list; starting the instance repairs or reports it.
    try:
        _private(root, directory=True)
        head_path = root / "head.json"
        if not head_path.exists():
            return result
        revision = _json(head_path).get("revision", "")
        if type(revision) is not str or re.fullmatch(r"r-[0-9a-f]{32}", revision) is None:
            raise ValueError("Invalid identity snapshot")
        folder = root / "revisions" / revision
        _private(folder, directory=True)
        path = folder / VIRTUAL_STATE
        if path.exists():
            _private(path)
            value = _virtual_value(_decode(_read(path, 4096, private=True)), instance_id=instance_id)
            result["identity"] = {key: value["identity"][key] for key in ("serial", "address")}
    except (OSError, ValueError, KeyError, TypeError):
        result["warning"] = "无法读取此实例保存的虚拟身份；下次启动时会自动整理实例数据"
    return result


def prepare_session_storage(library_root, session_root, instance_id, plan, *,
                            virtual_key_file=None, virtual_key_sha256=None, virtual_serial=None, virtual_address=None):
    """Snapshot accepted NAND into a new private session; never execute a core.

    The host creates library/instances/<uuid> mode 0700. Accepted revisions stay
    outside the guest grant. Working disks and private state are session copies.
    """
    instance_id = _uuid(instance_id)
    session_root = _path(session_root)
    base = {"available": False, "instanceID": instance_id, "kind": "nand-data-and-oob",
            "norPersistent": False, "consistency": "crash-consistent", "virtualPeerEnabled": False,
            "virtualPeerReason": "当前核心或固件未启用已核验的虚拟通信"}
    capability = plan["identity"].get("runtimeCapabilities", {}).get("nandPersistence")
    if capability is None:
        return {**base, "reason": "当前核心未声明已绑定身份的 NAND 写回能力；本次使用原临时数据盘"}
    firmware = _firmware(plan, capability)
    identity_model = (virtual_profile(firmware.profile) or (None, firmware.model))[1]
    requested_identity = requested_virtual_identity(virtual_serial, virtual_address, identity_model)
    root = _instance_root(library_root, instance_id)
    # An interrupted earlier run (crash, forced quit, lost receipt) is settled
    # here rather than refusing the start: a healthy stopped run is committed,
    # anything else rolls back to the last committed snapshot.
    from .maintenance import reset_instance
    reset_instance(library_root, instance_id)
    store = _open_or_create(root, firmware.model)
    clones = session_root / "instance-data"
    try:
        if store.info["model"] != firmware.model:
            raise ValueError("此实例的数据属于 " + store.info["model"] + "，不能用 " + firmware.model
                             + " 硬件预设启动；请换回原硬件预设，或新建实例")
        old = store.head()
        virtual_capability = plan["identity"].get("runtimeCapabilities", {}).get("virtualPeer")
        virtual_enabled = False
        if virtual_capability is not None:
            from .virtual_peer import checked_capability
            checked_capability(virtual_capability, plan["identity"]["qemu"]["sha256"])
            virtual_key = _capability_profile_id(virtual_capability.get("profiles", {}),
                                                 firmware.profile, firmware.nor_ap_sha256)
            virtual_layout = (virtual_capability.get("profiles", {}).get(virtual_key)
                              if virtual_key is not None else None)
            virtual_enabled = (virtual_profile(firmware.profile) is not None and bool(firmware.nor_bytes)
                and type(virtual_layout) is dict
                and _hex(virtual_layout.get("apSHA256"), 64))
        virtual_state = None
        previous_identity = None
        if old and VIRTUAL_SHA in old[1]:
            previous_identity, _, _ = _virtual_read(old[0] / VIRTUAL_STATE, instance_id=instance_id)
        # Only an explicit change of an existing identity starts a new device;
        # a first identity for existing data is added on top of that data.
        rebuild_identity = bool(old and requested_identity is not None and previous_identity is not None
            and any(previous_identity["identity"][key] != value for key, value in requested_identity.items()))
        identity_note = None
        if requested_identity is not None and not virtual_enabled:
            # The requested serial/address waits for a firmware with verified
            # virtual-peer support; the start itself is never refused for it.
            requested_identity, rebuild_identity = None, False
            identity_note = "所选固件没有已核验的虚拟通信支持，本次未写入虚拟序列号和蓝牙地址"
        if virtual_enabled and (not old or rebuild_identity or VIRTUAL_SHA not in old[1]):
            virtual_state = _create_virtual_state(plan["spec"]["project"], instance_id,
                profile_id=firmware.profile, ap_sha256=firmware.nor_ap_sha256 or None, virtual_key_file=virtual_key_file,
                virtual_key_sha256=virtual_key_sha256)
            if requested_identity is not None:
                virtual_state["identity"].update(requested_identity)
            if rebuild_identity and previous_identity is not None:
                virtual_state["identityVersion"] = previous_identity["identityVersion"] + 1
            _virtual_value(virtual_state, instance_id=instance_id)
        seed = plan["identity"]["prepared"].get("seed") if not old or rebuild_identity else None
        private_seed = None
        if seed:
            private_seed = session_root / "instance-initial-seed.bin"
            # Copy a hash-pinned synthetic seed, never factory/credential data.
            if _pin(seed["path"])["sha256"] != seed["sha256"]:
                raise ValueError("Synthetic seed identity changed")
            _copy(Path(seed["path"]), private_seed)
        transaction = store.begin(firmware, synthetic_seed=private_seed,
                                  synthetic_seed_sha256=seed["sha256"] if seed else None,
                                  virtual_state=virtual_state, rebuild_identity=rebuild_identity, _previous_head=old)
        clones.mkdir(mode=0o700)
        for name, _, _, _ in firmware.images:
            _copy(store.active / name, clones / name)
        carry_notes = list(transaction.get("notes", [])) + ([identity_note] if identity_note else [])
        binding = {**base, "available": True, "reason": "仅持久化 NAND 用户区和 OOB；NOR 尚未持久化",
                   "sessionRoot": str(session_root), "model": firmware.model, "profileID": firmware.profile,
                   "transactionID": store.active.name, "previousRevision": transaction["previous_revision"],
                   "dataImage": str(clones / "nand-data.bin"), "spareImage": str(clones / "nand-spare.bin"),
                   "dataSHA256": _initial_pin(clones / "nand-data.bin"),
                   "spareSHA256": _initial_pin(clones / "nand-spare.bin"),
                   "dataOffset": firmware.data_offset, "dataBytes": firmware.data_bytes,
                   "dataFormat": firmware.data_format, "qemuSHA256": capability["qemuSHA256"]}
        # This changes only newly authored revision files. The accepted HEAD
        # remains available until a healthy stop commits the new identity.
        if firmware.nor_bytes:
            binding.update(kind="nand-oob-and-nor", norPersistent=True,
                reason="NAND、OOB 与此精确固件声明的 NOR 可写区共同提交；文件系统干净卸载尚未验证",
                norImage=str(clones / "nor-config.bin"), norSHA256=_initial_pin(clones / "nor-config.bin"),
                norOffset=firmware.nor_offset, norBytes=firmware.nor_bytes, norInitialize=transaction["nor_initialize"],
                )
            if firmware.nor_ap_sha256:
                binding.update(norAPSHA256=firmware.nor_ap_sha256, norAPBytes=firmware.nor_ap_bytes,
                    norAPOffset=firmware.nor_ap_offset, norFlashBytes=firmware.nor_flash_bytes,
                    norFactoryOffset=firmware.nor_factory_offset)
            if firmware.nor_config_bytes:
                binding.update(norConfigOffset=firmware.nor_config_offset,
                               norConfigBytes=firmware.nor_config_bytes)
        if firmware.model != "miwear.watch.q65" and any(Path(item["path"]).name == "instance-props" for item in plan["identity"].get("builders", [])):
            # Firmware settings and boot clock, written into persist.db by the
            # runner before the core opens the private data image.
            from .instance_settings import materialize
            # The instance hardware preset binds the board options; a preset
            # without that device keeps the emulated default.
            settings_path, settings_sha = materialize(library_root, instance_id, clones, model=firmware.model)
            binding.update(settingsPath=settings_path, settingsSHA256=settings_sha)
        if (store.active / VIRTUAL_STATE).exists():
            _, _, digest = _virtual_read(store.active / VIRTUAL_STATE, instance_id=instance_id)
            _copy(store.active / VIRTUAL_STATE, clones / VIRTUAL_STATE)
            binding.update(virtualPeerStateSchema=1, virtualPeerStatePath=str(clones / VIRTUAL_STATE),
                           virtualPeerStateSHA256=digest, virtualPeerEnabled=virtual_enabled)
            if virtual_enabled:
                binding["virtualPeerReason"] = "本实例的合成通信身份与 NAND、OOB、NOR 在同一快照提交"
        if carry_notes:
            binding["reason"] = binding["reason"] + "。" + "；".join(carry_notes)
            binding["carryOver"] = carry_notes
        pending = _json(store.root / "pending.json")
        _atomic_json(store.root / "pending.json", {**pending, "sessionRoot": str(session_root)})
        _atomic_json(session_root / "instance-storage.json", binding)
        return binding
    except BaseException:
        if store.active is not None:
            store.finish(success=False)
        raise
    finally:
        store.close()


def validate_binding(binding, *, project, profile_id, model, qemu_sha256, capability, initial=True,
                     nor_capability=None, ap_sha256=None, virtual_capability=None):
    if not isinstance(binding, dict) or binding.get("available") is not True:
        raise ValueError("Expected an enabled instance storage binding")
    _uuid(binding.get("instanceID"))
    expected = _path(project).parent.parent / "session"
    if (binding.get("sessionRoot") != str(expected) or binding.get("profileID") != profile_id
            or binding.get("model") != model or not _hex(binding.get("qemuSHA256"), 64)
            or capability is None
            or re.fullmatch(r"r-[0-9a-f]{32}", binding.get("transactionID", "")) is None
            or type(binding.get("norPersistent")) is not bool or binding.get("consistency") != "crash-consistent"):
        raise ValueError("Instance storage differs from exact session/core/model")
    nor = None
    if binding["norPersistent"]:
        nor_key = (_capability_profile_id(nor_capability["profiles"], profile_id, ap_sha256)
                   if nor_capability is not None else None)
        if nor_key is None:
            raise ValueError("NOR capability lacks this exact firmware profile")
        nor = _nor_capability(nor_capability, nor_key, ap_sha256, qemu_sha256)
        source_layout = nor_capability["profiles"][nor_key]
        legacy_nor = set(source_layout) == {"apSHA256", "offset", "bytes", "factoryOffset"}
        if (type(binding.get("norInitialize")) is not bool or type(binding.get("norOffset")) is not int
                or type(binding.get("norBytes")) is not int
                or binding["norOffset"] != nor["offset"] or binding["norBytes"] != nor["bytes"]
                or (not legacy_nor and (
                    type(binding.get("norAPBytes")) is not int or binding["norAPBytes"] != nor["apBytes"]
                    or type(binding.get("norAPOffset")) is not int or binding["norAPOffset"] != nor["apOffset"]
                    or type(binding.get("norFlashBytes")) is not int or binding["norFlashBytes"] != nor["flashBytes"]
                    or type(binding.get("norFactoryOffset")) is not int
                    or binding["norFactoryOffset"] != nor["factoryOffset"]))
                or (legacy_nor and any(k in binding for k in
                    ("norAPSHA256", "norAPBytes", "norAPOffset", "norFlashBytes", "norFactoryOffset")))
                or (("norConfigOffset" in binding) != ("configOffset" in nor))
                or (("norConfigBytes" in binding) != ("configBytes" in nor))
                or ("configOffset" in nor and (
                    type(binding.get("norConfigOffset")) is not int
                    or type(binding.get("norConfigBytes")) is not int
                    or binding["norConfigOffset"] != nor["configOffset"]
                    or binding["norConfigBytes"] != nor["configBytes"]))):
            raise ValueError("NOR binding differs from capability")
    elif any(k in binding for k in ("norImage", "norSHA256", "norOffset", "norBytes", "norInitialize",
                                    "norAPSHA256", "norAPBytes", "norAPOffset", "norFlashBytes",
                                    "norFactoryOffset", "norConfigOffset", "norConfigBytes")):
        raise ValueError("Disabled NOR binding has unexpected writable state")
    firmware = Firmware(model, profile_id, binding.get("dataOffset"), binding.get("dataBytes"), binding.get("dataFormat"),
                        nor["offset"] if nor else 0, nor["bytes"] if nor else 0,
                        nor["apSHA256"] if nor else "", nor["apBytes"] if nor else 0,
                        nor["apOffset"] if nor else 0, nor["flashBytes"] if nor else 0,
                        nor["factoryOffset"] if nor else 0, nor.get("configOffset", 0) if nor else 0,
                        nor.get("configBytes", 0) if nor else 0)
    firmware.validate()
    for name, size, digest_key, role in firmware.images:
        path = expected / "instance-data" / name
        if binding.get(role) != str(path) or re.fullmatch(r"[0-9a-f]{64}", binding.get(digest_key, "")) is None:
            raise ValueError("Instance image path or identity is invalid")
        _private(path, size=size)
        # Preparation/commit own snapshot content integrity; launch only checks
        # this private working file's path, permissions and exact size.
    if "settingsPath" in binding or "settingsSHA256" in binding:
        path = expected / "instance-data" / "instance-settings.json"
        if binding.get("settingsPath") != str(path) or not _hex(binding.get("settingsSHA256"), 64):
            raise ValueError("Instance settings path or identity is invalid")
        _private(path)
    # Legacy bindings without these additive fields remain valid. Disabled
    # sessions may still carry immutable state for an older selected Engine.
    enabled = binding.get("virtualPeerEnabled", False)
    if type(enabled) is not bool:
        raise ValueError("Virtual peer enablement must be explicit boolean")
    has_state = any(key in binding for key in VIRTUAL_FIELDS)
    if has_state:
        path = expected / "instance-data" / VIRTUAL_STATE
        if (not all(key in binding for key in VIRTUAL_FIELDS) or binding["virtualPeerStatePath"] != str(path)
                or type(binding["virtualPeerStateSchema"]) is not int or binding["virtualPeerStateSchema"] != 1
                or not _hex(binding[VIRTUAL_SHA], 64)):
            raise ValueError("Virtual state binding path/schema/hash is invalid")
        if initial:
            _virtual_read(path, binding[VIRTUAL_SHA], instance_id=binding["instanceID"])
    if enabled:
        from .virtual_peer import checked_capability
        checked_capability(virtual_capability, qemu_sha256)
        virtual_descriptor = virtual_profile(profile_id)
        if not has_state or nor is None or virtual_descriptor is None:
            raise ValueError("Virtual peer requires exact profile and joint NOR storage")
    if _json(expected / "instance-storage.json") != binding:
        raise ValueError("Instance binding record changed")
    return firmware


def instance_arguments(binding, *, run_directory=None):
    arguments = ["--instance-data-image", binding["dataImage"],
                 "--instance-spare-image", binding["spareImage"],
                 "--instance-data-sha256", binding["dataSHA256"],
                 "--instance-spare-sha256", binding["spareSHA256"]]
    if binding.get("norPersistent") is True:
        arguments += ["--instance-nor-image", binding["norImage"], "--instance-nor-sha256", binding["norSHA256"]]
        if binding["norInitialize"]:
            arguments.append("--instance-nor-initialize")
    if binding.get("settingsPath"):
        arguments += ["--instance-settings", binding["settingsPath"]]
    if binding.get("virtualPeerEnabled") is True:
        run = _path(run_directory) if run_directory is not None else None
        if (run is None or run.parent != Path(binding["sessionRoot"])
                or re.fullmatch(r"m-[0-9a-f]{12}", run.name) is None):
            raise ValueError("Virtual peer output requires this session's owned run")
        arguments += ["--virtual-peer-state", binding["virtualPeerStatePath"],
                      "--virtual-peer-state-sha256", binding[VIRTUAL_SHA],
                      "--virtual-peer-output", str(run / VIRTUAL_OUTPUT)]
    return arguments


def _stopped_health(session, plan, binding):
    broker = session / "native-broker"
    if not broker.exists():
        if any(session.glob("m-*")):
            raise ValueError("Run directory exists without native ownership evidence")
        return False, "prepared-session-never-started"
    config = _json(broker / "config.json")
    if not (broker / "state.json").exists() and not (broker / "launch.json").exists():
        return False, "native-guest-not-launched"
    state = _json(broker / "state.json")
    run = config.get("run", "")
    if (config.get("approvalDigest") != plan["approvalDigest"]
            or re.fullmatch(r"m-[0-9a-f]{12}", run) is None
            or state.get("id") != config.get("id") or state.get("run") != run
            or state.get("state") not in ("exited", "failed") or state.get("cleanup") != "reaped"):
        raise ValueError("模拟器进程尚未确认退出，本次数据暂不提交；已保存的快照保持不变")
    if state["state"] == "failed":
        return False, "native-guest-launch-or-cleanup-failed"
    try:
        result = _bounded_json(session / run / "result.json", private=True)
    except (OSError, ValueError):
        return False, "missing-or-invalid-run-result"
    if state.get("exitCode") not in (0, 2):
        return False, "forced-or-failed-runner-exit"
    if (result.get("returncode") != 0 or result.get("guest_faults") or result.get("fault_addresses")
            or result.get("cortex_fault_status") or result.get("guest_halt_loop_reached")
            or result.get("qmp_error") or result.get("storage_writeback_error")
            or result.get("virtual_peer_error")
            or result.get("diagnostic_errors") or result.get("outcome") not in
            ("time_limit", "interrupted", "window_closed", "qemu_stopped", "guest_poweroff", "guest_reboot")):
        return False, "guest-or-storage-health-unconfirmed"
    if (result.get("data_writeback") is not True or result.get("data_image") != binding["dataImage"]
            or result.get("data_spare_image") != binding["spareImage"]):
        return False, "writeback-result-differs-from-instance"
    if binding.get("norPersistent") is True and (result.get("nor_writeback") is not True
            or result.get("nor_image") != binding["norImage"]
            or result.get("nor_offset") != binding["norOffset"] or type(result.get("nor_offset")) is not int
            or result.get("nor_bytes") != binding["norBytes"] or type(result.get("nor_bytes")) is not int
            or (("norConfigOffset" in binding) and (
                result.get("nor_config_offset") != binding["norConfigOffset"]
                or result.get("nor_config_bytes") != binding["norConfigBytes"]))
            or (("norConfigOffset" not in binding) and (
                "nor_config_offset" in result or "nor_config_bytes" in result))
            or result.get("nor_initialized") is not binding["norInitialize"]
            or result.get("instance_nor_sha256") != binding["norSHA256"]):
        return False, "nor-writeback-result-differs-from-instance"
    return True, "core-exited-and-synchronous-storage-writes-complete; guest-filesystem-clean-unverified"


def _virtual_result(session, binding):
    """Read only after native ownership/exit and all disk health checks pass."""
    if VIRTUAL_SHA not in binding:
        return None, None
    original, raw, digest = _virtual_read(Path(binding["virtualPeerStatePath"]), binding[VIRTUAL_SHA],
                                          instance_id=binding["instanceID"])
    if binding.get("virtualPeerEnabled") is not True:
        return raw, digest
    config = _json(session / "native-broker/config.json")
    # _stopped_health established exact run identity; repeat the path constraint
    # rather than ever accepting an output path from the guest's JSON.
    run_name = config.get("run", "")
    if type(run_name) is not str or re.fullmatch(r"m-[0-9a-f]{12}", run_name) is None:
        raise ValueError("Invalid owned virtual state run")
    run = session / run_name
    result = _bounded_json(run / "result.json", private=True)
    summary = result.get("virtual_peer_state")
    if (type(summary) is not dict or set(summary) != {"inputStateSHA256", "outputStateSHA256", "stateWritten"}
            or summary["inputStateSHA256"] != digest or type(summary["stateWritten"]) is not bool):
        raise ValueError("Virtual state result differs from instance")
    output = run / VIRTUAL_OUTPUT
    if not summary["stateWritten"]:
        if summary["outputStateSHA256"] is not None or output.exists() or output.is_symlink():
            raise ValueError("Unreported virtual state output")
        return raw, digest
    if not _hex(summary["outputStateSHA256"], 64):
        raise ValueError("Invalid virtual state output hash")
    updated, raw, digest = _virtual_read(output, summary["outputStateSHA256"], instance_id=binding["instanceID"])
    mutable = {"binding", "guestLinkBond"}
    if ((updated["binding"] is None and updated.get("guestLinkBond") is None) or
            {k: v for k, v in updated.items() if k not in mutable} !=
            {k: v for k, v in original.items() if k not in mutable}):
        raise ValueError("Virtual output changed immutable synthetic identity")
    return raw, digest


def finish_session_storage(library_root, session_root, instance_id):
    """Publish only after native-owned cleanup and exact run-result validation.

    Success means one crash-consistent NAND/OOB/optional NOR/private-state revision, not a
    guest filesystem unmount, successful migration, or completed OTA.
    """
    session = _path(session_root)
    binding = _json(session / "instance-storage.json")
    if binding.get("instanceID") != _uuid(instance_id):
        raise ValueError("提交的实例与这次运行准备的实例不一致，未改动任何数据")
    result_path = session / "instance-storage-result.json"
    if result_path.exists():
        previous_result = _json(result_path)
        if previous_result.get("transactionID") != binding.get("transactionID"):
            raise ValueError("Finished storage receipt belongs to another transaction")
        return previous_result
    plan = _bounded_json(session / "backend-plan.json", private=True)
    from .minative import _digest
    descriptor = _json(session / "session.json")
    if (plan.get("approvalDigest") != _digest(plan["identity"])
            or descriptor.get("approvalDigest") != plan["approvalDigest"]
            or plan["identity"].get("instanceStorage") != binding):
        raise ValueError("Instance transaction is not bound to this prepared session")
    identity = plan["identity"]
    firmware = validate_binding(binding, project=plan["spec"]["project"], profile_id=plan["profileID"],
        model=identity["profile"]["model"], qemu_sha256=identity["qemu"]["sha256"],
        capability=identity.get("runtimeCapabilities", {}).get("nandPersistence"), initial=False,
        nor_capability=identity.get("runtimeCapabilities", {}).get("norPersistence"), ap_sha256=identity["ap"]["sha256"],
        virtual_capability=identity.get("runtimeCapabilities", {}).get("virtualPeer"))
    healthy, reason = _stopped_health(session, plan, binding)
    virtual_raw, virtual_sha = None, binding.get(VIRTUAL_SHA)
    if healthy:
        try:
            virtual_raw, virtual_sha = _virtual_result(session, binding)
        except (OSError, ValueError):
            healthy, reason = False, "virtual-state-result-invalid; previous-snapshot-preserved"
    receipt = {"ok": True, "version": 1, "instanceID": instance_id,
               "transactionID": binding["transactionID"], "committed": healthy,
               "previousRevision": binding["previousRevision"],
               "revision": binding["transactionID"] if healthy else binding["previousRevision"],
               "consistency": "crash-consistent" if healthy else "previous-snapshot-preserved",
               "norPersistent": binding["norPersistent"], "guestFilesystemClean": False, "reason": reason}
    if VIRTUAL_SHA in binding:
        receipt.update(virtualPeerStateSchema=1, virtualPeerStateSHA256=virtual_sha,
                       virtualPeerEnabled=binding.get("virtualPeerEnabled", False))
    store = InstanceStorage(_instance_root(library_root, instance_id))
    held = []
    try:
        # Finish is retried when a stopped app/helper loses the final receipt.
        # Verify the durable endpoint, including the actual accepted bytes,
        # rather than treating a missing pending record as an accepted run.
        held = store._idle_files(session / "instance-data", firmware)
        if not (store.root / "pending.json").exists():
            head = store.head()
            head_id = head[0].name if head else None
            if healthy:
                if (head_id != binding["transactionID"] or head[2] != firmware
                        or head[1].get("previous") != binding["previousRevision"]
                        or head[1].get(VIRTUAL_SHA) != virtual_sha
                        or any(head[1].get(key) != _initial_pin(Path(binding[role]))
                               for _, _, key, role in firmware.images)):
                    raise ValueError("找不到这次运行对应的数据提交记录；已保存的快照保持不变")
            elif (head_id != binding["previousRevision"] or
                  (store.root / "revisions" / binding["transactionID"]).exists()):
                raise ValueError("找不到这次运行对应的数据提交记录；已保存的快照保持不变")
            _atomic_json(result_path, receipt)
            return receipt
        pending = _json(store.root / "pending.json")
        if (pending.get("revision") != binding["transactionID"] or pending.get("sessionRoot") != str(session)
                or pending.get("firmware") != firmware.record
                or pending.get(VIRTUAL_SHA) != binding.get(VIRTUAL_SHA)):
            raise ValueError("此实例的未完成运行记录属于另一次会话；请重置该实例后再启动，已保存的数据不受影响")
        store.active = store.root / "revisions" / pending["revision"]
        if healthy:
            for source_fd, (name, _, _, _) in zip(held, firmware.images):
                target = store.active / (".accepted-" + uuid.uuid4().hex + "-" + name)
                fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
                try:
                    offset = 0
                    while chunk := os.pread(source_fd, 1024 * 1024, offset):
                        view = memoryview(chunk)
                        while view:
                            amount = os.write(fd, view)
                            if amount <= 0: raise OSError("Short instance image write")
                            view = view[amount:]
                        offset += len(chunk)
                    os.fsync(fd)
                    os.replace(target, store.active / name)
                finally:
                    os.close(fd)
                    if target.exists(): target.unlink()
            if virtual_raw is not None:
                target = store.active / (".accepted-" + uuid.uuid4().hex + "-" + VIRTUAL_STATE)
                try:
                    _virtual_write(target, virtual_raw)
                    os.replace(target, store.active / VIRTUAL_STATE)
                finally:
                    if target.exists(): target.unlink()
        store.finish(success=healthy)
        _atomic_json(result_path, receipt)
        if healthy and binding.get("settingsPath"):
            # Informational: the firmware's own settings as committed now.
            try:
                from .instance_settings import record_observed
                run = _json(session / "native-broker" / "config.json").get("run", "")
                if re.fullmatch(r"m-[0-9a-f]{12}", run):
                    record_observed(library_root, instance_id, session / run)
            except (OSError, ValueError):
                pass
        return receipt
    finally:
        for fd in held: os.close(fd)
        store.close()
