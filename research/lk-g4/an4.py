#!/usr/bin/env python3
"""an4.py -- disassembler con propagazione dei valori per l'LK di fabbrica.

Legge direttamente l'ELF (niente dipendenze da /tmp) e produce un listing
annotato:

  * coppie movw/movt ricostruite
  * letture MMIO risolte sull'indirizzo HC (0xf9824900) e CORE (0xf9824000)
  * scritture MMIO con indirizzo e valore

Mappa delle struct (ricavata dal codice, non dai nomi delle funzioni):

    dev/host +0x00  -> HC   = 0xf9824900   (finestra "hc_mem")
    dev/host +0x30  -> plat
    plat     +0x00  -> CORE = 0xf9824000   (finestra "core_mem")

Uso:
    python3 an4.py 0x0f913954 0x80        # start, numero di istruzioni
"""
import struct
import sys

from capstone import (CS_ARCH_ARM, CS_MODE_ARM, Cs)

ELF = 'aboot-lk-g4-h815.elf'
HC = 0xf9824900
CORE = 0xf9824000
GCC = 0xfc400000

MEM = {
    'dev': {0x00: HC, 0x30: 'plat'},
    'plat': {0x00: CORE},
}


def load(path):
    d = open(path, 'rb').read()
    e_phoff = struct.unpack_from('<I', d, 0x1C)[0]
    e_phentsize = struct.unpack_from('<H', d, 0x2A)[0]
    e_phnum = struct.unpack_from('<H', d, 0x2C)[0]
    segs = []
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        p_type, p_off, p_vaddr, _pp, p_filesz, p_memsz, _fl, _al = \
            struct.unpack_from('<8I', d, o)
        if p_type == 1:
            segs.append((p_vaddr, p_off, p_filesz))
    return d, segs


def vread(d, segs, va, n):
    out = b''
    for base, off, fsz in segs:
        if base <= va < base + fsz:
            o = off + (va - base)
            out += d[o:o + n]
            va += len(out)
            n -= len(out)
            if n <= 0:
                break
    return out


def lit(d, segs, addr):
    pcv = (addr + 8) & ~3
    try:
        return struct.unpack('<I', vread(d, segs, pcv, 4))[0]
    except Exception:
        return None


def window(addr):
    if HC <= addr < HC + 0x200:
        return 'HC', addr - HC
    if CORE <= addr < CORE + 0x1000:
        return 'CORE', addr - CORE
    if GCC <= addr < GCC + 0x100000:
        return 'GCC', addr - GCC
    return '?', addr


def main():
    start = int(sys.argv[1], 16)
    count = int(sys.argv[2], 16)
    d, segs = load(ELF)
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    md.detail = True
    code = vread(d, segs, start, count * 4 + 16)
    insns = []
    for ins in md.disasm(code, start):
        insns.append(ins)
        if len(insns) >= count:
            break

    # Le funzioni analizzate ricevono in r0 il puntatore dev/host (o il suo
    # plat). Con --dev si semina la mappa delle struct.
    # NB: capstone non usa i numeri ARM: R0=66, R1=67, ... R12(ip)=78,
    # R13(sp)=79, R14(lr)=80, R15(pc)=81.
    regval = {66: 'dev', 70: 'dev'} if '--dev' in sys.argv else {}
    for ins in insns:
        note = []
        ops = ins.operands

        if ins.mnemonic == 'movw' and len(ops) == 2 and ops[1].type == 2:
            regval[ops[0].reg] = ops[1].imm & 0xFFFF
            note.append('%s = 0x%04x (half)' % (rname(ops[0].reg), ops[1].imm & 0xFFFF))
        elif ins.mnemonic == 'movt' and len(ops) == 2 and ops[1].type == 2:
            r = ops[0].reg
            lo = regval.get(r, 0) & 0xFFFF
            regval[r] = ((ops[1].imm & 0xFFFF) << 16) | lo
            note.append('%s = 0x%08x' % (rname(r), regval[r]))
        elif ins.mnemonic == 'mov' and len(ops) == 2:
            a, b = ops
            if b.type == 1:
                regval[a.reg] = regval.get(b.reg)
                if regval[a.reg] is not None:
                    note.append('r%d = r%d (%s)' % (a.reg, b.reg,
                                                   fmt(regval[a.reg])))
            elif b.type == 2:
                regval[a.reg] = b.imm
                note.append('%s = 0x%x' % (rname(a.reg), b.imm))
            else:
                regval.pop(a.reg, None)
        elif ins.mnemonic in ('ldr', 'ldrb', 'ldrh'):
            if len(ops) == 2 and ops[1].type == 3:
                m = ops[1].mem
                if m.base == 15:
                    v = lit(d, segs, ins.address)
                    regval[ops[0].reg] = v
                    if v is not None:
                        note.append('%s = [literal] 0x%08x' % (rname(ops[0].reg), v))
                    continue
                base = regval.get(m.base)
                if isinstance(base, str) and base in MEM:
                    v = MEM[base].get(m.disp)
                    if v is not None:
                        regval[ops[0].reg] = v
                        note.append('%s = %s+0x%x -> %s' %
                                    (rname(ops[0].reg), base, m.disp, fmt(v)))
                        continue
                if isinstance(base, int):
                    note.append('%s <- [0x%08x] (valore non noto)' %
                                (rname(ops[0].reg), base + m.disp))
                regval[ops[0].reg] = None
        elif ins.mnemonic in ('str', 'strb', 'strh'):
            if len(ops) == 2 and ops[1].type == 3:
                m = ops[1].mem
                base = regval.get(m.base)
                val = regval.get(ops[0].reg)
                if isinstance(base, int):
                    addr = (base + m.disp) & 0xFFFFFFFF
                    win, off = window(addr)
                    note.append('WRITE 0x%08x  %s+0x%x  <- %s' %
                                (addr, win, off, fmt(val)))
        elif ins.mnemonic in ('pop', 'ldmia') and 'pc' in ins.op_str:
            note.append('=== RETURN ===')

        raw = vread(d, segs, ins.address, 4)
        print('0x%08x  %-8s  %-34s%s' %
              (ins.address, raw.hex(), (ins.mnemonic + ' ' + ins.op_str).strip(),
               ('   ; ' + ' | '.join(note)) if note else ''))


def rname(reg):
    # capstone ARM: R0=66 ... R12(ip)=78, SP=79, LR=80, PC=81
    n = reg - 66
    return {12: 'ip', 13: 'sp', 14: 'lr', 15: 'pc'}.get(n, 'r%d' % n)


def fmt(v):
    if v is None:
        return 'r?'
    if isinstance(v, str):
        return v
    return '0x%x' % v


if __name__ == '__main__':
    main()
