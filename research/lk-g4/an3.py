import struct, sys
from capstone import *

d = open('/tmp/g4dump/aboot.img','rb').read()
BASE_VA = 0x0f900000
TEXT_OFF, TEXT_SIZE = 0x8000, 0xe93d8

def vread(va, n):
    o = TEXT_OFF + (va - BASE_VA)
    if o < 0 or o+n > len(d): return None
    return d[o:o+n]

def find_strings():
    """return {vaddr: string} for printable ascii runs >=6"""
    out = {}
    i = TEXT_OFF
    end = TEXT_OFF+TEXT_SIZE
    run = b''
    start = 0
    while i < end:
        c = d[i]
        if 32 <= c < 127:
            if not run: start = i
            run += bytes([c])
        else:
            if len(run) >= 6:
                out[BASE_VA + (start-TEXT_OFF)] = run.decode()
            run = b''
        i += 1
    if len(run) >= 6:
        out[BASE_VA + (start-TEXT_OFF)] = run.decode()
    return out

STR = find_strings()
print("strings found:", len(STR))

# --- targeted PC-relative LDR scan ---
targets = {}
for va, s in STR.items():
    if any(k in s for k in ('mmc write failure','mmc read failure','MMC Boot','mmc init failed','not emmc boot','sdhci','mmc_dev','GPT:')):
        targets[va] = s[:60]

print("\n=== candidate strings ===")
for va,s in sorted(targets.items()):
    print(f"  0x{va:08x} {s!r}")

hits = []
for o in range(TEXT_OFF, TEXT_OFF+TEXT_SIZE-4, 4):
    W = struct.unpack_from('<I', d, o)[0]
    addr = BASE_VA + (o - TEXT_OFF)
    tgt = None
    # ARM LDR imm: cond 01 0 I P U B W L Rn Rt imm12
    # ARM: cond 010 P U 0 W L Rn=1111 Rt imm12  -> LDR Rt,[pc,#imm]
    if (W & 0x0FFF0000) in (0x051F0000, 0x059F0000):
        imm = W & 0xFFF
        U = (W>>23)&1
        base = (addr+8) & ~3
        tgt = base + (imm if U else -imm)
    # Thumb-2: ldr.w Rt,[pc,#imm12]  = F8DF Rt(4) imm12(12)
    elif (W & 0xFF7F0000) == 0xF8DF0000:
        imm = W & 0xFFF
        base = (addr+4) & ~3
        tgt = base + imm
    if tgt is not None:
        b = vread(tgt,4)
        if b is None: continue
        v = struct.unpack('<I', b)[0]
        if v in targets:
            hits.append((addr, tgt, v, 'arm'))

print("\n=== LDR xrefs ===")
for addr, pool, v, kind in hits:
    print(f"  0x{addr:08x}: ldr -> pool 0x{pool:08x} = 0x{v:08x}  {targets[v]!r}")
