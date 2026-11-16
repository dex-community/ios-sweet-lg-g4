/*
 * LG G4 (H815) hardware constants.
 *
 * Every value here is read from the phone's own device tree
 * (msm8992-lg-h815-boot.dts) or from the stock LK boot image
 * (aicp/unpacked_dt.img, a QCDT v3 table). The file:line references are to
 * "../lg g4/msm8992-lg-h815-boot.dts" unless stated otherwise.
 *
 * The G4 is an MSM8992 (Cortex-A53 + Cortex-A57, ARMv8.0), 3 GiB LPDDR3.
 * It is *not* the SM8150 the sweet shim was written for: every MMIO base in
 * the sweet shim had to be replaced.
 */
#pragma once

/* ---- Clocks, power, reset (shim/shim.c) ---- */

/* qcom,msm-imem@fe80f000, restart_reason@65c (DTS:3683, 3700) */
#define G4_IMEM_BASE		0xfe80f000UL
#define G4_RESTART_REASON	(G4_IMEM_BASE + 0x65c)

/*
 * LK's reboot reasons. The bootloader reads the restart reason register at
 * boot (platform/msm_shared/reboot.c: check_reboot_mode()) and picks the
 * mode from it:
 *
 *   if (reboot_mode == RECOVERY_MODE)  boot_into_recovery = 1;
 *   else if (reboot_mode == FASTBOOT_MODE) boot_into_fastboot = 1;
 *
 * The values are in platform/msm_shared/reboot.h, and the register is the
 * IMEM one above because the H815 is not an msm8994 (that one uses
 * 0xfe87f000 + 0x65c instead, RESTART_REASON_ADDR2).
 */
#define LK_RECOVERY_MODE	0x77665502U
#define LK_FASTBOOT_MODE	0x77665500U

/* qcom,pshold (DTS:3678) */
#define G4_PSHOLD		0xfc4ab000UL

/* qcom,wdt@f9017000 (DTS:3721). The apps watchdog is armed at boot; the
 * shim must disable it or it resets the phone mid-bring-up. */
#define G4_WDT_BASE		0xf9017000UL
#define G4_WDT_DISABLE		0x04	/* WDOG_HALT / disable bit at +0x04 */
#define G4_WDT_EN		0x08	/* enable register offset */

/* ---- Interrupt controller (shim/shim.c, tools/mkadt.py) ---- */

/* qcom,msm-qgic2: distributor @ 0xf9000000, redistributor @ 0xf9002000.
 * (DTS:3484-3497) */
#define G4_GICD_BASE		0xf9000000UL
#define G4_GICD_SIZE		0x1000UL
#define G4_GICR_BASE		0xf9002000UL
#define G4_GICR_SIZE		0x1000UL

/* arm,armv8-timer with 4 PPIs (2,3,4,1), flags 0xf08, 19.2 MHz (DTS:3497).
 * GICv3 PPI numbering: INTID = 16 + PPI.
 *   PPI 2 -> INTID 18 physical timer, PPI 3 -> INTID 19 virtual timer. */
#define G4_VTIMER_INTID		19
#define G4_PTIMER_INTID		18
#define G4_TIMEBASE		19200000

/* ---- CPU bring-up (shim/shim.c) ---- */

/* The H815 device tree has NO psci node: AOSP's h815 port carries the
 * comment "psci is broken" and deletes it, and the CPU nodes use
 * enable-method = "qcom,8994-arm-cortex-acc" (Qualcomm's SCF/ACC path,
 * not PSCI CPU_ON). Secondary cores are therefore NOT startable with the
 * sweet shim's smc(PSCI_CPU_ON). The AOSP h815 port reserves the ARM
 * spin-table handover slot at 0x06000000 instead. */
#define G4_SPIN_TABLE		0x06000000UL
#define G4_NR_CPUS		6	/* 4x A53 + 2x A57 */

/* ---- Display (shim/console.h) ---- */
/*
 * Panel "LGD INCELL 1440p Dual 0/1 cmd mode dsi panel":
 *   qcom,mdss-dsi-panel-width  = 0x2d0  (720 per DSI link)
 *   qcom,mdss-dsi-panel-height = 0xa00  (2560)
 *   qcom,mdss-dsi-bpp          = 0x18  (24 bpp, RGB888)
 *   qcom,mdss-dsi-panel-type   = "dsi_cmd_mode"   <-- COMMAND mode
 *
 * The command-mode detail matters: unlike sweet's video-mode panel, writing
 * into RAM does not make anything appear. The DSI controller has to be
 * programmed to send the buffer. The shim only writes into the region the
 * bootloader left configured; the real display driver is Phase 4.
 *
 * cont_splash_mem: 0x03400000, 0xc00000 (12 MiB) (DTS:13337).
 *   1440*2560*3 = 10.5 MiB fits; 1440*2560*4 = 14.1 MiB does not.
 */
