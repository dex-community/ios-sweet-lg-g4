#!/usr/bin/env python3
"""qemu-emmc.py -- run the shim's eMMC path under QEMU against a model.

WHAT THIS IS
    There is no model of the MSM8992 SDCC1 controller: under QEMU the two
    register windows (HC = 0xf9824900, CORE = 0xf9824000) are served by a
    software model built into the shim (shim/emmc_model.inc, SHIM_EMMC_MODEL=1).
    The model logs every access and can be told to behave like a mute card,
    like a card that only answers after the PWRCTL acknowledgement, or like
    a card that only answers after N milliseconds.

WHAT THIS IS NOT
    It is NOT evidence about the phone. A green run here only says that the
    shim's logic does what it is supposed to do: it issues the register
    accesses in the right order, it times out instead of hanging, it encodes
    the result, and it does not crash or loop. Whether the real card
    answers can only be measured on the real hardware.

    The modes are tests of CODE against a hypothetical model.

Usage:
    python3 tools/qemu-emmc.py --mode mute
    python3 tools/qemu-emmc.py --mode ack
    python3 tools/qemu-emmc.py --mode delay --delay-ms 500
    python3 tools/qemu-emmc.py --mode always

Options:
    --mode mute|ack|delay|always   how the model card behaves
    --delay-ms N                   how long the "delay" mode waits
    --secs N                       how long QEMU runs (default 40)
    --out DIR                      where to put the log (default shim/out/qemu-emmc)
    --no-build                     do not rebuild the shim
    --variant NAME                 only for the report: free text
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

LOG_MAGIC = b'G4MMLOG1'
ENTRY = struct.Struct('<8I')		# seq dir win off size val tsc repeat
HDR = struct.Struct('<8s4I')		# magic n mode delay_ms checksum
# the block is: header, xfer_words, pad, then the entries
XFER_OFF = HDR.size

WIN = {0: 'CORE', 1: 'HC', 2: 'GCC'}

# The device tree of the test machine. Same shape as the one tools/qemu-g4.py
# builds, plus the two properties the model reads.
TEST_DTS = """\
/dts-v1/;
/ {
	#address-cells = <1>;
	#size-cells = <1>;
	model = "LG G4 shim eMMC model test";
	compatible = "lg,h815", "qcom,msm8992";

	chosen {
		bootargs = "console=null";
		linux,initrd-start = <0x41000000>;
		linux,initrd-end = <0x41000040>;
		emmc-model = <%d>;
		emmc-delay-ms = <%d>;
	};

	memory@40000000 {
		device_type = "memory";
		reg = <0x40000000 0x40000000>;
	};

	framebuffer@40000000 {
		compatible = "simple-framebuffer";
		reg = <0x40000000 0x200000>;
		width = <1280>;
		height = <480>;
		stride = <5120>;
		format = "r8g8b8";
	};
};
"""

MODES = {'mute': 0, 'ack': 1, 'delay': 2, 'always': 3}


def build_shim(diag=False):
    env = dict(os.environ)
    env['SHIM_HW_DIAG'] = '1'
    env['SHIM_EMMC_MODEL'] = '1'
    env['RAW_TIMING'] = '1'
    env['FINE_TIMING'] = '1'
    if diag:
        env['DIAG_VARIANTS'] = '1'
        env['DIAG_TIMING'] = '1'
    else:
        env['DIAG_VARIANTS'] = '0'
        env['DIAG_TIMING'] = '0'
    print('== building the shim with SHIM_EMMC_MODEL=1 ==')
    r = subprocess.run(['./build.sh'], cwd=os.path.join(ROOT, 'shim'), env=env,
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit('build failed:\n' + r.stdout + r.stderr)
    tail = [x for x in r.stdout.splitlines() if 'warning' in x or 'error' in x]
    for x in tail:
        print('   ' + x)
    print('   ' + [x for x in r.stdout.splitlines() if 'Image' in x][-1])


def make_dtb(mode, delay_ms, tmp):
    dts = os.path.join(tmp, 'test.dts')
    dtb = os.path.join(tmp, 'test.dtb')
    with open(dts, 'w') as f:
        f.write(TEST_DTS % (MODES[mode], delay_ms))
    r = subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb', '-o', dtb, dts],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit('dtc failed:\n' + r.stderr)
    return dtb


def run_qemu(image, dtb, outdir, secs):
    mem_file = os.path.join(outdir, 'qemu-ram.bin')
    qemu_log = os.path.join(outdir, 'qemu.log')
    cmd = [
        'qemu-system-aarch64',
        '-M', 'virt,gic-version=3,memory-backend=mem',
        '-cpu', 'cortex-a53',
        '-object', f'memory-backend-file,id=mem,size=1G,mem-path={mem_file},share=on',
        '-kernel', image,
        '-dtb', dtb,
        '-display', 'none',
        '-monitor', 'none',
        '-serial', 'none',
        '-no-reboot',
        '-d', 'guest_errors',
        '-D', qemu_log,
    ]
    print('== QEMU ==')
    print('   ' + ' '.join(cmd))
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True)
    t0 = time.time()
    while p.poll() is None and time.time() - t0 < secs:
        time.sleep(0.5)
    if p.poll() is None:
        p.terminate()
        try:
            out, _ = p.communicate(timeout=20)
        except subprocess.TimeoutExpired:
            p.kill()
            out, _ = p.communicate()
    else:
        out, _ = p.communicate()
    if out:
        print(out[-1500:])
    return mem_file, qemu_log


def find_log(mem_file):
    """Find the model log in the RAM dump and parse it."""
    with open(mem_file, 'rb') as f:
        data = f.read()
    pos = data.find(LOG_MAGIC)
    if pos < 0:
        return None
    magic, n, mode, delay_ms, checksum = HDR.unpack_from(data, pos)
    xfer_words = struct.unpack_from('<I', data, pos + XFER_OFF)[0]
    entries = []
    off = pos + XFER_OFF + 8
    for i in range(n):
        if off + ENTRY.size > len(data):
            break
        entries.append(ENTRY.unpack_from(data, off))
        off += ENTRY.size
    total = 0
    for e in entries:
        total = (total + sum(e)) & 0xffffffff
    return {
        'n': n, 'mode': mode, 'delay_ms': delay_ms,
        'checksum': checksum, 'computed': total,
        'entries': entries, 'offset': pos, 'xfer_words': xfer_words,
    }


def regname(win, off):
    known = {
        ('CORE', 0x00): 'CORE+0x00 power',
        ('CORE', 0x50): 'CORE+0x50 mci_ver',
        ('CORE', 0x78): 'CORE+0x78 hc_mode',
        ('CORE', 0xdc): 'CORE+0xdc pwrctl_status',
        ('CORE', 0xe0): 'CORE+0xe0 pwrctl_mask',
        ('CORE', 0xe4): 'CORE+0xe4 pwrctl_clear',
        ('CORE', 0xe8): 'CORE+0xe8 pwrctl_ctl',
        ('HC', 0x08): 'HC+0x08 argument',
        ('HC', 0x0c): 'HC+0x0c transfer',
        ('HC', 0x0e): 'HC+0x0e command',
        ('HC', 0x10): 'HC+0x10 response',
        ('HC', 0x20): 'HC+0x20 buf_port',
        ('HC', 0x24): 'HC+0x24 present',
        ('HC', 0x28): 'HC+0x28 ctrl1',
        ('HC', 0x29): 'HC+0x29 power',
        ('HC', 0x2c): 'HC+0x2c clk_ctrl',
        ('HC', 0x2f): 'HC+0x2f swrst',
        ('HC', 0x30): 'HC+0x30 int_status',
        ('HC', 0x34): 'HC+0x34 int_enable',
        ('HC', 0x38): 'HC+0x38 int_signal',
        ('HC', 0x3c): 'HC+0x3c ctrl2',
        ('HC', 0x40): 'HC+0x40 caps1',
        ('HC', 0x44): 'HC+0x44 caps2',
        ('HC', 0x10c): 'HC+0x10c vendor_spec',
        ('GCC', 0x4c4): 'GCC+0x4c4 sdc1_apps_cbcr',
        ('GCC', 0x4c8): 'GCC+0x4c8 sdc1_ahb_cbcr',
    }
    return known.get((WIN[win], off), '%s+0x%x' % (WIN[win], off))


def write_log(path, info, mode, delay_ms):
    with open(path, 'w') as f:
        f.write('# Access log of the eMMC software model (shim/emmc_model.inc)\n')
        f.write('# mode: %s (%d), delay: %d ms\n' % (mode, info['mode'], delay_ms))
        f.write('# entries: %d, checksum: 0x%08x (computed 0x%08x)\n' %
                (info['n'], info['checksum'], info['computed']))
        f.write('# This is a MODEL, not the MSM8992 SDCC1: it proves nothing about\n')
        f.write('# the phone. It proves that the shim issues these accesses, in this\n')
        f.write('# order, and that it times out instead of hanging.\n')
        f.write('#\n#  seq  dir  window  offset  size  value      register\n')
        for e in info['entries']:
            seq, dr, win, off, size, val, tsc, rep = e
            f.write('%5d  %-4s  %-5s  0x%03x  %-4d  0x%08x  x%-6d %s\n' %
                    (seq, 'R' if dr == 0 else 'W', WIN[win], off, size, val,
                     rep, regname(win, off)))


def summarize(info):
    out = []
    writes = [e for e in info['entries'] if e[1] == 1]
    out.append('accesses: %d (writes: %d)' % (len(info['entries']), len(writes)))
    # the interesting registers
    for win, off, label in ((1, 0x29, 'HC+0x29 bus power'),
                             (0, 0xdc, 'CORE+0xdc pwrctl status'),
                             (0, 0xe0, 'CORE+0xe0 pwrctl mask'),
                             (0, 0xe4, 'CORE+0xe4 pwrctl clear'),
                             (0, 0xe8, 'CORE+0xe8 pwrctl ctl')):
        seqs = [(e[0], 'R' if e[1] == 0 else 'W', e[5], e[7])
                for e in info['entries'] if e[2] == win and e[3] == off]
        if seqs:
            out.append('%s: %s' % (label, ', '.join(
                '%s%d=0x%x%s' % (d, s, v, '' if rp == 1 else 'x%d' % rp)
                for s, d, v, rp in seqs)))
        else:
            out.append('%s: never touched' % label)
    # the commands
    cmds = [(e[0], e[5] >> 8, e[5] & 0xff, e[7]) for e in info['entries']
            if e[2] == 1 and e[3] == 0x0e and e[1] == 1]
    shown = cmds[:14]
    out.append('commands written (%d): %s%s' %
               (len(cmds), ', '.join('CMD%d(0x%02x)x%d' % (c, fl, rp)
                                     for _, c, fl, rp in shown),
                ' ...' if len(cmds) > len(shown) else ''))
    out.append('bulk accesses to the buffer port: %d (not logged)' %
               info['xfer_words'])
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mode', default='mute', choices=sorted(MODES))
    ap.add_argument('--delay-ms', type=int, default=100)
    ap.add_argument('--secs', type=float, default=40.0)
    ap.add_argument('--out', default=os.path.join(ROOT, 'shim', 'out', 'qemu-emmc'))
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--diag', action='store_true',
                    help='build the diagnostic sweep (16 -> 6 variants)')
    args = ap.parse_args()

    outdir = os.path.abspath(args.out)
    os.makedirs(outdir, exist_ok=True)

    if not args.no_build:
        build_shim(args.diag)

    image = os.path.join(ROOT, 'shim', 'out', 'Image')
    with tempfile.TemporaryDirectory() as tmp:
        dtb = make_dtb(args.mode, args.delay_ms, tmp)
        mem_file, qemu_log = run_qemu(image, dtb, outdir, args.secs)

    info = find_log(mem_file)
    if not info:
        print('!! the model log was not found in the RAM dump: the shim did '
              'not reach the eMMC path (see %s)' % qemu_log)
        return 1

    log_path = os.path.join(outdir, 'emmc-access-%s.log' % args.mode)
    write_log(log_path, info, args.mode, args.delay_ms)
    print('\n== summary ==')
    print(summarize(info))
    print('\nlog: %s' % log_path)
    if info['checksum'] != info['computed']:
        print('!! checksum mismatch')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
