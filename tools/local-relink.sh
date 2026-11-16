#!/bin/bash
# Relink XNU locally, without a CI build.
#
#   tools/local-relink.sh BASE_RUN [source.c ...]
#   tools/local-relink.sh 43808900044                     # null relink (validation)
#   tools/local-relink.sh 43808900044 osfmk/vm/vm_kern.c # one changed file
#
# The CI already ships everything needed to rebuild the kernel incrementally:
#
#   xnu-kernel     kernel.development.sweet.a  (1098 object files, ALL of them)
#                  kernel.development.sweet.link/* (linkarguments, exp files)
#                  kernel-support/* (libfirehose_kernel.a, nonlto.o)
#   xnu-buildtree  generated headers, MIG output, file lists, symbolsets.plist
#
# This script recompiles ONLY the translation units you name, using the exact
# compile line from the CI log (same flags, same -mcpu=cortex-a53+crypto),
# replaces those members in the archive, and relinks with the local clang/ld.
# It does not need Xcode 16 or the macOS 15 SDK: the generated headers come
# from the CI build tree, and libcc_kext comes from the local Xcode toolchain.
#
# Output: xnu/out-LOCAL/kernel.development.sweet
# Then:   ./tools/build-g4-image.sh LOCAL g4
#         KERNEL=xnu/out-LOCAL/kernel.development.sweet ./tools/xnu-boot-test.sh LOCAL g4
#
# A NULL relink (no source files) is the sanity check: the kernel it produces
# must pass tools/check-xnu-binary.sh and boot exactly like the CI kernel.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

BASE_RUN=${1:-}
if [ -z "$BASE_RUN" ]; then
	sed -n '2,24p' "$0"
	exit 1
fi
shift
FILES=${*:-}

REPO=${REPO:-dex-community/ios-sweet-lg-g4}
ART=$ROOT/xnu/out-$BASE_RUN
TREE=$ROOT/work/buildtree
BASE=$TREE/x
LOG=$ROOT/work/ci-log/$BASE_RUN.log
WORK=$ROOT/work/local-relink
OUT=$ROOT/xnu/out-LOCAL
SRC=${SRC:-$ROOT/work/xnu-patchtest}
CI_ROOT=/Users/runner/work/ios-sweet-lg-g4/ios-sweet-lg-g4/dxb
XCODE16=/Applications/Xcode_16.app/Contents/Developer
LINKDIR=$ART/kernel.development.sweet.link
OBJDIR=$BASE/build/xnu.obj/DEVELOPMENT_ARM64_SWEET

say() { printf '%s\n' "$*"; }

# --- 1. the artifacts ------------------------------------------------------
if [ ! -f "$ART/kernel.development.sweet" ]; then
	say "== downloading xnu-kernel from run $BASE_RUN"
	gh run download -R "$REPO" "$BASE_RUN" -n xnu-kernel -D "$ART" >/dev/null
fi
if [ ! -d "$BASE/build/xnu.obj" ]; then
	say "== downloading xnu-buildtree from run $BASE_RUN"
	gh run download -R "$REPO" "$BASE_RUN" -n xnu-buildtree -D "$TREE" >/dev/null
	mkdir -p "$BASE"
	tar xzf "$TREE/buildtree.tgz" -C "$BASE"
fi
if [ ! -s "$LOG" ]; then
	say "== fetching the CI log of run $BASE_RUN"
	mkdir -p "$ROOT/work/ci-log"
	JOB=$(gh run view "$BASE_RUN" -R "$REPO" --json jobs --jq '.jobs[0].databaseId')
	gh run view --log -R "$REPO" --job="$JOB" > "$LOG" 2>/dev/null
fi

SUPPORT=$ART/kernel-support
if [ ! -f "$SUPPORT/libfirehose_kernel.a" ]; then
	say "missing $SUPPORT/libfirehose_kernel.a"
	say "the CI workflow publishes it since commit 0096; use a run built AFTER"
	say "that change (or copy it from dxb/fakeroot/usr/local/lib/kernel/)."
	exit 1
fi
if [ ! -d "$SRC/osfmk" ]; then
	say "no patched XNU source tree at $SRC"
	say "run tools/check-xnu-patch-chain.sh first (it applies the whole series)."
	exit 1
fi

CC_RT=$(ls /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib/clang/*/lib/darwin/libclang_rt.cc_kext.a 2>/dev/null | head -1 || true)
[ -n "$CC_RT" ] || CC_RT=$(ls /Library/Developer/CommandLineTools/usr/lib/clang/*/lib/darwin/libclang_rt.cc_kext.a 2>/dev/null | head -1 || true)
[ -n "$CC_RT" ] || { say "no libclang_rt.cc_kext.a in the local toolchain"; exit 1; }

# --- 2. recompile the changed translation units ----------------------------
rm -rf "$WORK"
mkdir -p "$WORK/objs"
cp "$LINKDIR/kernel.development.sweet.a" "$WORK/kernel.a"

