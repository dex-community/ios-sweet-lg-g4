#!/bin/bash
# Build the LG G4 (H815) shim and wrap it in the boot.img the phone's LK
# bootloader expects: an LK header with dt_size at offset 40 and a QCDT v3
# device-tree table appended after the ramdisk.
#
#   shim/build.sh [payload.bin]
#
# With a payload it is packed as the boot image ramdisk; without one an
# empty newc cpio is used and the image only runs the shim diagnostics.
set -euo pipefail
cd "$(dirname "$0")"

# aarch64-elf-gcc (brew) or aarch64-linux-gnu-gcc (Debian/Ubuntu cross)
CC=${CC:-$(command -v aarch64-elf-gcc || command -v aarch64-linux-gnu-gcc)}
OBJCOPY=${OBJCOPY:-$(command -v aarch64-elf-objcopy || command -v aarch64-linux-gnu-objcopy)}
READELF=${READELF:-$(command -v aarch64-elf-readelf || command -v aarch64-linux-gnu-readelf)}
NM=${NM:-$(command -v aarch64-elf-nm || command -v aarch64-linux-gnu-nm)}
[ -n "$CC" ] || { echo "error: no aarch64 cross compiler found (brew install aarch64-elf-gcc)" >&2; exit 1; }

# The device tree table LK reads. The default is the table extracted from
# this phone's own boot partition (dtb/h815-phone.dt.img); dtb/h815.dt.img
# is the same table out of the AICP zip (the two differ in three unrelated
# properties). Override with DT_IMG=..., or build a one-entry table with
# --dtb (see tools/mkbootimg_lg.py).
DT_IMG=${DT_IMG:-../dtb/h815-phone.dt.img}
DTB=${DTB:-}

# SHIM_HW_DIAG=0 builds a variant that touches no phone-specific MMIO, so it
# can run under QEMU (which has no GIC at the G4's address, no watchdog).
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
[ "${SHIM_HW_DIAG:-1}" = "1" ] && CFLAGS="$CFLAGS -DSHIM_HW_DIAG=1" || CFLAGS="$CFLAGS -DSHIM_HW_DIAG=0"
# The QEMU test screen is smaller than the phone panel, so it uses a
# smaller font to fit the same text.
CFLAGS="$CFLAGS -DCON_SCALE=${CON_SCALE:-3}"
# Pixel format of the splash window. The panel carries 24 bpp over DSI
# (qcom,mdss-dsi-bpp = <0x18>) and the 12 MiB cont_splash_mem only fits
# RGB888 (1440*2560*3 = 10.5 MiB; 32 bpp would need 14.1 MiB). The QEMU
# framebuffer is 32 bpp, so that build passes FB_BPP=32.
CFLAGS="$CFLAGS -DFB_BPP=${FB_BPP:-24}"
FORCE_WAIT_S=${FORCE_WAIT_S:-0}
CFLAGS="$CFLAGS -DSHIM_FORCE_WAIT_S=$FORCE_WAIT_S"
CFLAGS="$CFLAGS -DSHIM_RAW_TIMING=${RAW_TIMING:-0}"
CFLAGS="$CFLAGS -DSHIM_FINE_TIMING=${FINE_TIMING:-0}"
CFLAGS="$CFLAGS -DSHIM_FINE_BASE_S=${FINE_BASE_S:-40}"
# The diagnostic build sweeps the bring-up variants in one boot and reports
# the result through the wait time (see shim/emmc_diag.inc).
CFLAGS="$CFLAGS -DEMMC_DIAG_VARIANTS=${DIAG_VARIANTS:-0}"
CFLAGS="$CFLAGS -DSHIM_DIAG_TIMING=${DIAG_TIMING:-0}"
# The QEMU build serves the eMMC register windows from the software model
# (shim/emmc_model.inc): there is no SDCC1 controller under QEMU. The model
# is a made up controller, so this build says nothing about the phone.
CFLAGS="$CFLAGS -DSHIM_EMMC_MODEL=${SHIM_EMMC_MODEL:-0}"
# The T2 read-only probe of SDCC1 (eMMC) and SDCC2 (the microSD slot):
# it reads the controllers and prints what it finds into the pstore log.
CFLAGS="$CFLAGS -DSHIM_SD_PROBE=${SHIM_SD_PROBE:-0}"
# Which phases of the T2 probe run in this build (one configuration per boot):
#   1 = A eMMC read only, 2 = B eMMC after reset, 4 = C SDCC2 read only,
#   8 = D SDCC2 software reset (DANGEROUS: it hung the phone on 2026-11-09).
CFLAGS="$CFLAGS -DSHIM_SD_PROBE_PHASE=0x$(printf '%x' "${SD_PROBE_PHASE:-3}")u"
# The watchdog guard: it turns a bus hang into a reset. The bark time is in
# milliseconds; the wait loops pet it (shim/emmc.c).
CFLAGS="$CFLAGS -DSHIM_WDT_GUARD=${SHIM_WDT_GUARD:-1}"
# The self-test of the MMIO allowlist (the QEMU test uses it to prove that a
# write outside the allowlist never reaches the bus).
CFLAGS="$CFLAGS -DSHIM_MMIO_SELFTEST=${SHIM_MMIO_SELFTEST:-0}"
CFLAGS="$CFLAGS -DG4_WDT_GUARD_MS=${WDT_GUARD_MS:-60000}"
# The phone has DRAM at 0xff00000, where its ramoops region is. QEMU's
# virt machine has its RAM at 0x40000000, and only 128 MiB of it (the
# machine's default: the memory-backend-file is the backing store, not the
# size), so there the record goes to 0x44000000, inside the RAM and clear
# of the image (0x40200000), the framebuffer (0x40000000) and the device
# tree (0x48000000).
if [ "${SHIM_EMMC_MODEL:-0}" = "1" ]; then
	CFLAGS="$CFLAGS -DRAMOOPS_BASE=0x44000000UL -DRAMOOPS_BASE2=0"
