#!/usr/bin/env python3
"""Read XNU's embedded panic log out of the phone's RAM.

XNU writes its panic text into the region named by the device tree's "pram"
node (pe_init.c: check_for_panic_log() -> ml_io_map_wcomb()). On the G4 that
region is 0xc0000000 (tools/mkadt.py: G4_PANIC_LOG_PA), in the middle of the
upper DRAM bank: plain System RAM, which the recovery can read back through
/dev/mem after the watchdog reboot.

    tools/read-pram.py PANIC_PA PANIC_SIZE [OUT.bin]
    tools/read-pram.py --marks

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


def show_marks(argv):
    """Decode the page the boot markers of patch 0082 are written to."""
    base = 0x90003000
    blob = read_pa(base, 0x1000)
    # offset -> (marker number, label), in the order the kernel reaches them.
    names = {
        0x00: (1, "_start reached"),
        0x40: (2, "bootstrap page tables built"),
        0x100: (5, "common_start entered"),
        0x140: (6, "TCR_EL1 written"),
        0x180: (7, "TTBR0/TTBR1 written"),
        0x1c0: (8, "MAIR_EL1 written, TLB invalidated"),
        0x200: (9, "VBAR_EL1 set"),
        0x240: (10, "about to write SCTLR_EL1"),
        0x80: (3, "first fetch with the MMU on"),
        0xc0: (4, "returning into arm_init"),
    }
    print(f"boot markers at {base:#x}:")
    last = None
    for off, (num, name) in names.items():
        val = struct.unpack_from("<I", blob, off)[0]
        ok = val == 0x5EED0000 | num
        print(f"  +{off:#04x}  {val:#010x}  {'OK ' if ok else '--- '} "
              f"{num:2d} {name}")
        if ok:
            last = (num, name)
    if last:
        print(f"  => the kernel got as far as marker {last[0]}: {last[1]}")
    else:
        print("  => no marker: _start never ran, the jump itself is wrong")

    info_off = 0x800
    phys_base, mem_size, tcr, ttbr0, sctlr = struct.unpack_from(
        "<5Q", blob, info_off)
    print(f"info at {base + info_off:#x} (written with marker 2 and 3):")
    print(f"  phys_base {phys_base:#x}  mem_size {mem_size:#x}")
    print(f"  end       {phys_base + mem_size:#x} "
          "(the upper DRAM bank ends at 0xe0000000)")
    print(f"  TCR_EL1   {tcr:#018x}")
    print(f"  TTBR0_EL1 {ttbr0:#018x}")
    print(f"  SCTLR_EL1 {sctlr:#018x}")
    if not phys_base and not mem_size:
        print("  (all zero: the kernel died before marker 2)")
    elif not tcr:
        print("  (translation registers blank: the kernel died before marker 3)")
    return 0


def main(argv):
    if len(argv) >= 2 and argv[1] == "--marks":
        return show_marks(argv)
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
