#!/bin/bash
# Local pre-test: recompile, on this machine, the XNU translation units touched
# by our patches, using the exact compile command line the CI used (same flags,
# same -mcpu=cortex-a53+crypto) and then check the result for LSE atomics.
#
#   tools/local-pretest.sh [source-file ...]
#
# Without arguments it recompiles the files our patches touch. It needs the
# build tree of a successful CI run:
#
#   gh run download <RUN_ID> -n xnu-buildtree -D /tmp/ci-buildtree
#
# The tree is cached in /tmp/ci-buildtree; the compile commands are taken from
# the CI log of that run, so this reproduces the CI environment closely enough
# to catch "Invalid register name", missing defines and LSE regressions in
# seconds instead of waiting for a full CI build.
set -uo pipefail
cd "$(dirname "$0")/.."

RUN_ID=${RUN_ID:-43067814044}
TREE=${TREE:-/tmp/ci-buildtree}
WORK=${WORK:-/tmp/local-pretest}
BASE=$TREE/x
CI_ROOT="/Users/runner/work/ios-sweet-lg-g4/ios-sweet-lg-g4/dxb"

if [ ! -d "$BASE/build/xnu.obj" ]; then
	echo "no build tree in $TREE - run: gh run download $RUN_ID -n xnu-buildtree -D $TREE" >&2
	exit 1
fi

mkdir -p "$WORK"

# files touched by the SWEET patches
FILES=${*:-"osfmk/arm/commpage/commpage_asm.s osfmk/arm64/copyio.c osfmk/arm64/pcb.c osfmk/arm64/platform_tests.c osfmk/arm/arm_init.c osfmk/arm/pmap/pmap.c bsd/dev/sweet_emmc.c"}

# grab the compile command lines from the CI log
LOG=$WORK/ci-$RUN_ID.log
if [ ! -s "$LOG" ]; then
	echo "fetching CI log of run $RUN_ID ..."
	gh run view --log --job=$(gh run view "$RUN_ID" --json jobs -q '.jobs[0].databaseId') > "$LOG" 2>/dev/null
fi

fail=0

# libfirehose_kernel is built by xcodebuild, not by the xnu makefiles, so it has
# no CI compile line to copy. Reproduce its flags here: the firehose slice must
# be pinned to cortex-a53 (the arm64-apple-macos15 triple implies +lse, so
# -march alone does not remove the LSE atomics).
echo "== libfirehose (synthetic LSE test) =="
cat > "$WORK/lse.c" <<'EOF'
#include <stdatomic.h>
_Atomic(long) g;
long f(long d) { return atomic_fetch_add_explicit(&g, d, memory_order_acq_rel); }
EOF
for flags in "-march=armv8-a+crypto" "-mcpu=cortex-a53+crypto"; do
	rm -f "$WORK/lse.o"
	/usr/bin/clang -target arm64-apple-macos15.0 -O2 $flags -c "$WORK/lse.c" -o "$WORK/lse.o" 2>/dev/null
	lse=$(/usr/bin/objdump -d "$WORK/lse.o" 2>/dev/null | grep -cE '\b(casa|casp|ldadd|ldclr|ldeor|ldset|swp)[a-z]*\b')
	llsc=$(/usr/bin/objdump -d "$WORK/lse.o" 2>/dev/null | grep -cE '\b(ldxr|stxr|ldaxr|stlxr)\b')
	echo "     [$flags] LSE=$lse LL/SC=$llsc"
done
rm -f "$WORK/lse.o" "$WORK/lse.c"

for src in $FILES; do
	base=$(basename "$src")
	cmd=$(grep -a "clang -c" "$LOG" | grep -a "/xnu/$src\$" | head -1)
	if [ -z "$cmd" ]; then
		echo "?? no CI compile line for $src (skipped)"
		continue
	fi
	# strip the "build Build <timestamp> " log prefix
	cmd=${cmd#*toolchain/usr/bin/clang}
	cmd="/usr/bin/clang${cmd}"
	# CI paths -> local tree
	cmd=${cmd//$CI_ROOT/$BASE}
	cmd=${cmd//\/Applications\/Xcode_16.app\/Contents\/Developer\/Toolchains\/XcodeDefault.xctoolchain\/usr\/bin\/clang//usr/bin/clang}
	cmd=${cmd//\/Applications\/Xcode_16.app\/Contents\/Developer\/Platforms\/MacOSX.platform\/Developer\/SDKs\/MacOSX15.0.sdk//$BASE/sdk-kernel-platform}
	# per-target EXPORT_HDRS dirs -> build tree layout
	cmd=$(printf '%s' "$cmd" | sed -E "s#-I$BASE/build/xnu.obj/EXPORT_HDRS/([a-z]+)#-I$BASE/build/xnu.obj/EXPORT_HDRS/\\1 -I$BASE/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/\\1/DEVELOPMENT#g")
	# local SDK for the kernel framework private headers
	cmd=${cmd//$BASE\/sdk-kernel-platform\/\/System/$BASE/sdk-kernel-platform}

	out=$WORK/$base.o
	# the CI command has no -o (make passes the object path), so add ours
	case "$cmd" in
	*" -o "*) cmd=$(printf '%s' "$cmd" | sed -E "s#-o [^ ]+#-o $out#") ;;
	*) cmd="$cmd -o $out" ;;
	esac

	if (cd "$BASE/xnu" && eval "$cmd") > "$WORK/$base.log" 2>&1; then
		lse=$(/usr/bin/objdump -d "$out" 2>/dev/null | grep -cE '\b(casa|casp|ldadd|ldclr|ldeor|ldset|swp)[a-z]*\b')
		llsc=$(/usr/bin/objdump -d "$out" 2>/dev/null | grep -cE '\b(ldxr|stxr|ldaxr|stlxr)\b')
		echo "ok   $src  (LSE=$lse LL/SC=$llsc)"
		[ "$lse" != "0" ] && { echo "     ^ LSE atomics present!"; fail=1; }
	else
		echo "FAIL $src"
		grep -a "error:" "$WORK/$base.log" | head -5
		fail=1
	fi
done

exit $fail
