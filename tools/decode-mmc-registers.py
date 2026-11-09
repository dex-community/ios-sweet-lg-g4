#!/usr/bin/env python3
"""decode-mmc-registers.py -- read the dump of tools/read-mmc-registers.sh.

WHAT IT IS
    The measurements of the two SD controllers are taken from the RECOVERY
    (a real kernel with real drivers), where reading the registers is safe:
    a write to a controller whose clock is off is what hung the phone twice
    on 2026-11-09 (GUIDE-05-MMIO-SAFE.md).

    This script turns the raw dump into the three tables that matter:

      1. the TLMM mux of the ten SDC pins (is SDCC2 muxed on SDCC2?);
      2. the GCC clock of SDCC1 and SDCC2, with the REAL frequency
         (the SDHCI divider is ignored on MSM: the clock is the GCC one);
      3. the capabilities and the state of the two SDHCI hosts.

    The frequency is computed with the formula of the CAF driver itself
    (drivers/clk/qcom/clock-local2.c:1142-1162):

        pre_div = parent / ((div_val + 1) >> 1)      div_val = CFG & 0x1f
        n      = (~n_val) + m                        n_val is sign extended
        rate   = pre_div * m / n

Usage:
    python3 tools/decode-mmc-registers.py research/raw-.../mmc-registers.txt
"""
import re
import sys

# The parent of each source (the GCC "src_sel" field).
SOURCES = {
    0: ('XO', 19.2e6),
    1: ('GPLL0', 600e6),
    5: ('GPLL4', 1376e6 / 4),
}

# The ten SDC pins: the "sdc" pinctrl node of the device tree.
PINS = [
    (0, 'sdc1_clk'), (1, 'sdc1_cmd'), (2, 'sdc1_data'), (3, 'sdc1_rclk'),
    (4, 'sdc2_clk'), (5, 'sdc2_cmd'), (6, 'sdc2_data'),
    (7, 'sdc3_clk'), (8, 'sdc3_cmd'), (9, 'sdc3_data'),
]

SECTION = re.compile(r'^--\s+(\S+)\s+base=(0x[0-9a-f]+)\s+size=(0x[0-9a-f]+)')
WORD = re.compile(r'^[0-9a-f]{8}\s+((?:[0-9a-f]{8}\s+)*)')


def parse(path):
    """-> {section_name: {offset: value}}"""
    out = {}
    name = None
    with open(path) as f:
        for line in f:
            line = line.strip()
            m = SECTION.match(line)
            if m:
                name = m.group(1)
                out[name] = {}
                continue
            # "od -A x" prints the address with the minimum number of
            # digits ("0000000", not "00000000"): do not require eight.
            if name is None or not line or not re.match(r'^[0-9a-f]+\s', line):
                continue
            off = int(line.split()[0], 16)
            words = line.split()[1:]
            for i, w in enumerate(words):
                if len(w) != 8:
                    break
                out[name][off + i * 4] = int(w, 16)
    return out


def r32(sec, off):
    return sec.get(off, 0)


def tlmm(sec):
    print('\n== the TLMM mux of the ten SDC pins ==')
    print('   the mux survives a reset, so this is what the last boot left')
    print('   %-4s %-11s %-12s %-12s %s' % ('pin', 'name', 'CTL', 'MUX', 'function'))
    for pin, name in PINS:
        ctl = r32(sec, pin * 8)
        mux = r32(sec, pin * 8 + 4)
        print('   %-4d %-11s 0x%08x   0x%08x   %d' %
              (pin, name, ctl, mux, mux & 0x7))


def rcg(sec, off, label):
    cmd = r32(sec, off)
    cfg = r32(sec, off + 4)
    m = r32(sec, off + 8)
    n_raw = r32(sec, off + 0xc)
    d_raw = r32(sec, off + 0x10)
    src = (cfg >> 8) & 0x7
    div = cfg & 0x1f
    name, parent = SOURCES.get(src, ('?%d' % src, 0))
    n = ((~n_raw) & 0xffffffff) + (m & 0xffffffff)
    pre = parent / ((div + 1) >> 1) if div else parent
    rate = pre * m / n if n else 0
    print('   %s: CMD=0x%08x CFG=0x%08x M=0x%08x N=0x%08x D=0x%08x' %
          (label, cmd, cfg, m, n_raw, d_raw))
    print('       source=%s (%d) div_val=%d m=%d n=%d -> %.6g Hz' %
          (name, src, div, m, n, rate))
    return rate


def clocks(sec):
    print('\n== the GCC clock of the two slots ==')
    print('   (the SDHCI divider is ignored on MSM: this IS the card clock)')
    r1 = rcg(sec, 0x4d0, 'SDCC1')
    r2 = rcg(sec, 0x510, 'SDCC2')
    if r1 and abs(r1 - 400000) < 1000:
        print('   -> SDCC1 is at ~400 kHz: the identification clock')
    elif r1:
        print('   -> SDCC1 is at %.6g Hz, NOT 400 kHz' % r1)
    return r1, r2


def host(sec, label):
    print('\n== %s ==' % label)
    caps1 = r32(sec, 0x40)
    caps2 = r32(sec, 0x44)
    present = r32(sec, 0x24)
    ctrl1 = r32(sec, 0x28)
    clk = r32(sec, 0x2c)
    power = r32(sec, 0x29)
    vendor = r32(sec, 0x10c)
    # The voltage capabilities are NOT in CAPS1: the CAF driver reads them
    # from the vendor spec capabilities register (sdhci-msm.c:3232-3240,
    # CORE_3_3V_SUPPORT bit 24, CORE_3_0V_SUPPORT bit 25, CORE_1_8V bit 26).
    vcaps = r32(sec, 0x11c)
    print('   CAPS1=0x%08x CAPS2=0x%08x' % (caps1, caps2))
    base_mhz = (caps1 >> 8) & 0xff
    print('   base clock: %d MHz (CAPS1[15:8])' % base_mhz)
    print('   voltage capabilities (HC+0x11c=0x%08x): 3.3V=%s 3.0V=%s 1.8V=%s' %
          (vcaps,
           'yes' if vcaps & (1 << 24) else 'no',
           'yes' if vcaps & (1 << 25) else 'no',
           'yes' if vcaps & (1 << 26) else 'no'))
    print('   PRESENT=0x%08x  CTRL1=0x%08x  CLK_CTRL=0x%08x  POWER=0x%02x' %
          (present, ctrl1, clk, power))
    print('   vendor spec 0x10c=0x%08x  (PWRSAVE=%d MCLK_SEL=%d PAD_SWITCH=%d)' %
          (vendor,
           1 if vendor & (1 << 1) else 0,
           (vendor >> 8) & 3,
           1 if vendor & (1 << 16) else 0))
    if caps1 == 0 or caps1 == 0xffffffff:
        print('   -> the window reads %s: the controller is NOT readable'
              % ('all ones' if caps1 == 0xffffffff else 'zeros'))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    sec = parse(path)
    print('# the dump: %s' % path)
    print('# the sections found: %s' % ', '.join(sorted(sec)))
    if 'TLMM' in sec:
        tlmm(sec['TLMM'])
    if 'GCC' in sec:
        clocks(sec['GCC'])
    for name, label in (('SDCC1_HC', 'the SDCC1 host (the eMMC)'),
                        ('SDCC2_HC', 'the SDCC2 host (the microSD slot)')):
        if name in sec:
            host(sec[name], label)
    return 0


if __name__ == '__main__':
    sys.exit(main())
