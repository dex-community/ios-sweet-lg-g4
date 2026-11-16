#!/bin/bash
# auto-display-test.sh -- Automated test of the display bring-up shim.
#
# 1. If phone is in recovery (ADB), reboots to bootloader automatically.
# 2. Boots the shim with `fastboot boot shim/out/shim-boot.img` (NEVER flash).
# 3. The shim runs for 20s, then reboots to TWRP. WHAT IT PAINTS depends on
#    DISPLAY_MODE (mode 0 paints nothing by design, mode 4 talks straight
#    to the panel GRAM over DCS). The script reads the mode from
#    out/run-manifest.txt and prints the right expectation, so the console
#    never claims HELLO was painted in a baseline run. The progress is
#    always readable in the pstore log.
# 4. The shim automatically reboots into TWRP recovery.
# 5. This script waits for TWRP ADB, pulls the pstore log, and prints results.
#    It also prints out/run-manifest.txt so the run is attributable.
set -euo pipefail
cd "$(dirname "$0")/.."

SHIM_IMG="shim/out/shim-boot.img"
[ -f "$SHIM_IMG" ] || { echo "error: $SHIM_IMG not found. Run cd shim && ./build.sh first." >&2; exit 1; }

echo "== [0/4] Run manifest (copy this into the test report) =="
cat shim/out/run-manifest.txt 2>/dev/null || echo "(no manifest: rebuild with shim/build.sh)"

echo "== [1/4] Checking device connection =="
if adb devices 2>/dev/null | grep -qE "recovery$|device$"; then
    echo "[+] Phone detected in Recovery (ADB). Rebooting to Fastboot bootloader..."
    adb reboot bootloader || true
    sleep 3
fi

echo "Waiting for Fastboot (up to 30s)..."
for _ in $(seq 1 15); do
    if fastboot devices | grep -q .; then break; fi
    sleep 2
done

if ! fastboot devices | grep -q .; then
    echo "[!] Fastboot not detected. Put phone in Fastboot (Volume Down + USB)." >&2
    exit 1
fi
fastboot devices
echo "[+] Fastboot device ready!"

echo
echo "== [2/4] Booting shim image via 'fastboot boot' (non-destructive) =="
fastboot boot "$SHIM_IMG"

echo
echo "== [3/4] Shim is running! =="
MODE=$(sed -n 's/.* mode=\([0-9][0-9]*\) .*/\1/p' shim/out/run-manifest.txt 2>/dev/null | tail -1)
echo "Run mode from manifest: ${MODE:-unknown}"
case "${MODE:-unknown}" in
    0) echo "D0 BASELINE: the shim paints NOTHING. Expect the LG logo unchanged." ;;
    1) echo "D1: HELLO written into cont_splash, ZERO display MMIO." \
          "A visible change means LK still scans that buffer." ;;
    2) echo "D1b: core clocks + read-only BOOT dump + one DSI frame trigger." \
          "Read the BOOT lines in the pstore log below." ;;
    3) echo "D3: staged MDP bring-up. Watch the stage number on the panel." ;;
    4) echo "D4: HELLO pushed straight into panel GRAM via DCS, MDP untouched." \
          "Watch for HELLO and a red/green blinking square." ;;
    5) echo "D5: re-armed the LK pipeline (HS + TE select on both DSI links)." \
          "HELLO painted in BOTH cont_splash and panel GRAM; FR* lines count frames." ;;
    6) echo "D6 CONTACT PROBE: DCS 0x28 (display OFF) sent in HS mode." \
          "The screen must go BLACK for 20s if the panel obeys DCS." ;;
    7) echo "D7 CLEAN HELLO: HS DCS, 0x3A=0x77 (24bpp), HELLO at page 0." \
          "Watch for readable HELLO at the TOP of the screen." ;;
    8) echo "D8 BIG BARS: eight full-width colour bars in cont_splash." \
          "If bars appear the MDP path reads our buffer." ;;
    9) echo "D9 DCS ONLY: HELLO in panel GRAM, ZERO MDP frame kicks." \
          "Watch for readable HELLO at the TOP of the screen." ;;
    10) echo "D10 FULL GRAM: whole panel should turn SOLID RED." \
          "No text, no MDP kicks." ;;
    11) echo "D11 CONSOLE: HELLO + stage + label + blinking top bar." \
          "Written straight into panel GRAM at 720 columns." ;;
    *) echo "Unknown display mode: check shim/out/run-manifest.txt." ;;
esac
echo "Then the phone will automatically reset into TWRP recovery."
echo "Waiting for ADB connection from TWRP recovery (up to 120s)..."

for i in $(seq 1 60); do
    if adb devices 2>/dev/null | grep -qE "recovery$|device$"; then
        echo
        echo "[+] TWRP Recovery detected on ADB!"
        break
    fi
    printf "."
    sleep 2
done
echo

if ! adb devices 2>/dev/null | grep -qE "recovery$|device$"; then
    echo "[!] ADB timeout. Phone may require manual check."
    adb devices || true
    fastboot devices || true
    exit 1
fi

echo
echo "== [4/4] Extracting diagnostic logs from pstore =="
PMSG="/tmp/g4-pmsg.bin"
CONSOLE="/tmp/g4-console.bin"
rm -f "$PMSG" "$CONSOLE"

if adb pull /sys/fs/pstore/pmsg-ramoops-0 "$PMSG" 2>/dev/null; then
    echo "=== PMSG LOG (The Shim's execution record) ==="
    python3 tools/read-pstore.py "$PMSG" || cat "$PMSG"
fi

if adb pull /sys/fs/pstore/console-ramoops-0 "$CONSOLE" 2>/dev/null; then
    echo "=== CONSOLE LOG ==="
    python3 tools/read-pstore.py "$CONSOLE" || cat "$CONSOLE"
fi

echo
echo "[+] Automated display test cycle completed successfully!"