fi
# The T2 probe is a separate translation unit so that the eMMC code stays
# untouched (the instruction was: do not touch emmc*). It is always compiled
# (it is small and it has no side effects when SHIM_SD_PROBE=0), so that the
# link line does not change from one build to the next.
SRC="start.S shim.c console.c mmu.c fdt.c loader.c emmc.c lib.c rs8.c pstore.c wdt.c sd_probe.c mmio_selftest.c"

# The static half of the MMIO allowlist: a raw store on a device window
# would bypass the protection that keeps a write to a dead controller from
# hanging the phone (see shim/mmio.h). It fails the build.
python3 ../tools/check-mmio-writes.py || {
	echo "error: the MMIO allowlist has a hole (see above)" >&2
	exit 1
}

mkdir -p out
for f in $SRC; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done
# The image runs wherever LK puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$($READELF -r out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/emmc.o out/lib.o out/rs8.o out/pstore.o out/wdt.o out/sd_probe.o out/mmio_selftest.o 2>/dev/null | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/emmc.o out/lib.o out/rs8.o out/pstore.o out/wdt.o out/sd_probe.o out/mmio_selftest.o -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# LK copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
# (BSD awk has no strtonum, so do the hex conversion in the shell.)
size=$($NM out/shim.elf | awk '$3=="_end"{print $1}')
size=$((0x$size))
truncate -s "$size" out/Image

# Ramdisk: an xnu payload (tools/mkpayload.py) if given as $1, else an
# empty newc cpio (LK dislikes a zero-size ramdisk on some builds).
if [ -n "${1:-}" ]; then
	cp "$1" out/ramdisk.bin
else
	(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
fi

# Wrap it the way the G4's LK wants to read it.
if [ -n "$DTB" ]; then
	python3 ../tools/mkbootimg_lg.py --kernel out/Image --ramdisk out/ramdisk.bin \
		--dtb "$DTB" --out out/shim-boot.img
else
	python3 ../tools/mkbootimg_lg.py --kernel out/Image --ramdisk out/ramdisk.bin \
		--dt-img "$DT_IMG" --out out/shim-boot.img
fi

# Host render of the console layout.
gcc -O1 -Wall -o out/host_test console.c host_test.c && ./out/host_test
ls -l out/Image out/shim-boot.img out/host.ppm
