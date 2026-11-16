/*
 * clocks.c -- the clocks of the SoC, in one place.
 *
 * See clocks.h for the rule this module exists to enforce.
 *
 * EVERY FUNCTION HERE WAITS FOR THE CLOCK BEFORE IT RETURNS. A clock
 * that is "enabled" but not yet running is the worst case of all: the
 * first write to the controller behind it would hang the bus.
 */
#include <stdint.h>
#include "clocks.h"
#include "g4.h"
#include "mmio.h"
#include "console.h"
#include "trail.h"
#include "timer.h"

/* The GCC (the global clock controller). */
#define GCC_BASE	((uintptr_t)G4_GCC_BASE)

/* The MMSS clock controller. */
#define MMSS_BASE	((uintptr_t)G4_MMSS_CC_BASE)

/* A CBCR: bit 0 enables the branch, bit 31 says "it is off". */
#define CBCR_ENABLE_BIT	BIT(0)
#define CBCR_OFF_BIT	0x80000000UL

/* The MMSS branch status: the four bits [31:28] read zero or two when the
 * branch is on (drivers/clk/qcom/clock-local2.c: BRANCH_ON_VAL = 0,
 * BRANCH_NOC_FSM_ON_VAL = 2). */
#define CBCR_STATUS_MASK		0xf0000000UL
#define CBCR_STATUS_ON			0x00000000UL
#define CBCR_STATUS_NOC_FSM_ON		0x20000000UL

/* Qualcomm RCG2 register offsets and masks */
#define CMD_RCGR_OFF		0x00
#define CFG_RCGR_OFF		0x04
#define CMD_CONFIG_UPDATE_BIT	BIT(0)
#define CFG_SRC_SHIFT		8
#define CFG_SRC_MASK		0x7UL
#define CFG_DIV_SHIFT		0
#define CFG_DIV_MASK		0x1fUL

/* The trail codes of this module. */
#define TRAIL_GCC	0x2800
#define TRAIL_MMSS	0x2810
#define TRAIL_COUNT	0x2820

static unsigned g_clocks_on;

/*
 * Switch a GCC branch clock on.
 *
 * The LK leaves the SDCC1 clocks off (it turns them off before it
 * jumps), and the Linux driver turns them on before it touches the
 * controller. The status bit (31) clears when the clock runs.
 */
int clocks_gcc_branch(uint32_t off)
{
	uintptr_t reg = GCC_BASE + off;
	uint32_t v = mmio_rd32(reg);

	trail(TRAIL_GCC, off);
	if (!(v & CBCR_ENABLE_BIT)) {
		mmio_wr32(reg, v | CBCR_ENABLE_BIT);
		for (int i = 0; i < 1000; i++) {
			if (!(mmio_rd32(reg) & CBCR_OFF_BIT))
				break;
			timer_delay_us(10);
		}
	}
	v = mmio_rd32(reg);
	trail(TRAIL_GCC + 1, v);
	if (v & CBCR_OFF_BIT)
		return -1;
	g_clocks_on++;
	return 0;
}

/*
 * Switch an MMSS branch clock on and wait for it.
 *
 * The status field of an MMSS CBCR is [31:28] and it reads zero or two when
 * the branch is on (drivers/clk/qcom/clock-local2.c: BRANCH_ON_VAL = 0,
 * BRANCH_NOC_FSM_ON_VAL = 2).
 */
int clocks_mmss_branch(uint32_t off)
{
	uintptr_t reg = MMSS_BASE + off;
	uint32_t v = mmio_rd32(reg);

	trail(TRAIL_MMSS, off);
	if (!(v & CBCR_ENABLE_BIT)) {
		mmio_wr32(reg, v | CBCR_ENABLE_BIT);
		__asm__ volatile("dsb sy" ::: "memory");
	}
	for (int i = 0; i < 10000; i++) {
		v = mmio_rd32(reg);
		uint32_t st = v & CBCR_STATUS_MASK;
		if (st == CBCR_STATUS_ON || st == CBCR_STATUS_NOC_FSM_ON)
			break;
		timer_delay_us(10);
	}
	v = mmio_rd32(reg);
	trail(TRAIL_MMSS + 1, v);
	uint32_t st = v & CBCR_STATUS_MASK;
	if (st != CBCR_STATUS_ON && st != CBCR_STATUS_NOC_FSM_ON) {
		con_hex2("CLK BR FAIL", "off=", off, "val=", v);
		return -1;
	}
	g_clocks_on++;
	return 0;
}

/*
 * Switch an MMSS root oscillator source and divider.
 *
 * In Qualcomm RCG2 hardware:
 *   CMD_RCGR is at off + 0x0 (bit 0 = CONFIG_UPDATE_BIT)
 *   CFG_RCGR is at off + 0x4 (bits [4:0] = DIV, bits [10:8] = SRC_SEL)
 *
 * If src and div are both 0xffffffff, we do not touch the register:
 * LK or firmware already configured it.
 */
