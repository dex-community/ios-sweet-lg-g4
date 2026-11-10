#!/bin/bash
# Read the display registers of a phone that is running, and save them to one
# file. Read-only: debugfs only, no writes to the panel, no writes to storage.
#
#   tools/read-mdp-registers.sh [out.txt]
#
# The phone must have adb up (recovery is fine: TWRP's own kernel is driving
# the panel, so the MDP is clocked and every register reads a live value).
#
# The register blocks come from the phone's own device tree
# (msm8992-lg-h815-boot.dts: qcom,mdss-pipe-rgb-off = <0x15000 0x17000 0x19000>,
# qcom,mdss-ctl-off = <0x2000 ...>, qcom,mdss-intf-off = <0x6b000 ... 0x6c000>,
# qcom,mdss-mixer-intf-off = <0x45000 0x46000>, qcom,mdss-pingpong-off = ...).
#
# The debugfs files are the driver's own: <block>_off takes "offset count" in
# hex and selects the range, <block>_reg then dumps it.
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/docs/mdp-$(date +%Y%m%d-%H%M%S).txt}
ADB=${ADB:-adb}

if ! $ADB devices 2>/dev/null | grep -qE '(recovery|device)$'; then
	echo "no phone on adb" >&2
	exit 1
fi

# Every block the phone's DT lists, plus the DSI side.
BLOCKS="
2000 64 CTL0
19000 130 RGB2_SSPP
19200 30 RGB2_CROSSBAR
15000 130 RGB0_SSPP
17000 130 RGB1_SSPP
5000 130 VIG0_SSPP
7000 130 VIG1_SSPP
9000 130 VIG2_SSPP
45000 38c MIXER0
46000 38c MIXER1
6c000 254 INTF2
6b000 254 INTF1
71000 d0 PINGPONG0
71800 d0 PINGPONG1
1000 408 MDP_TOP
"

{
	echo "=== mdp registers $(date -u '+%Y-%m-%dT%H:%M:%SZ') ==="
	$ADB shell 'uname -a' 2>&1

	echo
	echo "=== debugfs ==="
	if ! $ADB shell 'test -d /d/mdp' 2>/dev/null; then
		$ADB shell 'mkdir -p /d; mount -t debugfs none /d' >/dev/null 2>&1
	fi
	$ADB shell 'mount | grep debugfs' 2>&1

	echo
	echo "=== high level state (/d/mdp/dump) ==="
	$ADB shell 'cat /d/mdp/dump' 2>&1 </dev/null

	echo
	echo "=== the panel debug root (/d/mdss_panel/dsi0) ==="
	for f in xres yres clk_rate mipi/frame_rate dynamic_fps \
		min_refresh_rate max_refresh_rate physical_width physical_height \
		te/tear_check_en te/te_start_pos te/te_refx100 te/te_sync_cfg_height \
		mipi/data_lane0 mipi/data_lane1 mipi/data_lane2 mipi/data_lane3 \
		lcdc/h_front_porch lcdc/h_back_porch lcdc/h_pulse_width \
		lcdc/v_front_porch lcdc/v_back_porch lcdc/v_pulse_width \
		lcdc/underflow_clr lcdc/border_clr \
		fbc/enable fbc/bpp mipi/t_clk_pre mipi/t_clk_post \
		panel_ack_disabled brightness_max bl_max bl_min; do
		printf '%-28s ' "$f"
		$ADB shell "cat /d/mdss_panel/dsi0/$f 2>/dev/null" 2>&1 </dev/null
	done

	echo
	echo "=== register blocks ==="
	# adb reads stdin: without </dev/null it eats the loop's own input.
	while read -r off cnt name; do
		[ -n "${name:-}" ] || continue
		echo
		echo "--- $name  off=0x$off cnt=0x$cnt"
		$ADB shell "echo $off $cnt > /d/mdp/off; cat /d/mdp/reg" 2>&1 </dev/null
	done <<< "$BLOCKS"

	echo
	echo "=== DSI controllers and PHY ==="
	for base in dsi0_ctrl dsi1_ctrl dsi0_phy dsi1_phy dsi0_phy_regulator; do
		echo
		echo "--- $base"
		$ADB shell "cat /d/mdp/${base}_off; cat /d/mdp/${base}_reg" 2>&1
	done

	echo
	echo "=== panel registers (DCS reads) ==="
	$ADB shell 'cat /d/mdp/panel_off; cat /d/mdp/panel_reg' 2>&1

	echo
	echo "=== counter of frame plays (twice, 2 s apart) ==="
	$ADB shell 'cat /d/mdp/stat' 2>&1
	sleep 2
	$ADB shell 'cat /d/mdp/stat' 2>&1
} | tee "$OUT"

echo
echo "saved: $OUT"
