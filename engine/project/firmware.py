"""Hash-locked Q66 AP analysis. Addresses are derived from this image only."""
from __future__ import annotations

import hashlib
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_AP = ROOT.parent / "Azki Music/xiaomi-band-11-177/firmware/4.100.177/vela_ap.bin"
EXPECTED_SHA256 = "ff74c6f467529963b6afd40669437d8ec39a02e48c10d0893739847b3ba3dc2b"
FLASH_BASE = 0x0C000000
AP_BASE = 0x0C0C0000
DATA_BASE = 0x2C0C0000


class Firmware:
    def __init__(self, path: Path = DEFAULT_AP):
        self.path = Path(path).resolve()
        self.data = self.path.read_bytes()
        self.sha256 = hashlib.sha256(self.data).hexdigest()
        if self.sha256 != EXPECTED_SHA256:
            raise ValueError(f"Unsupported AP SHA-256: {self.sha256}; expected {EXPECTED_SHA256}")

    def word(self, offset):
        return struct.unpack_from("<I", self.data, offset)[0]

    @property
    def entry(self):
        return self.word(0x64)

    @property
    def stack(self):
        return self.word(0x68)

    def copy_regions(self):
        # Literal pools referenced by original reset/copy routines. These are
        # not inferred from a different BES board's linker addresses.
        specs = [
            ("boot_sram", 0x1D0, 0x1D4, 0x1D8, 0x0C0C01A0, True),
            ("sram", 0x22C, 0x230, 0x234, 0x0C0C01E4, True),
            ("fast_sram", 0x238, 0x23C, 0x240, 0x0C0C01E4, True),
            ("psram", 0x2A4, 0x2A8, 0x2AC, 0x0C0C024C, True),
            ("psram_nc", 0x2B8, 0x2BC, 0x2C0, 0x0C0C024C, True),
            ("data", 0x70, 0x74, 0x78, 0x0C0C003A, False),
        ]
        regions = []
        for name, src_o, dst_o, end_o, routine, end_is_src in specs:
            src, dst, end = (self.word(x) for x in (src_o, dst_o, end_o))
            length = end - (src if end_is_src else dst)
            offset = src - DATA_BASE
            if length < 0 or offset < 0 or offset + length > len(self.data):
                raise ValueError(f"Invalid copy region: {name}")
            regions.append(dict(name=name, source=src, destination=dst, length=length,
                                ap_offset=offset, routine=routine,
                                literal_offsets=[src_o, dst_o, end_o]))
        return regions

    def zero_regions(self):
        return [dict(name=name, start=self.word(a), end=self.word(b), literal_offsets=[a,b])
                for name,a,b in [("boot_bss",0x1DC,0x1E0),("sram_bss",0x244,0x248),
                                 ("psram_bss",0x2B0,0x2B4),("psram_nc_bss",0x2C4,0x2C8),
                                 ("bss",0x7C,0x80)]]

    def disassemble(self, address, length=128):
        from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_MCLASS
        offset = self.resolve_address(address)
        if offset is None:
            return []
        cs = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
        return [dict(address=i.address, bytes=i.bytes.hex(), mnemonic=i.mnemonic, operands=i.op_str)
                for i in cs.disasm(self.data[offset:offset+length], address)]

    def resolve_address(self,address):
        for base in (AP_BASE,DATA_BASE,0x080C0000,0x280C0000):
            if base<=address<base+len(self.data):
                return address-base
        if 0x00200000<=address<0x00400000:
            address=address-0x00200000+0x20000000
        if 0x1C000000<=address<0x1C800000:
            address+=0x20000000
        if 0x18000000<=address<0x18800000:
            address+=0x24000000
        if 0x38000000<=address<0x38800000:
            address+=0x04000000
        for q in self.copy_regions():
            if q["destination"]<=address<q["destination"]+q["length"]:
                return q["ap_offset"]+address-q["destination"]
        return None

    def builtin_apps(self):
        # 16-byte entries match openvela's struct builtin_s without optional
        # identity/task-heap fields. Require bounded names, Thumb targets,
        # plausible priorities/stacks and the terminating null entry.
        entries=[]
        for offset in range(0x94061C,0x94089C,16):
            name_ptr,priority,stack,entry=struct.unpack_from("<4I",self.data,offset)
            name_offset=name_ptr-DATA_BASE
            if not (0<=name_offset<len(self.data) and 0<=priority<=255 and
                    0<stack<=0x100000 and entry&1 and self.resolve_address(entry&~1) is not None):
                raise ValueError("Invalid builtin entry")
            end=self.data.find(b"\0",name_offset,name_offset+100)
            if end<0:
                raise ValueError("Invalid builtin name")
            name=self.data[name_offset:end].decode("ascii")
            entries.append(dict(name=name,priority=priority,stack_bytes=stack,entry=entry,ap_offset=offset))
        if self.data[0x94089C:0x9408AC]!=bytes(16):
            raise ValueError("Missing builtin table terminator")
        return entries

    def romfs(self):
        """Return structurally valid embedded ROMFS images, not magic strings."""
        results = []
        for match in re.finditer(b"-rom1fs-", self.data):
            off = match.start()
            size = struct.unpack_from(">I", self.data, off+8)[0]
            if size < 32 or off+size > len(self.data) or size % 16:
                continue
            blob = self.data[off:off+size]
            check = blob[:min(512,size)]
            if sum(struct.unpack(f">{len(check)//4}I",check)) & 0xFFFFFFFF:
                continue
            name_end = blob.find(b"\0",16)
            if name_end < 16:
                continue
            results.append(dict(offset=off,size=size,volume=blob[16:name_end].decode("utf-8"),
                                sha256=hashlib.sha256(blob).hexdigest()))
        return results

    def report(self):
        trailer_start = self.data.rfind(b"CHIP=")
        trailer = self.data[trailer_start:].split(b"\0",1)[0].decode("ascii")
        properties = dict(line.split("=",1) for line in trailer.splitlines() if "=" in line)
        return dict(schema_version=1, firmware="4.100.177", ap_path=str(self.path),
                    ap_sha256=self.sha256, ap_bytes=len(self.data), properties=properties,
                    architecture_evidence={"chip":"best1503", "cpu":"Cortex-M33",
                        "source":"vendor/bes/chips/bes/Kconfig: ARCH_CHIP_BES1503", 
                        "note":"Instruction model; complete SoC and ROM are not included."},
                    entry=self.entry, bootstrap=AP_BASE+0x10, initial_msp=self.stack,
                    initial_msplim=self.word(0x6C), vector_table=0x2006BC00,
                    copy_regions=self.copy_regions(), zero_regions=self.zero_regions(),
                    romfs=self.romfs(), entry_disassembly=self.disassemble(AP_BASE+0x10,0x54),
                    builtin_apps=self.builtin_apps(),
                    interface_anchors={"lv_timer_handler":0x0C3AC47C,
                        "app_lookup":0x0C6AA915,"page_lookup":0x0C693E0D,"page_register":0x0C694BC5,
                        "miwear_main_from_builtin_table":0x0C7DD2D9},
                    limitations=["AP handoff only; Boot ROM and bootloaders not emulated",
                        "Public openvela is a source reference, not an exact product source build",
                        "Display and touch controllers are not implemented"])

    def romfs_entries(self, image):
        """Bounded, read-only ROMFS traversal; never follows host symlinks."""
        blob=self.data[image["offset"]:image["offset"]+image["size"]]
        visited=set()
        entries=[]
        def name_at(offset):
            end=blob.find(b"\0",offset)
            if end<offset:
                raise ValueError("Unterminated ROMFS name")
            return blob[offset:end].decode("utf-8"),(end+16)&~15
        def chain(offset,parent,depth=0):
            if depth>32:
                raise ValueError("ROMFS directory depth exceeded")
            while offset:
                if offset%16 or offset+16>len(blob) or offset in visited:
                    raise ValueError("Invalid/cyclic ROMFS entry")
                visited.add(offset)
                raw,spec,size,checksum=struct.unpack_from(">4I",blob,offset)
                name,start=name_at(offset+16)
                kind=raw&7
                if name not in (".",".."):
                    if not name or "/" in name or "\\" in name:
                        raise ValueError("Unsafe ROMFS filename")
                    path=f"{parent}/{name}".lstrip("/")
                    entry=dict(path=path,type=kind,size=size,offset=offset,data_offset=start)
                    if kind in (2,3):
                        if start+size>len(blob):
                            raise ValueError("Truncated ROMFS file")
                        data=blob[start:start+size]
                        entry["sha256"]=hashlib.sha256(data).hexdigest()
                        if kind==3:
                            entry["target"]=data.decode("utf-8")
                    entries.append(entry)
                    if kind==1:
                        chain(spec,path,depth+1)
                offset=raw&~15
        _,first=name_at(16)
        chain(first,"")
        return entries

    def extract_romfs(self,output: Path):
        inventories=[]
        for image in self.romfs():
            entries=self.romfs_entries(image)
            root=output / f"romfs-{image['offset']:x}"
            root.mkdir(parents=True,exist_ok=True)
            for entry in entries:
                if entry["type"]!=2:
                    continue
                dest=root/entry["path"]
                if not dest.resolve().is_relative_to(root.resolve()):
                    raise ValueError("ROMFS extraction path escaped destination")
                dest.parent.mkdir(parents=True,exist_ok=True)
                start=image["offset"]+entry["data_offset"]
                dest.write_bytes(self.data[start:start+entry["size"]])
            inventories.append(dict(**image,output=str(root),entries=entries))
        return inventories
