#!/bin/bash
# Flash the shim on the phone, let it run, and report how long it stayed up.
#
#   tools/run-shim.sh              # flash shim/out/shim-boot.img and time it
#
# The shim asks the bootloader to come up in recovery, so the phone shows up
# in "adb devices" as "recovery" when it is done. The elapsed time carries
# the stage of the eMMC bring-up (stage * 10 seconds after the shim
# started), which is how the stage reaches the computer when the card
# cannot be written to.
set -euo pipefail
cd "$(dirname "$0")/.."

IMG=${IMG:-shim/out/shim-boot.img}

echo "== rebooting into the bootloader =="
adb reboot bootloader || true
for _ in $(seq 1 30); do
	fastboot devices | grep -q . && break
	sleep 1
done
fastboot devices

echo "== flashing $IMG =="
fastboot flash boot "$IMG"
fastboot reboot

t0=$(python3 -c 'import time; print(time.time())')
echo "== waiting for the phone to come back in recovery =="
# The phone leaves fastboot as soon as the bootloader takes over. That is
# the reference point: the elapsed time from there to the recovery is the
# bootloader's own boot time plus the shim's deadline (stage * 10 s).
t_fastboot_exit=""
found=""
while :; do
	now=$(python3 -c 'import time; print(time.time())')
	if adb devices | grep -q recovery; then
		found=yes
		t1=$now
		break
	fi
	if [ -z "$t_fastboot_exit" ] && ! fastboot devices | grep -q .; then
		t_fastboot_exit=$now
	fi
	sleep 0.2
done

if [ -z "$found" ]; then
	echo "TIMEOUT: the phone did not come back in recovery"
	exit 1
fi

python3 - "$t0" "$t_fastboot_exit" "$t1" <<'EOF'
import sys
t0, tf, t1 = (float(x) if x else 0.0 for x in sys.argv[1:4])

EVENTS = [
    "ALL OK: the log was written to the card and read back",
    "the card answered to neither CMD5 (awake) nor CMD7 (select)",
    "CMD16 timed out",
    "CMD16: the controller reported an error",
    "CMD16: the card answered with a fatal R1",
    "the EXT_CSD read (CMD8) timed out",
    "the EXT_CSD read: the controller reported an error",
    "the EXT_CSD read failed in another way",
    "the sector write timed out",
    "the sector write: the controller reported an error",
    "the driver did not get that far (or failed in another way)",
    "the read back failed or did not match what was written",
    "the core has no 16K page granule: mmu_enable() refused",
    "mmu_enable() refused for another reason",
    "the MMU is on with the 16K page tables",
    "the controller does not answer at all (0xffffffff on the registers)",
    "the capability registers are empty",
    "the card clock is not enabled",
    "the internal clock never stabilised",
    "the command line is stuck busy",
    "the controller is fine but the card does not answer",
    "the card does not answer to the full identification (CMD0/1/2/3/7)",
    "the controller was gating the card clock (CORE_CLK_PWRSAVE)",
]

print(f"elapsed from fastboot reboot : {t1 - t0:.1f} s")
if tf:
    e = t1 - tf
    print(f"elapsed from the bootloader : {e:.1f} s")
    # The bootloader + the TWRP boot take a fixed amount of time; the rest
    # is what the shim waited, and the shim encodes its diagnostic event
    # in it: 40 s plus 20 s per event slot.
    #
    # BOOT_OVERHEAD was measured with two builds that forced the wait
    # (FORCE_WAIT_S=5 -> 40.2 s, FORCE_WAIT_S=60 -> 95.1 s: a
    # difference of 54.9 s for a difference of 55 s of wait). The boot
    # time jitters by a few seconds, which is why the event unit is 20 s.
    BOOT_OVERHEAD = 35.2
    wait = e - BOOT_OVERHEAD
    print(f"shim wait                   : {wait:.1f} s")
    if wait < 0:
        print("  (the shim waited less than the boot overhead: it crashed "
              "before the timing channel could encode anything)")
        raise SystemExit
    n = int(round(wait))
    if n >= 500:
        off = n - 500
        if off >= 100:
            off -= 100
            print(f"  -> CRASH on a secondary core: the fault was {off} x 32 "
                  f"bytes after shim_main (0x{off * 32:x})")
        else:
            print(f"  -> CRASH: the fault was {off} x 32 bytes after "
                  f"shim_main (0x{off * 32:x})")
    elif n >= 40:
        ev = int(round((n - 40) / 20.0))
        ev = max(0, min(ev, len(EVENTS) - 1))
        print(f"  -> event {ev}: {EVENTS[ev]}")
    else:
        print(f"  -> too short to decode ({n} s): the overhead estimate is "
              "off, or the shim died early")
EOF
