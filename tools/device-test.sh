#!/bin/bash
# Boot a kernel on the G4 and run the read-only device battery, all in one
# command, saving every answer to one evidence file.
#
#   tools/device-test.sh /tmp/kci/Image [out.txt]
#   tools/device-test.sh --collect [out.txt]
#
# What it checks in the same boot (no second boot, no flash):
#   - the kernel release and cmdline
#   - DEVMEM / HFS / HFSPLUS, from /proc/config.gz when the kernel has it,
#     otherwise from /proc/filesystems and /dev/mem
#   - the HFS+ rootfs on userdata: mount flags, dmesg, df, root listing,
#     SystemVersion.plist, and the sha256 of /usr/lib/dyld
#   - the same file read by the userspace reader on the raw block device,
#     when HFSPLUS=/path/to/hfsplus is given (two independent paths must agree)
#   - the shim log left in RAM at 0x50000000 (C2OK, EMMC lines)
#
# "fastboot boot" only: the boot partition is never written. The rootfs is
# only ever mounted read-only.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
COLLECT=0
if [ "${1:-}" = "--collect" ]; then
	COLLECT=1
	shift
	OUT=${1:-/tmp/device-test-$(date +%Y%m%d-%H%M%S).txt}
	IMAGE=
else
	IMAGE=${1:-}
	OUT=${2:-/tmp/device-test-$(date +%Y%m%d-%H%M%S).txt}
fi
BOOTIMG=/tmp/device-test-boot.img
HFSPLUS=${HFSPLUS:-}
FAIL=0

say() { printf '%s\n' "$*" | tee -a "$OUT"; }
sh_() { say "--- adb shell: $1"; adb shell "$1" 2>&1 | tee -a "$OUT"; }

: > "$OUT"
say "=== device test $(date -u '+%Y-%m-%dT%H:%M:%SZ') ==="
say "host: $(uname -srm)"
if [ "$COLLECT" = 0 ] && [ -n "$IMAGE" ]; then
	[ -f "$IMAGE" ] || { say "missing: $IMAGE"; exit 1; }
	say "image: $IMAGE $(wc -c < "$IMAGE") bytes"
	shasum -a 256 "$IMAGE" 2>/dev/null | tee -a "$OUT" || true
fi

# --- boot image -------------------------------------------------------------
if [ "$COLLECT" = 0 ]; then
	HEAD=$(head -c 8 "$IMAGE" 2>/dev/null || true)
	if [ "$HEAD" = "ANDROID!" ]; then
		BOOTIMG=$IMAGE
	else
		say "=== packing (gzip kernel + TWRP ramdisk + dt.img) ==="
		GZ=$(mktemp /tmp/kernel-XXXXXX.gz)
		gzip -9 -c "$IMAGE" > "$GZ" || { say "gzip failed"; exit 1; }
		"$ROOT/tools/pack-twrp-kernel.sh" "$GZ" "$BOOTIMG" 2>&1 | tee -a "$OUT" \
			| grep -E 'sha256|FAIL|bytes:' || true
		rm -f "$GZ"
	fi
	say "boot image: $BOOTIMG $(wc -c < "$BOOTIMG") bytes"
	shasum -a 256 "$BOOTIMG" 2>/dev/null | tee -a "$OUT" || true

	# --- reach fastboot ----------------------------------------------------
	say "=== fastboot ==="
	if ! fastboot devices 2>/dev/null | grep -q .; then
		if adb devices 2>/dev/null | grep -qE '(recovery|device)$'; then
			adb reboot bootloader >/dev/null 2>&1 || true
		fi
		for _ in $(seq 1 20); do
			sleep 3
			fastboot devices 2>/dev/null | grep -q . && break
		done
	fi
	if ! fastboot devices 2>/dev/null | grep -q .; then
		say "no phone in fastboot and adb did not reach it"
		exit 1
	fi
	fastboot devices 2>&1 | tee -a "$OUT"

	say "=== boot (temporary, nothing is written) ==="
	fastboot boot "$BOOTIMG" 2>&1 | tail -3 | tee -a "$OUT"

	say "=== waiting for adb ==="
	for i in $(seq 1 40); do
		adb devices 2>/dev/null | grep -qE '(recovery|device)$' && break
		sleep 3
		say "t+$((i * 3))s"
	done
fi
if ! adb devices 2>/dev/null | grep -qE '(recovery|device)$'; then
	say "adb never came up"
	exit 1
fi
adb devices 2>&1 | tee -a "$OUT"

# --- the battery ------------------------------------------------------------
say "=== kernel ==="
sh_ 'uname -a; cat /proc/cmdline'

say "=== config (runtime) ==="
sh_ 'if [ -r /proc/config.gz ]; then zcat /proc/config.gz | grep -E "^CONFIG_(DEVMEM|HFS_FS|HFSPLUS_FS|IKCONFIG)="; else echo "no /proc/config.gz (CONFIG_IKCONFIG_PROC=n)"; fi'

say "=== filesystems and /dev/mem ==="
sh_ 'grep -iE "hfs|ext4" /proc/filesystems; ls -l /dev/mem'

say "=== the HFS+ volume on userdata ==="
sh_ 'mount | grep -E "mmcblk0p50|hfsplus" || echo "not mounted"'
sh_ 'dmesg | grep -iE "hfsplus|backup volume" | tail -10'
if ! adb shell 'mount | grep -q "mmcblk0p50 on /data"' 2>/dev/null; then
	say "userdata is not on /data, trying a read-only mount on /mnt/ios"
	sh_ 'mkdir -p /mnt/ios; mount -t hfsplus -o ro /dev/block/mmcblk0p50 /mnt/ios; echo "mount rc=$?"'
fi
IROOT=/data
adb shell 'test -f /data/System/Library/CoreServices/SystemVersion.plist' 2>/dev/null || IROOT=/mnt/ios
sh_ "df -h $IROOT; ls -la $IROOT | head -30"
sh_ "cat $IROOT/System/Library/CoreServices/SystemVersion.plist"
sh_ "sha256sum $IROOT/usr/lib/dyld"

if adb shell "test -f $IROOT/System/Library/CoreServices/SystemVersion.plist" 2>/dev/null; then
	say "HFS+ rootfs: OK ($IROOT is readable)"
else
	say "HFS+ rootfs: FAIL (SystemVersion.plist not readable)"
	FAIL=1
fi

if [ -n "$HFSPLUS" ] && [ -f "$HFSPLUS" ]; then
	say "=== the same files through the userspace reader ==="
	adb push "$HFSPLUS" /tmp/hfsplus >/dev/null 2>&1
	sh_ 'chmod 755 /tmp/hfsplus; /tmp/hfsplus /dev/block/mmcblk0p50 ls / | head -25'
	sh_ 'chmod 755 /tmp/hfsplus; /tmp/hfsplus /dev/block/mmcblk0p50 cat /usr/lib/dyld | sha256sum'
else
	say "(userspace reader skipped: set HFSPLUS=/path/to/hfsplus)"
fi

say "=== the shim log left in RAM (0x50000000, 48 pages) ==="
if adb exec-out 'dd if=/dev/mem bs=4096 skip=327680 count=48 2>/dev/null' > /tmp/device-test-shim.bin 2>/dev/null; then
	say "read $(wc -c < /tmp/device-test-shim.bin) bytes"
	strings -a /tmp/device-test-shim.bin | grep -aE 'SHIM LG|EMMC|C2OK|C2TRY|PANIC|WDT armed' | tee -a "$OUT"
else
	say "cannot read /dev/mem"
fi

say "=== done: $OUT ==="
[ "$FAIL" = 0 ] || exit 1
