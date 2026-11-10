#!/bin/bash
# Pack a kernel into a boot image the LG G4's LK bootloader accepts.
#
# The pieces come from the recovery image that is known to boot on this
# phone (the TeamWin TWRP 3.7 one), and they are reused as they are:
#
#   backup/nh-parts/ramdisk.bin  the TWRP ramdisk, byte for byte
#   backup/nh-parts/dt.img      the QCDT v3 table with the phone's own
#                                device tree (the one the shim uses too)
#
# Every header field is copied from the image that was booting on the phone
# (twrp-nethunter.img), so that the only difference is the kernel:
#
#   kernel 0x8000, ramdisk 0x2200000, tags 0x100, page size 4096
#
# Usage:
#   tools/pack-twrp-kernel.sh <Image> <out.img>
#
# The result stays under the size the LK accepts for "fastboot boot"
# (about 28 MB), which is why the phone is never flashed to run a test.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=${1:?usage: pack-twrp-kernel.sh <Image> <out.img>}
OUT=${2:?usage: pack-twrp-kernel.sh <Image> <out.img>}

RAMDISK="$ROOT/backup/nh-parts/ramdisk.bin"
DTIMG="$ROOT/backup/nh-parts/dt.img"

for f in "$IMAGE" "$RAMDISK" "$DTIMG"; do
	if [ ! -f "$f" ]; then
		echo "missing: $f" >&2
		exit 1
	fi
done

CMDLINE='console=ttyHSL0,115200,n8 androidboot.console=ttyHSL0 androidboot.hardware=qcom user_debug=31 ehci-hcd.park=3 lpm_levels.sleep_disabled=1 androidboot.init_fatal_reboot_target=recovery'

python3 "$ROOT/tools/mkbootimg_lg.py" \
	--kernel "$IMAGE" \
	--ramdisk "$RAMDISK" \
	--dt-img "$DTIMG" \
	--kernel-addr 0x8000 \
	--ramdisk-addr 0x2200000 \
	--tags-addr 0x100 \
	--page-size 4096 \
	--cmdline "$CMDLINE" \
	--name twrp-nh \
	--out "$OUT"

python3 "$ROOT/tools/verify_bootimg.py" "$OUT" || true
