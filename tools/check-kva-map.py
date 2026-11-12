#!/usr/bin/env python3
"""Check the bootstrap page tables of XNU from the RAM of the phone.

XNU builds two bootstrap maps in start.s: a V=P one (TTBR0) and a KVA one
(TTBR1). The kernel text is fetched through the KVA map, so if that map is
wrong the first instruction fetch after the MMU is enabled aborts: the
exception vector cannot be fetched either, the kernel faults in a loop and
writes nothing. That is what the markers of the phone show: marker 11 (the
one right before the write of SCTLR_EL1) is there, marker 12 (right after)
is not, and no exception was recorded.

This script reads the translation state that the kernel wrote (TCR_EL1,
TTBR0_EL1 and TTBR1_EL1 are in the info block at 0x90003800) and walks the
tables in the RAM, to see where the kernel entry really lands.

    tools/check-kva-map.py [PAYLOAD]

PAYLOAD is the shim/out/*.bin that was booted (its header carries the
virtual and physical base of the kernel). Without it the script only
prints the tables it finds.

Everything is read with the guard of tools/read-pram.py: a read outside the
RAM of the G4 hangs the AXI bus of the phone.
"""
import importlib.util
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("rp", ROOT / "tools" / "read-pram.py")
rp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rp)

# ARM64 translation table descriptors (osfmk/arm64/proc_reg.h).
TTE_VALID = 1 << 0
TTE_TYPE_MASK = 3 << 1
TTE_TABLE = 3 << 1
TTE_BLOCK = 1 << 1
TTE_TABLE_MASK = 0x0000FFFFFFFFF000
TTE_BLOCK_MASK = 0x0000FFFFFFFF0000
TTE_AF = 1 << 10
TTE_NX = 1 << 54
TTE_PNX = 1 << 53

# The granule codes of TCR_EL1 (proc_reg.h: TCR_TG0/TG1_GRANULE_*).
TG_4KB = 0
TG_64KB = 1
TG_16KB = 2


def read_at(pa, size):
    """Read `size` bytes at `pa`, which may be unaligned: the page that
    contains it is read and the wanted bytes are cut out."""
    page = pa & ~(rp.PAGE - 1)
    offset = pa - page
    count = (offset + size + rp.PAGE - 1) // rp.PAGE
    blob = rp.read_pa(page, count * rp.PAGE)
    return blob[offset:offset + size]


def entry64(pa):
    return struct.unpack("<Q", read_at(pa, 8))[0]


def is_table(e):
    return bool(e & TTE_VALID) and (e & TTE_TYPE_MASK) == TTE_TABLE


def is_block(e):
    return bool(e & TTE_VALID) and (e & TTE_TYPE_MASK) == TTE_BLOCK


def granule_of(tcr, high):
    """The granule of TTBR0 (low) or TTBR1 (high) and the shifts of the
    walk: L1 covers 2^39, L2 and L3 depend on the granule."""
    if high:
        code = (tcr >> 30) & 3
    else:
        code = (tcr >> 14) & 3
    if code == TG_16KB:
        return "16 KB", 25, 15
    if code == TG_4KB:
        return "4 KB", 30, 21
    if code == TG_64KB:
        return "64 KB", 24, 16
    return "? (reserved code %d)" % code, 30, 21


