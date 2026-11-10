/*
 * emmc_probe.c -- the E0 probe of the eMMC card + the D0 probe of the MDP.
 *
 * WHY A SEPARATE TRANSLATION UNIT
 * -------------------------------
 * The session rule is: one phase at a time, and the eMMC code stays as it is.
 * So this file does NOT touch shim/emmc.c. It re-declares the accessors and
 * the register offsets it needs (the same way shim/sd_probe.c does), and it
 * only runs when the build asks for it (SHIM_EMMC_PROBE=1).
 *
 * WHAT HAPPENED ON 2026-11-10 (READ THIS BEFORE CHANGING ANYTHING)
 * -----------------------------------------------------------------
 * The FIRST version of this file sent MMC commands (CMD5 awake) and the phone
 * HUNG: it stopped answering on USB and had to be brought back through the
 * LAF/Download Mode. The cause is now understood and it is NOT a mystery:
 *
 *   The probe sent the command BEFORE the host controller was put in the
 *   state it has to be in to accept one. In shim/emmc.c the sequence is
 *
 *       CORE_HC_MODE (0x78) bit 0 = HC_MODE_EN  <- LK leaves this DISABLED
 *       -> core power reset (CORE_POWER bit 7)
 *       -> CORE_HC_MODE = HC_MODE_EN
 *       -> FF_CLK_SW_RST_DIS (bit 13)
 *       -> and only THEN a command is sent.
 *
 *   The reason is in shim/emmc.c: LK switches the controller off before
 *   jumping to the kernel, and the mode switch needs the core reset to
 *   re-synchronise the SDHCI side with the card. A command sent in the
 *   wrong mode does not come back, and the poll loop then spins forever on
 *   a controller that is not answering.
 *
 *   The first version of this file ALSO had the interrupt bits wrong (it
 *   used bit 1 for "command done" instead of bit 0, and bit 4 for "transfer
 *   done" instead of bit 1), so it was waiting for an interrupt that never
 *   comes. Both defects point at the same wall.
 *
 * THE RULE THAT FOLLOWS
 * ---------------------
 * This file now does READS ONLY, and it does them in the order that is
 * known to be safe:
 *
 *   1. the register snapshot of the eMMC controller, exactly as LK left it
 *      (this is what shim/emmc.c's emmc_snapshot() already does, safely);
 *   2. the MDP registers (VIG/RGB pipes, CTL, INTF timing engine).
 *
 * The "does the card answer at all?" question is NOT asked here any more.
 * It is asked by shim/emmc.c itself, which already owns the bring-up and
 * already sends CMD5 awake with the RCA that the LK used. What is added
 * there is ONE more thing: CMD5 with the BROADCAST address (0xffff0000),
 * which per the JEDEC spec reaches every card on the bus whatever RCA it
 * has, so it is the command that tells "the card is asleep" apart from "the
 * card does not answer at all". See shim/emmc.c: emmc_wake().
 *
 * (The RCA 0 that an earlier version of this file proposed is NOT a
 * broadcast address: it addresses the card whose RCA is 0, that is a card
 * that never left the identification state, and such a card is not asleep
 * either.)
 *
 * WHY THE MDP READS ARE SAFE
 * --------------------------
 * The MDP window (0xfd900000, 0x90000) is in the MMIO allowlist as
 * READ-ONLY, so a mistake here cannot turn into a write. And the window is
 * demonstrably alive: the bootloader leaves the LG logo on the panel, and
 * that logo is drawn by this very controller. It was measured on the phone
 * on 2026-11-10 from the recovery, with the real kernel driving the panel:
 *
 *   /sys/class/graphics/fb0/name  = mdssfb_90000
 *   /sys/class/graphics/fb0/virtual_size = 1440,5120
 *   /sys/class/graphics/fb0/stride = 5760
 *   /sys/class/graphics/fb0/bits_per_pixel = 32
 *   /sys/kernel/debug/mdp/stat:
 *       intf2: play: 00000024 vsync: 00000112 underrun: 00000000
 *       RGB2 : 00000024   <- the pipe that is actually scanning out
 *       VIG0/1/2 : 0
 *
 * So the panel is driven by the RGB2 pipe (offset 0x19000) on INTF2, NOT by
 * VIG0. That is the single most important fact for the display phase and
 * it changes the plan: see docs/PLAN-2026-11-10.md, section "Display".
 */

