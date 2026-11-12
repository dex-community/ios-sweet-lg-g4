#!/bin/bash
# Apply the whole xnu/patches series to a clean xnu-8796 tree and report which
# patches fail. The CI build script (blacktop/darwin-xnu-build) SKIPS a patch
# that does not apply and only prints a warning, so a green CI does NOT mean
# that our patches are in the kernel. This script makes the failure visible.
#
#   tools/check-xnu-patch-chain.sh [TREE]
#
# TREE defaults to /tmp/xnu-patchtest and must be a clean checkout of
# xnu-8796 (git worktree add --detach /tmp/xnu-patchtest HEAD).
#
# The dxb setup edits (sed) are reproduced, because they change the files the
# patches are applied to.
set -uo pipefail
cd "$(dirname "$0")/.."

TREE=${1:-/tmp/xnu-patchtest}
PATCHES=xnu/patches

if [ ! -d "$TREE/bsd" ]; then
	echo "no xnu tree at $TREE" >&2
	exit 1
fi

cd "$TREE" || exit 1

# Start from a clean tree every time: the chain is order dependent, so a
# partially applied tree gives meaningless results. The tree is reset to its
# first commit (the pristine xnu-8796 import), not just to HEAD: a tree that
# was used to write a patch may carry a commit of the already applied chain.
BASE=$(git rev-list --max-parents=0 HEAD | tail -1)
git reset --hard "$BASE" >/dev/null 2>&1
git clean -fdq 2>/dev/null

# the same edits the CI build script makes before patching
sed -i '' 's|^AVAILABILITY_PL="${SDKROOT}/${DRIVERKITROOT}|AVAILABILITY_PL="${FAKEROOT_DIR}|g' \
	bsd/sys/make_symbol_aliasing.sh 2>/dev/null
sed -i '' 's|^#include.*BSD.xcconfig.*||g' libsyscall/Libsyscall.xcconfig 2>/dev/null
sed -i '' 's|^LDFLAGS_KERNEL_SDK	= -L$(SDKROOT).*|LDFLAGS_KERNEL_SDK	= -L$(FAKEROOT_DIR)/usr/local/lib/kernel -lfirehose_kernel|g' \
	makedefs/MakeInc.def 2>/dev/null

ok=0
failed=0
failed_list=()

# The dxb build runs patches/*.sh from the XNU source root BEFORE the patches.
# They stage the Apple sources (hfs, libpthread, bearssl) into the tree, so the
# patch chain only applies when they ran. The staged trees are cached: they are
# a git clone each and the check is run many times.
STAGE_CACHE=${STAGE_CACHE:-/tmp/xnu-stage-cache}
STAGED_DIRS="bsd/sweet_hfs bsd/pthread/sweet_libpthread libkern/crypto/sweet_bearssl"

if [ "${CHECK_KEEP_TREE:-0}" != "1" ]; then
	if [ -d "$STAGE_CACHE" ]; then
		for d in $STAGED_DIRS; do
			[ -d "$STAGE_CACHE/$d" ] && mkdir -p "$(dirname "$TREE/$d")" && cp -R "$STAGE_CACHE/$d" "$TREE/$d"
		done
	else
		for s in "$OLDPWD"/$PATCHES/*.sh; do
			echo "  stage $(basename "$s")"
			(cd "$TREE" && MACHINE_CONFIG=SWEET MACOS_VERSION=13.5 bash "$s") || {
				echo "  staging script failed: $s" >&2
				exit 2
			}
		done
		mkdir -p "$STAGE_CACHE"
		for d in $STAGED_DIRS; do
			[ -d "$TREE/$d" ] && mkdir -p "$STAGE_CACHE/$(dirname "$d")" && cp -R "$TREE/$d" "$STAGE_CACHE/$d"
		done
	fi
fi

for p in "$OLDPWD"/$PATCHES/*.patch; do
	name=$(basename "$p")
	# 0020-sweet-native-sdhci.patch is an ALTERNATIVE approach (the UniXNU
	# IOKit driver instead of the BSD block driver). It is not tracked and
	# not shipped: the CI never sees it, so it is not part of the chain.
	case "$name" in
	0020-sweet-native-sdhci.patch)
		printf '  skip  %s (alternative approach, not shipped)\n' "$name"
		continue
		;;
	esac
	if out=$(git apply --check "$p" 2>&1); then
		git apply "$p"
		ok=$((ok + 1))
		printf '  ok    %s\n' "$name"
	else
		failed=$((failed + 1))
		failed_list+=("$name")
		printf '  FAIL  %s\n        %s\n' "$name" "$(printf '%s' "$out" | head -3 | tr '\n' ' ')"
	fi
done

echo
echo "applied: $ok   failed: $failed"
if [ "$failed" -gt 0 ]; then
	echo "failing patches:"
	printf '  %s\n' "${failed_list[@]}"
	exit 1
fi