def walk(root, va, tcr, label, high):
    """Walk the tables of `root` for the virtual address `va`."""
    name, l2_shift, l3_shift = granule_of(tcr, high)
    print(f"  {label}: root {root:#x}, granule {name}")
    if name.startswith("?"):
        print("    (unknown granule: the walk is not possible)")
        return None

    l1_index = (va >> 39) & 0x1FF
    l1 = entry64(root + l1_index * 8)
    print(f"    L1[{l1_index:#x}] = {l1:#018x} "
          f"{'(table)' if is_table(l1) else '(NOT A TABLE)'}")
    if not is_table(l1):
        return None
    l2_table = l1 & TTE_TABLE_MASK

    l2_index = (va >> l2_shift) & 0x1FF
    l2 = entry64(l2_table + l2_index * 8)
    print(f"    L2[{l2_index:#x}] = {l2:#018x} "
          f"{'(table)' if is_table(l2) else '(block)' if is_block(l2) else '(NOT A TABLE)'}")
    if is_block(l2):
        pa = (l2 & TTE_BLOCK_MASK) | (va & ((1 << l2_shift) - 1))
        print(f"    => VA {va:#x} maps to PA {pa:#x} "
              f"(1 block of 2^{l2_shift} bytes)")
        return pa
    if not is_table(l2):
        return None
    l3_table = l2 & TTE_TABLE_MASK

    l3_index = (va >> l3_shift) & 0x1FF
    l3 = entry64(l3_table + l3_index * 8)
    print(f"    L3[{l3_index:#x}] = {l3:#018x} "
          f"{'(block)' if is_block(l3) else '(NOT A BLOCK)'}")
    if not is_block(l3):
        return None
    pa = (l3 & TTE_BLOCK_MASK) | (va & ((1 << l3_shift) - 1))
    attrs = []
    if l3 & TTE_AF:
        attrs.append("AF")
    if l3 & TTE_NX:
        attrs.append("NX")
    if l3 & TTE_PNX:
        attrs.append("PNX")
    attrs.append(f"attrindx {(l3 >> 2) & 0x7}")
    print(f"    => VA {va:#x} maps to PA {pa:#x} ({', '.join(attrs)})")
    return pa


def main(argv):
    payload = argv[1] if len(argv) > 1 else None

    print("=== the translation state the kernel wrote ===")
    # The info block is inside the marker page: read the page and slice it.
    page = rp.read_pa(rp.MARK_BASE, rp.PAGE)
    blob = page[rp.MARK_INFO - rp.MARK_BASE:]
    info = {}
    for rel, name in rp.INFO_FIELDS:
        info[name] = struct.unpack_from("<Q", blob, rel)[0]
    for name in ("phys_base", "mem_size", "TCR_EL1", "TTBR0_EL1", "TTBR1_EL1"):
        print(f"  {name:<10} {info[name]:#018x}")

    tcr = info["TCR_EL1"]
    ttbr0 = info["TTBR0_EL1"] & TTE_TABLE_MASK
    ttbr1 = info["TTBR1_EL1"] & TTE_TABLE_MASK
    if not tcr or not ttbr1:
        print("  (the kernel did not write the translation state)")
        return 1

    entry_va = None
    entry_pa = None
    if payload:
        head = Path(payload).read_bytes()[:0x48]
        if head[:8] != b"SWEETXNU":
            print(f"{payload}: not a SWEETXNU payload")
            return 1
        virt_base, entry_va, = struct.unpack_from("<Q", head, 0x10)
        phys_base, = struct.unpack_from("<Q", head, 0x38)
        # The entry VA is in the payload header (offset 0x18). The physical
        # address of the entry is the same offset inside the kernel image,
        # which the shim copied to phys_base + (va - virt_base).
        entry_pa = phys_base + (entry_va - virt_base)
        print(f"  payload: virt_base {virt_base:#x} phys_base {phys_base:#x}")
        print(f"  entry:   VA {entry_va:#x} -> PA {entry_pa:#x}")

    print()
    print("=== the KVA map (TTBR1): the kernel text comes from here ===")
    va = entry_va if entry_va else info["phys_base"]
    got = walk(ttbr1, va, tcr, "TTBR1", True)
    if got is not None and entry_pa is not None:
        print(f"    expected PA {entry_pa:#x}: "
              f"{'OK' if got == entry_pa else 'MISMATCH'}")

    print()
    print("=== the V=P map (TTBR0) ===")
    walk(ttbr0, 0x90003000, tcr, "TTBR0", False)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