int clocks_mmss_root(uint32_t off, uint32_t src, uint32_t div)
{
	if (src == 0xffffffffu && div == 0xffffffffu)
		return 0;

	uintptr_t cmd_reg = MMSS_BASE + off + CMD_RCGR_OFF;
	uintptr_t cfg_reg = MMSS_BASE + off + CFG_RCGR_OFF;
	uint32_t cfg = mmio_rd32(cfg_reg);

	trail(TRAIL_MMSS + 2, off);
	if (src != 0xffffffffu) {
		cfg &= ~(CFG_SRC_MASK << CFG_SRC_SHIFT);
		cfg |= (src & CFG_SRC_MASK) << CFG_SRC_SHIFT;
	}
	if (div != 0xffffffffu) {
		cfg &= ~(CFG_DIV_MASK << CFG_DIV_SHIFT);
		cfg |= (div & CFG_DIV_MASK) << CFG_DIV_SHIFT;
	}
	mmio_wr32(cfg_reg, cfg);
	__asm__ volatile("dsb sy" ::: "memory");

	/* Trigger configuration update */
	uint32_t cmd = mmio_rd32(cmd_reg);
	cmd |= CMD_CONFIG_UPDATE_BIT;
	mmio_wr32(cmd_reg, cmd);
	__asm__ volatile("dsb sy" ::: "memory");

	/* Wait for configuration update to take effect (bit 0 clears) */
	for (int i = 0; i < 1000; i++) {
		if (!(mmio_rd32(cmd_reg) & CMD_CONFIG_UPDATE_BIT))
			break;
		timer_delay_us(1);
	}

	trail(TRAIL_MMSS + 3, mmio_rd32(cmd_reg));
	g_clocks_on++;
	return 0;
}

/*
 * The clocks of the eMMC.
 *
 * The LK turns the SDCC1 clocks off before it jumps, so they have to be
 * switched on before the controller is touched. The application clock
 * (the one that feeds the card) comes first: it is the one the card
 * sees.
 */
int clocks_sdcc1(void)
{
	if (clocks_gcc_branch(GCC_SDCC1_APPS_CBCR) < 0)
		return -1;
	if (clocks_gcc_branch(GCC_SDCC1_AHB_CBCR) < 0)
		return -1;
	return 0;
}

/*
 * The clocks of the display.
 *
 * The roots first (they carry the source and the divider), then the
 * branches. The order inside is not important for the bus (the roots
 * are not buses), but the whole sequence has to be over before the
 * display driver touches the DSI or the MDP.
 *
 * The pixel and the byte clocks come from the DSI PHY PLL (source 1):
 * the divider field is zero because the division happens in the PHY
 * (POSTDIV2) and in the clock tree (drivers/clk/qcom/clock-mmss-8992.c,
 * the byte0 and pclk0 frequency tables).
 */
int clocks_display_core(void)
{
	/* The roots whose source the kernel knows and this code does not:
	 * they keep it. */
	clocks_mmss_root(MMSS_RCGR_MDP, 0xffffffffu, 0xffffffffu);
	clocks_mmss_root(MMSS_RCGR_VSYNC, 0xffffffffu, 0xffffffffu);
	clocks_mmss_root(MMSS_RCGR_ESC0, 0xffffffffu, 0xffffffffu);
	clocks_mmss_root(MMSS_RCGR_ESC1, 0xffffffffu, 0xffffffffu);

	/* The core branches: AHB, AXI, MDP, ESC0/1, VSYNC. */
	if (clocks_mmss_branch(MMSS_CBCR_MDSS_AHB) < 0) {
		con_str("CLK", "AHB branch failed");
		return -1;
	}
	if (clocks_mmss_branch(MMSS_CBCR_MDSS_AXI) < 0) {
		con_str("CLK", "AXI branch failed");
		return -1;
	}
	if (clocks_mmss_branch(MMSS_CBCR_MDSS_MDP) < 0) {
		con_str("CLK", "MDP branch failed");
		return -1;
	}
	if (clocks_mmss_branch(MMSS_CBCR_MDSS_ESC0) < 0) {
		con_str("CLK", "ESC0 branch failed");
		return -1;
	}
	if (clocks_mmss_branch(MMSS_CBCR_MDSS_ESC1) < 0) {
		con_str("CLK", "ESC1 branch failed");
		return -1;
	}
	if (clocks_mmss_branch(MMSS_CBCR_MDSS_VSYNC) < 0) {
		con_str("CLK", "VSYNC branch failed");
		return -1;
	}

	trail(TRAIL_COUNT, g_clocks_on);
	return 0;
}

int clocks_display_pixel(void)
{
	/* The roots whose source is known (DSI0 PHY PLL). */
	clocks_mmss_root(MMSS_RCGR_PCLK0, MMSS_SRC_DSI0_PHY_PLL, 0);
	clocks_mmss_root(MMSS_RCGR_PCLK1, MMSS_SRC_DSI0_PHY_PLL, 0);
	clocks_mmss_root(MMSS_RCGR_BYTE0, MMSS_SRC_DSI0_PHY_PLL, 0);
	clocks_mmss_root(MMSS_RCGR_BYTE1, MMSS_SRC_DSI0_PHY_PLL, 0);

	/* Pixel and Byte branches: only enable when DSI PHY PLL is locked! */
	if (clocks_mmss_branch(MMSS_CBCR_MDSS_PCLK0) < 0) {
		con_str("CLK", "PCLK0 branch failed");
		return -1;
	}
	if (clocks_mmss_branch(MMSS_CBCR_MDSS_PCLK1) < 0) {
		con_str("CLK", "PCLK1 branch failed");
		return -1;
	}
	if (clocks_mmss_branch(MMSS_CBCR_MDSS_BYTE0) < 0) {
		con_str("CLK", "BYTE0 branch failed");
		return -1;
	}
	if (clocks_mmss_branch(MMSS_CBCR_MDSS_BYTE1) < 0) {
		con_str("CLK", "BYTE1 branch failed");
		return -1;
	}

	trail(TRAIL_COUNT, g_clocks_on);
	return 0;
}

int clocks_display(void)
{
	if (clocks_display_core() < 0)
		return -1;
	return clocks_display_pixel();
}

unsigned clocks_enabled_count(void)
{
	return g_clocks_on;
}
