/*
 * display.h -- the display bring-up test of the shim (2026-11-15).
 *
 * WHAT THIS IS FOR
 * ----------------
 * The shim has no visible console. This module is the experiment that
 * tries to give it one: it does NOT jump to XNU, it paints HELLO and a
 * numeric stage on the panel and holds the picture until the watchdog
 * brings the phone back to recovery.
 *
 * It is deliberately a SEPARATE track from the XNU boot work. The missing
 * console is not the reason XNU does not start: the kernel dies inside
 * MSR SCTLR_EL1 because the Cortex-A53 has no 16 KiB granule
 * (NOTES.md, 2026-11-12; fix in xnu/patches/0085). See docs/INDEX.md.
 *
 * WHY A STAGE NUMBER, NOT JUST "HELLO"
 * ------------------------------------
 * A static HELLO says "the shim reached disp_draw()". It cannot say where
 * a later hang happened. A framebuffer console can: every step repaints
 * the screen with its own number BEFORE touching the hardware, and the
 * hold loop advances a heartbeat. If the CPU wedges on step 3, the panel
 * keeps showing "3"; on step 7, "7". That is exactly the on-screen
 * equivalent of the marker ring in RAM, and it is why this file paints
 * more than one word.
 *
 * It only works once scan-out works. So the console is the REWARD for
 * solving the display, not the tool used to solve it: the first runs are
 * the bisection below, and the progress is read from pstore as usual.
 *
 * THE ORDER IS THE POINT
 * ----------------------
 * A controller's clock goes ON before a single register of that
 * controller is written. A write to a window whose clock is off does not
 * complete: the transaction stays on the AXI bus and the CPU stops inside
 * the access (this happened twice, 2026-11-09). Modes 0 and 1 therefore
 * touch NO MMIO at all: they only write DRAM.
 */
#pragma once
#include <stdint.h>

/*
 * DISPLAY_MODE, one knob for the whole bisection.
 *
 *   0  baseline: no display reads, no display writes. Proves the LG logo
 *      is stable when the shim does nothing.
 *   1  minimal touch: bounded fill of the 12 MiB cont_splash window,
 *      HELLO + stage text, cache clean. ZERO MMIO. Answers: is LK still
 *      scanning that memory at handoff?
 *   2  D1 + core clocks + read-only BOOT dump + one DSI frame trigger.
 *      NOT zero-MMIO: the trigger would hang the bus with the clocks off.
 *   3  full staged bring-up: clocks -> PLL -> DSI -> MDP -> frame ->
 *      panel. Bisected by DISPLAY_STAGE.
 *   4  DCS DIRECT TO PANEL GRAM (added after D0/D1, 2026-11-15). No MDP,
 *      no CTL, no RGB/LM/INTF, no SMMU: DCS 0x2A/0x2B window + 0x2C
 *      pixels straight over the DSI links. Bisected by DISPLAY_STAGE:
 *      1 = core clocks + BOOT dump, 2 = + PLL + DCS probe, 3 = + HELLO.
 *   5  RE-ARM LK'S PIPELINE (2026-11-16, after the D4 BOOT dump showed
 *      RGB0/RGB1 still point at cont_splash and both DSI links are live).
 *      Paints BOTH cont_splash and the GRAM, forces HS mode on the DCS DMA
 *      engine, sets DSI_TRIG_CTRL BIT(31) (te_sel = the kernel's value
 *      0x80000004) on both links, and kicks MDP_SW_TRIGGER every 500 ms.
 *      ZERO MDP/CTL/RGB/LM/INTF register writes. FR* lines count frames.
 *   6  CONTACT PROBE: sends DCS 0x28 (display off) in HS mode and holds
 *      the panel off for 20 s. A BLACK screen proves the panel obeys DCS;
 *      a logo that stays proves the packets never leave the host.
 */
#ifndef SHIM_DISPLAY_MODE
#define SHIM_DISPLAY_MODE	1
#endif

/* Pixel format of the minimal-mode buffer: 24 = inherited RGB888 pipes,
 * 32 = ARGB8888. The full mode always uses 32 (RGB2 is programmed so). */
#ifndef DISP_FB_BPP
#define DISP_FB_BPP		24
#endif

/* The stage of the full bring-up (only meaningful with mode 3). */
#define DISP_STAGE_CLOCK	1	/* the MMSS clocks only            */
#define DISP_STAGE_PLL		2	/* + the DSI PHY PLL               */
#define DISP_STAGE_DSI		3	/* + the DSI controller and PHY    */
#define DISP_STAGE_MDP		4	/* + the MDP (CTL, RGB2, INTF2)    */
#define DISP_STAGE_DRAW		5	/* + the picture and the kick      */

#ifndef SHIM_DISPLAY_STAGE
#define SHIM_DISPLAY_STAGE	DISP_STAGE_DRAW
#endif

/* How long to watch CTL_FLUSH after a commit. This is informational:
 * pending flush bits are NORMAL on this SoC, see display.c. */
#ifndef DISP_FLUSH_POLL_MS
#define DISP_FLUSH_POLL_MS	200
#endif

/* Set by build.sh from a hash of the sources and the flags, so every
 * pstore record can be attributed to the exact binary that produced it. */
#ifndef G4_BUILD_ID
#define G4_BUILD_ID		0
#endif

/*
 * The result of the test, in the order the steps are run. The shim prints
 * it and the trail keeps it, so a run that hangs still leaves a record
 * of how far it got.
 */
#define DISP_OK_CLOCKS		0x01
#define DISP_OK_PLL		0x02
#define DISP_OK_DSI		0x04
#define DISP_OK_MDP		0x08
#define DISP_OK_DRAWN		0x10
#define DISP_OK_PANEL_ON	0x20
#define DISP_OK_BASELINE	0x40
#define DISP_OK_GRAM		0x80	/* mode 4: HELLO pushed via DCS 0x2C    */

/* Run the whole test. Returns the bitmap above. */
uint32_t display_test(void);

/* Kick one frame out (DSI software trigger on both links). */
void disp_mdp_kick(void);

/* Repaint the heartbeat line during the hold loop. A frozen number means
 * the CPU is wedged; a running number means the test is alive. */
void disp_screen_beat(unsigned beat);

/* The panel: sleep out and display on. */
int display_panel_on(void);
