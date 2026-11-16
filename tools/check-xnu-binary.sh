#!/bin/bash
# Check the built XNU kernel on the binary itself.
#
# The CI is green even when a patch does not apply (the build script only
# prints a warning and goes on), so "the build passed" says nothing about
# what is really inside the kernel. This script checks the things that
# matter for the SWEET/G4 port.
#
#   tools/check-xnu-binary.sh [KERNEL]
#
# KERNEL defaults to the kernel of the last CI run in xnu/out-*.
#
# What is checked:
#   1. the granule of TCR_EL1 (the G4 has no 16 KiB granule: the kernel
#      must be built for 4 KiB, so TG0 must be 0b00 and T0SZ/T1SZ 26);
#   2. the link address of the kernel (the 37 bit layout of BCM2837:
#      0xfffffff007004000, not the 48 bit one of ARM_LARGE_MEMORY);
#   3. the boot markers (sweet_mark and the 0x5EED constants), so we know
#      the patch that writes them is really in the binary;
#   4. no LSE atomics (the Cortex-A53 has no LSE: casa/ldadd trap).
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

# The 4 KiB TCR_EL1 is NOT a fixed literal: the shareability, TBI and
# E0PD bits change with the board config, so matching one magic constant
# gives a FALSE NEGATIVE on a kernel that is in fact correct.
# Measured 2026-11-16 on the CI artifact 43313974215:
#   4 KiB build -> 0x022a51a251a   (T0SZ 26, TG0 0b00, T1SZ 26, TG1 0b10)
#   old 16 KiB  -> 0x0226511a511   (T0SZ 17, TG0 0b10)
# The checker therefore DECODES the fields instead of guessing the whole
# word. The old value is still rejected explicitly.
OLD_TCR=0x0226511a511

if [ $# -ge 1 ]; then
	K=$1
else
	K=$(ls -t xnu/out-*/kernel.development.sweet 2>/dev/null | head -1)
fi

if [ -z "${K:-}" ] || [ ! -f "$K" ]; then
	echo "no kernel found (download one with: gh run download <RUN> -n xnu-kernel -D xnu/out-<RUN>)" >&2
	exit 2
fi

echo "kernel: $K"
fail=0

# ---------------------------------------------------------------- granule
# Find every 8-byte constant that looks like a TCR_EL1 and decode its
# fields. A 4 KiB build needs T0SZ=26, TG0=0b00, T1SZ=26, TG1=0b10.
granule=$(python3 - "$K" "$OLD_TCR" <<'PY'
import sys, struct
data = open(sys.argv[1], 'rb').read()
old = int(sys.argv[2], 16)
found = {}
has_old = 0
# A TCR_EL1 constant lives in an 8-byte-aligned data slot, NOT in an
# unaligned instruction stream. Scanning every byte offset produced 55
# false positives (A64 opcodes); aligned + high-bits-zero + IPS=40bit
# leaves exactly the real TCR.
for i in range(0, len(data) - 8, 8):
    w = struct.unpack_from('<Q', data, i)[0]
    if w == old:
        has_old += 1
    if (w >> 40) != 0 or ((w >> 32) & 7) != 2:
        continue
    if (w & 0x3f) != 26 or ((w >> 16) & 0x3f) != 26:
        continue
    if ((w >> 14) & 3) != 0 or ((w >> 30) & 3) != 2:
        continue
    found[w] = found.get(w, 0) + 1
print('old=%d' % has_old)
for w, n in sorted(found.items()):
    print('tcr=0x%016x count=%d T0SZ=%d TG0=%d T1SZ=%d TG1=%d IPS=%d' %
          (w, n, w & 0x3f, (w >> 14) & 3, (w >> 16) & 0x3f,
           (w >> 30) & 3, (w >> 32) & 7))
PY
)
echo "$granule" | grep '^tcr=' || true
n_tcr=$(printf '%s\n' "$granule" | grep -c '^tcr=' || true)
n_old=$(printf '%s\n' "$granule" | sed -n 's/^old=\([0-9]*\)$/\1/p')
if [ "${n_tcr:-0}" -ge 1 ]; then
	echo "ok    4 KiB TCR_EL1 found ($n_tcr distinct value(s))"
else
	echo "FAIL  no TCR_EL1 with TG0=0b00 / T0SZ=26 in the binary"
	fail=1
fi
if [ "${n_old:-1}" -eq 0 ]; then
	echo "ok    the 16 KiB TCR_EL1 $OLD_TCR is gone"
else
	echo "FAIL  the 16 KiB TCR_EL1 $OLD_TCR is still in the binary ($n_old time(s))"
	fail=1
fi

# ------------------------------------------------------------ link address
text_vmaddr=$(/usr/bin/otool -l "$K" 2>/dev/null | awk '
	/^ *segname __TEXT$/ { intext = 1; next }
	intext && /vmaddr/ { print $2; exit }')
if [ -z "$text_vmaddr" ]; then
	echo "FAIL  cannot read the __TEXT vmaddr"
	fail=1
elif [ "$text_vmaddr" = "0xfffffff007004000" ]; then
	echo "ok    __TEXT vmaddr $text_vmaddr (37 bit kernel address space)"
else
	echo "FAIL  __TEXT vmaddr $text_vmaddr, expected 0xfffffff007004000"
	fail=1
fi

# ---------------------------------------------------------------- markers
marks=$(/usr/bin/nm "$K" 2>/dev/null | grep -c "sweet_mark")
if [ "$marks" -ge 1 ]; then
	echo "ok    sweet_mark is in the symbol table"
else
	echo "FAIL  sweet_mark is not in the symbol table"
	fail=1
fi
imms=$(/usr/bin/objdump -d "$K" 2>/dev/null | grep -c "#0x5eed")
if [ "$imms" -ge 1 ]; then
	echo "ok    $imms boot marker immediates (0x5eed....)"
else
	echo "FAIL  no boot marker immediates (0x5eed....): patch 0082/0084 is not in the binary"
	fail=1
fi

# -------------------------------------------------------------------- LSE
# ldaxr/stlxr are LL/SC, they are fine; only the atomics with LSE are not.
lse=$(/usr/bin/objdump -d "$K" 2>/dev/null | grep -cE "\b(casa|casal|cas|casp|ldadd|ldset|swp)\b")
if [ "$lse" -eq 0 ]; then
	echo "ok    no LSE atomics"
else
	echo "FAIL  $lse LSE atomics: the Cortex-A53 traps on them"
	fail=1
fi

echo
if [ "$fail" -eq 0 ]; then
	echo "all the checks passed"
else
	echo "SOME CHECKS FAILED"
fi
exit "$fail"
