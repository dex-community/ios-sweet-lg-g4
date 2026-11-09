#!/bin/bash
# recover-boot.sh -- put the phone back on Android with the stock boot image.
#
# The shim can hang the phone (it did on 2026-11-09: a write to a controller
# whose clock was off blocked the AXI bus, and with the watchdog stopped the
# phone stayed on the LG logo and never came back on USB).
#
# When that happens the phone has to be brought back to fastboot BY HAND
# (volume down + power on the G4), and then this script flashes the stock
# boot image and reboots. It writes NOTHING but the boot partition.
#
#   tools/recover-boot.sh
#
set -euo pipefail
cd "$(dirname "$0")/.."

IMG=${IMG:-backup/boot-stock.img}

[ -f "$IMG" ] || { echo "missing $IMG" >&2; exit 1; }
echo "== stock boot image =="
ls -l "$IMG"
sha256sum "$IMG" 2>/dev/null || shasum -a 256 "$IMG"

echo "== waiting for fastboot (up to 120 s) =="
for _ in $(seq 1 60); do
	if fastboot devices | grep -q .; then
		break
	fi
	sleep 2
done
fastboot devices || true
if ! fastboot devices | grep -q .; then
	echo "the phone is not in fastboot: bring it up with VOLUME DOWN + POWER" >&2
	exit 1
fi

echo "== flashing the stock boot =="
fastboot flash boot "$IMG"
fastboot reboot

echo "== waiting for Android =="
for _ in $(seq 1 60); do
	if adb devices | grep -qE "device$"; then
		break
	fi
	sleep 2
done
adb devices
echo "sys.boot_completed = $(adb shell 'getprop sys.boot_completed' | tr -d '\r')"
adb shell 'getprop ro.build.version.release' | tr -d '\r'
