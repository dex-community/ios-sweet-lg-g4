#!/usr/bin/env python3
"""Read XNU's embedded panic log out of the phone's RAM.

XNU writes its panic text into the region named by the device tree's "pram"
node (pe_init.c: check_for_panic_log() -> ml_io_map_wcomb()). On the G4 that
region is 0xc0000000 (tools/mkadt.py: G4_PANIC_LOG_PA), in the middle of the
upper DRAM bank: plain System RAM, which the recovery can read back through
/dev/mem after the watchdog reboot.

    tools/read-pram.py PANIC_PA PANIC_SIZE [OUT.bin]
    tools/read-pram.py --marks
    tools/read-pram.py --exc

--marks decodes the boot markers of patch 0082 (osfmk/arm64/start.s): the
words 0x5EED000N written to the page at 0x90003000, the info block at
0x90003800 and the exception probe at 0x90005000. See MARKERS below for the
whole layout.

The address and the size must be the ones in the device tree:

    python3 tools/adt.py dump shim/out/g4.adt | grep -A3 'pram {'

Every read goes through /dev/mem with dd. The phone's AXI bus hangs if a
read lands outside RAM (there are holes at 0x06400000-0x0f3fffff and
0x0fe00000-0x0fefffff), so the address is checked against the memory map of
the G4 before anything is read. The persistent_ram window (0x0ff00000) is
listed in /proc/iomem as RAM but is NOT readable: a dd there returns nothing
and the recovery dies, so it is refused here too.
"""
import struct
import subprocess
import sys

PAGE = 4096
EMBEDDED_PANIC_MAGIC = 0x44454544		# "DEED"

# RAM the G4 really has, from the recovery's /proc/iomem. A read outside
# these windows hangs the bus (no abort, the CPU stalls until the watchdog
# gives up), so refuse it instead of trying. persistent_ram (0x0ff00000) is
# deliberately absent even though /proc/iomem lists it: reading it there kills
# the recovery, which means XNU cannot use it either.
G4_RAM = [
    (0x00000000, 0x063fffff),
    (0x07200000, 0x073fffff),
    (0x0f400000, 0x0fefffff),
    (0x10000000, 0x5fffffff),
    (0x80000000, 0xdfffffff),
]


def in_ram(pa, size):
    for lo, hi in G4_RAM:
        if pa >= lo and pa + size - 1 <= hi:
            return True
    return False


def read_pa(pa, size):
    if pa % PAGE:
        sys.exit(f"PA {pa:#x} is not page aligned")
    if not in_ram(pa, size):
        sys.exit(f"PA {pa:#x}+{size:#x} is outside RAM: a read here hangs the bus")
    skip = pa // PAGE
    count = (size + PAGE - 1) // PAGE
    cmd = ["adb", "exec-out",
           f"dd if=/dev/mem bs={PAGE} skip={skip} count={count} 2>/dev/null"]
    out = subprocess.run(cmd, capture_output=True).stdout
    if len(out) < size:
        sys.exit(f"read {len(out)} of {size} bytes at PA {pa:#x}: "
                 "no /dev/mem, or the phone is not in the recovery")
    return out[:size]


def decode(blob):
    if len(blob) < 0x40:
        return None
    (magic, crc, version, flags, log_off, log_len,
     ss_off, ss_len, other_off, other_len) = struct.unpack_from("<10I", blob, 0)
    if magic != EMBEDDED_PANIC_MAGIC:
        return {"magic": magic, "text": None}
    text = blob[log_off:log_off + log_len].split(b"\0")[0]
    return {
        "magic": magic, "crc": crc, "version": version, "flags": flags,
        "log_offset": log_off, "log_len": log_len,
        "stackshot": (ss_off, ss_len), "other": (other_off, other_len),
        "text": text.decode("ascii", "replace"),
    }


# Exception class of a synchronous exception, from ESR_EL1[31:26].
ESR_EC = {
    0x00: "unknown reason (undefined instruction?)",
    0x01: "trapped WF*",
    0x07: "access to SIMD/FP",
    0x0E: "illegal execution state",
    0x18: "trapped MSR/MRS (system register)",
    0x20: "instruction abort from a lower EL",
    0x21: "instruction abort from the same EL",
    0x22: "PC alignment",
    0x24: "data abort from a lower EL",
    0x25: "data abort from the same EL",
    0x26: "SP alignment",
    0x2C: "trapped FP exception",
    0x2F: "SError",
}

