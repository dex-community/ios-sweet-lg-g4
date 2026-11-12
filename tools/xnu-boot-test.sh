#!/bin/bash
# Boot XNU on the phone and read back what it said.
#
#   tools/xnu-boot-test.sh [RUN_ID] [TAG]
#   tools/xnu-boot-test.sh --collect [OUT]
#
# The cycle, always the same:
#
#   1. fastboot boot shim/out/shim-boot-<TAG>.img
#      The LK bootloader loads the shim, the shim brings the eMMC host up,
#      writes its log and hands over to the XNU payload.
#   2. wait for the phone to settle: XNU either boots, or it panics and the
#      phone reboots into the recovery.
#   3. fastboot boot /tmp/device-test-boot.img
#      The custom TWRP (the CI H815 kernel with CONFIG_DEVMEM plus the TWRP
#      ramdisk). It is the only image on this phone that has /dev/mem, and
#      without /dev/mem the RAM cannot be read: XNU says what it says in the
#      RAM (the log of the eMMC driver, the console ring of the kernel) and
#      the reset does not scrub the DRAM.
#   4. read the RAM.
#
# Nothing is ever written to the phone: only "fastboot boot", never
# "fastboot flash".
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

COLLECT=0
if [ "${1:-}" = "--collect" ]; then
	COLLECT=1
	shift
	OUT=${1:-$ROOT/work/xnu-boot-test-$(date +%Y%m%d-%H%M%S).txt}
	RUN_ID=
	TAG=g4
else
	RUN_ID=${1:-}
	TAG=${2:-g4}
	OUT=$ROOT/work/xnu-boot-test-$(date +%Y%m%d-%H%M%S).txt
fi

# The custom TWRP with /dev/mem lives in the project: /tmp is cleaned by the
# system and without it the RAM of the phone cannot be read.
TWRP_IMG=${TWRP_IMG:-$ROOT/work/device-test-boot.img}
[ -f "$TWRP_IMG" ] || TWRP_IMG=/tmp/device-test-boot.img
SHIM_IMG=shim/out/shim-boot-$TAG.img
PAYLOAD=shim/out/$TAG.bin
KERNEL=${KERNEL:-}

say() { printf '%s\n' "$*" | tee -a "$OUT"; }
sh_() { say "--- adb shell: $1"; adb shell "$1" 2>&1 | tee -a "$OUT"; }

: > "$OUT"
say "=== xnu boot test $(date -u '+%Y-%m-%dT%H:%M:%SZ') ==="

if [ ! -f "$TWRP_IMG" ]; then
	say "missing $TWRP_IMG: the custom TWRP with /dev/mem is what makes the"
	say "RAM readable. Build it with tools/device-test.sh (it packs the CI"
	say "kernel, the TWRP ramdisk and dt.img into it)."
	exit 1
fi

# --- 1. the XNU boot -------------------------------------------------------
if [ "$COLLECT" = 0 ]; then
	[ -f "$SHIM_IMG" ] || { say "missing $SHIM_IMG"; exit 1; }
	if [ -z "$KERNEL" ]; then
		KERNEL=$(ls -t xnu/out-*/kernel.development.* 2>/dev/null | head -1 || true)
	fi
	say "shim image : $SHIM_IMG"
	say "payload    : $PAYLOAD"
	say "kernel     : ${KERNEL:-?}"

	if ! fastboot devices 2>/dev/null | grep -q .; then
		if adb devices 2>/dev/null | grep -qE '(recovery|device)$'; then
			adb reboot bootloader >/dev/null 2>&1 || true
		fi
		for _ in $(seq 1 20); do
			sleep 3
			fastboot devices 2>/dev/null | grep -q . && break
		done
	fi
	fastboot devices 2>/dev/null | grep -q . || { say "no phone in fastboot"; exit 1; }

	say "=== boot XNU (temporary, nothing is written) ==="
	fastboot boot "$SHIM_IMG" 2>&1 | tail -3 | tee -a "$OUT"

	say "=== waiting for the phone to settle ==="
	for i in $(seq 1 30); do
		sleep 5
		state=up
		fastboot devices 2>/dev/null | grep -q . && state=fastboot
		adb devices 2>/dev/null | grep -qE '(recovery|device)$' && state=adb
		say "t+$((i * 5))s: $state"
		[ "$state" = fastboot ] && break
	done

	# Se il telefono è in recovery invece di fastboot, dobbiamo tornare in fastboot
	# LK boot in recovery quando XNU crasha - dobbiamo forzare il ritorno in fastboot
	if [ "$state" = "adb" ]; then
		say "=== phone in recovery mode, forcing fastboot ==="
		# Metodo 1: da recovery, reboot in bootloader
		adb reboot bootloader 2>/dev/null || true
		sleep 5
		for _ in $(seq 1 20); do
			fastboot devices 2>/dev/null | grep -q . && break
			sleep 3
		done
		fastboot devices 2>/dev/null | grep -q . || {
			# Metodo 2: se non funziona, forza con keys
			say "forcing fastboot with volume down + power"
		}
	fi

	# --- 3. the custom TWRP, the only image with /dev/mem -------------------
	say "=== boot the custom TWRP ($TWRP_IMG) ==="
	if ! fastboot devices 2>/dev/null | grep -q .; then
		if adb devices 2>/dev/null | grep -qE '(recovery|device)$'; then
			adb reboot bootloader >/dev/null 2>&1 || true
		else
			say "the phone answers neither adb nor fastboot: it is running XNU."
			say "Reboot it into the bootloader (power + volume down) and run"
			say "this script again with --collect."
			exit 2
		fi
		for _ in $(seq 1 20); do
			sleep 3
			fastboot devices 2>/dev/null | grep -q . && break
		done
	fi
	fastboot devices 2>/dev/null | grep -q . || { say "no phone in fastboot"; exit 1; }
	fastboot boot "$TWRP_IMG" 2>&1 | tail -3 | tee -a "$OUT"

	say "=== waiting for adb ==="
	for i in $(seq 1 40); do
		adb devices 2>/dev/null | grep -qE '(recovery|device)$' && break
		sleep 3
	done
fi

adb devices 2>/dev/null | grep -qE '(recovery|device)$' || { say "adb never came up"; exit 1; }
adb devices 2>&1 | tee -a "$OUT"

# --- 4. read the RAM --------------------------------------------------------
say "=== the RAM ==="
sh_ 'ls -l /dev/mem'

if [ -n "$KERNEL" ] && [ -f "$KERNEL" ] && [ -f "$PAYLOAD" ]; then
	say "=== what XNU said ==="
	python3 tools/read-xnu-log.py "$KERNEL" "$PAYLOAD" 2>&1 | tee -a "$OUT"
else
	say "(no kernel or payload to compute the addresses: skipping the XNU log)"
fi

# The boot markers live in a page of plain RAM that the recovery does not
# reuse, so they survive the reset: they say how far the kernel got.
say "=== the boot markers ==="
python3 tools/read-pram.py --marks 2>&1 | tee -a "$OUT" || true

say "=== the exception probe ==="
python3 tools/read-pram.py --exc 2>&1 | tee -a "$OUT" || true

say "=== the log of the shim ==="
python3 tools/read-shim-log.py 2>&1 | tee -a "$OUT" || true

say "=== done: $OUT ==="
