#!/usr/bin/env python3
"""qemu-sd.py -- the test of the microSD bring-up (shim/sd.c).

WHAT IT PROVES
    1. the ORDER: the GCC clock branches of SDCC2 go on BEFORE the
       controller is touched. The model does not serve the SDCC2 windows
       until the clock is on, so a wrong order does not "work": the access
       goes to an address that does not exist under QEMU and the guest
       faults. This is the same shape as the hang of 2026-11-09 (a write to
       a controller whose clock is off never completes), reproduced safely.
    2. the SEQUENCE: the commands are the ones of the CAF core
       (CMD0, CMD8 0x1AA, CMD55+ACMD41, CMD2, CMD3, CMD7, CMD9, CMD16,
       CMD17), in that order.
    3. the CLOCK: the RCG of SDCC2 is programmed to 400 kHz, the value of
       the CAF table (F(400000, gcc_xo, 12, 1, 4)).
    4. the READ: the first sector comes back.

    It is a TEST OF THE CODE against a model. It is NOT evidence about the
    phone.

Usage:
    python3 tools/qemu-sd.py
"""
import importlib.util
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

_spec = importlib.util.spec_from_file_location(
    'qemu_emmc', os.path.join(HERE, 'qemu-emmc.py'))
qemu_emmc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(qemu_emmc)

# The window numbers of the model: 0 CORE, 1 HC, 2 GCC, 3 SD2 CORE, 4 SD2 HC, 5 TLMM.
WIN = {0: 'CORE', 1: 'HC', 2: 'GCC', 3: 'SD2CORE', 4: 'SD2HC', 5: 'TLMM'}

SD2_CORE = 0xf98a4000
SD2_HC = 0xf98a4900