# Fault status of a data/instruction abort, from ESR_EL1[5:0].
DFSC = {
    0x05: "translation fault, level 1",
    0x06: "translation fault, level 2",
    0x07: "translation fault, level 3",
    0x09: "access flag fault, level 1",
    0x0A: "access flag fault, level 2",
    0x0B: "access flag fault, level 3",
    0x0D: "permission fault, level 1",
    0x0E: "permission fault, level 2",
    0x0F: "permission fault, level 3",
    0x10: "synchronous external abort",
    0x21: "alignment fault",
}


# Where the boot markers live, and how they are laid out. This is what
# patch 0082 (osfmk/arm64/start.s) writes, in the working tree of /tmp/gicbase:
#
#   0x90003000 + off   the marker word 0x5EED000N
#   0x90003800         the info block (see INFO_FIELDS)
#   0x90005000         the exception probe: ESR_EL1, FAR_EL1, ELR_EL1, SPSR_EL1
#   0x90003300         0x5EED0B00 | vector slot, when an exception is taken
#
# The page at 0x90003000 is plain System RAM of the upper DRAM bank, which
# the recovery does not reuse, so it survives the watchdog reset.
MARK_BASE = 0x90003000
MARK_INFO = 0x90003800
EXC_INFO = 0x90005000
MARK_MAGIC = 0x5EED0000

# (offset in the page, marker number, label), in the order the kernel reaches
# them. The number is the low byte of the marker word: 0x5EED000N.
MARKERS = [
    (0x000, 1, "_start reached (the bootloader's MMU is still on)"),
    (0x040, 2, "bootstrap page tables built"),
    (0x100, 5, "common_start entered, MMU still off"),
    (0x140, 6, "TCR_EL1 written from sysreg_restore"),
    (0x180, 7, "TTBR0/TTBR1 written"),
    (0x1c0, 8, "MAIR_EL1 written, TLB invalidated"),
    (0x200, 9, "VBAR_EL1 points at the exception probe"),
    (0x240, 10, "about to write SCTLR_EL1 (MMU + caches)"),
    (0x2c0, 11, "SCTLR_EL1 with M=1 written (MMU on, caches still off)"),
    (0x080, 3, "first fetch with the MMU and the caches on"),
    (0x0c0, 4, "returning into arm_init"),
    (0x340, 12, "IOKitBSDInit entered (matching the root device)"),
    (0x380, 13, "about to mount the root filesystem (HFS+)"),
]

# The info block: what the kernel computed for itself, 8 words at 0x90003800.
# phys_base and mem_size come from the boot args after the slide adjustment,
# the registers are sampled when the marker that follows them is written.
INFO_FIELDS = [
    (0x00, "phys_base"),
    (0x08, "mem_size"),
    (0x10, "TCR_EL1"),
    (0x18, "TTBR0_EL1"),
    (0x20, "SCTLR_EL1"),
    (0x28, "CurrentEL"),
    (0x30, "MPIDR_EL1"),
    (0x38, "MIDR_EL1"),
]


def show_exception():
    """Print what the exception probe (patch 0082, sweet_exc_vectors) saved."""
    blob = read_pa(EXC_INFO, 0x40)
    esr, far, elr, spsr = struct.unpack_from("<4Q", blob, 0)
    ec = (esr >> 26) & 0x3F
    print(f"exception registers at 0x90005000:")
    print(f"  ESR_EL1  {esr:#018x}  EC {ec:#x}: "
          f"{ESR_EC.get(ec, 'unknown class')}")
    if ec in (0x20, 0x21, 0x24, 0x25):
        print(f"           fault status {esr & 0x3F:#x}: "
              f"{DFSC.get(esr & 0x3F, 'unknown')}")
        if ec in (0x24, 0x25):
            print(f"           {'write' if esr & 0x40 else 'read'} access")
    print(f"  FAR_EL1  {far:#018x}")
    print(f"  ELR_EL1  {elr:#018x}  (the instruction that faulted)")
    print(f"  SPSR_EL1 {spsr:#018x}")


def decode_midr(midr):
    """Name the CPU out of MIDR_EL1, as far as it is worth it.

    The part number is the field at bits[15:4]: for the ARM cores it holds
    0xb00 for the Cortex-A53, 0xd0b for the Cortex-A76 and so on.
    """
    impl = (midr >> 24) & 0xFF
    part = (midr >> 4) & 0xFFF
    known = {
        0xB00: "Cortex-A53",
        0xD04: "Cortex-A35",
        0xD05: "Cortex-A55",
        0xD01: "Cortex-A57",
        0xD08: "Cortex-A72",
        0xD09: "Cortex-A73",
        0xD0A: "Cortex-A75",
        0xD0B: "Cortex-A76",
        0xD0C: "Neoverse-N1",
    }
    name = known.get(part)
    if impl == 0x41 and name:
        return name
    return f"implementer {impl:#x}, part {part:#x}"


