#!/bin/bash
# Build the LG G4 phone image: CI kernel + G4 ADT -> payload -> LK boot image.
#
#   tools/build-g4-image.sh [RUN_ID] [TAG]
#
# RUN_ID  a GitHub Actions run of the xnu workflow (default: the last
#         successful one of dex-community/ios-sweet-lg-g4)
# TAG    a name for the output files (default: g4)
#
# The kernel comes from the CI (macos-14, Xcode 15.x: the only Xcode whose
# iig matches the xnu-8796 IORPC ABI). The ADT describes the G4 to the
# kernel (RAM map, GIC, framebuffer, eMMC) while the userland still sees
# an iPhone X, and the payload is the kernel+ADT+boot_args block that
# shim/loader.c hands to XNU.
#
# Outputs (in shim/out/):
#   <tag>.adt          the Apple Device Tree for the G4
#   <tag>.bin          the payload the shim boots
#   shim-boot-<tag>.img  the LK boot image to flash on the phone
set -euo pipefail
cd "$(dirname "$0")/.."

REPO=${REPO:-dex-community/ios-sweet-lg-g4}
RUN_ID=${1:-}
TAG=${2:-g4}
KERNEL_CONFIG=${KERNEL_CONFIG:-DEVELOPMENT}
# The cmdline the upstream uses on the sweet: one CPU (no PSCI on the G4),
# the kernel console, and the mockfs ram disk as the root.
CMDLINE=${CMDLINE:-"debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=md0"}

if [ -z "$RUN_ID" ]; then
	RUN_ID=$(gh run list -R "$REPO" --workflow xnu --status success \
		--limit 1 --json databaseId --jq '.[0].databaseId')
	[ -n "$RUN_ID" ] || { echo "no successful xnu run in $REPO" >&2; exit 1; }
fi
echo "== xnu CI run $RUN_ID =="

mkdir -p xnu/out
if [ ! -d "xnu/out-$RUN_ID" ]; then
	gh run download -R "$REPO" "$RUN_ID" -n xnu-kernel -D "xnu/out-$RUN_ID"
fi
cp -R "xnu/out-$RUN_ID/." xnu/out/ 2>/dev/null || true

KERNEL=$(ls xnu/out/kernel.development.* xnu/out/kernel.* 2>/dev/null | head -1 || true)
[ -n "$KERNEL" ] || { echo "no kernel in the artifact" >&2; exit 1; }
echo "== kernel: $KERNEL =="

echo "== ADT (G4) =="
python3 tools/mkadt.py "shim/out/$TAG.adt" --device g4

echo "== payload =="
python3 tools/mkpayload.py "$KERNEL" "shim/out/$TAG.adt" "shim/out/$TAG.bin" \
	--cmdline "$CMDLINE"

echo "== LK boot image =="
(cd shim && ./build.sh "out/$TAG.bin")
cp shim/out/shim-boot.img "shim/out/shim-boot-$TAG.img"

echo "== check =="
python3 tools/unpack_bootv2.py "shim/out/shim-boot-$TAG.img" "shim/out/check-$TAG"
cmp "shim/out/check-$TAG/ramdisk" "shim/out/$TAG.bin" && echo "ramdisk == payload: ok"
shasum -a 256 "shim/out/shim-boot-$TAG.img"
echo
echo "To boot (temporary, nothing is written to the phone):"
echo "  fastboot boot shim/out/shim-boot-$TAG.img"