#define G4_FB_BASE		0x20000000UL
#define G4_FB_SIZE		0xc00000UL
#define G4_FB_W			1440
#define G4_FB_H			2560

/* ---- Display hardware: the addresses the phone's own device tree gives ----
 *
 * Everything below comes from "../lg g4/msm8992-lg-h815-boot.dts" and from
 * the phone's own kernel tree ("../lg g4/G4-Titan-Kernel-titan_bpf2",
 * the msm-3.18 MDSS stack). The numbers are the ones the kernel itself
 * uses, so the shim programs the same registers the kernel does.
 *
 *   qcom,mmsscc@fd8c0000          the MMSS clock controller
 *   qcom,mdss_mdp@fd900000        the MDP
 *   qcom,mdss_dsi@fd994000        the DSI0 controller
 *   qcom,mdss_dsi_pll@fd994300    the DSI0 PHY PLL
 */

/* qcom,mmsscc@fd8c0000 (DTS:3871). The MDSS branch clocks (MDSS_AHB,
 * MDSS_MDP, MDSS_PCLK0/1, MDSS_BYTE0/1, MDSS_ESC0/1, MDSS_VSYNC) and their
 * root oscillators (MDP_CMD_RCGR, PCLK0_CMD_RCGR, BYTE0_CMD_RCGR,
 * ESC0_CMD_RCGR) live here: drivers/clk/qcom/clock-mmss-8992.c. */
#define G4_MMSS_CC_BASE		0xfd8c0000UL
#define G4_MMSS_CC_SIZE		0x4000UL

/* The global clock controller (GCC). The SDCC1 branch clocks (the eMMC)
 * live here: drivers/clk/qcom/clock-local2.c and the LK binary
 * (0x0f912cd8). */
#define G4_GCC_BASE		0xfc400000UL
#define G4_GCC_SIZE		0x2000UL

/* The root oscillators (the "CMD_RCGR" registers): bits [10:8] select the
 * source, bits [3:0] the divider, bit 0 enables the branch.
 * drivers/clk/qcom/clock-local2.c: rcg_set_src_div(). */
#define G4_RCGR_PCLK0		0x2000UL	/* PCLK0_CMD_RCGR  */
#define G4_RCGR_PCLK1		0x2020UL	/* PCLK1_CMD_RCGR  */
#define G4_RCGR_MDP		0x2040UL	/* MDP_CMD_RCGR    */
#define G4_RCGR_VSYNC		0x2080UL	/* VSYNC_CMD_RCGR  */
#define G4_RCGR_BYTE0		0x2120UL	/* BYTE0_CMD_RCGR  */
#define G4_RCGR_BYTE1		0x2140UL	/* BYTE1_CMD_RCGR  */
#define G4_RCGR_ESC0		0x2160UL	/* ESC0_CMD_RCGR   */
#define G4_RCGR_ESC1		0x2180UL	/* ESC1_CMD_RCGR   */

/* The branch clocks (the "CBCR" registers): bit 0 enables, bits [31:28] are
 * the status field that has to read back as 0 (branch on).
 * drivers/clk/qcom/clock-local2.c: branch_clk_enable(). */
#define G4_CBCR_MDSS_AHB	0x2308UL
#define G4_CBCR_MDSS_AXI	0x2310UL
#define G4_CBCR_MDSS_PCLK0	0x2314UL
#define G4_CBCR_MDSS_PCLK1	0x2318UL
#define G4_CBCR_MDSS_MDP	0x231cUL
#define G4_CBCR_MDSS_EXTPCLK	0x2324UL
#define G4_CBCR_MDSS_VSYNC	0x2328UL
#define G4_CBCR_MDSS_BYTE0	0x233cUL
#define G4_CBCR_MDSS_BYTE1	0x2340UL
#define G4_CBCR_MDSS_ESC0	0x2344UL
#define G4_CBCR_MDSS_ESC1	0x2348UL

/* qcom,mdss_mdp@fd900000 (DTS:759). The block offsets come from the same
 * device tree: qcom,mdss-ctl-off = <0x2000 ...>, qcom,mdss-pipe-rgb-off =
 * <0x15000 0x17000 0x19000>, qcom,mdss-intf-off = <0x6b000 ... 0x6c000>. */
#define G4_MDP_BASE		0xfd900000UL
#define G4_MDP_SIZE		0x90000UL
#define G4_MDP_CTL_OFF		0x2000UL	/* the CTL block  */
#define G4_MDP_RGB0_OFF		0x15000UL
#define G4_MDP_RGB1_OFF		0x17000UL
#define G4_MDP_RGB2_OFF		0x19000UL	/* the pipe that drives the panel */
#define G4_MDP_INTF1_OFF	0x6b000UL
#define G4_MDP_INTF2_OFF	0x6c000UL	/* the interface in command mode */