def show_info(blob):
    """Decode the info block at 0x90003800."""
    off = MARK_INFO - MARK_BASE
    print(f"info at {MARK_INFO:#x}:")
    values = {}
    for rel, name in INFO_FIELDS:
        values[name] = struct.unpack_from("<Q", blob, off + rel)[0]
        print(f"  {name:<10} {values[name]:#018x}")
    print()

    phys_base = values["phys_base"]
    mem_size = values["mem_size"]
    if phys_base or mem_size:
        print(f"  phys_base {phys_base:#x} + mem_size {mem_size:#x} "
              f"= {phys_base + mem_size:#x}")
        print("  (the upper DRAM bank ends at 0xe0000000, the slot that "
              "holds the kernel ends at 0xdff00000)")
    current_el = values["CurrentEL"]
    if current_el:
        el = (current_el >> 2) & 0x3
        print(f"  the kernel is running at EL{el}")
        if el == 2:
            print("  (entered at EL2: XNU does not enable its own "
                  "translation here, the bootloader's tables stay in charge)")
    mpidr = values["MPIDR_EL1"]
    if mpidr:
        # MPIDR_EL1: Aff3 bits[39:32], Aff2 bits[23:16], Aff1 bits[15:8],
        # Aff0 bits[7:0]. Aff0 is the index of the core in its cluster.
        print(f"  core: aff3 {mpidr >> 32 & 0xFF:#x} aff2 "
              f"{mpidr >> 16 & 0xFF:#x} aff1 {mpidr >> 8 & 0xFF:#x} "
              f"aff0 {mpidr & 0xFF:#x} (CPU {mpidr & 0xFF:#x})")
    midr = values["MIDR_EL1"]
    if midr:
        print(f"  CPU: {decode_midr(midr)}")
    tcr = values["TCR_EL1"]
    if tcr:
        print(f"  TCR_EL1: T0SZ {tcr & 0x3F:#x} TG0 {(tcr >> 15) & 3:#x} "
              f"SH0 {(tcr >> 8) & 3:#x} IRGN0 {(tcr >> 12) & 7:#x} "
              f"ORGN0 {(tcr >> 10) & 7:#x} TG1 {(tcr >> 30) & 3:#x}")
    sctlr = values["SCTLR_EL1"]
    if sctlr:
        print(f"  SCTLR_EL1: M {sctlr & 1:#x} C {(sctlr >> 2) & 1:#x} "
              f"I {(sctlr >> 12) & 1:#x}")
    return values


def show_marks(argv):
    """Decode the page the boot markers of patch 0082 are written to."""
    blob = read_pa(MARK_BASE, 0x1000)
    print(f"boot markers at {MARK_BASE:#x}:")
    seen = {}
    for off, num, name in MARKERS:
        val = struct.unpack_from("<I", blob, off)[0]
        ok = val == MARK_MAGIC | num
        print(f"  +{off:#04x}  {val:#010x}  {'OK ' if ok else '--- '} "
              f"{num:2d} {name}")
        if ok:
            seen[num] = name

    # A marker word somewhere else means the layout moved: say so instead of
    # reading the wrong offsets.
    known = {off for off, _, _ in MARKERS}
    for off in range(0, 0x1000, 4):
        val = struct.unpack_from("<I", blob, off)[0]
        if val & 0xFFFFFF00 == MARK_MAGIC and off not in known:
            print(f"  +{off:#04x}  {val:#010x}  unexpected marker word "
                  "outside the known offsets")

    print()
    show_info(blob)
    print()

    # The exception probe: which vector ran, and the registers it saved.
    exc = struct.unpack_from("<I", blob, 0x300)[0]
    if exc & 0xFFFFFFF0 == 0x5EED0B00:
        slot = exc & 0xF
        kinds = {
            0x0: "current EL, SP0: synchronous",
            0x1: "current EL, SP0: IRQ",
            0x2: "current EL, SP0: FIQ",
            0x3: "current EL, SP0: SError",
            0x4: "current EL, SPx: synchronous",
            0x5: "current EL, SPx: IRQ",
            0x6: "current EL, SPx: FIQ",
            0x7: "current EL, SPx: SError",
            0x8: "lower EL, AArch64: synchronous",
            0x9: "lower EL, AArch64: IRQ",
            0xA: "lower EL, AArch64: FIQ",
            0xB: "lower EL, AArch64: SError",
            0xC: "lower EL, AArch32: synchronous",
            0xD: "lower EL, AArch32: IRQ",
            0xE: "lower EL, AArch32: FIQ",
            0xF: "lower EL, AArch32: SError",
        }
        print(f"exception probe at +0x0300: {exc:#010x} "
              f"EXCEPTION TAKEN: {kinds[slot]}")
        show_exception()
    elif exc not in (0, 0xFFFFFFFF):
        print(f"exception probe at +0x0300: {exc:#010x} (unexpected)")

    print()
    print("=> " + verdict(seen))
    return 0


