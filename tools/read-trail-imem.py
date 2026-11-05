#!/usr/bin/env python3
"""Read the IMEM breadcrumb ring written by shim/trail.h and decode it.

  tools/read-trail-imem.py                 # from the phone (recovery, adb root)
  tools/read-trail-imem.py --file dump.bin # decode a saved 4 KiB IMEM page
  options: --off 0x700 --size 0x200 (must match the shim build)
"""
import argparse, struct, subprocess, sys

IMEM = 0xfe80f000
MAGIC = 0x47345452

def read_phone():
    skip = IMEM // 4096
    cmd = ["adb", "exec-out", f"dd if=/dev/mem bs=4096 skip={skip} count=1 2>/dev/null"]
    data = subprocess.run(cmd, capture_output=True).stdout
    if len(data) != 4096:
        sys.exit(f"read {len(data)} bytes, expected 4096: no /dev/mem or no root? "
                 "(try: adb root; adb shell ls -l /dev/mem)")
    return data

R1_STATES = ["idle","ready","ident","stby","tran","data","rcv","prg","dis","btst","slp"]
def r1(v):
    st = (v >> 9) & 0xf
    n = R1_STATES[st] if st < len(R1_STATES) else str(st)
    fatal = v & 0xe4180000
    return f"R1=0x{v:08x} state={n}" + (f" FATAL=0x{fatal:08x}" if fatal else "")

def intbits(v):
    names = {0:"cmd-done",1:"xfer-done",15:"ERR",16:"cmd-timeout",17:"cmd-crc",18:"cmd-endbit",
             19:"cmd-index",20:"data-timeout",21:"data-crc",22:"data-endbit",23:"cur-limit",
             24:"autocmd12",25:"adma"}
    return ",".join(n for b, n in names.items() if v >> b & 1) or "-"

def describe(code, val):
    hi, lo = code & 0xff00, code & 0xff
    fixed = {
        0x0001: lambda: f"emmc step {val}",
        0x0010: lambda: f"CORE+0x78 (HC mode) = 0x{val:08x}  HC_MODE_EN={val & 1}",
        0x0011: lambda: f"present state = 0x{val:08x}",
        0x0012: lambda: f"clk/timeout/swrst = 0x{val:08x}",
        0x0013: lambda: f"host ctrl2 = 0x{val:08x}",
        0x0014: lambda: f"CORE+0x10c = 0x{val:08x}",
        0x0015: lambda: f"HC+0x10c = 0x{val:08x}",
        0x0016: lambda: f"CORE+0x78 after HC-mode check = 0x{val:08x}",
        0x0020: lambda: f"EXT_CSD SEC_COUNT = {val}",
        0x00f0: lambda: f"emmc_prepare rc = {struct.unpack('<i', struct.pack('<I', val))[0]}",
        0x00f1: lambda: f"write spare1 rc = {struct.unpack('<i', struct.pack('<I', val))[0]}",
        0x00f2: lambda: f"write spare2 rc = {struct.unpack('<i', struct.pack('<I', val))[0]}",
        0x00f3: lambda: f"readback rc = {struct.unpack('<i', struct.pack('<I', val))[0]}",
        0x00f4: lambda: f"readback identical = {val}",
        0x00f8: lambda: "emmc_write_log() enter",
        0x1000: lambda: f"PHASE {val}",
        0x1100: lambda: f"first emmc error = {struct.unpack('<i', struct.pack('<I', val))[0]}",
        0x1101: lambda: f"first emmc stage = {val}",
        0x1102: lambda: f"first raw cmd error = {struct.unpack('<i', struct.pack('<I', val))[0]}",
        0x1103: lambda: f"first emmc step = {val}",
        0x2000: lambda: f"*** EXCEPTION slot {val}",
        0x2001: lambda: f"*** ELR = 0x{val:08x}",
        0x2002: lambda: f"*** ESR = 0x{val:08x} (EC=0x{val >> 26:02x})",
        0x2003: lambda: f"*** FAR = 0x{val:08x}",
        0x3000: lambda: f"reset() entered, after_s={val}",
    }
    if code in fixed:
        return fixed[code]()
    if hi == 0x0100: return f"CMD{lo} ok   resp0=0x{val:08x}"
    if hi == 0x0200:
        rc = struct.unpack('<i', struct.pack('<I', val))[0]
        why = {-1: "lines never idle", -2: "timeout (no response)", -3: "controller error", -4: "R1 fatal"}.get(rc, "?")
        return f"CMD{lo} FAIL rc={rc} ({why})"
    if hi == 0x0300: return f"CMD{lo} int status 0x{val:08x} [{intbits(val)}]"
    if hi == 0x0400: return f"CMD{lo} xfer ok  {r1(val)}"
    if hi == 0x0500:
        rc = struct.unpack('<i', struct.pack('<I', val))[0]
        return f"CMD{lo} xfer FAIL rc={rc}"
    if hi == 0x0500 and lo >= 0x10: pass
    if code & 0xfff0 == 0x0510: return f"CMD{code & 0xf} xfer failed at micro-step {val}"
    return f"code 0x{code:04x} val 0x{val:08x}"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file")
    ap.add_argument("--off", type=lambda x: int(x, 0), default=0x700)
    ap.add_argument("--size", type=lambda x: int(x, 0), default=0x200)
    a = ap.parse_args()
    page = open(a.file, "rb").read() if a.file else read_phone()
    reg = page[a.off:a.off + a.size]
    magic, count, n, _ = struct.unpack_from("<4I", reg, 0)
    if magic != MAGIC:
        sys.exit(f"no trail (magic 0x{magic:08x}). Either the shim did not run, "
                 "the IMEM area was not free (trail_init refuses to write), "
                 "or IMEM does not survive the reset. Run tools/imem-probe.sh.")
    print(f"trail: {count} events, ring of {n}" + ("  (OLDEST EVENTS LOST)" if count > n else ""))
    first = max(0, count - n)
    prev = None
    for i in range(first, count):
        slot = i % n
        tag, val = struct.unpack_from("<II", reg, 16 + slot * 8)
        ms, code = tag >> 16, tag & 0xffff
        d = "" if prev is None else f"+{(ms - prev) & 0xffff:5d}ms"
        prev = ms
        print(f"{i:4d} {ms:6d}ms {d:>9}  {describe(code, val)}")

main()
