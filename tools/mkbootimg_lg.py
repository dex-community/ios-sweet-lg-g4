#!/usr/bin/env python3
"""Build an Android boot image the way the LG G4's LK bootloader expects it.

The sweet shim used `mkbootimg --header_version 2 --dtb <raw.dtb>`, which is
what a Qualcomm ABL bootloader wants. The G4 is booted by LK, and LK reads
the device tree from a QCDT v3 table that is appended to the boot image:

    +---------------------+  page 0
    | boot_img_hdr        |  LK's struct boot_img_hdr (see LK app/aboot/bootimg.h)
    +---------------------+
    | kernel              |  kernel_size bytes, page aligned
    +---------------------+
    | ramdisk             |  ramdisk_size bytes, page aligned
    +---------------------+
    | dt.img (QCDT v3)    |  dt_size bytes, page aligned
    +---------------------+

Two details make this different from a stock AOSP header:

  * LK's `struct boot_img_hdr` has `dt_size` at offset 40 (AOSP v0 has
    `unused` there). The G4's own stock boot.img stores the dt.img size
    exactly there, so we do the same.
  * The device tree is a QCDT v3 table, not a raw DTB. LK picks the entry
    whose platform/board id matches the hardware and copies that DTB to
    tags_addr before jumping to the kernel with the DTB pointer in x0.

Usage:
    mkbootimg_lg.py --kernel out/Image --ramdisk out/ramdisk.bin \
                     --dt-img dtb/h815.dt.img --out out/shim-boot.img

    # or build the QCDT table from a single DTB:
    mkbootimg_lg.py --kernel out/Image --ramdisk out/ramdisk.bin \
                     --dtb dtb/boot.dtb --out out/shim-boot.img
"""
import argparse
import hashlib
import os
import struct
import sys

BOOT_MAGIC = b'ANDROID!'
BOOT_NAME_SIZE = 16
BOOT_ARGS_SIZE = 512
HEADER_SIZE = 608		# struct boot_img_hdr, LK app/aboot/bootimg.h

# QCDT v3, LK platform/msm_shared/include/dev_tree.h
DEV_TREE_MAGIC = 0x54444351		# "QCDT"
DEV_TREE_VERSION_V3 = 3
DEV_TREE_HEADER_SIZE = 12
DEV_TREE_ENTRY_SIZE = 40		# struct dt_entry

# The G4's stock boot.img (aicp/boot.img) uses kernel 0x8000, ramdisk
# 0x1000000, tags 0x100. LK copies the selected DTB to tags_addr, and the
# H815's DTB entry is 307200 bytes (0x4b000), so a tags address of 0x100
# would land on top of a kernel at 0x8000. LG's own image has exactly that
# overlap and still boots, which means its LK overrides the header
# addresses (the msm8996 project does the same with ABOOT_FORCE_*).
# We still pick a layout that is safe if the addresses are honoured:
#   kernel  0x00008000 .. 0x0003e000
#   dtb     0x00400000 .. 0x0044b100   (tags_addr)
#   ramdisk 0x01000000 ..
DEFAULT_KERNEL_ADDR = 0x00008000
DEFAULT_RAMDISK_ADDR = 0x01000000
DEFAULT_TAGS_ADDR = 0x00400000
DEFAULT_PAGE_SIZE = 4096