for src in $FILES; do
	base=$(basename "$src" .c).o
	cmd=$(grep -a "clang -c" "$LOG" | grep -a "/xnu/$src\$" | head -1 || true)
	[ -n "$cmd" ] || { say "no CI compile line for $src"; exit 1; }
	cmd=${cmd#*toolchain/usr/bin/clang}
	cmd="/usr/bin/clang${cmd}"
	cmd=${cmd//$CI_ROOT/$BASE}
	cmd=${cmd//$XCODE16\/Toolchains\/XcodeDefault.xctoolchain\/usr\/bin\/clang//usr/bin/clang}
	cmd=${cmd//$XCODE16\/Platforms\/MacOSX.platform\/Developer\/SDKs\/MacOSX15.0.sdk/$BASE\/sdk-kernel-platform}
	cmd=$(printf '%s' "$cmd" | sed -E "s#-I$BASE/build/xnu.obj/EXPORT_HDRS/([a-z]+)#-I$BASE/build/xnu.obj/EXPORT_HDRS/\\1 -I$OBJDIR/\\1/DEVELOPMENT#g")
	cmd=${cmd//$BASE\/sdk-kernel-platform\/\/System/$BASE\/sdk-kernel-platform}
	# the source itself comes from the locally patched tree, not the CI tree
	cmd=${cmd//$BASE\/xnu\//$SRC/}
	out=$WORK/objs/$base
	case "$cmd" in
	*" -o "*) cmd=$(printf '%s' "$cmd" | sed -E "s#-o [^ ]+#-o $out#") ;;
	*) cmd="$cmd -o $out" ;;
	esac
	say "== compile $src"
	(cd "$SRC" && eval "$cmd") > "$WORK/$base.log" 2>&1 || {
		grep -a "error:" "$WORK/$base.log" | head -10
		exit 1
	}
	# no LSE atomics: the Cortex-A53 of the G4 traps on casa/ldadd
	LSE=$(/usr/bin/objdump -d "$out" 2>/dev/null | grep -cE '\b(casa|casp|ldadd|ldclr|ldeor|ldset|swp)[a-z]*\b' || true)
	[ "$LSE" = "0" ] || { say "$src contains LSE atomics (LSE=$LSE)"; exit 1; }
	ar r "$WORK/kernel.a" "$out"
done
if [ -n "$FILES" ]; then
	ranlib "$WORK/kernel.a"
fi

# --- 3. relink -------------------------------------------------------------
say "== link (local clang/ld)"
python3 - "$LINKDIR/kernel.development.sweet.linkarguments" "$SUPPORT" "$CC_RT" \
	"$WORK/kernel.a" "$OBJDIR" "$WORK/kernel.unstripped" <<'PY' > "$WORK/link.sh"
import sys
args, support, cc_rt, archive, objdir, out = sys.argv[1:]
flags = open(args).read().strip()
flags = flags.replace("-L/Users/runner/work/ios-sweet-lg-g4/ios-sweet-lg-g4/dxb/fakeroot/usr/local/lib/kernel",
                      "-L" + support)
# -lcc_kext is a compiler-rt archive; the modern toolchain spells it differently
flags = flags.replace("-lcc_kext", cc_rt)
extra = " ".join([
    "-Wl,-force_load," + archive,
    "-Wl,-alias_list," + objdir + "/all-alias.exp",
    "-Wl,-sectcreate,__LINKINFO,__symbolsets," + objdir + "/symbolsets.plist",
    "-Wl,-segprot,__LINKINFO,r--,r--",
])
nonlto = support + "/nonlto.o"
import os
if os.path.exists(nonlto):
    extra += " " + nonlto
print("/usr/bin/clang++ " + flags + " " + extra + " -o " + out)
PY
bash "$WORK/link.sh" > "$WORK/link.log" 2>&1 || {
	tail -25 "$WORK/link.log"
	exit 1
}
/usr/bin/strip -S "$WORK/kernel.unstripped" -o "$WORK/kernel.development.sweet"

# --- 4. publish ------------------------------------------------------------
mkdir -p "$OUT"
cp "$WORK/kernel.development.sweet" "$OUT/kernel.development.sweet"
mkdir -p "$OUT/kernel.development.sweet.link"
cp "$LINKDIR"/*.exp "$LINKDIR"/*.linkarguments "$LINKDIR"/*.linkarchives \
	"$OUT/kernel.development.sweet.link/" 2>/dev/null || true

say "== check"
./tools/check-xnu-binary.sh "$OUT/kernel.development.sweet" | tail -9
say ""
say "local kernel : $OUT/kernel.development.sweet ($(stat -f%z "$OUT/kernel.development.sweet") bytes)"
say "CI kernel    : $ART/kernel.development.sweet ($(stat -f%z "$ART/kernel.development.sweet") bytes)"
say "(byte differences are expected: the linker version is newer)"
say ""
say "next: SHIM_DISPLAY_TEST=0 DISPLAY_MODE=13 SKIP_EMMC=0 ./tools/build-g4-image.sh LOCAL g4"
