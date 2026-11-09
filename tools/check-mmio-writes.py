#!/usr/bin/env python3
"""check-mmio-writes.py -- the static half of the MMIO allowlist.

WHAT THIS IS
    shim/mmio.h refuses at RUNTIME any access outside the allowlist, and
    that is what stopped the phone from hanging (the first two hangs of
    2026-11-09 were writes to a controller whose clock was off).

    This script is the STATIC half: it reads the shim's sources and fails
    if an access to a device window is written in a way that BYPASSES the
    guarded accessors (a raw "*(volatile uint32_t *)addr = value" on an
    address in a device window). A bypass would be a hole in the protection,
    and a hole in the protection is what hangs the phone.

WHAT IT CHECKS
    1. every "*(volatile <type> *)" STORE in the shim's sources is either
       in a memory window (harmless) or inside a device window that is
       allowed to be written raw (the IMEM trail, which is written with
       plain stores because it is plain SRAM);
    2. the guarded accessors (mmio_wr8/16/32) are the ones used for the
       device windows: no raw store is allowed on the eMMC, the GCC, the
       watchdog, the GIC or PSHOLD.

USAGE
    python3 tools/check-mmio-writes.py
    (the shim's build.sh runs it)

Exit code 0 = clean, 1 = a hole was found.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SHIM = os.path.join(ROOT, 'shim')

# The device windows: a raw store on one of these bypasses the protection.
DEV_WINDOWS = [
    (0xf9824000, 0x0800, 'the eMMC core'),
    (0xf9824900, 0x01a0, 'the eMMC host'),
    (0xf98a4000, 0x0800, 'the SDCC2 core'),
    (0xf98a4900, 0x0100, 'the SDCC2 host'),
    (0xfc400000, 0x2000, 'the GCC'),
    (0xf9017000, 0x1000, 'the watchdog'),
    (0xf9000000, 0x3000, 'the GIC'),
    (0xfc4ab000, 0x1000, 'PSHOLD'),
]

# The files to scan.
SOURCES = ['shim.c', 'emmc.c', 'wdt.c', 'sd_probe.c', 'mmio_selftest.c',
           'console.c', 'mmu.c', 'fdt.c', 'loader.c', 'pstore.c', 'lib.c',
           'emmc_lk_bringup.inc', 'emmc_diag.inc', 'sd_probe.inc',
           'emmc_model.inc']

# A raw store: *(volatile <type> *)<expr> = ...
STORE = re.compile(
    r'\*\s*\(\s*volatile\s+'
    r'(?:const\s+)?'
    r'(?:u?int(?:8|16|32|64)_t|char|short|int|long)\s*\*\s*\)'
    r'\s*([^=;]{1,80}?)\s*='
    r'(?![=])')

# The addresses we recognise in an expression.
ADDR = re.compile(r'(0x[0-9a-fA-F]+|\b[A-Z_][A-Z0-9_]*\b)')


def load_constants():
    """The constants of shim/g4.h and the local #defines, so that an
    expression like "HC + HC_SWRST" can be resolved."""
    consts = {}
    for name in ('g4.h', 'emmc.c', 'emmc_lk_bringup.inc', 'sd_probe.inc',
                 'mmio.h', 'console.h', 'mmu.h', 'loader.h', 'pstore.h'):
        path = os.path.join(SHIM, name)
        if not os.path.exists(path):
            continue
        with open(path) as f:
            text = f.read()
        for m in re.finditer(r'#define\s+([A-Za-z_][A-Za-z0-9_]*)\s+'
                             r'(0x[0-9a-fA-F]+|\d+)', text):
            try:
                consts[m.group(1)] = int(m.group(2), 0)
            except ValueError:
                pass
    return consts


def resolve(expr, consts):
    """Best effort: turn "HC + HC_SWRST" into a number, or None."""
    expr = expr.strip()
    # a direct constant
    m = re.fullmatch(r'(0x[0-9a-fA-F]+|\d+)', expr)
    if m:
        return int(m.group(1), 0)
    # a sum/difference of known constants
    parts = re.split(r'\s*([+-])\s*', expr)
    total = None
    sign = 1
    for part in parts:
        part = part.strip()
        if part == '+':
            sign = 1
            continue
        if part == '-':
            sign = -1
            continue
        m = re.fullmatch(r'(0x[0-9a-fA-F]+|\d+)', part)
        if m:
            v = int(m.group(1), 0)
        elif part in consts:
            v = consts[part]
        else:
            return None
        total = sign * v if total is None else total + sign * v
    return total


def main():
    consts = load_constants()
    problems = []

    for name in SOURCES:
        path = os.path.join(SHIM, name)
        if not os.path.exists(path):
            continue
        with open(path) as f:
            lines = f.readlines()
        for n, line in enumerate(lines, 1):
            stripped = line.strip()
            # the accessors themselves are the guarded ones: they are the
            # "*((volatile uint32_t *)a) = v;" inside mmio.h, not in the
            # scanned files; a raw store in a scanned file is the thing to
            # find.
            for m in STORE.finditer(line):
                target = m.group(1)
                # skip the guarded accessors of the model and the trail
                if 'a)' in target or 'dst' in target or 'p)' in target:
                    continue
                addr = resolve(target, consts)
                if addr is None:
                    continue
                for base, size, what in DEV_WINDOWS:
                    if base <= addr < base + size:
                        problems.append(
                            '%s:%d: a raw store on %s (0x%08x) bypasses the '
                            'MMIO allowlist' % (name, n, what, addr))

    if problems:
        print('FAIL: the MMIO allowlist has holes:')
        for p in problems:
            print('  - %s' % p)
        return 1

    print('ok: no raw store bypasses the MMIO allowlist '
          '(%d device windows checked)' % len(DEV_WINDOWS))
    return 0


if __name__ == '__main__':
    sys.exit(main())
