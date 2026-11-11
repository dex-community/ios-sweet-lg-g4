#!/usr/bin/env python3
"""Read the console log of XNU out of the phone's RAM.

XNU keeps every byte that reaches the console in a 4 MiB ring in the kernel
BSS (bsd/dev/sweet_ufs.c, the sweet_log_append hook on _cnputs). The display
of this phone does not work yet and the serial line is on the headphone jack,
so the ring is the only way to see what the kernel said.

The phone reboots when the kernel panics, but the RAM keeps its content, so
the ring can be read afterwards with a kernel that has CONFIG_DEVMEM (the
NetHunter kernel), the same way tools/read-shim-log.py reads the log of the
shim.

    tools/read-xnu-log.py KERNEL PAYLOAD          # addresses only
    tools/read-xnu-log.py KERNEL PAYLOAD          # and read from the phone
    tools/read-xnu-log.py KERNEL PAYLOAD dump.bin # from a dump

KERNEL is the kernel.development.sweet from the CI, PAYLOAD the shim/out/*.bin
that was booted (its header carries virt_base and phys_base). The physical
address of a kernel symbol is:

    pa = va - virt_base + phys_base

which is the same mapping phystokv() uses (osfmk/arm64/arm_vm_init.c).
"""
import struct
import subprocess
import sys

PAGE = 4096
RING_SIZE = 4 << 20


def payload_header(path):
    with open(path, "rb") as f:
        head = f.read(0x48)
    magic = head[:8]
    if magic != b"SWEETXNU":
        sys.exit(f"{path}: not a SWEETXNU payload")
    version, nsegs = struct.unpack_from("<II", head, 8)
    virt_base, = struct.unpack_from("<Q", head, 0x10)
    phys_base, = struct.unpack_from("<Q", head, 0x38)
    return dict(virt_base=virt_base, phys_base=phys_base, nsegs=nsegs)


def macho_symbols(path):
    """The symbols of a Mach-O: {name: (address, size)}."""
    with open(path, "rb") as f:
        buf = f.read()
    magic, = struct.unpack_from("<I", buf, 0)
    if magic != 0xfeedfacf:
        sys.exit(f"{path}: not a Mach-O")
    ncmds, = struct.unpack_from("<I", buf, 16)
    off = 32
    symtab = None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", buf, off)
        if cmd == 2:		# LC_SYMTAB
            _, _, symoff, nsyms, stroff, strsize = struct.unpack_from("<6I", buf, off)
            symtab = (symoff, nsyms, stroff, strsize)
        off += size
    if symtab is None:
        sys.exit(f"{path}: no symbol table (is the kernel stripped?)")
    symoff, nsyms, stroff, strsize = symtab
    out = {}
    for i in range(nsyms):
        n_strx, n_type, _, n_value = struct.unpack_from("<IBBH", buf, symoff + i * 16)
        if n_value == 0:
            continue
        end = buf.index(b"\0", stroff + n_strx)
        name = buf[stroff + n_strx:end].decode("ascii", "replace")
        if name.startswith("_"):
            name = name[1:]
        out[name] = n_value
    return out


def read_phone(pa, size):
    skip = pa // PAGE
    count = (size + PAGE - 1) // PAGE
    cmd = ["adb", "exec-out",
           f"dd if=/dev/mem bs={PAGE} skip={skip} count={count} 2>/dev/null"]
    data = subprocess.run(cmd, capture_output=True).stdout
    if len(data) < PAGE:
        sys.exit(f"read {len(data)} bytes: no /dev/mem, or the kernel is "
                 "without CONFIG_DEVMEM?")
    return data


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    kernel, payload = argv[1], argv[2]
    ph = payload_header(payload)
    syms = macho_symbols(kernel)

    for name in ("sweet_log_ring", "sweet_log_total"):
        if name not in syms:
            sys.exit(f"{name} is not in the symbol table of {kernel}")
    ring_va = syms["sweet_log_ring"]
    total_va = syms["sweet_log_total"]

    to_pa = lambda va: va - ph["virt_base"] + ph["phys_base"]
    ring_pa, total_pa = to_pa(ring_va), to_pa(total_va)
    print(f"virt_base 0x{ph['virt_base']:x}  phys_base 0x{ph['phys_base']:x}")
    print(f"sweet_log_ring  va 0x{ring_va:x}  pa 0x{ring_pa:x}")
    print(f"sweet_log_total va 0x{total_va:x}  pa 0x{total_pa:x}")

    dump = None
    for arg in argv[3:]:
        if arg.endswith(".bin"):
            dump = arg
    if dump is None:
        # The counter is 8 bytes, then the ring. Read them together when
        # they are close enough, otherwise in two reads.
        head = read_phone(total_pa, 8)
        total, = struct.unpack_from("<Q", head, 0)
        ring = read_phone(ring_pa, RING_SIZE)
    else:
        with open(dump, "rb") as f:
            blob = f.read()
        base = int(ph["phys_base"])
        total = struct.unpack_from("<Q", blob, total_pa - base)[0]
        ring = blob[ring_pa - base:ring_pa - base + RING_SIZE]

    if total == 0:
        sys.exit("the ring is empty: the kernel wrote nothing to the console")
    if total > RING_SIZE:
        print(f"the ring wrapped: {total} bytes written, the last "
              f"{RING_SIZE} are in the buffer")
        start = total - RING_SIZE
    else:
        start = 0
    text = bytes(ring[start:total]).decode("ascii", "replace")
    text = "".join(ch if (32 <= ord(ch) < 127 or ch in "\t\n\r") else "." for ch in text)
    sys.stdout.write(text)
    if not text.endswith("\n"):
        sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
