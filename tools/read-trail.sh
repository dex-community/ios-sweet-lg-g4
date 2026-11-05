#!/bin/bash
# Read the shim log back from the phone.
#
# The shim writes one 512 byte sector into each of the two spare
# partitions (spare1 = p21, spare2 = p36), always at sector 256 of the
# partition, and asks the bootloader to come up in recovery. This script
# does the other half of the loop: pull those two sectors and print them.
#
#   tools/read-trail.sh            # pull and decode
#
# Needs the phone in recovery (the shim reboots into it by itself).
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=${OUT:-/tmp/g4-trail}
mkdir -p "$OUT"

for p in 21 36; do
	adb shell "dd if=/dev/block/mmcblk0p$p of=/sdcard/g4-trail.bin bs=512 skip=256 count=1" >/dev/null 2>&1
	adb pull /sdcard/g4-trail.bin "$OUT/p$p.bin" >/dev/null 2>&1
	adb shell rm -f /sdcard/g4-trail.bin >/dev/null 2>&1 || true
done

python3 - "$OUT" <<'EOF'
import struct, sys, os

out = sys.argv[1]
for part in ("p21", "p36"):
	path = os.path.join(out, part + ".bin")
	if not os.path.exists(path):
		print(f"{part}: missing")
		continue
	data = open(path, "rb").read()
	if len(data) < 24:
		print(f"{part}: short ({len(data)} bytes)")
		continue
	marker = data[:8]
	stage, err, detail = struct.unpack_from("<III", data, 8)
	rca = int.from_bytes(data[20:22], "little")
	text = data[24:].split(b"\x00", 1)[0].decode("ascii", "replace")
	print(f"=== {part} ===")
	print(f"marker  {marker!r}")
	print(f"stage   {stage}")
	print(f"error   {err}")
	print(f"detail  0x{detail:08x}")
	print(f"rca     0x{rca:04x}")
	print("--- log ---")
	print(text)
EOF
