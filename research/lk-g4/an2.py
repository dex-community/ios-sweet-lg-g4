import struct
from capstone import *

d = open('/tmp/g4dump/aboot.img','rb').read()
TEXT_VA, TEXT_OFF, TEXT_SIZE = 0x0f900000, 0x8000, 0xe93d8

def vread(vaddr, n):
    o = TEXT_OFF + (vaddr - TEXT_VA)
    return d[o:o+n]

md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
md.detail = True

code = vread(TEXT_VA, TEXT_SIZE)
insns = list(md.disasm(code, TEXT_VA))
print("total insns", len(insns))
idx = {i.address: k for k, i in enumerate(insns)}

TARGETS = {
 0x0f9648d8:'STR:mmc init failed!',
 0x0f966284:'STR:target is not emmc boot',
 0x0f9e4808:'LIT:GCC_BASE 0xfc400000',
 0x0f9e480c:'LIT:+4',
}

def lit_target(ins):
    """return resolved literal value for ldr rx,[pc,#imm]"""
    if ins.mnemonic.startswith('ldr') and len(ins.operands)==2:
        op = ins.operands[1]
        if op.type == 3:  # MEM
            base, disp = op.mem.base, op.mem.disp
            if base == 15 and ins.size == 4:
                pcv = (ins.address + 8) & ~3
                try:
                    return struct.unpack('<I', vread(pcv, 4))[0], pcv
                except Exception:
                    return None, None
    return None, None

refs = {}
for k, ins in enumerate(insns):
    val, pool = lit_target(ins)
    if val in TARGETS:
        refs.setdefault(TARGETS[val], []).append((ins.address, pool, k))

print("\n=== references ===")
for name, lst in refs.items():
    for addr, pool, k in lst:
        print(f"  {name:34} ldr @0x{addr:08x} (pool 0x{pool:08x})")

# --- dump function around a given instruction index: find prologue backwards ---
def find_func_start(k):
    for j in range(k, max(0, k-400), -1):
        ins = insns[j]
        if ins.mnemonic == 'push' and 'lr' in ins.op_str:
            return j
    return max(0, k-60)

def dump(name, k, before=40, after=160):
    fs = find_func_start(k)
    print(f"\n{'='*70}\n### {name}  func@0x{insns[fs].address:08x} .. refs@0x{insns[k].address:08x}\n{'='*70}")
    for j in range(fs, min(len(insns), fs+after)):
        ins = insns[j]
        mark = ' <<<' if j == k else ''
        # annotate literal
        ann = ''
        val, pool = lit_target(ins)
        if val is not None:
            ann = f'  ; =0x{val:08x}'
            if val in TARGETS: ann += ' ' + TARGETS[val]
        print(f"  {ins.address:08x}: {ins.mnemonic:8} {ins.op_str:32}{ann}{mark}")

import sys
which = sys.argv[1] if len(sys.argv)>1 else None
for name, lst in refs.items():
    if which and which not in name: continue
    for addr, pool, k in lst:
        dump(name, k)
