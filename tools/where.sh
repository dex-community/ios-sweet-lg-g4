#!/bin/bash
# Decode the shim wait time into the faulting instruction.
#
#   tools/where.sh 62
#
# The shim encodes, in the time it waits before the reset, the offset of
# the faulting instruction from the start of shim_main, in instructions
# (a 4 byte step). This script turns that number into a source line.
#
# The build must have been made with RAW_TIMING=1.
set -euo pipefail
cd "$(dirname "$0")/.."

N=${1:?usage: tools/where.sh <n>}

ELF=shim/out/shim.elf
[ -f "$ELF" ] || { echo "no $ELF: build first" >&2; exit 1; }

BASE=$(nm "$ELF" | awk '$3=="shim_main"{print "0x"$1}')
[ -n "$BASE" ] || { echo "shim_main not found in $ELF" >&2; exit 1; }
BASE=$((BASE))

if [ "$N" -ge 128 ]; then
    echo "  the fault was on a SECONDARY core (flag 128)"
    N=$((N - 128))
fi

# The shim counts the offset in 64 byte units (16 instructions), so that the
# number fits in the single byte of the wait time.
ADDR=$((BASE + N * 64))
printf "shim_main is at %s\n" "$(printf '0x%x' "$BASE")"
printf "unit %s -> address %s (base + %s bytes)\n" \
	"$N" "$(printf '0x%x' "$ADDR")" "$((N * 64))"

# The nearest symbol at or below the address.
nm -n "$ELF" | python3 -c '
import sys
a = int("'"$ADDR"'", 0)
sym = None; off = 0
for line in sys.stdin:
    p = line.split()
    if len(p) != 3 or p[2] not in "Tt":
        continue
    v = int(p[0], 16)
    if v <= a:
        sym, off = p[2], a - v
    elif sym:
        break
if sym:
    print(f"  symbol: {sym} +{off} (0x{off:x})")
'

echo "  context:"
DIS=""
command -v llvm-objdump >/dev/null 2>&1 && DIS=llvm-objdump
[ -z "$DIS" ] && command -v objdump >/dev/null 2>&1 && DIS=objdump
if [ -n "$DIS" ]; then
    "$DIS" -d --no-show-raw-insn "$ELF" 2>/dev/null | python3 -c '
import sys, re
a = int("'"$ADDR"'", 0)
pat = re.compile(r"^\s*([0-9a-f]+):")
for line in sys.stdin:
    m = pat.match(line)
    if not m:
        continue
    v = int(m.group(1), 16)
    if a - 24 <= v <= a + 24:
        print("    " + line.rstrip())
'
fi
