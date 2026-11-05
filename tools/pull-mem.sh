#!/bin/bash
# Pull a physical memory range off the phone and look for things in it.
#
# The shim draws its console into the bootloader's continuous-splash window
# (cont_splash_mem at 0x03400000). The panel does not show it - the G4's
# panel is in DSI command mode and the bootloader re-programmed the
# controller - but the pixels are still in RAM, so they can be read back
# with root in a recovery and decoded with tools/fb2text.py.
#
# The same scan finds a flattened device tree that the bootloader left in
# RAM: LK fills in /memory before it jumps to the kernel, and that filled
# tree is not in the stored DTB.
#
# Usage:
#   pull-mem.sh dump  <hex-addr> <bytes> <outfile>
#   pull-mem.sh scan  <hex-addr> <bytes>        # look for FDTs and strings
set -euo pipefail

cmd=${1:-}
addr=${2:-0x0}
bytes=${3:-0x100000}
out=${4:-}

need_root() {
	local state
	state=$(adb get-state 2>/dev/null || true)
	if [ "$state" != "device" ] && [ "$state" != "recovery" ]; then
		echo "no device (state=${state:-none})" >&2
		exit 1
	fi
	if [ "$(adb shell id 2>/dev/null | cut -d' ' -f1)" != "uid=0(root)" ]; then
		echo "need root: run this from a recovery with adb root" >&2
		exit 1
	fi
}

case "$cmd" in
dump)
	need_root
	[ -n "$out" ] || { echo "usage: pull-mem.sh dump <addr> <bytes> <out>" >&2; exit 2; }
	sk=$(( addr / 4096 ))
	cnt=$(( bytes / 4096 ))
	echo "reading $bytes bytes at $addr (skip=$sk count=$cnt)"
	adb exec-out dd if=/dev/mem bs=4096 skip="$sk" count="$cnt" 2>/dev/null > "$out"
	ls -l "$out"
	;;
scan)
	need_root
	echo "scanning $bytes bytes at $addr for device trees..."
	sk=$(( addr / 4096 ))
	cnt=$(( bytes / 4096 ))
	adb exec-out dd if=/dev/mem bs=4096 skip="$sk" count="$cnt" 2>/dev/null | python3 - "$addr" <<'PY'
import struct, sys

base = int(sys.argv[1], 0)
data = sys.stdin.buffer.read()
print(f'read {len(data)} bytes')

magic = b'\xd0\x0d\xfe\xed'
i = 0
hits = 0
while True:
    i = data.find(magic, i)
    if i < 0:
        break
    hits += 1
    at = base + i
    size = struct.unpack('>I', data[i + 4:i + 8])[0]
    # A device tree's total size must fit in what we read.
    ok = i + size <= len(data)
    print(f'  FDT at {at:#x} totalsize={size} fits={ok}')
    if ok and hits <= 4:
        with open(f'/tmp/fdt-{at:x}.dtb', 'wb') as f:
            f.write(data[i:i + size])
        print(f'    saved /tmp/fdt-{at:x}.dtb')
    i += 4
if not hits:
    print('  no flattened device tree found in that range')

for needle in (b'LG G4 (H815) shim started', b'SWEETXNU', b'Qualcomm Technologies, Inc. MSM8992'):
    j = data.find(needle)
    print(f'  {needle.decode()!r}: ' + (f'found at {base + j:#x}' if j >= 0 else 'not found'))
PY
	;;
*)
	echo "usage: pull-mem.sh {dump|scan} <hex-addr> <bytes> [outfile]" >&2
	exit 2
	;;
esac
