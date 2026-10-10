"""Ports TMNF addresses to TMUF (the method from GOTCHA.md, "Two TmForever.exe builds").

The two executables are read from $TM_RE_BIN/tmnf/TmForever.exe and $TM_RE_BIN/tmuf/TmForever.exe
(default ~/tools/tm-re/bin; copies, because the game folders are under Program Files).

    python tools/port_address.py fn <tmnf_va> [length]   find the function's TMUF address
    python tools/port_address.py ptr <tmnf_va>          find where TMNF stores this pointer (vtable slots) - TMNF only
    python tools/port_address.py vt <tmnf_vtable> <slot> port a vtable: the TMUF vtable whose slot holds the ported function

Bytes covered by .reloc entries (absolute addresses) and the rel32 after E8/E9 calls/jumps are
masked; the match is searched within +-64 KB of the TMNF address and must be unique.
"""
import os
import struct
import sys
from pathlib import Path

BIN = Path(os.environ.get("TM_RE_BIN", Path.home() / "tools" / "tm-re" / "bin"))


class Pe:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        nsec = struct.unpack_from("<H", self.data, pe + 6)[0]
        opt = struct.unpack_from("<H", self.data, pe + 20)[0]
        self.base = struct.unpack_from("<I", self.data, pe + 24 + 28)[0]
        reloc_rva, reloc_size = struct.unpack_from("<II", self.data, pe + 24 + 96 + 5 * 8)
        self.sections = []
        for i in range(nsec):
            s = pe + 24 + opt + i * 40
            vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", self.data, s + 8)
            self.sections.append((vaddr, max(vsize, rsize), raddr, rsize))
        self.relocs = set()
        off = self.rva_to_off(reloc_rva) if reloc_size else None
        end = off + reloc_size if off is not None else 0
        while off is not None and off < end:
            page, size = struct.unpack_from("<II", self.data, off)
            if size == 0:
                break
            for j in range((size - 8) // 2):
                entry = struct.unpack_from("<H", self.data, off + 8 + j * 2)[0]
                if entry >> 12 == 3:
                    self.relocs.add(self.base + page + (entry & 0xFFF))
            off += size

    def rva_to_off(self, rva):
        for vaddr, vsize, raddr, rsize in self.sections:
            if vaddr <= rva < vaddr + vsize and rva - vaddr < rsize:
                return raddr + rva - vaddr
        return None

    def read(self, va, n):
        off = self.rva_to_off(va - self.base)
        return self.data[off:off + n]

    def masked(self, va, n):
        raw = bytearray(self.read(va, n))
        mask = bytearray(b"\xff" * n)
        for i in range(n):
            if va + i in self.relocs:
                mask[i:i + 4] = b"\0\0\0\0"
        for i in range(n - 4):
            if raw[i] in (0xE8, 0xE9) and mask[i]:
                mask[i + 1:i + 5] = b"\0\0\0\0"
        return bytes(raw), bytes(mask)

    def find(self, raw, mask, near, radius=0x10000):
        hits = []
        start = self.rva_to_off(near - radius - self.base) or 0
        stop = min(self.rva_to_off(near + radius - self.base) or len(self.data), len(self.data) - len(raw))
        for off in range(start, stop + 1):
            d = self.data
            if all((d[off + i] & mask[i]) == (raw[i] & mask[i]) for i in range(len(raw))):
                hits.append(off)
        return [self.off_to_va(o) for o in hits]

    def off_to_va(self, off):
        for vaddr, vsize, raddr, rsize in self.sections:
            if raddr <= off < raddr + rsize:
                return self.base + vaddr + off - raddr
        return None


def port_fn(nf, uf, va, length=64):
    raw, mask = nf.masked(va, length)
    hits = uf.find(raw, mask, va)
    if len(hits) == 1:
        raw256, mask256 = nf.masked(va, 256)
        if uf.find(raw256, mask256, hits[0], 16) == hits:
            return hits[0], "confirmed over 256 bytes"
        return hits[0], "unique over %d bytes (256 differ)" % length
    return None, "%d hits: %s" % (len(hits), " ".join("%08x" % h for h in hits[:8]))


def main():
    nf, uf = Pe(BIN / "tmnf" / "TmForever.exe"), Pe(BIN / "tmuf" / "TmForever.exe")
    cmd, va = sys.argv[1], int(sys.argv[2], 16)
    if cmd == "fn":
        addr, note = port_fn(nf, uf, va, int(sys.argv[3]) if len(sys.argv) > 3 else 64)
        print("TMNF %08x -> TMUF %s (%s)" % (va, "%08x" % addr if addr else "?", note))
    elif cmd == "vt":
        slot = int(sys.argv[3])
        fn = struct.unpack("<I", nf.read(va + slot * 4, 4))[0]
        addr, note = port_fn(nf, uf, fn)
        print("slot %d: TMNF %08x -> TMUF %s (%s)" % (slot, fn, "%08x" % addr if addr else "?", note))
        if addr:
            needle = struct.pack("<I", addr)
            spots = [uf.off_to_va(i) for i in range(len(uf.data) - 3) if uf.data[i:i + 4] == needle]
            for s in spots:
                print("  TMUF pointer at %08x -> vtable %08x (if slot %d)" % (s, s - slot * 4, slot))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
