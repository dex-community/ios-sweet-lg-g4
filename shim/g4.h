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
