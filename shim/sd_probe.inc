/*
 * sd_probe.inc -- the T2 measurement of the eMMC controller (SDCC1).
 *
 * WHAT THIS IS
 * ------------
 * A measurement, not a driver. It sends NO command to any card and it
 * writes NOTHING: every access is a read, and every read is inside the MMIO
 * allowlist (shim/mmio.h), so only the windows this shim has already driven
 * on this phone are touched.
 *
 * The questions:
 *
 *   Q1  What did the LK leave in the eMMC controller? In particular in
 *       HC+0x10c (vendor spec: CORE_CLK_PWRSAVE, CORE_HC_MCLK_SEL,
 *       CORE_IO_PAD_PWR_SWITCH) and in the GCC clock registers of SDCC1
 *       (the RCG is the REAL card clock on MSM: SDHCI_QUIRK2_ALWAYS_USE_BASE_CLOCK,
 *       sdhci-msm.c:3489).
 *   Q2  Does the software reset of the SDHCI block (the one the bring-up
 *       does) erase that state?
 *
 * The two are measured around the bring-up, not with a reset of our own:
 *
 *   sd_probe_before()   the state the LK left
 *   ... the bring-up ... (it resets the host, exactly as the LK does)
 *   sd_probe_after()    the state after that reset
 *
 * ---------------------------------------------------------------------------
 * HISTORY: the first two versions of this probe HUNG THE PHONE (2026-11-09).
 *
 * v1 wrote HC+0x2f = 1 (software reset) on SDCC2, a controller nobody had
 *    ever initialized: the write never completed, the AXI bus stalled, the
 *    CPU stopped inside the access, the watchdog was not armed and the
 *    phone stayed on the LG logo, unreachable, until it was recovered with
 *    the buttons.
 * v2 read the TLMM (the pin mux) at 0xfd510000, a window the shim had
 *    never touched: the same hang, for a different reason.
 *
 * Three lessons, all built into this file:
 *
 *   1. NEVER WRITE to a controller that has not been initialized, and never
 *      touch a window that has not been driven. The allowlist in
 *      shim/mmio.h now refuses both, at runtime, in the accessors.
 *   2. Every phase leaves a trail entry in IMEM before it touches
 *      anything, so even a hang says WHERE it hung (the trail survives the
 *      reset: tools/read-trail-imem.py).
 *   3. A hang must be SELF-LIMITING: a phase that hangs leaves its marker
 *      behind, and the next boot skips it instead of hanging again.
 *
 * The measurements that need the pin mux (TLMM) or the SDCC2 controller are
 * done from the recovery, where a real kernel is running: see
 * tools/read-mmc-registers.sh. Zero risk for the phone, and the numbers
 * are read from a live system instead of from a half-initialized bootloader
 * state.
 * ---------------------------------------------------------------------------
 */

/*
 * The phases. One build = one configuration (the rule of the session: the
 * variants of the eMMC sweep must not be concatenated in the same boot).
 *
 *   A  the eMMC controller as the LK left it, before the bring-up.
 *   B  the same registers after the bring-up (and after its reset).
 */
#ifndef SHIM_SD_PROBE_PHASE
#define SHIM_SD_PROBE_PHASE	(0x03u)	/* A | B */
#endif

#define SD_PH_A		(1u << 0)
#define SD_PH_B		(1u << 1)

/* The marker of a phase that hangs: written before it starts, so that the
 * next boot can skip it. */
#define SD_TRAIL_RISKY		0xdffeu

/* GCC registers (clock-gcc-8992.c:88-93; the 8994 table is identical). */
#ifndef GCC_SDCC1_APPS_CMD_RCGR
#define GCC_SDCC1_APPS_CMD_RCGR	0x4d0
#endif
#ifndef GCC_SDCC1_APPS_CBCR
#define GCC_SDCC1_APPS_CBCR	0x4c4
#endif
#ifndef GCC_SDCC1_AHB_CBCR
#define GCC_SDCC1_AHB_CBCR	0x4c8
#endif

/* The RCG layout (clock-local2.c:48-52): CMD_RCGR, then CFG_RCGR at +4,
 * M at +8, N at +0xc, D at +0x10. */
#define RCG_CFG(off)		((off) + 4)
#define RCG_M(off)		((off) + 8)
#define RCG_N(off)		((off) + 0xc)
#define RCG_D(off)		((off) + 0x10)

/* The bits we care about in the vendor spec register (sdhci-msm.c:96-105,
 * ipotesi(nome Linux)). */
#define VENDOR_CLK_PWRSAVE	(1u << 1)
#define VENDOR_HC_MCLK_SEL	(3u << 8)
#define VENDOR_HC_MCLK_SEL_DFLT	(2u << 8)
#define VENDOR_IO_PAD_PWR_SWITCH (1u << 16)
#define VENDOR_HC_SELECT_IN_EN	(1u << 18)
#define VENDOR_HC_SELECT_IN	(7u << 19)

/* The trail codes: 0xd000 | phase for "starting", 0xd100 | phase for
 * "finished", 0xd200 | step for "accessing register N". */
#define SD_TRAIL_START(ph)	trail(0xd000u | (ph), (uint32_t)(SHIM_SD_PROBE_PHASE))
#define SD_TRAIL_END(ph)	trail(0xd100u | (ph), sd_probe_step)
#define SD_TRAIL_REG(step)	trail(0xd200u | ((step) & 0xffu), 0)

/* ------------------------------------------------------------------ */
/* The read helpers                                                    */
/* ------------------------------------------------------------------ */

/*
 * A guarded read: the trail says which register was being read when the
 * run stopped.
 */
static uint32_t sd_probe_step;

#define SD_PROBE(tag)	do { sd_probe_step = (tag); SD_TRAIL_REG(tag); } while (0)

