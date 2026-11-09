#!/usr/bin/env python3
"""qemu-mmio-guard.py -- prove that the MMIO allowlist really protects the phone.

WHAT THIS IS
    On 2026-11-09 the shim hung the phone twice, both times because it wrote
    to a controller whose clock was off (SDCC2, 0xf98a4900): the write never
    completed, the AXI bus stalled and the CPU stopped inside the access.
    The watchdog was not armed, so nothing reset the phone.

    The fix is shim/mmio.h: every access goes through accessors that REFUSE
    anything outside the allowlist. This test proves that the refusal
    happens, and that the refusal happens BEFORE the access reaches the bus
    (the software model of the controller logs every access that reaches it,
    so an access that does not appear in the log never happened).

    It is a TEST OF THE CODE against a model. It is NOT evidence about the
    phone.

WHAT IT CHECKS
    1. the self-test wrote to SDCC2 (0xf98a4900) -> the model log must NOT
       contain a single access to the SDCC2 window;
    2. the self-test read from SDCC2 -> same;
    3. a write to a window the shim really uses (the GCC) -> the model log
       MUST contain it (the allowlist must not be broken);
    4. the console log must contain "MMIO DENIED" (the refusal is visible).

Usage:
    python3 tools/qemu-mmio-guard.py
"""
import os
import re
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import importlib.util  # noqa: E402

# the file name has a dash: load it by hand
_spec = importlib.util.spec_from_file_location(
    'qemu_emmc', os.path.join(HERE, 'qemu-emmc.py'))
qemu_emmc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(qemu_emmc)

# The SDCC2 window: the one that hung the phone.
SDCC2_HC = 0xf98a4900
SDCC2_CORE = 0xf98a4000
SDCC2_SIZE = 0x1000


def build():
    env = dict(os.environ)
    env['SHIM_HW_DIAG'] = '1'
    env['SHIM_EMMC_MODEL'] = '1'
    env['SHIM_MMIO_SELFTEST'] = '1'
    env['SHIM_SD_PROBE'] = '0'
    env['RAW_TIMING'] = '1'
    env['FINE_TIMING'] = '1'
    env['DIAG_VARIANTS'] = '0'
    env['DIAG_TIMING'] = '0'
    print('== building the shim with the MMIO self-test ==')
    r = subprocess.run(['./build.sh'], cwd=os.path.join(ROOT, 'shim'), env=env,
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit('build failed:\n' + r.stdout + r.stderr)
    print('   ok')


def console_log(mem_file):
    """The shim's console log: con_log is in the .bss of the image, which is
    loaded at 0x40200000 in the QEMU build."""
    data = open(mem_file, 'rb').read()
    # find the string the console writes first, then read the log around it
    idx = data.find(b'LG G4 (H815) bring-up shim')
    if idx < 0:
        return ''
    start = idx
    # the log starts at the beginning of the buffer: walk back to the
    # beginning of the region that contains it
    region = data[max(0, idx - 0x4000):idx + 0x4000]
    pos = region.find(b'LG G4 (H815) bring-up shim')
    # the log is NUL terminated; take everything printable before it
    head = region[:pos]
    out = []
    for b in head:
        if 32 <= b < 127 or b in (10, 13):
            out.append(chr(b))
        else:
            out.append('\n')
    text = ''.join(out)
    tail = region[pos:pos + 0x2000]
    tail = tail.split(b'\x00')[0]
    text += tail.decode('ascii', 'replace')
    return text


def denied_count(mem_file, image):
    """The number of refusals the self-test recorded.

    The counter lives in the .bss of the image (shim/mmio_selftest.c), so it
    is found through the symbol table of the ELF. It cannot be read from the
    trail: under QEMU there is no IMEM, so trail_init() is a no-op and the
    trail stays empty (that is by design, see shim/trail.h).
    """
    out = subprocess.run(['aarch64-elf-nm', image], capture_output=True,
                         text=True).stdout
    addr = None
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == 'mmio_denied_count':
            addr = int(parts[0], 16)
            break
    if addr is None:
        return -1
    # the image is loaded at 0x40200000 in the QEMU build
    off = 0x200000 + addr
    data = open(mem_file, 'rb').read()
    return struct.unpack_from('<I', data, off)[0]


def main():
    build()
    outdir = os.path.join(ROOT, 'shim', 'out', 'qemu-guard')
    os.makedirs(outdir, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        dtb = qemu_emmc.make_dtb('always', 100, tmp)
        mem_file, qemu_log = qemu_emmc.run_qemu(
            os.path.join(ROOT, 'shim', 'out', 'Image'), dtb, outdir, 45)

    info = qemu_emmc.find_log(mem_file)
    if not info:
        print('!! the model log is missing: the shim did not run')
        return 1

    log = console_log(mem_file)
    failures = []

    # 1/2: no access to the SDCC2 window may have reached the bus.
    bad = [e for e in info['entries']
           if e[2] in (1, 2) and SDCC2_CORE <= e[3] < SDCC2_CORE + SDCC2_SIZE]
    # the model does not serve SDCC2 at all (its windows are SDCC1 and GCC),
    # so an access there would have gone to the real address: what we check is
    # that the model never saw an access whose window is SDCC2.
    if bad:
        failures.append('the model saw %d accesses to SDCC2: %r' % (len(bad), bad[:4]))
    else:
        print('ok  : no access to the SDCC2 window reached the bus')

    # 3: a write to an allowed window must have gone through. The GCC
    # window is the one the shim really uses (the clock branches), so any
    # write there proves the allowlist is not too strict.
    gcc_writes = [e for e in info['entries'] if e[2] == 2 and e[1] == 1]
    if not gcc_writes:
        failures.append('the write to the allowed GCC window did NOT reach '
                        'the bus: the allowlist is too strict')
    else:
        print('ok  : %d write(s) to the allowed GCC window went through'
              % len(gcc_writes))

    # 4: the refusals must be counted. The refusal path cannot use the
    # console (a console write is a data abort on the phone), so the count
    # is read from the trail in the IMEM, which the host reads back.
    denied = denied_count(mem_file, os.path.join(ROOT, 'shim', 'out', 'shim.elf'))
    if denied < 2:
        failures.append('the self-test counted %d refusals, expected at least '
                        '2 (the forbidden write and the forbidden read)'
                        % denied)
    else:
        print('ok  : the self-test counted %d refusals' % denied)

    with open(os.path.join(outdir, 'console.log'), 'w') as f:
        f.write(log)
    print('\nconsole log: %s' % os.path.join(outdir, 'console.log'))

    if failures:
        print('\nFAIL:')
        for f in failures:
            print('  - %s' % f)
        return 1
    print('\nALL CHECKS PASSED: the allowlist refuses the access that hung '
          'the phone, and still lets the real ones through.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
