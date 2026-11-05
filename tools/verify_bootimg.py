#!/usr/bin/env python3
"""Check an LK boot image the way the phone's bootloader reads it.

The G4's LK bootloader parses the image like this (LK app/aboot/aboot.c and
platform/msm_shared/dev_tree.c):

    page_size     = hdr->page_size
    kernel_actual = round_up(hdr->kernel_size,   page_size)
    ramdisk_actual= round_up(hdr->ramdisk_size,  page_size)
    second_actual = round_up(hdr->second_size,   page_size)
    dt_table      = image + page_size + kernel_actual + ramdisk_actual + second_actual
    table->magic  == "QCDT", version 3, num_entries entries of 40 bytes
    entries[i]    = platform_id, variant_id, board_hw_subtype, soc_rev,
                    pmic_rev[4], offset, size
    the DTB at entries[i].offset is copied to hdr->tags_addr and its pointer
    is handed to the kernel in x0

This script reproduces that and then checks which entry LK would select for
the H815 (qcom,msm-id = <0xfb 0x00>, qcom,board-id = <0xb64 0x00>).

Usage: verify_bootimg.py shim/out/shim-boot.img
"""
import struct
import subprocess
import sys
import tempfile

BOOT_MAGIC = b'ANDROID!'
DEV_TREE_MAGIC = 0x54444351
DEV_TREE_HEADER_SIZE = 12
DEV_TREE_ENTRY_SIZE = 40
FDT_MAGIC = 0xd00dfeed

fail = False


def check(cond, message):
    global fail
    print(('  ok   ' if cond else '  FAIL ') + message)
    if not cond:
        fail = True
    return cond


def fdt_text(dtb):
    """The decompiled device tree as text."""
    with tempfile.NamedTemporaryFile(suffix='.dtb') as f:
        f.write(dtb)
        f.flush()
        out = subprocess.run(['dtc', '-I', 'dtb', '-O', 'dts', '-o', '-', f.name],
                             capture_output=True, text=True)
    return out.stdout


def fdt_props(dtb, path='/'):
    """Return {name: value} for the properties of one node, via dtc.

    path '/' is the root node. With path None, every property in the tree
    is collected (first match wins).
    """
    with tempfile.NamedTemporaryFile(suffix='.dtb') as f:
        f.write(dtb)
        f.flush()
        out = subprocess.run(['dtc', '-I', 'dtb', '-O', 'dts', '-o', '-', f.name],
                             capture_output=True, text=True)
    if out.returncode != 0:
        return {}
    props = {}
    node = None
    for line in out.stdout.splitlines():
        s = line.strip()
        if s.endswith('{'):
            name = s[:-1].strip()
            node = '/' if name == '/' else name.lstrip('/')
        elif s == '};':
            node = None
        elif '=' in s and (node is not None) and (path is None or node == path):
            k, _, v = s.partition('=')
            props.setdefault(k.strip(), v.strip().rstrip(';'))
    return props


def main():
    path = sys.argv[1]
    data = open(path, 'rb').read()

    print(f'{path}: {len(data)} bytes')
    print('header')
    check(data[:8] == BOOT_MAGIC, 'magic is ANDROID!')
    (kernel_size, kernel_addr, ramdisk_size, ramdisk_addr, second_size,
     second_addr, tags_addr, page_size, dt_size) = struct.unpack('<9I', data[8:44])
    unused = struct.unpack('<I', data[44:48])[0]
    check(page_size == 4096, f'page_size is 4096 (got {page_size})')
    check(unused == 0, f'unused is 0 (got {unused})')
    check(second_size == 0, f'no second stage (got {second_size})')
    print(f'  kernel  {kernel_size} @ {kernel_addr:#x}')
    print(f'  ramdisk {ramdisk_size} @ {ramdisk_addr:#x}')
    print(f'  tags    {tags_addr:#x}')
    print(f'  dt_size {dt_size}')

    def ru(n):
        return (n + page_size - 1) // page_size * page_size

    dt_table_offset = page_size + ru(kernel_size) + ru(ramdisk_size) + ru(second_size)
    print('device tree table')
    check(dt_table_offset + dt_size <= len(data),
          'dt.img fits inside the image')
    table = data[dt_table_offset:dt_table_offset + dt_size]
    magic, version, count = struct.unpack('<III', table[:DEV_TREE_HEADER_SIZE])
    check(magic == DEV_TREE_MAGIC, f'magic is QCDT (got {magic:#x})')
    check(version == 3, f'version is 3 (got {version})')

    print('entries')
    picked = None
    for i in range(count):
        o = DEV_TREE_HEADER_SIZE + i * DEV_TREE_ENTRY_SIZE
        (pid, vid, subtype, soc_rev, pm0, pm1, pm2, pm3, off, size) = \
            struct.unpack('<10I', table[o:o + DEV_TREE_ENTRY_SIZE])
        blob = table[off:off + size]
        ok = len(blob) == size and struct.unpack('>I', blob[:4])[0] == FDT_MAGIC
        props = fdt_props(blob, None) if ok else {}
        board = props.get('qcom,board-id', '?')
        msm = props.get('qcom,msm-id', '?')
        mark = ''
        if ok and '0xb64' in board:
            picked = (i, pid, vid, blob)
            mark = '   <== H815 (board-id 0xb64)'
        print(f'  [{i}] plat={pid:#x} var={vid:#x} off={off:#x} size={size} '
              f'fdt={"ok" if ok else "BAD"} {msm} {board}{mark}')

    print('selection')
    check(picked is not None, 'an entry with board-id 0xb64 exists (the H815)')
    if picked:
        i, pid, vid, blob = picked
        check(pid in (0xfb, 0xfc), f'entry platform id is 0xfb/0xfc (got {pid:#x})')
        check(vid == 0xb64, f'entry variant id is 0xb64 (got {vid:#x})')
        # No two loaded regions may overlap, whatever LK decides to honour.
        regions = [('dtb', tags_addr, size), ('kernel', kernel_addr, kernel_size),
                   ('ramdisk', ramdisk_addr, ramdisk_size)]
        for a in range(len(regions)):
            for b in range(a + 1, len(regions)):
                na, sa, la = regions[a]
                nb, sb, lb = regions[b]
                overlap = sa < sb + lb and sb < sa + la
                check(not overlap,
                      f'{na} ({sa:#x}+{la:#x}) does not overlap {nb} ({sb:#x}+{lb:#x})')
        props = fdt_props(blob, None)
        # The H815 has no simple-framebuffer node: the panel is driven by the
        # MDP in command mode and the bootloader leaves a cont_splash_mem
        # window at 0x03400000. Check that the carve-out is still there.
        text = fdt_text(blob)
        check('cont_splash_mem' in text and '0x3400000' in text,
              'the selected DTB describes the cont_splash_mem window at 0x3400000')
        print(f'  DTB: {props.get("model", "?")} / {props.get("compatible", "?")}')

    print('FAIL' if fail else 'PASS')
    return 1 if fail else 0


if __name__ == '__main__':
    sys.exit(main())
