#!/usr/bin/env python3
"""Decode the two sectors the shim writes on the card.

  tools/read-shim-diag.py sp1a.bin sp1b.bin

sp1a = the log sector  : "G4SHIMLG" | stage u32 | error s32 | detail u32 |
                          rca u16 | text (488 bytes)
sp1b = the diag sector : "G4SHIMDG" | 26 u32 words (the D_* list of emmc.c)

The sectors live on the two empty spares of the phone's GPT:
  p21 (spare1) LBA 259328 and 259329
  p36 (spare2) LBA 385280 and 385281
Read them from the recovery with:
  adb shell "dd if=/dev/block/mmcblk0p21 of=/sdcard/a bs=512 skip=256 count=1"
  adb pull /sdcard/a a.bin
"""
import sys

# The D_* list of shim/emmc.c, in order.
DIAG = [
    "D_FORMAT", "D_RUNG", "D_CORE_POWER", "D_CORE_MCIVER", "D_CORE_HCMODE",
    "D_CORE_100", "D_CORE_10C", "D_HC_100", "D_HC_10C", "D_PRESENT",
    "D_CTRL1", "D_CLKCTL", "D_INTSTAT", "D_INTEN", "D_INTSIG", "D_CTRL2",
    "D_CAPS1", "D_CAPS2", "D_HCMODE_AFTER", "D_DAT0", "D_CMD16_R1", "D_RCA",
    "D_SEC_COUNT", "D_EXT_MODE", "D_ADDR_MODE", "D_FAIL_INT", "D_CMD_ERR",
]

STAGES = {
    0: "start", 1: "probe", 2: "HC mode", 3: "CMD16", 4: "wake", 5: "CMD16 (2)",
    6: "EXT_CSD", 7: "write", 8: "read back", 9: "done", 10: "re-init",
}


def u32(b, o):
    return int.from_bytes(b[o:o + 4], "little")


def s32(b, o):
    return int.from_bytes(b[o:o + 4], "little", signed=True)


def show_log(path):
    d = open(path, "rb").read()
    if d[:8] != b"G4SHIMLG":
        print(f"{path}: not a shim log sector (marker {d[:8]!r})")
        return
    stage = u32(d, 8)
    error = s32(d, 12)
    detail = u32(d, 16)
    rca = int.from_bytes(d[20:22], "little")
    print(f"== {path}: log sector")
    print(f"   stage  : {stage} ({STAGES.get(stage, '?')})")
    print(f"   error  : {error}")
    print(f"   detail : 0x{detail:08x} (R1 of CMD16)")
    print(f"   rca    : {rca}")
    print("   text:")
    for line in d[24:].split(b"\x00")[0].decode("utf-8", "replace").splitlines():
        print(f"     | {line}")


def show_diag(path):
    d = open(path, "rb").read()
    if d[:8] != b"G4SHIMDG":
        print(f"{path}: not a shim diag sector (marker {d[:8]!r})")
        return
    print(f"== {path}: diag sector")
    for i, name in enumerate(DIAG):
        v = u32(d, 8 + i * 4)
        if v == 0 and name in ("D_RUNG", "D_CORE_100", "D_CORE_10C",
                               "D_HC_100", "D_HC_10C", "D_INTSTAT",
                               "D_INTSIG", "D_SEC_COUNT", "D_EXT_MODE"):
            continue
        print(f"   {name:16s} = 0x{v:08x} ({v})")


if __name__ == "__main__":
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        raise SystemExit(1)
    for p in args:
        d = open(p, "rb").read(8)
        if d == b"G4SHIMLG":
            show_log(p)
        elif d == b"G4SHIMDG":
            show_diag(p)
        else:
            print(f"{p}: unknown marker {d!r}")
