#!/bin/bash
# Boot a kernel on the phone and check what it can do there.
#
#   tools/boot-test-kernel.sh <Image>
#
# The kernel is packed with the recovery ramdisk and the device tree of the
# phone (tools/pack-twrp-kernel.sh), and started with "fastboot boot":
# nothing is written to the phone, and the boot partition is not touched.
#
# The phone has to be in fastboot: the script reboots it there from the
# recovery with "adb reboot bootloader".
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=${1:?usage: boot-test-kernel.sh <Image>}
OUT=${2:-/tmp/twrp-h815-hfs.img}

if [ ! -f "$IMAGE" ]; then
	echo "missing: $IMAGE" >&2
	exit 1
fi

echo "=== packing the kernel (gzip, as the bootloader expects) ==="
GZ=$(mktemp /tmp/kernel-XXXXXX.gz)
gzip -9 -c "$IMAGE" > "$GZ"
ls -la "$GZ"
"$ROOT/tools/pack-twrp-kernel.sh" "$GZ" "$OUT" 2>&1 | grep -E 'sha256|bytes:' | head -3

echo
echo "=== to fastboot ==="
adb reboot bootloader 2>&1
for i in $(seq 1 20); do
	sleep 3
	if fastboot devices 2>/dev/null | grep -q .; then
		echo "in fastboot"
		break
	fi
done
fastboot devices

echo
echo "=== boot ==="
fastboot boot "$OUT" 2>&1 | tail -3

echo
echo "=== waiting for adb ==="
for i in $(seq 1 30); do
	sleep 5
	if adb devices 2>/dev/null | grep -qE '(device|recovery)$'; then
		echo "adb is up"
		break
	fi
	echo "t+$((i * 5))s"
done
adb devices

echo
"$ROOT/tools/check-kernel-hfs.sh"
