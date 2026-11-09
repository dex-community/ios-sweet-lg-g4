#!/usr/bin/env python3
"""write-sd-payload.py -- write a payload (test or XNU) to microSD for LG G4.

Layout on microSD (/dev/block/mmcblk1):
  Sector 2048 (LBA 2048): G4PAYLD1 header (512 bytes)
    - magic: "G4PAYLD1"
    - version: 1
    - len: payload length in bytes
    - crc32: zlib crc32 of payload
    - flags: 0
  Sector 2049+ (LBA 2049): payload binary (padded to 512 bytes)

Usage:
  # Prepare test payload image:
  python3 tools/write-sd-payload.py --test --out /tmp/sd_payload.img

  # Or prepare XNU payload image from g4.bin:
  python3 tools/write-sd-payload.py shim/out/g4.bin --out /tmp/sd_payload.img

  # Or write directly to device via ADB (in TWRP recovery):
  python3 tools/write-sd-payload.py --test --device
  python3 tools/write-sd-payload.py shim/out/g4.bin --device
"""
import argparse
import struct
import subprocess
import sys
import zlib


MAGIC = b"G4PAYLD1"
VERSION = 1
HDR_LBA = 2048


def make_payload_image(data: bytes) -> bytes:
    crc = zlib.crc32(data) & 0xffffffff
    hdr = bytearray(512)
    hdr[0:8] = MAGIC
    struct.pack_into("<I", hdr, 8, VERSION)
    struct.pack_into("<I", hdr, 12, len(data))
    struct.pack_into("<I", hdr, 16, crc)
    struct.pack_into("<I", hdr, 20, 0)

    # Pad data to 512-byte blocks
    pad_len = (512 - (len(data) % 512)) % 512
    padded_data = data + (b"\x00" * pad_len)

    print(f"Payload size: {len(data)} bytes ({len(padded_data) // 512} blocks)")
    print(f"CRC32: 0x{crc:08X}")
    return bytes(hdr) + padded_data


def write_to_device(img: bytes):
    import tempfile
    with tempfile.NamedTemporaryFile(suffix=".img") as tmp:
        tmp.write(img)
        tmp.flush()
        print("Pushing payload image to device /tmp/sd_payload.img...")
        subprocess.check_call(["adb", "push", tmp.name, "/tmp/sd_payload.img"])

    blocks = len(img) // 512
    print(f"Writing {blocks} blocks to /dev/block/mmcblk1 seek={HDR_LBA}...")
    cmd = (
        f"dd if=/tmp/sd_payload.img of=/dev/block/mmcblk1 bs=512 seek={HDR_LBA} "
        f"count={blocks} conv=notrunc && rm -f /tmp/sd_payload.img && sync"
    )
    subprocess.check_call(["adb", "shell", cmd])
    print("Done! Payload successfully written to microSD.")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("payload", nargs="?", help="Path to payload binary (e.g. g4.bin)")
    p.add_argument("--test", action="store_true", help="Generate 512-byte test pattern")
    p.add_argument("--out", help="Write payload image to local file")
    p.add_argument("--device", action="store_true", help="Write directly to connected device in TWRP")
    args = p.parse_args()

    if args.test:
        # Standard test pattern: 0x5a ^ i for i in range(512)
        data = bytes([(0x5a ^ i) & 0xff for i in range(512)])
    elif args.payload:
        with open(args.payload, "rb") as f:
            data = f.read()
    else:
        p.error("Specify either a payload file or --test")

    img = make_payload_image(data)

    if args.out:
        with open(args.out, "wb") as f:
            f.write(img)
        print(f"Wrote {len(img)} bytes to {args.out}")

    if args.device:
        write_to_device(img)


if __name__ == "__main__":
    main()
