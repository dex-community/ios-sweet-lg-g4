#!/bin/bash
# Check what the kernel with CONFIG_DEVMEM and CONFIG_HFSPLUS_FS can do on
# the phone. The phone must be running that kernel (fastboot boot, no flash)
# with adb up.
#
#   tools/check-kernel-hfs.sh
#
# It reads only: the iOS rootfs on userdata is mounted read-only, and the
# IMEM page is dumped, never written.
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
ADB=${ADB:-adb}
TMP=$(mktemp -d)

echo "=== the kernel ==="
$ADB shell 'uname -a; cat /proc/version' 2>&1 | head -3

echo
echo "=== /dev/mem ==="
$ADB shell 'ls -l /dev/mem 2>&1' | head -3

echo
echo "=== filesystems ==="
$ADB shell 'cat /proc/filesystems' | grep -iE 'hfs|ext4' || echo "no hfs in /proc/filesystems"

echo
echo "=== the IMEM trail (0xfe80f000, one page) ==="
# 0xfe80f000 / 4096 = 1042063
if $ADB exec-out 'dd if=/dev/mem bs=4096 skip=1042063 count=1 2>/dev/null' > "$TMP/imem.bin"; then
	echo "$(wc -c < "$TMP/imem.bin") bytes read"
	python3 "$ROOT/tools/read-trail-imem.py" --file "$TMP/imem.bin"
else
	echo "cannot read /dev/mem"
fi

echo
echo "=== the iOS rootfs on userdata (read-only) ==="
$ADB shell 'mkdir -p /mnt/ios' 2>/dev/null
if $ADB shell 'mount -t hfsplus -o ro /dev/block/mmcblk0p50 /mnt/ios' 2>&1 | head -3; then
	$ADB shell 'ls -la /mnt/ios' 2>&1 | head -25
	echo "--- the root of the filesystem ---"
	$ADB shell 'ls -la /mnt/ios/rootfs 2>/dev/null || ls -la /mnt/ios/private 2>/dev/null' | head -15
	$ADB shell 'umount /mnt/ios' 2>&1 | head -2
else
	echo "the mount did not work"
fi

rm -rf "$TMP"
