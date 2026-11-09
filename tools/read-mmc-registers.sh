#!/usr/bin/env python3
"""read-mmc-registers.sh -- read the MMC controllers' registers from the phone.

WHAT THIS IS
    A measurement done from the RECOVERY (or from any system with root and
    /dev/mem), not from the shim. The shim must never touch a window it has
    not already driven: on this SoC a write to a controller whose clock is
    off blocks the AXI bus and hangs the phone (it happened twice on
    2026-11-09). Here the phone runs a real kernel with real drivers, so
    reading the registers is safe, and the numbers describe a live system.

WHAT IT READS
    * the TLMM mux of the ten SDC pins (pinctrl@fd510000, the "sdc" node of
      the device tree: pins 0..3 are SDC1, 4..6 are SDC2, 7..9 are SDC3),
    * the two SDHCI hosts (SDCC1 at 0xf9824900, SDCC2 at 0xf98a4900),
    * the GCC clock registers of SDCC1 and SDCC2 (the RCG is the REAL card
      clock on MSM: SDHCI_QUIRK2_ALWAYS_USE_BASE_CLOCK, sdhci-msm.c:3489).

USAGE
    tools/read-mmc-registers.sh [outfile]

    Run it in the recovery (adb shell) or from a rooted shell. It only
    READS: it uses "dd if=/dev/mem", never a write.

NOTES
    * /dev/mem may answer ENXIO on this phone in Android (it does); the
      recovery has it.
    * The addresses are the ones in the phone's own device tree
      (msm8992-lg-h815-boot.dts: sdhci@f9824900, sdhci@f98a4900,
      pinctrl@fd510000, qcom,gcc@fc400000).
"""
set -euo pipefail

OUT=${1:-/sdcard/mmc-registers.txt}
HERE=$(cd "$(dirname "$0")" && pwd)

# The windows to read: name:base:size
WINDOWS="
TLMM:0xfd510000:0x4000
GCC:0xfc400000:0x2000
SDCC1_HC:0xf9824900:0x1a0
SDCC1_CORE:0xf9824000:0x800
SDCC2_HC:0xf98a4900:0x11c
SDCC2_CORE:0xf98a4000:0x800
"

echo "== the MMC controllers, read from the phone ==" > "$OUT"
{
	echo "date: $(date 2>/dev/null || echo unknown)"
	echo "uname: $(uname -a 2>/dev/null)"
	echo
	echo "== /proc/iomem (the controller windows as the kernel sees them) =="
	grep -i -E "sdhci|sdcc|f9824|f98a4|fd510000|fc400000" /proc/iomem 2>&1 || true
	echo
	echo "== the windows =="
	for w in $WINDOWS; do
		name=${w%%:*}
		rest=${w#*:}
		base=${rest%%:*}
		size=${rest##*:}
		skip=$(( base / 4096 ))
		count=$(( size / 4096 ))
		[ "$count" -lt 1 ] && count=1
		echo
		echo "-- $name base=$base size=$size (skip=$skip count=$count) --"
		if dd if=/dev/mem bs=4096 skip="$skip" count="$count" 2>/dev/null | od -A x -t x4 -v | head -200; then
			:
		else
			echo "(dd failed: no /dev/mem here?)"
		fi
	done
	echo
	echo "== the TLMM pins, decoded =="
	# pin n: CTL at base+n*8, MUX at base+n*8+4
	python3 - <<'PY' 2>&1 || true
import struct, subprocess
names = {0:"sdc1_clk",1:"sdc1_cmd",2:"sdc1_data",3:"sdc1_rclk",
         4:"sdc2_clk",5:"sdc2_cmd",6:"sdc2_data",
         7:"sdc3_clk",8:"sdc3_cmd",9:"sdc3_data"}
try:
    raw = subprocess.run(["dd","if=/dev/mem","bs=4096","skip=0xfd510","count=1"],
                         capture_output=True).stdout
except Exception as e:
    print("dd failed:", e); raise SystemExit
if len(raw) < 64:
    print("/dev/mem not readable here"); raise SystemExit
print("%-4s %-10s %-10s %-10s %s" % ("pin","name","CTL","MUX","mux function"))
for n in range(10):
    ctl, mux = struct.unpack_from("<II", raw, n*8)
    print("%-4d %-10s 0x%08x 0x%08x %d" % (n, names.get(n,"?"), ctl, mux, mux & 7))
PY
	echo
	echo "== the GCC clock of the two SD slots =="
	echo "SDCC1: RCG 0xfc4004d0 (cmd) 0x4d4 (cfg) 0x4d8 (M) 0x4dc (N) 0x4e0 (D)"
	echo "SDCC2: RCG 0xfc400510 (cmd) 0x514 (cfg) 0x518 (M) 0x51c (N) 0x520 (D)"
	echo
	echo "(the raw dumps above contain them)"
} >> "$OUT" 2>&1

echo "written: $OUT"
cat "$OUT" | head -60
