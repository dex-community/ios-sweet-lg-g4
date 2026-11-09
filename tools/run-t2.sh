#!/bin/bash
# run-t2.sh -- one diagnostic boot with the T2 probe, then the stock boot back.
#
# WHAT IT DOES, IN ORDER
#   1. the phone goes to the bootloader (adb reboot bootloader)
#   2. fastboot flash boot shim/out/shim-boot.img   (the ONLY write)
#   3. the shim runs; it leaves its log in the ramoops console zone and asks
#      the bootloader to come up in recovery
#   4. the log is pulled (tools/fetch-pstore.sh) and the trail in IMEM too
#   5. IMMEDIATELY the stock boot goes back (backup/boot-stock.img) and the
#      phone reboots into Android; the script verifies sys.boot_completed=1
#
# Step 5 is not optional: the shim must never stay in the boot partition.
#
#   MODE=diag tools/run-t2.sh
#
# If the phone does not come back (a hang), the script says so and stops:
# bringing it back to fastboot needs the buttons (volume down + power).
set -uo pipefail
cd "$(dirname "$0")/.."

# The default image is the BASELINE (the build that was already proven on
# the phone, without the probe). The rule of the session: the baseline first,
# one phase at a time.
IMG=${IMG:-shim/out/shim-boot-baseline.img}
STOCK=${STOCK:-backup/boot-stock.img}
OUT=${OUT:-research/raw-2026-11-09/pstore-t2.bin}
WAIT_S=${WAIT_S:-240}

wait_fastboot() {
	for _ in $(seq 1 60); do
		fastboot devices 2>/dev/null | grep -q . && return 0
		sleep 2
	done
	return 1
}

wait_adb() {
	for _ in $(seq 1 "$1"); do
		adb devices 2>/dev/null | grep -qE "device$|recovery$" && return 0
		sleep 2
	done
	return 1
}

echo "== 0. what we are flashing =="
ls -l "$IMG"
shasum -a 256 "$IMG" 2>/dev/null || sha256sum "$IMG" 2>/dev/null || true

echo "== 1. bootloader =="
echo "(su questo telefono 'adb reboot bootloader' viene IGNORATO dalla ROM"
echo " stock: il telefono torna su Android. Il bootloader si raggiunge con i"
echo " tasti: volume giu' + power.)"
if ! wait_fastboot; then
	echo "FAIL: il telefono non e' in fastboot."
	echo "      Tieni premuto VOLUME GIU' e premi POWER finche' non compare"
	echo "      FASTBOOT MODE, poi rilancia questo script."
	exit 1
fi

echo "== 2. flash the shim =="
fastboot flash boot "$IMG" || { echo "FAIL: flash"; exit 1; }
fastboot reboot

echo "== 3. waiting for the shim to come back ($WAIT_S s) =="
if ! wait_adb "$((WAIT_S / 2))"; then
	echo "FAIL: the phone did not come back. It needs the buttons:"
	echo "      volume down + power, then fastboot flash boot $STOCK"
	exit 2
fi

echo "== 4. the log =="
bash tools/fetch-pstore.sh "$OUT" || echo "(no pstore record)"
echo
echo "== the trail in IMEM =="
bash tools/read-trail-imem.py 2>&1 | tail -40 || echo "(no trail)"

echo "== 5. the stock boot goes back =="
adb reboot bootloader || true
wait_fastboot || { echo "FAIL: no fastboot for the restore"; exit 3; }
fastboot flash boot "$STOCK" || { echo "FAIL: restore"; exit 3; }
fastboot reboot

echo "== 6. Android =="
wait_adb 90 || { echo "FAIL: no adb"; exit 4; }
for _ in $(seq 1 60); do
	[ "$(adb shell 'getprop sys.boot_completed' 2>/dev/null | tr -d '\r')" = "1" ] && break
	sleep 2
done
echo "sys.boot_completed = $(adb shell 'getprop sys.boot_completed' | tr -d '\r')"
adb shell 'getprop ro.build.version.release' | tr -d '\r'
echo "== done =="
