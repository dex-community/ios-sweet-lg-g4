import struct, sys
from capstone import *

d = open('/tmp/g4dump/aboot.img','rb').read()

# --- parse ELF32 program headers ---
e_phoff = struct.unpack_from('<I', d, 0x1c)[0]
e_phentsize = struct.unpack_from('<H', d, 0x2a)[0]
e_phnum = struct.unpack_from('<H', d, 0x2c)[0]
segs = []
for i in range(e_phnum):
    o = e_phoff + i*e_phentsize
    p_type, p_off, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack_from('<8I', d, o)
    if p_type == 1:
        segs.append((p_vaddr, p_off, p_filesz, p_memsz, p_flags))
        print(f"LOAD vaddr=0x{p_vaddr:08x} off=0x{p_off:x} filesz=0x{p_filesz:x} memsz=0x{p_memsz:x} flags={p_flags}")

# vaddr -> byte lookup (file backed)
def vread(vaddr, n):
    out = b''
    for va, off, fsz, msz, fl in segs:
        if va <= vaddr < va+fsz:
            o = off + (vaddr-va)
            out += d[o:o+n]
            vaddr += n
            n -= len(out)
            if n <= 0: break
    return out

def vfind(needle, va0=None):
    """find needle in file, return list of vaddr"""
    res = []
    s = 0
    while True:
        i = d.find(needle, s)
        if i < 0: break
        s = i+1
        for va, off, fsz, msz, fl in segs:
            if off <= i < off+fsz:
                res.append(va + (i-off))
                break
    return res

strings_of_interest = [b"mmc init failed!", b"target is not emmc boot", b"MMC Boot: MBR read failed!",
                       b"Could not read partition from mmc", b"failed to set sdc%u_iface_clk",
                       b"read_emmc", b"aboot", b"MMC Boot:"]
print("\n=== string vaddrs ===")
for s in strings_of_interest:
    for v in vfind(s):
        print(f"  {s[:40]!r:45} -> 0x{v:08x}")

# --- find literal pool words that are hardware addresses ---
TARGETS = {
 0xf9824900:'SDHCI_HC_BASE(hc_mem)', 0xf9824000:'SDHCI_CORE_BASE(core_mem)',
 0xfc400000:'GCC_BASE', 0x0f9824900:'SDHCI_HC', 0x0fc400000:'GCC',
 0xf9828000:'?', 0x07800000:'?', 0x0f9e00000:'?',
}
print("\n=== literal pool hits (4-byte constants) ===")
hits = {}
for va, off, fsz, msz, fl in segs:
    for o in range(off, off+fsz-4, 4):
        w = struct.unpack_from('<I', d, o)[0]
        if w in TARGETS:
            v = va + (o-off)
            hits.setdefault(w, []).append(v)
            print(f"  0x{w:08x} ({TARGETS[w]:24}) at vaddr 0x{v:08x} (file 0x{o:x})")
