#!/usr/bin/env python3
"""Read the shim's log out of the phone's RAM.

The shim leaves its log at 0x50000000 before it resets the phone. That is
plain DRAM, so a kernel with CONFIG_DEVMEM can read it, and it does not need
the pstore (which every Linux kernel overwrites as soon as it boots).

    tools/read-shim-log.py                 # from the phone, over adb
    tools/read-shim-log.py dump.bin        # from a dump

The phone has to be running a kernel with CONFIG_DEVMEM; the recovery images
do not have it. See .github/workflows/kernel-h815.yml for that kernel and
tools/boot-test-kernel.sh for the whole cycle:

    fastboot boot shim/out/shim-boot.img     # the shim runs and writes the log
    fastboot boot twrp-h815-hfs.img          # the kernel with /dev/mem starts
    tools/read-shim-log.py                   # and the log is read
"""
import subprocess
import sys

SCRATCH = 0x50000000
PAGE = 4096
PAGES = 96
MARKER = "G4SHIMLG"


def read_phone():
    skip = SCRATCH // PAGE
    cmd = ["adb", "exec-out",
           f"dd if=/dev/mem bs={PAGE} skip={skip} count={PAGES} 2>/dev/null"]
    out = subprocess.run(cmd, capture_output=True).stdout
    if len(out) < PAGE:
        sys.exit(f"read {len(out)} bytes: no /dev/mem, or the kernel is "
                 "without CONFIG_DEVMEM?")
    return out


def main(argv):
    if len(argv) > 1:
        data = open(argv[1], "rb").read()
    else:
        data = read_phone()

    # The log is ASCII at the head of the area, the rest is whatever the
    # kernel left there.
    end = 0
    while end < len(data) and (32 <= data[end] < 127 or data[end] in (9, 10, 13)):
        end += 1
    text = data[:end].decode("ascii", "replace")
    if not text.strip():
        sys.exit("no log in the dump (the area is empty)")
    # Check for the G4SHIMLG marker to verify log integrity
    if not text.startswith(MARKER):
        sys.exit(f"no {MARKER} marker: the RAM has been overwritten, rerun the shim")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