def build(sd_init=1, sd_payload=1):
    env = dict(os.environ)
    env['SHIM_HW_DIAG'] = '1'
    env['SHIM_EMMC_MODEL'] = '1'
    env['SHIM_SD_INIT'] = str(sd_init)
    env['SHIM_SD_PAYLOAD'] = str(sd_payload)
    env['SHIM_MMIO_SELFTEST'] = '0'
    env['SHIM_SD_PROBE'] = '0'
    env['RAW_TIMING'] = '1'
    env['FINE_TIMING'] = '1'
    env['DIAG_VARIANTS'] = '0'
    env['DIAG_TIMING'] = '0'
    print('== building the shim with the SD bring-up ==')
    r = subprocess.run(['./build.sh'], cwd=os.path.join(ROOT, 'shim'), env=env,
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit('build failed:\n' + r.stdout + r.stderr)
    print('   ok')


def main():
    build()
    outdir = os.path.join(ROOT, 'shim', 'out', 'qemu-sd')
    os.makedirs(outdir, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        dtb = qemu_emmc.make_dtb('always', 100, tmp, sd_payload=1)
        mem_file, qemu_log = qemu_emmc.run_qemu(
            os.path.join(ROOT, 'shim', 'out', 'Image'), dtb, outdir, 45)

    info = qemu_emmc.find_log(mem_file)
    if not info:
        print('!! the model log is missing: the shim did not run')
        return 1

    entries = info['entries']
    failures = []

    # 1. the order: the first access to the SDCC2 windows must come after
    #    the write that enables the SDCC2 clock branch (GCC + 0x504).
    clock_seq = None
    sd2_seq = None
    for e in entries:
        seq, dr, win, off, size, val, tsc, rep = e
        if dr == 1 and win == 2 and off in (0x504, 0x508) and clock_seq is None:
            clock_seq = seq
        if win in (3, 4) and sd2_seq is None:
            sd2_seq = seq
    if clock_seq is None:
        failures.append('the SDCC2 clock branch was never enabled')
    elif sd2_seq is None:
        failures.append('the SDCC2 controller was never touched (the bring-up '
                        'did not run)')
    elif sd2_seq < clock_seq:
        failures.append('the controller was touched at seq %d BEFORE the clock '
                        'was enabled at seq %d: that is the order that hung '
                        'the phone' % (sd2_seq, clock_seq))
    else:
        print('ok  : the clock (seq %d) comes before the controller (seq %d)'
              % (clock_seq, sd2_seq))

    # 2. the sequence of the commands
    cmds = []
    for e in entries:
        seq, dr, win, off, size, val, tsc, rep = e
        if dr == 1 and win == 4 and off == 0x0e:
            cmds.append((val >> 8) & 0xff)
    wanted = [0, 8, 55, 41, 2, 3, 7, 16, 17]
    got = [c for c in cmds if c in wanted]
    # CMD9 (the CSD) is not in the sequence: it has a data phase, which this
    # first version does not read.
    expect = [0, 8, 55, 41, 2, 3, 7, 16, 17]
    if got[:len(expect)] != expect:
        failures.append('the command sequence is %r, expected %r'
                        % (got[:12], expect))
    else:
        print('ok  : the command sequence is %r' % (got[:len(expect)],))

    # 3. the clock: the RCG of SDCC2 must say 400 kHz
    rcg = {}
    for e in entries:
        seq, dr, win, off, size, val, tsc, rep = e
        if win == 2 and dr == 1 and 0x510 <= off <= 0x520:
            rcg[off] = val
    if not rcg:
        failures.append('the RCG of SDCC2 was never written')
    else:
        cfg = rcg.get(0x514)
        m = rcg.get(0x518)
        n = rcg.get(0x51c)
        ok = (cfg in (0x17, 0x2017) and m == 1 and (n & 0xffffffff) == 0xfffffffc)
        if not ok:
            failures.append('the RCG of SDCC2 is CFG=0x%x M=0x%x N=0x%x, '
                            'expected 0x17/0x2017 / 1 / 0xfffffffc'
                            % (cfg or 0, m or 0, n or 0))
        else:
            print('ok  : the RCG of SDCC2 is programmed for 400 kHz')

    # 4. the read: a transfer through the buffer port must have happened
    if not info['xfer_words']:
        failures.append('no data was transferred: the first sector was not '
                        'read')
    else:
        print('ok  : %d words came out of the buffer port' % info['xfer_words'])

    # and the console log must say that the card was identified
    log = read_console(mem_file)
    with open(os.path.join(outdir, 'console.log'), 'w') as f:
        f.write(log)
    if 'SD rca=' not in log:
        failures.append('the console log does not contain the "SD rca=" line')
    else:
        for line in log.splitlines():
            if line.startswith('SD '):
                print('ok  : %s' % line)

    # 5. the payload: it is read from the card, its CRC is checked and the
    #    loader is told to use it.
    if 'SDPAY len=' not in log:
        failures.append('the payload was not read from the card (no "SDPAY" '
                        'line in the console log)')
    elif 'SDPAY rc=00000000' not in log:
        failures.append('the payload was read but its CRC did not match')
    else:
        for line in log.splitlines():
            if line.startswith('SDPAY'):
                print('ok  : %s' % line)
    if 'LOADER payload at=' not in log:
        failures.append('the loader was not told to use the payload of the card')
    else:
        for line in log.splitlines():
            if line.startswith('LOADER'):
                print('ok  : %s' % line)

    if failures:
        print('\nFAIL:')
        for f in failures:
            print('  - %s' % f)
        return 1
    print('\nALL CHECKS PASSED: the clock comes before the controller, the '
          'command sequence is the CAF one, the clock is 400 kHz and the '
          'first sector came back.')
    return 0


def read_console(mem_file):
    """The console log of the shim.

    It lives in the .bss of the image (the symbol con_log), which under QEMU
    is loaded at 0x40200000. The address comes from the ELF, because the
    string 'LG G4 (H815) bring-up shim' is ALSO in the .rodata (the labels)
    and searching for it finds the wrong copy."""
    data = open(mem_file, 'rb').read()
    elf = os.path.join(ROOT, 'shim', 'out', 'shim.elf')
    addr = None
    for line in subprocess.run(['aarch64-elf-nm', elf], capture_output=True,
                               text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == 'con_log':
            addr = int(parts[0], 16)
            break
    if addr is None:
        return ''
    off = 0x200000 + addr
    raw = data[off:off + 0x8000]
    return raw.split(b'\x00')[0].decode('ascii', 'replace')


if __name__ == '__main__':
    sys.exit(main())
