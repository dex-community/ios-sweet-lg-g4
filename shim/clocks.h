/*
 * clocks.h -- the clocks of the SoC, in one place.
 *
 * THE RULE
 * --------
 * A write to a controller whose clock is off never completes: the
 * transaction stays on the AXI bus, the CPU stops inside the access and
 * the phone is bricked until it is reflashed. That happened twice on this
 * project (2026-11-09). The rule is therefore not a style choice:
 *
 *     THE CLOCK OF A CONTROLLER GOES ON BEFORE A SINGLE REGISTER OF
 *     THAT CONTROLLER IS WRITTEN.
 *
 * This module is the only place in the shim that switches a clock on, so
 * that the rule has exactly one implementation and every driver can be
 * checked against it.
 *
 * THE TWO CLOCK CONTROLLERS OF THIS SoC
 * -------------------------------------
 *
 *   GCC   0xfc400000  the global clock controller: the branch clocks of
 *                     the rest of the SoC (the SDCC1 of the eMMC, the
 *                     CPU, ...). A branch clock is a "CBCR" register: bit
 *                     0 enables it, bit 31 says that it is OFF while it
 *                     clears.
 *                     drivers/clk/qcom/clock-local2.c: branch_clk_enable().
 *
 *   MMSS  0xfd8c0000  the clock controller of the multimedia subsystem:
 *                     the MDSS branch clocks (AHB, AXI, MDP, PCLK, BYTE,
 *                     ESC, VSYNC) and their root oscillators ("CMD_RCGR").
 *                     drivers/clk/qcom/clock-mmss-8992.c: the offsets,
 *                     drivers/clk/qcom/clock-local2.c: rcg_set_src_div().
 *
 * The addresses come from the phone's own device tree
 * ("../lg g4/msm8992-lg-h815-boot.dts"): qcom,mmsscc@fd8c0000 and the
 * clock controller at 0xfc400000.
 */
#pragma once
#include <stdint.h>

/* ---- the GCC (0xfc400000) ---- */

/* The SDCC1 (the eMMC) branch clocks. */
#define GCC_SDCC1_APPS_CBCR	0x4c4
#define GCC_SDCC1_AHB_CBCR	0x4c8

/* ---- the MMSS clock controller (0xfd8c0000) ---- */

/* The root oscillators: the source and the divider of a branch. */
#define MMSS_RCGR_PCLK0		0x2000
#define MMSS_RCGR_PCLK1		0x2020
#define MMSS_RCGR_MDP		0x2040
#define MMSS_RCGR_VSYNC		0x2080
#define MMSS_RCGR_BYTE0		0x2120
#define MMSS_RCGR_BYTE1		0x2140
#define MMSS_RCGR_ESC0		0x2160
#define MMSS_RCGR_ESC1		0x2180

/* The branch clocks. */
#define MMSS_CBCR_MDSS_AHB	0x2308
#define MMSS_CBCR_MDSS_AXI	0x2310
#define MMSS_CBCR_MDSS_PCLK0	0x2314
#define MMSS_CBCR_MDSS_PCLK1	0x2318
#define MMSS_CBCR_MDSS_MDP	0x231c
#define MMSS_CBCR_MDSS_EXTPCLK	0x2324
#define MMSS_CBCR_MDSS_VSYNC	0x2328
#define MMSS_CBCR_MDSS_BYTE0	0x233c
#define MMSS_CBCR_MDSS_BYTE1	0x2340
#define MMSS_CBCR_MDSS_ESC0	0x2344
#define MMSS_CBCR_MDSS_ESC1	0x2348

/* The source of the display clocks: the DSI PHY PLL (source 1 for DSI0,
 * 2 for DSI1). drivers/clk/qcom/clock-mmss-8992.c:
 * dsi0phypll_mm_source_val. */
#define MMSS_SRC_DSI0_PHY_PLL	1

/*
 * Switch one GCC branch clock on and wait for it.
 *
 * The status bit (31) of a CBCR says that the clock is OFF while it is
 * set: it clears when the clock runs. Returns 0 when the clock is on,
 * -1 when it did not come on in time.
 */
int clocks_gcc_branch(uint32_t off);

/*
 * Switch one MMSS branch clock on and wait for it.
 *
 * The status field of an MMSS CBCR is the four bits [31:28]: they read
 * zero when the branch is on (drivers/clk/qcom/clock-local2.c:
 * branch_clk_halt_check(), BRANCH_ON_VAL).
 */
int clocks_mmss_branch(uint32_t off);

/*
 * Switch one MMSS root oscillator on.
 *
 *   src  the source, in the field [10:8] (1 = the DSI PHY PLL)
 *   div  the divider, in the field [3:0]
 *
 * A source of 0xffffffff means "keep the one that is there": the kernel
 * knows where the clock of a module comes from, and writing a source
 * that is wrong takes the clock away from the module that needs it.
 */
int clocks_mmss_root(uint32_t off, uint32_t src, uint32_t div);

/* The clocks of the eMMC (the SDCC1): the AHB and the application
 * clocks. Used by the eMMC driver before it touches the controller. */
int clocks_sdcc1(void);

/*
 * The clocks of the display:
 * 1. clocks_display_core(): MDSS AHB/AXI, MDP, escape clocks and VSYNC.
 *    Must be enabled FIRST before touching DSI/MDP or DSI PLL registers.
 * 2. clocks_display_pixel(): PCLK0/1 and BYTE0/1.
 *    Must be enabled AFTER the DSI0 PHY PLL is locked, because the PLL
 *    is their root source clock!
 * 3. clocks_display(): runs both in sequence.
 */
int clocks_display_core(void);
int clocks_display_pixel(void);
int clocks_display(void);

/* How many clocks this module switched on: for the log. */
unsigned clocks_enabled_count(void);
