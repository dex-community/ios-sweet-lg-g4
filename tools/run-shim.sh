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
print(f"elapsed from fastboot reboot : {t1 - t0:.1f} s")
if tf:
    e = t1 - tf
    print(f"elapsed from the bootloader : {e:.1f} s")
    # The shim waits until stage * 20 s (+ 3 s per unit of the raw command
    # error) after it started; the bootloader itself takes about 1 s.
    print("stage / error candidates:")
    for stage in range(0, 13):
        for err in range(0, 14):
            total = 1 + stage * 20 + err * 3
            if abs(total - e) < 2.5:
                print(f"  stage {stage:2d}, command error -{err} -> {total} s")
EOF
