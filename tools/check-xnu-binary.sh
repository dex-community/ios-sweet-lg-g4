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

# The TCR_EL1 the 4 KiB build must contain:
#   T0SZ = 26, TG0 = 0b00 (4 KiB), T1SZ = 26, TG1 = 0b10 (4 KiB),
#   IPS = 40 bit, SH0/1 outer, IRGN/ORGN write-back.
WANT_TCR=0x002a51a161a
# The TCR_EL1 of the old 16 KiB build: it must NOT be there any more.
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
# Search the whole file for the 8 byte little endian constant.
have_want=$(python3 - "$K" "$WANT_TCR" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
val = int(sys.argv[2], 16)
pat = val.to_bytes(8, 'little')
print(data.count(pat))
PY
)
have_old=$(python3 - "$K" "$OLD_TCR" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
val = int(sys.argv[2], 16)
pat = val.to_bytes(8, 'little')
print(data.count(pat))
PY
)
if [ "$have_want" -ge 1 ]; then
	echo "ok    TCR_EL1 $WANT_TCR (4 KiB granule) found $have_want time(s)"
else
	echo "FAIL  TCR_EL1 $WANT_TCR (4 KiB granule) NOT in the binary"
	fail=1
fi
if [ "$have_old" -eq 0 ]; then
	echo "ok    the 16 KiB TCR_EL1 $OLD_TCR is gone"
else
	echo "FAIL  the 16 KiB TCR_EL1 $OLD_TCR is still in the binary ($have_old time(s))"
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
