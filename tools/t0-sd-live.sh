#!/bin/bash
# T0 — stato live di SDCC1 (eMMC) e SDCC2 (slot microSD) letto da Android.
#
# Non scrive nulla sul telefono: solo letture. Da usare DOPO che la microSD e'
# stata inserita e il telefono e' su Android.
#
#   tools/t0-sd-live.sh > research/raw-2026-11-09/t0-live.txt 2>&1
#
# Nota: alcuni file (/proc/iomem, debugfs) richiedono root; lo script tenta
# "adb root" e riporta l'esito, così il log dice cosa è stato letto e cosa no.
set -u
ADB=${ADB:-adb}

echo "===== adb root ====="
$ADB root 2>&1 | sed 's/\r$//'
sleep 1

run() {
	echo
	echo "===== $* ====="
	$ADB shell "$1" 2>&1 | sed 's/\r$//'
}

run 'date; uname -a; getprop sys.boot_completed; getprop ro.build.version.release'

run 'cat /proc/cmdline'

run 'ls -l /sys/class/mmc_host/'
run 'for h in /sys/class/mmc_host/mmc*; do echo "--- $h ---"; ls $h; done'

for h in mmc0 mmc1; do
	run "for f in name type caps clock power bus_width blocksize max_freq min_freq f_min f_max hs200 hs400 uhs signal_voltage non_removable sleep io_led; do if [ -e /sys/class/mmc_host/$h/\$f ]; then echo \"\$f = \$(cat /sys/class/mmc_host/$h/\$f)\"; fi; done"
done

run 'ls -l /sys/class/mmc_host/mmc1/'
run 'ls -l /sys/class/mmc_host/mmc1/mmc1:0001/ 2>&1'
run 'for f in name cid csd manfid oemid serial date hwrev fwrev type; do if [ -e "/sys/class/mmc_host/mmc1/mmc1:0001/$f" ]; then echo "$f = $(cat "/sys/class/mmc_host/mmc1/mmc1:0001/$f")"; fi; done'
run 'cat /sys/class/mmc_host/mmc1/mmc1:0001/uevent 2>&1'

run 'ls -l /dev/block/mmcblk* 2>&1'
run 'cat /proc/partitions'
run 'ls /sys/class/block/ | grep mmc'
run 'for b in /sys/class/block/mmcblk*; do echo "--- $b ---"; cat $b/size; readlink -f $b/device; done'

run 'ls /sys/class/regulator/ 2>&1'
run 'for r in /sys/class/regulator/*; do echo "--- $r ---"; for f in name uV min_uV max_uV state status enabled type; do if [ -e $r/$f ]; then echo "$f = $(cat $r/$f)"; fi; done; done'
run 'cat /sys/kernel/debug/regulator/regulator_summary 2>&1 | head -120'

run 'cat /sys/kernel/debug/gpio 2>&1 | head -80'
run 'ls -l /sys/class/gpio/ 2>&1'
run 'cat /sys/class/gpio/gpiochip*/label 2>&1; cat /sys/class/gpio/gpiochip*/ngpio 2>&1'

run 'ls /sys/kernel/debug/ 2>&1'
run 'cat /sys/kernel/debug/mmc1/ios 2>&1'
run 'ls -l /sys/kernel/debug/mmc1/ 2>&1'
run 'cat /sys/kernel/debug/mmc0/ios 2>&1'

run 'cat /proc/iomem 2>&1 | grep -i -E "sdhci|sdcc|mmc|0xf982|0xf983"'

run 'dmesg 2>&1 | grep -i -E "sdhci|sdcc|mmc|cd-gpios|card.detect|qcom.*mmc" | head -80'

run 'ls /sys/bus/platform/drivers/mmc_host/ 2>&1'
run 'ls /sys/bus/platform/drivers/regulator-rpm-smd/ 2>&1'
run 'cat /sys/kernel/debug/clk/clk_summary 2>&1 | grep -i -E "sdc|sdcc|mmc" | head -40'
run 'cat /sys/kernel/debug/clk/clk_summary 2>&1 | head -5'
