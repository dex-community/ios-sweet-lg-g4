#!/bin/sh
# Read-only: find out which part of the IMEM page (0xfe80f000) is free for the
# trail ring BEFORE flashing a shim that uses it. Phone in recovery, adb root.
set -e
SKIP=$((0xfe80f000 / 4096))
adb exec-out "dd if=/dev/mem bs=4096 skip=$SKIP count=1 2>/dev/null" > /tmp/imem.bin
[ "$(wc -c < /tmp/imem.bin)" -eq 4096 ] || { echo "cannot read /dev/mem (root? STRICT_DEVMEM?)"; exit 1; }
python3 - <<'PY'
import struct
d = open('/tmp/imem.bin','rb').read()
w = struct.unpack('<1024I', d)
print("non-zero words (offset: value):")
for i, v in enumerate(w):
    if v: print(f"  0x{i*4:03x}: 0x{v:08x}")
# longest zero runs, in bytes, 16-byte aligned
runs, start = [], None
for i in range(0, 1024, 4):
    z = not any(w[i:i+4])
    if z and start is None: start = i
    if (not z or i == 1020) and start is not None:
        end = i + 4 if z else i
        runs.append((end - start) * 4, ) if False else runs.append(((end - start) * 4, start * 4))
        start = None
runs.sort(reverse=True)
print("\nlargest all-zero areas (size, offset):")
for size, off in runs[:5]:
    print(f"  {size:5d} bytes at 0x{off:03x}")
print("\ndefault trail area: 0x700..0x900 (-DG4_TRAIL_OFF / -DG4_TRAIL_SIZE to change)")
PY
