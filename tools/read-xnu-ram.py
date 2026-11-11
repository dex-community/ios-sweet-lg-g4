#!/usr/bin/env python3
"""Read XNU's leftovers from the phone's RAM, after a crash.

The phone reboots into recovery by itself (watchdog), and recovery reuses the
RAM, so the kernel's own buffers are usually gone. Two places survive:

  - the panic region: XNU writes the panic log into the ADT's "pram" node
    (G4: PA 0xdff00000, 1 MiB, magic "DEED");
  - the fbcon framebuffer: the console text XNU draws at the fbcon PA
    (G4: PA 0x20000000), which is real DRAM on the phone.

    tools/read-xnu-ram.py PANIC PA FB PA [OUTDIR]
"""
import os
import struct
import subprocess
import sys

PAGE = 4096


def read_pa(pa, size):
    """Read `size` bytes at physical address `pa` from the phone's /dev/mem."""
    cmd = ["adb", "exec-out",
           f"dd if=/dev/mem bs={PAGE} skip={pa // PAGE} count={size // PAGE} 2>/dev/null"]
    out = subprocess.run(cmd, capture_output=True).stdout
    if len(out) < size:
        sys.exit(f"read {len(out)} of {size} bytes at PA {pa:#x}: no /dev/mem?")
    return out


def panic_text(blob):
    """Decode an embedded_panic_header (XNU 13.5) and its panic text."""
    if len(blob) < 0x40:
        return None
    magic, crc, version, flags, log_off, log_len = struct.unpack_from("<6I", blob, 0)
    if magic != 0x44454544:		# "DEED"
        return None
    text = blob[log_off:log_off + log_len]
    return {
        "magic": hex(magic), "version": version, "flags": hex(flags),
        "log_offset": log_off, "log_len": log_len,
        "text": text.split(b"\0")[0].decode("ascii", "replace"),
    }


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    panic_pa = int(argv[1], 0)
    fb_pa = int(argv[2], 0)
    outdir = argv[3] if len(argv) > 3 else "/tmp"
    os.makedirs(outdir, exist_ok=True)

    p = read_pa(panic_pa, 1 << 20)
    open(f"{outdir}/panic-region.bin", "wb").write(p)
    info = panic_text(p)
    if info:
        print("== panic region: VALID header ==")
        for k, v in info.items():
            if k != "text":
                print(f"  {k}: {v}")
        print("--- panic text ---")
        print(info["text"])
    else:
        print(f"panic region {panic_pa:#x}: no valid header "
              f"(first bytes {p[:16].hex()})")

    fb = read_pa(fb_pa, 0xc00000)
    open(f"{outdir}/fbcon.bin", "wb").write(fb)
    nz = sum(1 for i in range(0, len(fb), 4) if fb[i:i + 4] != b"\0\0\0\0")
    print(f"fbcon {fb_pa:#x}: {nz} non-zero pixels (of {len(fb) // 4}) -> {outdir}/fbcon.bin")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
