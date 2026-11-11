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
            if symtab is None or nsyms > symtab[1]:
                symtab = (symoff, nsyms, stroff, strsize)
        off += size
    if symtab is None:
        sys.exit(f"{path}: no symbol table (is the kernel stripped?)")
    symoff, nsyms, stroff, strsize = symtab
    out = {}
    for i in range(nsyms):
        # struct nlist_64: n_strx(4) n_type(1) n_sect(1) n_desc(2) n_value(8)
        n_strx, n_type, _, _, n_value = struct.unpack_from("<IBBHQ", buf, symoff + i * 16)
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

    to_pa = lambda va: va - ph["virt_base"] + ph["phys_base"]

    dump = None
    for arg in argv[3:]:
        if arg.endswith(".bin"):
            dump = arg

    def fetch(name, size):
        """The bytes of a symbol, from the phone or from a dump."""
        va = syms[name]
        pa = to_pa(va)
        if dump is None:
            data = read_phone(pa, size)
        else:
            with open(dump, "rb") as f:
                blob = f.read()
            base = int(ph["phys_base"])
            data = blob[pa - base:pa - base + size]
        return va, pa, data

    print(f"virt_base 0x{ph['virt_base']:x}  phys_base 0x{ph['phys_base']:x}")

    # The log of the eMMC driver: it runs before the BSD console exists, so
    # it keeps its own copy in the BSS.
    if "emmc_log_buf" in syms and "emmc_log_len" in syms:
        _, _, lenv = fetch("emmc_log_len", 4)
        length, = struct.unpack_from("<I", lenv, 0)
        _, _, buf = fetch("emmc_log_buf", max(length, 1))
        text = buf[:length].decode("ascii", "replace")
        print(f"=== the eMMC driver ({length} bytes) ===")
        if text.strip():
            print(text)
        else:
            print("(empty: the driver never ran, or the RAM was reused)")

    # The console ring of the kernel: every byte that reached the console.
    if "sweet_log_ring" in syms and "sweet_log_total" in syms:
        _, _, totv = fetch("sweet_log_total", 8)
        total, = struct.unpack_from("<Q", totv, 0)
        _, _, ring = fetch("sweet_log_ring", RING_SIZE)
        print(f"=== the console ring ({total} bytes) ===")
        if total == 0:
            print("(empty: the BSD console was not up yet)")
        else:
            start = total - RING_SIZE if total > RING_SIZE else 0
            text = bytes(ring[start:total]).decode("ascii", "replace")
            text = "".join(ch if (32 <= ord(ch) < 127 or ch in "\t\n\r") else "." for ch in text)
            sys.stdout.write(text)
            if not text.endswith("\n"):
                sys.stdout.write("\n")
    else:
        print("(this kernel has no console ring)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