/* qcom,mdss_dsi@fd994000 (DTS:1623): reg = <0xfd994000 0x260 (dsi_ctrl)
 * 0xfd994500 0x280 (dsi_phy) 0xfd994780 0x30 (dsi_phy_regulator)
 * 0xfd828000 0x108 (mmss_misc_phys)>. */
#define G4_DSI0_CTRL_BASE	0xfd994000UL
#define G4_DSI0_CTRL_SIZE	0x260UL
#define G4_DSI0_PHY_BASE		0xfd994500UL
#define G4_DSI0_PHY_SIZE		0x280UL
#define G4_DSI0_PHYREG_BASE	0xfd994780UL
#define G4_DSI0_PHYREG_SIZE	0x30UL

/* qcom,mdss_dsi_pll@fd994300 (DTS:611): reg = <0xfd994300 0x500 (pll_base)
 * 0xfd8c2300 0x08 (gdsc_base) 0xfd994200 0x64 (dynamic_pll_base)>. */
#define G4_DSI0_PLL_BASE	0xfd994300UL
#define G4_DSI0_PLL_SIZE	0x500UL
#define G4_DSI0_PLL_GDSC	0xfd8c2300UL

/* The panel, from the live phone (docs/display-2026-11-10.txt) and from
 * the panel's own device tree node
 * (arch/arm64/boot/dts/lge/dsi-panel-jdi-nt36850-dualmipi0-qhd-cmd.dtsi):
 *
 *   Panel #0 720x2560p60, 4 data lanes per link, dual link, 24 bpp on the
 *   wire, the MDP buffer in RGBA8888 with a 5760 byte stride.
 *   clk_rate (the DSI bit clock) = 910224000.
 */
#define G4_PANEL_W		720	/* per DSI link */
#define G4_PANEL_H		2560
#define G4_PANEL_BPP		24	/* on the wire */
#define G4_DSI_CLK_RATE		910224000UL
/* The VCO rate the DSI PLL is programmed for (the DSI clock times the
 * fixed divider of the 20nm PHY: /2 for the byte clock, times the number
 * of lanes). */
#define G4_DSI_VCO_RATE		1820448000UL

/* The framebuffer the FULL display test (DISPLAY_MODE=3) uses: 0x20000000
 * is plain RAM in the 0x10000000-0x5fffffff bank, it is the address the
 * loader already hands to XNU as the video frame buffer (shim/loader.c:
 * ba->Video.v_baseAddr), and at the shim's entry nothing else uses it.
 *
 * The minimal-touch test instead writes cont_splash_mem (0x03400000,
 * 12 MiB). A historical note here claimed that window is XPU-protected
 * from EL1 and that a write takes a data abort: that was NOT confirmed at
 * handover. console.c has always painted its log there (con_str ->
 * put_str) and old/displayTest.md test 5 recorded no XPU fault. See
 * docs/INDEX.md §4. */
#define G4_DISP_FB_BASE		0x20000000UL
#define G4_DISP_FB_STRIDE	5760	/* 1440 * 4 (RGBA8888) */

/* ---- Storage (Phase 3) ---- */

/* sdhci@f9824900 = sdhc1/mmc0, the eMMC host (DTS:6392-6496).
 * Same controller address and IRQs as the Lumia 950 XL (also MSM8992),
 * which is why UniXNU-lumia950xl is a usable eMMC reference. */
#define G4_EMMC_BASE		0xf9824900UL
#define G4_EMMC_SIZE		0x1a0UL
#define G4_EMMC_CORE_BASE	0xf9824000UL
#define G4_EMMC_CORE_SIZE	0x800UL
#define G4_EMMC_HC_IRQ		123
#define G4_EMMC_PWR_IRQ		138

/* SDCC2 = the microSD slot (DTS:6426-6459, sdhci@f98a4900).
 * The LK never initializes it (no reference to sdc2 in the bootloader), so
 * everything about it has to be done by the shim. */
#define G4_SD_BASE		0xf98a4900UL
#define G4_SD_SIZE		0x11cUL
#define G4_SD_CORE_BASE		0xf98a4000UL
#define G4_SD_CORE_SIZE		0x800UL

/* ---- PMIC / buttons (Phase 4) ---- */

/* SPMI arbiter qcom,spmi@fc4c0000 (DTS:3989-4006): core 0xfc4cf000,
 * intr 0xfc4cb000, cnfg 0xfc4ca000, IRQ 190. */
#define G4_SPMI_BASE		0xfc4c0000UL
#define G4_SPMI_INTR		0xfc4cb000UL
#define G4_SPMI_CNFG		0xfc4ca000UL
#define G4_SPMI_IRQ		190

/* PM8994 @ SPMI address 0x00, PMI8994 @ 0x01 (DTS:4066-4075). */
#define G4_PMIC_SSID		0x00
#define G4_PMI_SSID		0x01
