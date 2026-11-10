#!/usr/bin/env bash
# laf-tool.sh -- LG G4 (H815) Download Mode recovery utility using lglaf
#
# Use this when the phone is booted into Download Mode (hold Volume Up while inserting USB).
set -euo pipefail
cd "$(dirname "$0")/.."

PYTHON=".venv/bin/python3"
if [ ! -f "$PYTHON" ]; then
    echo "[!] Virtualenv not found. Setting up .venv..."
    python3 -m venv .venv
    .venv/bin/pip install pyusb cryptography
fi

CMD="${1:-help}"

case "$CMD" in
    list)
        echo "[*] Querying partition table via LAF (Download Mode)..."
        $PYTHON tools/lglaf/partitions.py --list
        ;;
    restore-boot)
        # THE VERIFIED IMAGE IS THE DEFAULT (2026-11-10).
        #
        # The old default preferred "kdz/extracted/boot.image", the boot
        # image of the ORIGINAL LG firmware (sha256 2267f77c...). That is
        # NOT the image this project works with: the phone boots the TWRP
        # 3.7 recovery from "boot" (backup/boot-stock.img, sha256
        # 070980b2...), and that is the image that has to come back after
        # every test. Restoring the KDZ one leaves the phone booting the
        # stock Android instead, with no ADB and no pstore.
        IMG="${2:-backup/boot-stock.img}"
        if [ ! -f "$IMG" ]; then
            IMG="kdz/extracted/boot.image"
        fi
        echo "[*] Restoring boot partition from $IMG..."
        $PYTHON tools/lglaf/partitions.py boot --restore "$IMG"
        echo "[+] Boot partition successfully restored!"
        ;;
    restore-recovery)
        IMG="${2:-kdz/extracted/recovery.image}"
        echo "[*] Restoring recovery partition from $IMG..."
        $PYTHON tools/lglaf/partitions.py recovery --restore "$IMG"
        echo "[+] Recovery partition successfully restored!"
        ;;
    reboot)
        echo "[*] Rebooting phone out of Download Mode..."
        $PYTHON tools/lglaf/lglaf.py -c "reboot" || true
        ;;
    reboot-recovery)
        echo "[*] Rebooting phone to recovery..."
        $PYTHON tools/lglaf/lglaf.py -c "reboot recovery" || true
        ;;
    shell)
        echo "[*] Opening LAF shell..."
        $PYTHON tools/lglaf/lglaf.py
        ;;
    *)
        echo "Usage: $0 {list|restore-boot [img]|restore-recovery [img]|reboot|reboot-recovery|shell}"
        echo ""
        echo "Instructions to enter Download Mode:"
        echo "  1. Unplug USB cable and remove/reinsert battery (or power off)."
        echo "  2. Hold the VOLUME UP button (the only working physical key)."
        echo "  3. While holding Volume Up, plug in the USB cable to your Mac."
        echo "  4. The phone will display the 'Firmware Update' / Download Mode screen."
        echo "  5. Run: $0 restore-boot"
        exit 1
        ;;
esac
