#!/usr/bin/env python3
"""Read the early console of XNU out of the phone's RAM.

XNU keeps its early diagnostics (patch 0010, osfmk/arm64/sweet_early.c) in a
text buffer in the BSS: the marks "SWEET EARLY <n>: <label>" of the boot
stages and, when the kernel panics, the text of the panic. The buffer is
written from C, so it is mapped as soon as the MMU is on, and it cannot
fault.

It is read back from the RAM the way the log of the eMMC driver is: the
physical address of a symbol comes from the header of the payload,

    pa = va - virt_base + phys_base

which is the mapping phystokv() uses (osfmk/arm64/arm_vm_init.c). The RAM
survives the reset, so the text of a kernel that panicked is still there.

    tools/read-xnu-early.py KERNEL [PAYLOAD] [OUT.bin]

PAYLOAD is the shim/out/*.bin that was booted (its header carries virt_base
and phys_base). OUT.bin is a raw dump of the region; without it the phone is
read over adb with a kernel that has /dev/mem (the custom TWRP).
"""
import struct
import subprocess
import sys

PAGE = 4096
DEFAULT_VIRT_BASE = 0xfffffe0006000000
DEFAULT_PHYS_BASE = 0x80000000


def macho_symbols(path):
    with open(path, "rb") as f:
        buf = f.read()
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
        n_strx, n_type, n_sect, _, n_value = struct.unpack_from("<IBBHQ", buf, symoff + i * 16)
        if n_value == 0:
            continue
        end = buf.index(b"\0", stroff + n_strx)
        name = buf[stroff + n_strx:end].decode("ascii", "replace")
        out[name.lstrip("_")] = n_value
    return out


def payload_header(path):
    with open(path, "rb") as f:
        head = f.read(0x48)
    if head[:8] != b"SWEETXNU":
        sys.exit(f"{path}: not a SWEETXNU payload")
    virt_base, = struct.unpack_from("<Q", head, 0x10)
    phys_base, = struct.unpack_from("<Q", head, 0x38)
    return virt_base, phys_base


def read_phone(pa, size):
    """Read `size` bytes at the physical address `pa`.

    A symbol is not page aligned in general, so the page that contains it is
    read and the wanted bytes are cut out. With a plain "skip=pa//PAGE" the
    bytes of the page start would be returned instead, and a length read at
    the wrong offset can be garbage: the caller would then ask for
    gigabytes, the read would run into the holes of the memory map and the
    phone would hang on the AXI bus.
    """
    base = pa & ~(PAGE - 1)
    offset = pa - base
    count = (offset + size + PAGE - 1) // PAGE
    cmd = ["adb", "exec-out",
           f"dd if=/dev/mem bs={PAGE} skip={base // PAGE} count={count} 2>/dev/null"]
    data = subprocess.run(cmd, capture_output=True).stdout
    if len(data) < offset + size:
        sys.exit(f"read {len(data)} bytes at PA {pa:#x}: no /dev/mem, or the "
                 "kernel is without CONFIG_DEVMEM?")
    return data[offset:offset + size]


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    syms = macho_symbols(argv[1])

    virt_base, phys_base = DEFAULT_VIRT_BASE, DEFAULT_PHYS_BASE
    payload = dump = None
    for arg in argv[2:]:
        if not arg.endswith(".bin"):
            payload = arg
        elif payload is None:
            payload = arg
        else:
            dump = arg
    if payload:
        virt_base, phys_base = payload_header(payload)

    to_pa = lambda va: va - virt_base + phys_base

    if "sweet_early_buf" not in syms:
        sys.exit("this kernel has no early console buffer")
    buf_va = syms["sweet_early_buf"]
    len_va = syms.get("sweet_early_len")

    def fetch(va, size):
        pa = to_pa(va)
        if dump is None:
            return read_phone(pa, size)
        with open(dump, "rb") as f:
            blob = f.read()
        return blob[pa - phys_base:pa - phys_base + size]

    length = 8192
    if len_va is not None:
        length, = struct.unpack_from("<I", fetch(len_va, 4), 0)
        # The length comes from the RAM: if it is garbage, reading it would
        # run into the holes of the memory map and hang the phone. The
        # buffer is 8 KiB, so anything bigger is wrong.
        if length > 1 << 20:
            sys.exit(f"the early console length {length} is not believable: "
                     "the RAM was reused, or the kernel never ran")
    text = fetch(buf_va, max(length, 1))[:length].decode("ascii", "replace")

    print(f"sweet_early_buf  va 0x{buf_va:x}  pa 0x{to_pa(buf_va):x}  {length} byte")
    if not text.strip():
        sys.exit("(the buffer is empty: the kernel wrote no early mark, or it "
                 "never reached arm_init)")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