def verdict(seen):
    """Say where the kernel stopped, out of the markers it wrote."""
    if 1 not in seen:
        return ("no marker 1: _start never ran, the jump into the kernel is "
                "wrong")
    if 2 not in seen:
        return ("marker 1 only: the kernel died building the bootstrap page "
                "tables")
    if 5 not in seen:
        return ("marker 2 only: the kernel died before common_start")
    if 6 not in seen:
        return "died in common_start, before TCR_EL1"
    if 7 not in seen:
        return "died writing TTBR0/TTBR1"
    if 8 not in seen:
        return "died writing MAIR_EL1 or invalidating the TLB"
    if 9 not in seen:
        return "died installing the exception probe in VBAR_EL1"
    if 10 not in seen:
        return "died between VBAR_EL1 and the SCTLR_EL1 write"
    if 11 not in seen:
        return ("marker 10 only: the SCTLR_EL1 write with M=1 faults, the "
                "MMU itself is the problem")
    if 3 not in seen:
        return ("marker 11 only: the MMU is on and the translation works, "
                "the full SCTLR_EL1 (the caches) kills the CPU")
    if 4 not in seen:
        return ("marker 3 only: the kernel runs with the MMU on and dies "
                "before returning into arm_init")
    if 12 not in seen:
        return ("marker 4: the kernel reached arm_init, but not IOKitBSDInit "
                "(the early console in the BSS has the details: "
                "tools/read-xnu-early.py)")
    if 13 not in seen:
        return ("marker 12: the kernel reached IOKitBSDInit, and died "
                "before mounting the root filesystem")
    return ("marker 13: the kernel reached the mount of the root filesystem "
            "(the HFS+ read). See the early console for what happens next")


def main(argv):
    if len(argv) >= 2 and argv[1] == "--marks":
        return show_marks(argv)
    if len(argv) >= 2 and argv[1] == "--exc":
        show_exception()
        return 0
    if len(argv) < 3:
        sys.exit(__doc__)
    pa = int(argv[1], 0)
    size = int(argv[2], 0)
    out = argv[3] if len(argv) > 3 else None
    if size > 0x100000:
        sys.exit("refusing to read more than 1 MiB of pram")

    blob = read_pa(pa, size)
    if out:
        open(out, "wb").write(blob)
        print(f"saved {len(blob)} bytes to {out}")

    info = decode(blob)
    if info is None:
        sys.exit("region too small")
    if info["magic"] != EMBEDDED_PANIC_MAGIC:
        nz = sum(1 for b in blob if b)
        print(f"PA {pa:#x}: no panic header (magic {info['magic']:#x}, "
              f"{nz} non-zero bytes of {len(blob)})")
        # Useful for any region, not just pram: show the first bytes.
        head = blob[:256]
        print("first 256 bytes:")
        for off in range(0, len(head), 16):
            chunk = head[off:off + 16]
            hexs = " ".join(f"{b:02x}" for b in chunk)
            text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
            print(f"  {pa + off:08x}  {hexs:<47}  {text}")
        for needle in (b"SWEET EARLY", b"G4SHIM", b"DEED"):
            at = blob.find(needle)
            if at >= 0:
                print(f"  contains {needle!r} at +{at:#x}")
        return 0

    print(f"PA {pa:#x}: panic header DEED version {info['version']} "
          f"flags {info['flags']:#x}")
    print(f"  log: offset {info['log_offset']} len {info['log_len']}")
    print(f"  stackshot: {info['stackshot']}  other: {info['other']}")
    print("--- panic text ---")
    print(info["text"])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