def align_up(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def build_qcdt(dtbs, page_size=DEFAULT_PAGE_SIZE):
    """Build a QCDT v3 table from [(dtb_bytes, meta), ...].

    meta is (platform_id, variant_id, board_hw_subtype, soc_rev, pmic_rev).
    The DTB blobs follow the entry table, page aligned, like Qualcomm's
    dtbTool does.
    """
    entries = b''
    blobs = []
    header_size = align_up(DEV_TREE_HEADER_SIZE + DEV_TREE_ENTRY_SIZE * len(dtbs), page_size)
    offset = header_size

    for dtb, meta in dtbs:
        platform_id, variant_id, subtype, soc_rev, pmic = meta
        entries += struct.pack('<4I4III',
                               platform_id, variant_id, subtype, soc_rev,
                               pmic[0], pmic[1], pmic[2], pmic[3],
                               offset, len(dtb))
        blobs.append(dtb)
        offset += align_up(len(dtb), page_size)

    out = struct.pack('<III', DEV_TREE_MAGIC, DEV_TREE_VERSION_V3, len(dtbs))
    out += entries
    out += bytes(header_size - len(out))
    for dtb in blobs:
        out += dtb
        out += bytes(align_up(len(dtb), page_size) - len(dtb))
    return out


def build_header(kernel_size, ramdisk_size, dt_size, kernel_addr, ramdisk_addr,
                 tags_addr, page_size, name, cmdline):
    h = bytearray(HEADER_SIZE)
    h[0:8] = BOOT_MAGIC
    struct.pack_into('<9I', h, 8,
                     kernel_size, kernel_addr,
                     ramdisk_size, ramdisk_addr,
                     0, 0,			# second_size, second_addr
                     tags_addr, page_size, dt_size)
    struct.pack_into('<I', h, 44, 0)		# unused
    h[48:48 + BOOT_NAME_SIZE] = name.encode()[:BOOT_NAME_SIZE].ljust(BOOT_NAME_SIZE, b'\0')
    h[64:64 + BOOT_ARGS_SIZE] = cmdline.encode()[:BOOT_ARGS_SIZE].ljust(BOOT_ARGS_SIZE, b'\0')
    return bytes(h)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--kernel', required=True)
    ap.add_argument('--ramdisk', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--dt-img', help='QCDT v3 dt.img to append verbatim')
    ap.add_argument('--dtb', help='single raw DTB; a one-entry QCDT table is built')
    ap.add_argument('--dtb-meta', default='0xfb,0xb64,0,0,0x10009,0x1000a,0,0',
                    help='platform,variant,subtype,soc_rev,pmic0..3 for --dtb')
    ap.add_argument('--cmdline', default='console=null')
    ap.add_argument('--name', default='g4shim')
    ap.add_argument('--kernel-addr', type=lambda s: int(s, 0), default=DEFAULT_KERNEL_ADDR)
    ap.add_argument('--ramdisk-addr', type=lambda s: int(s, 0), default=DEFAULT_RAMDISK_ADDR)
    ap.add_argument('--tags-addr', type=lambda s: int(s, 0), default=DEFAULT_TAGS_ADDR)
    ap.add_argument('--page-size', type=int, default=DEFAULT_PAGE_SIZE)
    args = ap.parse_args()

    if bool(args.dt_img) == bool(args.dtb):
        sys.exit('give exactly one of --dt-img or --dtb')

    kernel = open(args.kernel, 'rb').read()
    ramdisk = open(args.ramdisk, 'rb').read()

    if args.dt_img:
        dt_img = open(args.dt_img, 'rb').read()
        magic, version, count = struct.unpack('<III', dt_img[:DEV_TREE_HEADER_SIZE])
        if magic != DEV_TREE_MAGIC:
            sys.exit(f'{args.dt_img}: not a QCDT table (magic {magic:#x})')
        print(f'dt.img: QCDT v{version}, {count} entries, {len(dt_img)} bytes')
    else:
        dtb = open(args.dtb, 'rb').read()
        if dtb[:4] != b'\xd0\x0d\xfe\xed':
            sys.exit(f'{args.dtb}: not a flattened device tree')
        meta = [int(x, 0) for x in args.dtb_meta.split(',')]
        dt_img = build_qcdt([(dtb, tuple(meta))], args.page_size)
        print(f'dt.img: built QCDT v3, 1 entry, {len(dt_img)} bytes')

    header = build_header(len(kernel), len(ramdisk), len(dt_img),
                          args.kernel_addr, args.ramdisk_addr, args.tags_addr,
                          args.page_size, args.name, args.cmdline)

    ps = args.page_size
    body = bytearray()
    body += header.ljust(ps, b'\0')
    body += kernel.ljust(align_up(len(kernel), ps), b'\0')
    body += ramdisk.ljust(align_up(len(ramdisk), ps), b'\0')
    body += dt_img.ljust(align_up(len(dt_img), ps), b'\0')

    with open(args.out, 'wb') as f:
        f.write(body)

    print(f'{args.out}: {len(body)} bytes')
    print(f'  kernel  {len(kernel):>10} @ {args.kernel_addr:#x}')
    print(f'  ramdisk {len(ramdisk):>10} @ {args.ramdisk_addr:#x}')
    print(f'  dt.img  {len(dt_img):>10} (dt_size field at offset 40)')
    print(f'  tags    {args.tags_addr:#x}  page {ps}  cmdline {args.cmdline!r}')
    print(f'  sha256  {hashlib.sha256(body).hexdigest()}')


if __name__ == '__main__':
    main()