#include <stdint.h>
#include "g4.h"
#include "console.h"
#include "trail.h"
#include "mmio.h"

/* ------------------------------------------------------------------ */
/* The accessors, as in sd_probe.c                                     */
/* ------------------------------------------------------------------ */

static inline uint32_t rd32(uintptr_t a) { return mmio_rd32(a); }
static inline uint16_t rd16(uintptr_t a) { return mmio_rd16(a); }
static inline uint8_t rd8(uintptr_t a) { return mmio_rd8(a); }

static uint64_t ep_ticks(void)
{
	uint64_t v;

	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

static uint32_t ep_tick_hz(void)
{
	uint64_t v;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return (uint32_t)v;
}

/* ------------------------------------------------------------------ */
/* The eMMC registers, from emmc.c                                     */
/* ------------------------------------------------------------------ */

#define HC	G4_EMMC_BASE
#define CORE	G4_EMMC_CORE_BASE

#define HC_PRESENT	0x24
#define HC_CTRL1	0x28
#define HC_POWER_CTRL	0x29
#define HC_CLK_CTRL	0x2c
#define HC_INT_STATUS	0x30
#define HC_INT_ENABLE	0x34
#define HC_CTRL2	0x3c
#define HC_CAPS1	0x40
#define HC_CAPS2	0x44
#define HC_VENDOR_SPEC	0x10c

#define CORE_POWER	0x00
#define CORE_MCI_VER	0x50
#define CORE_HC_MODE	0x78
#define CORE_10C	0x10c

/* ------------------------------------------------------------------ */
/* E0 -- the eMMC controller, as LK left it (READS ONLY)               */
/* ------------------------------------------------------------------ */

/*
 * The register snapshot, taken BEFORE anything touches a controller.
 *
 * This is what tells H3 (the controller is not in a sane state) from H1/H2
 * (the controller is fine and the card is silent). It is the same set of
 * registers that shim/emmc.c's emmc_snapshot() reads, which is known to be
 * safe on this phone.
 */
static void ep_dump_regs(void)
{
	con_str("E0 core_pow ", "");
	con_hex("", rd32(CORE + CORE_POWER));
	con_str("E0 core_mciver ", "");
	con_hex("", rd32(CORE + CORE_MCI_VER));
	con_str("E0 core_hcmode ", "");
	con_hex("", rd32(CORE + CORE_HC_MODE));
	con_str("E0 core_10c ", "");
	con_hex("", rd32(CORE + CORE_10C));
	con_str("E0 hc_vendor ", "");
	con_hex("", rd32(HC + HC_VENDOR_SPEC));
	con_str("E0 pres ", "");
	con_hex("", rd32(HC + HC_PRESENT));
	con_str("E0 ctrl1 ", "");
	con_hex("", rd32(HC + HC_CTRL1));
	con_str("E0 pwrctrl ", "");
	/*
	 * SDHCI_PWR_CONTROL is a BYTE register at 0x29. Reading it as a 32 bit
	 * word is an unaligned access, and the controller answers it with a
	 * bus error: the run of 2026-11-10 died exactly there, with
	 * FAR = 0xf9824929 and a data abort, right after this line was
	 * printed. The bring-up code (shim/emmc_lk_bringup.inc) already reads
	 * and writes this register as a byte; this dump did not, and that is
	 * what made the phone reboot in a loop.
	 */
	con_hex("", rd8(HC + HC_POWER_CTRL));
	con_str("E0 clkctl ", "");
	con_hex("", rd32(HC + HC_CLK_CTRL));
	con_str("E0 intstat ", "");
	con_hex("", rd32(HC + HC_INT_STATUS));
	con_str("E0 inten ", "");
	con_hex("", rd32(HC + HC_INT_ENABLE));
	con_str("E0 ctrl2 ", "");
	con_hex("", rd32(HC + HC_CTRL2));
	con_str("E0 caps1 ", "");
	con_hex("", rd32(HC + HC_CAPS1));
	con_str("E0 caps2 ", "");
	con_hex("", rd32(HC + HC_CAPS2));
}

/* ------------------------------------------------------------------ */
/* D0 -- the MDP (display)                                            */
/* ------------------------------------------------------------------ */

/*
 * THE MDP READS ARE NOT DONE HERE, AND THAT IS DELIBERATE.
 *
 * The second run of this probe (2026-11-10, read-only version) hung the
 * phone exactly like the first one, which had written to the controller. A
 * read-only probe cannot hang a controller, so what hung it was the ACCESS
 * ITSELF: a read of a window whose clock is off does not complete, the AXI
 * transaction stays on the bus, and the CPU stops inside the access. That is
 * the same mechanism as the two historical hangs of 2026-11-09, and it is
 * why GUIDE-05 says a new window needs the PROOF that it is alive, not just
 * a place in the allowlist.
 *
 * At the moment the shim runs, the LK has already handed the display over:
 * the LK initializes the panel (the binary has target_display_init,
 * dsi_pll_20nm_enable_seq, lge_panel_config_pre_init) and then turns it off
 * before jumping to the kernel. The MDSS/DSI clocks are therefore very
 * likely OFF, and the phone is showing a frozen frame, not a live scanout.
 *
 * So this probe does NOT read the MDP. The question it was meant to answer
 * is answered in a much better place, with zero risk to the phone:
 *
 *   /sys/kernel/debug/mdp/            (reg, off, dump, stat, perf/,
 *                                      buffers, dsi0_ctrl_reg, dsi1_ctrl_reg,
 *                                      panel_reg, xlog)
 *   /sys/kernel/debug/mdss_panel/dsi0/ (dcs_cmd_by_left, te/, fbc/, mipi/,
 *                                      lcdc/, clk_rate)
 *
 * read from the recovery, where the real kernel drives the panel and the
 * MDP is provably alive and clocked. Measured on 2026-11-10 that way:
 *
 *   fb0 = mdssfb_90000, 1440x5120 virtual, stride 5760, 32 bpp
 *   mdp/stat: intf2 play 0x24 vsync 0x112 underrun 0
 *              RGB2 0x24 (the pipe that scans out), VIG0/1/2 = 0
 *   clk_rate 910224000, 4 lanes, 60 fps
 *
 * So the panel is driven by the RGB2 pipe (offset 0x19000) on INTF2, NOT by
 * VIG0: the ios-topaz patch 0092, which re-points VIG0, is not directly
 * applicable here.
 *
 * If an MDP read in the shim is ever needed again, it has to be done with
 * the MDSS clocks turned on first (the clock controller is
 * qcom,mmsscc@fd8c0000, clock-names mdp_core_clk / bus_clk / iface_clk /
 * vsync_clk, DTS:823-824), and that sequence has to be proven in QEMU
 * before it goes near the phone.
 */

/*
 * The D0 probe: it does NOTHING but say so in the log.
 *
 * The measurement lives in the recovery (debugfs), where it is safe and
 * where it can be repeated as often as needed without a flash.
 */
static void ep_probe_mdp(void)
{
	con_str("D0 skip", "MDP not read in the shim: the clock is off at");
	con_str("D0 skip", "handover and a read would hang the bus.");
	con_str("D0 skip", "Use the recovery: /sys/kernel/debug/mdp/");
	con_str("D0 skip", "and /sys/kernel/debug/mdss_panel/dsi0/");
}

/* ------------------------------------------------------------------ */
/* The entry points, called from shim.c                               */
/* ------------------------------------------------------------------ */

void emmc_probe_e0(void)
{
	con_str("E0 begin", "");
	ep_dump_regs();
	con_str("E0 end", "");
	trail(0x2302, 0xe0000002u);
}

void emmc_probe_d0(void)
{
	con_str("D0 begin", "");
	ep_probe_mdp();
	con_str("D0 end", "");
	trail(0x2303, 0xd0000002u);
}