/* The registers of the host we want to see, with their offsets. */
static const uint16_t sd_hc_offsets[] = {
	0x04, 0x08, 0x0c, 0x0e, 0x10, 0x20, 0x24, 0x28, 0x29, 0x2c, 0x2f,
	0x30, 0x34, 0x38, 0x3c, 0x40, 0x44, 0x10c
};

/* The CORE registers of the host. */
static const uint16_t sd_core_offsets[] = {
	0x00, 0x50, 0x78, 0x100, 0x108, 0x10c, 0x110, 0x1b0, 0x1b4,
	0xdc, 0xe0, 0xe4, 0xe8
};

/* ------------------------------------------------------------------ */
/* GCC                                                                  */
/* ------------------------------------------------------------------ */

/*
 * The RCG of a clock: CMD_RCGR, CFG_RCGR, M, N, D. These five registers say
 * what frequency the clock really runs at. For SDCC1 this is the eMMC card
 * clock (the SDHCI divider is ignored on MSM).
 */
static void sd_dump_rcg(uint32_t off, const char *what)
{
	con_hex2("RCG", what, rd32(G4_GCC_BASE + off), "cfg=",
		 rd32(G4_GCC_BASE + RCG_CFG(off)));
	con_hex2("RCG", what, rd32(G4_GCC_BASE + RCG_M(off)), "n=",
		 rd32(G4_GCC_BASE + RCG_N(off)));
	con_hex2("RCG", what, rd32(G4_GCC_BASE + RCG_D(off)), "x=", 0);
}

/* ------------------------------------------------------------------ */
/* The read of the host                                                */
/* ------------------------------------------------------------------ */

/*
 * Read the whole eMMC controller. Nothing is written: the reads are inside
 * the MMIO allowlist and the window is alive (the LK has just read the boot
 * image from the card with it).
 */
static void sd_probe_read_host(const char *when)
{
	unsigned i;

	con_str("PROBE", when);
	SD_PROBE(1);
	con_hex2("EMMC", "core00=", rd32(CORE + CORE_POWER),
		 "core50=", rd32(CORE + CORE_MCI_VER));
	SD_PROBE(2);
	con_hex2("EMMC", "core78=", rd32(CORE + CORE_HC_MODE),
		 "core10c=", rd32(CORE + 0x10c));
	SD_PROBE(3);
	con_hex2("EMMC", "hc24=", rd32(HC + HC_PRESENT),
		 "hc28=", rd32(HC + HC_CTRL1));
	SD_PROBE(4);
	con_hex2("EMMC", "hc2c=", rd32(HC + HC_CLK_CTRL),
		 "hc29=", rd8(HC + HC_POWER_CTRL));
	SD_PROBE(5);
	con_hex2("EMMC", "hc30=", rd32(HC + HC_INT_STATUS),
		 "hc40=", rd32(HC + HC_CAPS1));
	SD_PROBE(6);
	con_hex2("EMMC", "hc3c=", rd32(HC + HC_CTRL2),
		 "hc10c=", rd32(HC + HC_VENDOR_SPEC));

	/* The whole HC window, in order: what the state really is. */
	for (i = 0; i < sizeof(sd_hc_offsets) / sizeof(sd_hc_offsets[0]); i++) {
		SD_PROBE(0x10 + i);
		con_hex2("HC", "off=", sd_hc_offsets[i],
			 "val=", rd32(HC + sd_hc_offsets[i]));
	}
	for (i = 0; i < sizeof(sd_core_offsets) / sizeof(sd_core_offsets[0]); i++) {
		SD_PROBE(0x40 + i);
		con_hex2("CORE", "off=", sd_core_offsets[i],
			 "val=", rd32(CORE + sd_core_offsets[i]));
	}

	/* The GCC side of SDCC1: the real card clock. */
	SD_PROBE(0x80);
	con_hex2("GCC", "sdc1ahb=", rd32(G4_GCC_BASE + GCC_SDCC1_AHB_CBCR),
		 "sdc1app=", rd32(G4_GCC_BASE + GCC_SDCC1_APPS_CBCR));
	SD_PROBE(0x81);
	sd_dump_rcg(GCC_SDCC1_APPS_CMD_RCGR, "sdc1");
}

/* ------------------------------------------------------------------ */
/* Entry points                                                        */
/* ------------------------------------------------------------------ */

/*
 * Before the bring-up: the state the LK left.
 */
void sd_probe_before(void)
{
	con_str("PROBE", "A emmc before (read only)");
#if SHIM_SD_PROBE_PHASE & SD_PH_A
	SD_TRAIL_START(SD_PH_A);
	sd_probe_read_host("A emmc before (read only)");
	SD_TRAIL_END(SD_PH_A);
#endif
}

/*
 * After the bring-up: the state after the software reset that the bring-up
 * does. No write of our own.
 */
void sd_probe_after(void)
{
	con_str("PROBE", "B emmc after (read only)");
#if SHIM_SD_PROBE_PHASE & SD_PH_B
	SD_TRAIL_START(SD_PH_B);
	sd_probe_read_host("B emmc after (read only)");
	SD_TRAIL_END(SD_PH_B);
#endif
}

/*
 * The self-limiting check: if a previous boot left the marker of a phase
 * that hangs, that phase is skipped. With the allowlist in place there is
 * no phase that can hang, but the guard stays: it is the belt to the
 * allowlist's braces.
 */
int sd_probe_should_run(unsigned phase)
{
	if (trail_seen(SD_TRAIL_RISKY)) {
		con_str("PROBE", "a previous boot hung: the probe is skipped");
		return 0;
	}
	trail(SD_TRAIL_RISKY, phase);
	return 1;
}
