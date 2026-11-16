/*
 * display.c -- the display bring-up test of the LG G4 (MSM8992) shim.
 *
 * WHY
 * ---
 * The shim has no visible console. This module is the experiment that tries
 * to give it one: it does NOT jump to XNU, paints HELLO, a stage number and
 * a heartbeat, and holds the picture until the watchdog resets the phone.
 *
 * CORRECTION (2026-11-15): an older version of this comment claimed XNU
 * dies because LK leaves the MDSS/DSI clocks off. That hypothesis was
 * DISPROVEN. The kernel dies inside MSR SCTLR_EL1 because the Cortex-A53
 * has no 16 KiB translation granule (NOTES.md, 2026-11-12; fix in
 * xnu/patches/0085). The missing console is a symptom, not the cause.
 * This is a separate track: see docs/INDEX.md.
 *
 * The panel is DSI command mode, so refreshing the screen needs a kick
 * after every update. That is why the hold loop calls disp_screen_beat().
 *
 * THE GOLDEN RULE
 * ---------------
 * A write to a controller whose clock is off never completes: the
 * transaction stays on the AXI bus, the CPU stops inside the access, and
 * the phone is bricked until it is reflashed. That happened twice on this
 * project (2026-11-09). So the order here is not a style choice, it is the
 * difference between a test and a brick:
 *
 *   1. the MMSS clocks go on (this file touches NOTHING else before that);
 *   2. the DSI PHY PLL goes on;
 *   3. the DSI controller and the DSI PHY are programmed;
 *   4. the MDP is programmed (CTL, the RGB2 pipe, INTF2 in command mode);
 *   5. the picture is drawn and the CTL flush starts the scan-out.
 *
 * EVERY register write goes through mmio_wr32(), so the allowlist of
 * shim/mmio.h applies: the display windows are not writable until they
 * have been added there (they have been, on 2026-11-15).
 *
 * WHERE THE NUMBERS COME FROM
 * ---------------------------
 * Everything is taken from the phone itself:
 *
 *   - the addresses: the phone's own device tree,
 *     "../lg g4/msm8992-lg-h815-boot.dts" (nodes qcom,mmsscc@fd8c0000,
 *     qcom,mdss_mdp@fd900000, qcom,mdss_dsi@fd994000,
 *     qcom,mdss_dsi_pll@fd994300);
 *   - the register sequences: the kernel of this phone,
 *     "../lg g4/G4-Titan-Kernel-titan_bpf2" (msm-3.18 MDSS stack:
 *     drivers/video/msm/mdss/, drivers/clk/qcom/clock-mmss-8992.c,
 *     drivers/clk/qcom/mdss/mdss-dsi-20nm-pll-util.c);
 *   - the panel's DCS sequences: the panel's own device tree node,
 *     "arch/arm64/boot/dts/lge/dsi-panel-jdi-nt36850-dualmipi0-qhd-cmd.dtsi";
 *   - the live values: docs/display-2026-11-10.txt, the register dump
 *     taken from a phone that was actually driving the panel.
 *
 * THE REGISTER MAP OF THE DSI CONTROLLER
 * --------------------------------------
 * The kernel tree and the phone's own (older) kernel disagree about the
 * offsets of the DSI controller: the phone's map is the tree's map shifted
 * by four bytes, which is visible in the live dump (docs/display-2026-11-10.txt):
 *
 *   the tree (dsi_host_v2.h)        the live phone
 *   DSI_COMMAND_MODE_MDP_CTRL 0x3C  -> 0x40  (value 0x00000008)
 *   DSI_..._DCS_CMD_CTRL      0x40  -> 0x44  (value 0x00013C2C)
 *   DSI_DMA_CMD_OFFSET        0x44  -> 0x48  (a DMA address)
 *   DSI_DMA_CMD_LENGTH        0x48  -> 0x4C  (value 0x00000004)
 *   ..._STREAM0_CTRL          0x54  -> 0x58  (value 0x08710039)
 *   ..._STREAM0_TOTAL         0x58  -> 0x5C  (value 0x0A0002D0)
 *   DSI_TRIG_CTRL             0x80  -> 0x84  (value 0x80000004)
 *   DSI_CLKOUT_TIMING_CTRL    0xC0  -> 0xC4  (value 0x00002E2C)
 *
 * Every one of those values is the one the phone's kernel had when the
 * panel was working, and every one of them is what the msm-3.18 code
 * computes for this panel. So the shift is not a guess: it is measured.
 *
 * The default here is therefore the PHONE's map (DISP_DSI_MAP_SHIFT = 4).
 * Set DISP_DSI_MAP_SHIFT = 0 to use the tree's map instead.
 */
#include <stdint.h>
#include "g4.h"
#include "display.h"
#include "mmio.h"
#include "console.h"
#include "trail.h"
#include "pstore.h"
#include "clocks.h"
#include "timer.h"
#include "display-data.h"	/* single source of truth for all display facts */

/* The bit helper of the kernel (the shim has no linux headers). */
#ifndef BIT
#define BIT(n)		(1UL << (n))
#endif

/* The accessors of this module: every write goes through the allowlist
 * of shim/mmio.h. */
static uint32_t rd(uintptr_t a) { return mmio_rd32(a); }
static void wr(uintptr_t a, uint32_t v) { mmio_wr32(a, v); }

/* The delays come from the shared timebase (shim/timer.h): the counters
 * are switched on at the entry (shim_main), so every wait here ends. */
#define delay_us(us)	timer_delay_us(us)
#define delay_ms(ms)	timer_delay_ms(ms)

/*
 * A marker that survives the crash.
 *
 * The console log lives in RAM and it does not survive a hang (the shim
 * never reaches the end of the run, so the RAM copy is never written),
 * and the trail lives in the IMEM, which the recovery cannot read. The
 * ramoops is the one channel that survives everything: every step of the
 * bring-up therefore writes the log into it, so that a run that hangs
 * says exactly where it stopped.
 */
static inline void disp_mark(const char *msg)
{
	con_str("DISP", msg);
	pstore_console_write(con_log, (unsigned)con_log_len);
}

/* ------------------------------------------------------------------ */
/* The DSI PHY PLL (qcom,mdss_dsi_pll@fd994300)                        */
/* ------------------------------------------------------------------ */

/* The register offsets of the 20nm PHY PLL.
 * drivers/clk/qcom/mdss/mdss-dsi-20nm-pll-util.c:20-100. */
#define PLL_SYS_CLK_CTRL		0x0000
#define PLL_PLL_VCOTAIL_EN		0x0004
#define PLL_CMN_MODE			0x0008
#define PLL_IE_TRIM			0x000c
#define PLL_IP_TRIM			0x0010
#define PLL_PLL_PHSEL_CONTROL		0x0018
#define PLL_IPTAT_TRIM_VCCA_TX_SEL	0x001c
#define PLL_PLL_IP_SETI			0x0024
#define PLL_PLL_BKG_KVCO_CAL_EN		0x002c
#define PLL_PLL_CNTRL			0x0014
#define PLL_BIAS_EN_CLKBUFLR_EN		0x0030
#define PLL_PLL_CP_SETI			0x0034
#define PLL_PLL_IP_SETP			0x0038
#define PLL_PLL_CP_SETP			0x003c
#define PLL_SYSCLK_EN_SEL_TXBAND	0x0048
#define PLL_RESETSM_CNTRL		0x004c
#define PLL_RESETSM_CNTRL2		0x0050
#define PLL_RESETSM_CNTRL3		0x0054
#define PLL_DIV_REF1			0x0060
#define PLL_DIV_REF2			0x0064
#define PLL_KVCO_COUNT1			0x0068
#define PLL_KVCO_CAL_CNTRL		0x0070
#define PLL_KVCO_CODE			0x0074
#define PLL_VREF_CFG3			0x0080
#define PLL_PLLLOCK_CMP1		0x0090
#define PLL_PLLLOCK_CMP2		0x0094
#define PLL_PLLLOCK_CMP3		0x0098
#define PLL_PLL_VCO_TUNE			0x00a8
#define PLL_DEC_START1			0x00ac
#define PLL_SSC_EN_CENTER		0x00b4
#define PLL_FAUX_EN			0x00fc
#define PLL_DIV_FRAC_START1		0x0100
#define PLL_DIV_FRAC_START2		0x0104
#define PLL_DIV_FRAC_START3		0x0108
#define PLL_DEC_START2			0x010c
#define PLL_PLL_RXTXEPCLK_EN		0x0110
#define PLL_PLL_CRCTRL			0x0114
#define PLL_LOW_POWER_RO_CONTROL	0x013c
#define PLL_POST_DIVIDER_CONTROL	0x0140
#define PLL_RESET_SM			0x0150
#define PLL_PLLLOCK_CMP_EN		0x009c

/*
 * The divider of the byte clock, in the PHY: the VCO at 910.224 MHz is
 * divided by four there (POSTDIV2_CFG) and by two in the clock tree, so
 * the byte clock is 113.778 MHz, which is what the phone's kernel
 * programs (byte_clk_rate = clk_rate / 8 = 910224000 / 8).
 * drivers/clk/qcom/mdss/mdss-dsi-pll-util.c: fixed_4div_set_div().
 */
#define DSI_BYTE_DIV_PHY	4

#define PLL_BASE			((uintptr_t)G4_DSI0_PLL_BASE)
#define PLL1_BASE			((uintptr_t)0xfd996300UL)

static void pll_w(uint32_t off, uint32_t v)
{
	wr(PLL_BASE + off, v);
}

static void pll1_w(uint32_t off, uint32_t v)
{
	wr(PLL1_BASE + off, v);
}

/*
 * The VCO rate calculation.
 * drivers/clk/qcom/mdss/mdss-dsi-20nm-pll-util.c: pll_20nm_vco_rate_calc().
 *
 * The reference clock is the 19.2 MHz XO; the "duration" is 128 because
 * this panel's PLL node has "qcom,mdss-en-pll-90-phase" (the 90 degree
 * phase shift of the data lanes).
 */
struct vco_calc {
	uint32_t div_frac_start1, div_frac_start2, div_frac_start3;
	uint32_t dec_start1, dec_start2;
	uint32_t pll_plllock_cmp1, pll_plllock_cmp2, pll_plllock_cmp3;
};

static void vco_rate_calc(struct vco_calc *c, uint64_t vco_rate, uint64_t ref)
{
	const uint64_t multiplier = 1ULL << 20;
	uint64_t duration = 128;		/* the 90 degree phase */
	uint64_t dec_start_multiple, dec_start;
	uint64_t pll_comp_val;
	uint32_t div_frac_start;
	int div_frac_start1, div_frac_start2, div_frac_start3;
	int dec_start1, dec_start2;

	dec_start_multiple = (vco_rate * multiplier) / (2 * ref);
	div_frac_start = (uint32_t)(dec_start_multiple % multiplier);
	dec_start = dec_start_multiple / multiplier;

	dec_start1 = (int)((dec_start & 0x7f) | BIT(7));
	dec_start2 = (int)(((dec_start & 0x80) >> 7) | BIT(1));
	div_frac_start1 = (int)((div_frac_start & 0x7f) | BIT(7));
	div_frac_start2 = (int)(((div_frac_start >> 7) & 0x7f) | BIT(7));
	div_frac_start3 = (int)(((div_frac_start >> 14) & 0x3f) | BIT(6));

	pll_comp_val = (dec_start_multiple * 2 * (duration - 1)) / (10 * multiplier);

	c->dec_start1 = (uint32_t)dec_start1;
	c->dec_start2 = (uint32_t)dec_start2;
	c->div_frac_start1 = (uint32_t)div_frac_start1;
	c->div_frac_start2 = (uint32_t)div_frac_start2;
	c->div_frac_start3 = (uint32_t)div_frac_start3;
	c->pll_plllock_cmp1 = (uint32_t)(pll_comp_val & 0xff);
	c->pll_plllock_cmp2 = (uint32_t)((pll_comp_val >> 8) & 0xff);
	c->pll_plllock_cmp3 = (uint32_t)((pll_comp_val >> 16) & 0xff);
}

/*
 * The DSI PHY PLL enable sequence, ported from
 * drivers/clk/qcom/mdss/mdss-dsi-20nm-pll-util.c:
 * pll_20nm_vco_enable_seq() and the functions it calls.
 */
/* Stage-gated helpers: with DISPLAY_STAGE=1 some of them are compiled out,
 * so they need the unused attribute to keep the bisection builds warning
 * free (the warning says nothing about the hardware). */
static int __attribute__((unused)) dsi_pll_enable(void)
{
	struct vco_calc calc;

	/* 1. Check if the PLL is ALREADY locked by LK bootloader continuous splash */
	uint32_t st = rd(PLL_BASE + PLL_RESET_SM);
	con_hex1("PLL", "init rsm=", st);
	if ((st & BIT(5)) && (st & BIT(6))) {
		con_str("PLL", "already locked from bootloader");
		return 0;
	}

	/* 2. Configure common block 1 on BOTH DSI1 (pll_1_base) and DSI0 (pll_base) */
	pll1_w(PLL_PLL_VCOTAIL_EN, 0x82);
	pll1_w(PLL_BIAS_EN_CLKBUFLR_EN, 0x2a);
	pll1_w(PLL_BIAS_EN_CLKBUFLR_EN, 0x2b);
	pll1_w(PLL_RESETSM_CNTRL3, 0x02);

	pll_w(PLL_PLL_VCOTAIL_EN, 0x82);
	pll_w(PLL_BIAS_EN_CLKBUFLR_EN, 0x2a);
	pll_w(PLL_BIAS_EN_CLKBUFLR_EN, 0x2b);
	pll_w(PLL_RESETSM_CNTRL3, 0x02);

	/* pll_20nm_config_common_block_2() */
	pll_w(PLL_SYS_CLK_CTRL, 0x40);
	pll_w(PLL_IE_TRIM, 0x0f);
	pll_w(PLL_IP_TRIM, 0x0f);
	pll_w(PLL_PLL_PHSEL_CONTROL, 0x08);
	pll_w(PLL_IPTAT_TRIM_VCCA_TX_SEL, 0x0e);
	pll_w(PLL_PLL_BKG_KVCO_CAL_EN, 0x08);
	pll_w(PLL_SYSCLK_EN_SEL_TXBAND, 0x4a);
	pll_w(PLL_DIV_REF1, 0x00);
	pll_w(PLL_DIV_REF2, 0x01);
	pll_w(PLL_PLL_CNTRL, 0x07);
	pll_w(PLL_KVCO_CAL_CNTRL, 0x1f);
	pll_w(PLL_KVCO_COUNT1, 0x8a);
	pll_w(PLL_VREF_CFG3, 0x10);
	pll_w(PLL_SSC_EN_CENTER, 0x00);
	pll_w(PLL_FAUX_EN, 0x0c);
	pll_w(PLL_PLL_RXTXEPCLK_EN, 0x0a);
	pll_w(PLL_LOW_POWER_RO_CONTROL, 0x0f);
	pll_w(PLL_CMN_MODE, 0x00);

	/* pll_20nm_config_loop_bw() */
	pll_w(PLL_PLL_IP_SETI, 0x03);
	pll_w(PLL_PLL_CP_SETI, 0x3f);
	pll_w(PLL_PLL_IP_SETP, 0x03);
	pll_w(PLL_PLL_CP_SETP, 0x1f);
	pll_w(PLL_PLL_CRCTRL, 0x77);

	/* The rate. */
	vco_rate_calc(&calc, G4_DSI_CLK_RATE, 19200000);

	pll_w(PLL_DIV_FRAC_START1, calc.div_frac_start1);
	pll_w(PLL_DIV_FRAC_START2, calc.div_frac_start2);
	pll_w(PLL_DIV_FRAC_START3, calc.div_frac_start3);
	pll_w(PLL_DEC_START1, calc.dec_start1);
	pll_w(PLL_DEC_START2, calc.dec_start2);
	pll_w(PLL_PLLLOCK_CMP1, calc.pll_plllock_cmp1);
	pll_w(PLL_PLLLOCK_CMP2, calc.pll_plllock_cmp2);
	pll_w(PLL_PLLLOCK_CMP3, calc.pll_plllock_cmp3);
	pll_w(PLL_PLLLOCK_CMP_EN, 0x0d);	/* the 90 degree phase */

	/* The byte clock divider: the VCO divided by four in the PHY. */
	pll_w(PLL_POST_DIVIDER_CONTROL, DSI_BYTE_DIV_PHY - 1);

	/*
	 * pll_20nm_vco_init_lock(): the auto-lock sequence.
	 * pll_20nm_config_resetsm() + pll_20nm_config_vco_start().
	 */
	pll_w(PLL_KVCO_CODE, 0x00);
	pll_w(PLL_PLL_VCO_TUNE, 0x00);
	pll_w(PLL_RESETSM_CNTRL, 0x24);
	pll_w(PLL_RESETSM_CNTRL2, 0x07);

	pll_w(PLL_PLL_VCOTAIL_EN, 0x03);
	pll_w(PLL_RESETSM_CNTRL3, 0x02);
	delay_us(10);
	pll_w(PLL_RESETSM_CNTRL3, 0x03);

	/* pll_20nm_is_pll_locked(): bit 5 and bit 6 of RESET_SM. */
	for (int i = 0; i < 2000; i++) {
		st = rd(PLL_BASE + PLL_RESET_SM);

		if ((st & BIT(5)) && (st & BIT(6)))
			return 0;
		delay_us(10);
	}
	con_hex1("PLL", "fail rsm=", st);
	return -1;
}

/* ------------------------------------------------------------------ */
/* The DSI controller (qcom,mdss_dsi@fd994000)                         */
/* ------------------------------------------------------------------ */

/*
 * The offsets of the DSI controller, as the phone's own kernel has them.
 * They are the offsets of the tree (dsi_host_v2.h) plus the shift of four
 * bytes that the live dump of this phone shows (see the header of this
 * file). Set DISP_DSI_MAP_SHIFT to 0 to use the tree's offsets.
 */
#ifndef DISP_DSI_MAP_SHIFT
#define DISP_DSI_MAP_SHIFT	4
#endif

/* Ask for the tear signal (the panel's TE) to trigger the frames. */
#ifndef DISP_DSI_TE
#define DISP_DSI_TE		0
#endif

#define DSI_OFF(reg)	((reg) + DISP_DSI_MAP_SHIFT)

#define DSI_CTRL			0x0000
/* 2026-11-16: registers read by the extended BOOT dump. The offsets are
 * the tree's (dsi_host_v2.h); DSI_OFF() adds the +4 of the phone's map. */
#define DSI_STATUS			0x0004
#define DSI_FIFO_STATUS			0x0008
#define DSI_ACK_ERR_STATUS		0x0064
#define DSI_RDBK_DATA0			0x0068
#define DSI_COMMAND_MODE_DMA_CTRL	0x0038
#define DSI_COMMAND_MODE_MDP_CTRL	0x003c
#define DSI_COMMAND_MODE_MDP_DCS_CMD_CTRL	0x0040
#define DSI_DMA_CMD_OFFSET		0x0044
#define DSI_DMA_CMD_LENGTH		0x0048
#define DSI_COMMAND_MODE_MDP_STREAM0_CTRL	0x0054
#define DSI_COMMAND_MODE_MDP_STREAM0_TOTAL	0x0058
#define DSI_COMMAND_MODE_MDP_STREAM1_CTRL	0x005c
#define DSI_COMMAND_MODE_MDP_STREAM1_TOTAL	0x0060
#define DSI_TRIG_CTRL			0x0080
#define DSI_CMD_MODE_DMA_SW_TRIGGER	0x008c
#define DSI_CMD_MODE_MDP_SW_TRIGGER	0x0090
#define DSI_LANE_CTRL			0x00a8
#define DSI_LANE_SWAP_CTRL		0x00ac
#define DSI_CLKOUT_TIMING_CTRL		0x00c0
#define DSI_EOT_PACKET_CTRL		0x00c8
#define DSI_ERR_INT_MASK0		0x0108
#define DSI_INT_CTRL			0x010c
#define DSI_SOFT_RESET			0x0114
#define DSI_CLK_CTRL			0x0118
#define DSI_PHY_SW_RESET		0x0128

/* The bits of DSI_CTRL (dsi_host_v2.h:174-176 and dsi_host_v2.c:455-508). */
#define DSI_EN			BIT(0)
#define DSI_VIDEO_MODE_EN	BIT(1)
#define DSI_CMD_MODE_EN	BIT(2)
#define DSI_LANE0_EN		BIT(4)
#define DSI_LANE1_EN		BIT(5)
#define DSI_LANE2_EN		BIT(6)
#define DSI_LANE3_EN		BIT(7)
#define DSI_CLK_EN		BIT(8)

/*
 * Two bits that turned out to matter (2026-11-16, read from the phone's
 * own kernel and panel DTS):
 *
 *   DSI_COMMAND_MODE_DMA_CTRL BIT(26) = LOW POWER tx mode.
 *     dsi_host_v2.c:470-472 sets 0x14000000 ("from frame buffer, low power
 *     mode") and dsi_set_tx_power_mode() at :513-525 clears BIT(26) for HS.
 *     mdss_dsi_host.c:417-437 does the same at real offset 0x3c. The panel
 *     DTS declares on-command-state="dsi_lp_mode" but
 *     off-command-state="dsi_hs_mode": LK leaves the DMA command engine in
 *     LP, so a 15 KiB 0x2C pixel packet cannot be delivered that way.
 *
 *   DSI_TRIG_CTRL BIT(31) = te_sel (mdss_dsi_host.c:369-370). The panel DTS
 *     has te-pin-select=<1>, so the WORKING kernel writes 0x80000004
 *     (old/displayTest.md:262-263). The BOOT dump of 16/11 reads
 *     0x00000004 on BOTH links: LK left the TE source deselected and the
 *     MDP frame path frozen. That is the one measured difference between
 *     "splash congelato" and "kernel che guida il pannello".
 */
#define DSI_DMA_CTRL_LP			BIT(26)
#define DSI_TRIG_TE_SEL			BIT(31)

/* The bits of DSI_STATUS that the driver polls (dsi_host_v2.c:343, 560). */
#define DSI_STATUS_CMD_ENGINE_BUSY	BIT(2)
#define DSI_STATUS_CMD_DMA_BUSY	BIT(1)

/* The DSI host header (mdss_dsi_cmd.h:21-69). */
#define DSI_HDR_LAST	BIT(31)
#define DSI_HDR_LONG_PKT	BIT(30)
#define DSI_HDR_BTA	BIT(29)
#define DSI_HDR_VC(vc)		(((vc) & 0x03) << 22)
#define DSI_HDR_DTYPE(t)	(((t) & 0x03f) << 16)
#define DSI_HDR_DATA2(d)	(((d) & 0x0ff) << 8)
#define DSI_HDR_DATA1(d)	((d) & 0x0ff)
#define DSI_HDR_WC(wc)		((wc) & 0x0ffff)

#define DTYPE_DCS_WRITE		0x05	/* short write, no parameter  */
#define DTYPE_DCS_WRITE1	0x15	/* short write, one parameter  */
#define DTYPE_DCS_LWRITE	0x39	/* long write                   */

/*
 * The DSI command buffer. It has to be in RAM that the DMA engine can
 * read: the shim's own memory is fine, and 0x50000000 (the scratch log
 * area) is a good place, because it is plain RAM in the lower bank.
 */
#define DSI_CMD_BUF		0x50010000UL
/*
 * 2026-11-16: 64 KiB, originally 256 bytes and then 16 KiB. A DCS long
 * packet carries its word count in a 16 bit field, so 65532 payload bytes
 * is the largest packet that fits. With 16 KiB a full 720x2560 RGB888
 * frame needed 366 separate DMA transactions and the panel visibly
 * painted itself from top to bottom over several seconds. At 64 KiB one
 * packet carries 30 whole rows, so the same frame needs 86 transactions.
 * The old 256 byte limit was never checked: dsi_dcs_write() would have
 * written past the buffer for any long command. The window
 * 0x50010000..0x50020000 is plain DRAM, 64 KiB above the scratch copy of
 * the pstore log at 0x50000000, and nothing else in the shim uses it.
 */
#define DSI_CMD_BUF_SIZE	0x10000UL
#define DSI_CMD_MAX_PAYLOAD	0x0fffcUL

#define DSI0_BASE		((uintptr_t)G4_DSI0_CTRL_BASE)
#define DSI1_BASE		((uintptr_t)0xfd996000UL)
#define DSI_BASE		DSI0_BASE

static void dsi0_w(uint32_t off, uint32_t v) { wr(DSI0_BASE + DSI_OFF(off), v); }
static uint32_t dsi0_r(uint32_t off) { return rd(DSI0_BASE + DSI_OFF(off)); }

static void dsi1_w(uint32_t off, uint32_t v) { wr(DSI1_BASE + DSI_OFF(off), v); }
static uint32_t dsi1_r(uint32_t off) { return rd(DSI1_BASE + DSI_OFF(off)); }

static void dsi_w(uint32_t off, uint32_t v) { dsi0_w(off, v); }
/* 2026-11-15: the DSI0-only dsi_r() alias is gone. dsi_dcs_write() used
 * it to poll CMD_DMA_DONE, so a completion on DSI0 was reported as "the
 * panel answered" even when DSI1 was dead. Completion is now read from
 * both links explicitly. dsi_w() stays for the DSI0 PHY reset only. */

static void dsi_both_w(uint32_t off, uint32_t v)
{
	dsi0_w(off, v);
	dsi1_w(off, v);
}

/*
 * The DSI PHY. It is NOT in the controller's aperture: the phone's own
 * device tree splits the block in three windows that are all parts of the
 * SAME block:
 *
 *   qcom,mdss_dsi_pll@fd994300  0xfd994300 0x500  "pll_base"
 *        the PHY PLL registers, at +0x000 .. +0x150 (the clock driver
 *        writes them: SYS_CLK_CTRL at +0x000, RESET_SM at +0x150);
 *   qcom,mdss_dsi@fd994000      0xfd994500 0x280  "dsi_phy"
 *        the PHY registers, at +0x200 .. +0x480 of the same block;
 *   qcom,mdss_dsi@fd994000      0xfd994780 0x30   "dsi_phy_regulator"
 *        the regulator and the strength, at +0x480 of the same block.
 *
 * So the base of the PHY is 0xfd994300 and the offsets of the kernel
 * (dsi_host_v2.h) are relative to it. That is the interpretation the
 * windows of the device tree support; DISP_DSI_PHY_BASE can move it if
 * the readback of disp_dump() says otherwise.
 */
#ifndef DISP_DSI_PHY_BASE
#define DISP_DSI_PHY_BASE	G4_DSI0_PLL_BASE
#endif

#define DSI_PHY_BASE		((uintptr_t)DISP_DSI_PHY_BASE)

/* The PHY is a block of its own (see the comment above DSI_PHY_BASE). */
static void phy_w(uint32_t off, uint32_t v)
{
	wr(DSI_PHY_BASE + off, v);
}

static uint32_t phy_r(uint32_t off)
{
	return rd(DSI_PHY_BASE + off);
}

#define DSI_PHY_STRENGTH_CTRL_0	0x0480
#define DSI_PHY_STRENGTH_CTRL_2	0x0488
#define DSI_PHY_CTRL_0		0x0470
#define DSI_PHY_CTRL_3		0x047c
#define DSI_PHY_LDO_CNTRL	0x04b0
#define DSI_PHY_REGULATOR_CTRL_0	0x0500
#define DSI_PHY_REGULATOR_CTRL_1	0x0504
#define DSI_PHY_REGULATOR_CTRL_2	0x0508
#define DSI_PHY_REGULATOR_CTRL_3	0x050c
#define DSI_PHY_REGULATOR_CTRL_4	0x0510
#define DSI_PHY_REGULATOR_CAL_PWR_CFG	0x0518
#define DSI_PHY_CAL_HW_TRIGGER		0x0528
#define DSI_PHY_CAL_SW_CFG2		0x0534
#define DSI_PHY_CAL_HW_CFG0		0x0538
#define DSI_PHY_CAL_HW_CFG1		0x053c
#define DSI_PHY_CAL_HW_CFG2		0x0540
#define DSI_PHY_CAL_HW_CFG3		0x0544
#define DSI_PHY_CAL_HW_CFG4		0x0548
#define DSI_PHY_REGULATOR_CAL_STATUS0	0x0550
#define DSI_PHY_TIMING_CTRL_0		0x0440
#define DSI_PHY_LANE_BASE		0x0300
#define DSI_PHY_LANE_STRIDE		0x0040

/*
 * The PHY values of this phone, from its own device tree
 * (msm8992-lg-h815-boot.dts:1638-1641):
 *
 *   qcom,platform-strength-ctrl = [77 06]
 *   qcom,platform-regulator-settings = [03 03 03 00 20 07 01]
 *   qcom,platform-lane-config = [02 a0 00 00 20 00 00 01 46
 *                                 02 a0 00 00 40 00 00 01 46
 *                                 02 a0 00 40 20 00 00 01 46
 *                                 02 a0 00 40 00 00 00 01 46
 *                                 00 a0 00 80 00 00 00 01 46]
 *
 * The lane block holds six bytes per lane: CFG0, CFG1, CFG2,
 * TEST_DATAPATH, TEST_STR0, TEST_STR1 (at +0, +4, +8, +12, +20, +24).
 */
static const uint8_t disp_lane_cfg[5][6] = {
	{0x02, 0xa0, 0x00, 0x00, 0x20, 0x00},
	{0x02, 0xa0, 0x00, 0x00, 0x40, 0x00},
	{0x02, 0xa0, 0x00, 0x40, 0x20, 0x00},
	{0x02, 0xa0, 0x00, 0x40, 0x00, 0x00},
	{0x00, 0xa0, 0x00, 0x80, 0x00, 0x00},
};
static const uint8_t disp_phy_timing[12] = {
	0xeb, 0x38, 0x26, 0x00, 0x6a, 0x6c, 0x2c, 0x3c, 0x2f, 0x03, 0x04, 0x00
};
static const uint8_t disp_phy_regulator[6] = {
	0x03, 0x03, 0x03, 0x00, 0x20, 0x07
};

static void __attribute__((unused)) dsi_phy_init(void)
{
	/* The PHY soft reset (dsi_io_v2.c:368-378). */
	dsi_w(DSI_PHY_SW_RESET, 0x0001);
	delay_us(1000);
	dsi_w(DSI_PHY_SW_RESET, 0x0000);
	delay_us(100);

	/* The strength (dsi_io_v2.c:248-253). */
	phy_w(DSI_PHY_STRENGTH_CTRL_0, 0x77);
	phy_w(DSI_PHY_STRENGTH_CTRL_2, 0x06);

	/* The PHY control (dsi_io_v2.c:255-260). */
	phy_w(DSI_PHY_CTRL_0, 0x5f);
	phy_w(DSI_PHY_CTRL_3, 0x10);

	/* The regulator (dsi_io_v2.c:262-274). */
	phy_w(DSI_PHY_LDO_CNTRL, 0x25);
	phy_w(DSI_PHY_REGULATOR_CTRL_0, disp_phy_regulator[0]);
	phy_w(DSI_PHY_REGULATOR_CTRL_1, disp_phy_regulator[1]);
	phy_w(DSI_PHY_REGULATOR_CTRL_2, disp_phy_regulator[2]);
	phy_w(DSI_PHY_REGULATOR_CTRL_3, disp_phy_regulator[3]);
	phy_w(DSI_PHY_REGULATOR_CTRL_4, disp_phy_regulator[4]);
	phy_w(DSI_PHY_REGULATOR_CAL_PWR_CFG, disp_phy_regulator[5]);

	/* The calibration (dsi_io_v2.c:276-302). */
	phy_w(DSI_PHY_CAL_SW_CFG2, 0x00);
	phy_w(DSI_PHY_CAL_HW_CFG1, 0x5a);
	phy_w(DSI_PHY_CAL_HW_CFG3, 0x10);
	phy_w(DSI_PHY_CAL_HW_CFG4, 0x01);
	phy_w(DSI_PHY_CAL_HW_CFG0, 0x01);
	phy_w(DSI_PHY_CAL_HW_TRIGGER, 0x01);
	delay_ms(5);
	phy_w(DSI_PHY_CAL_HW_TRIGGER, 0x00);
	for (int i = 0; i < 5000; i++) {
		if (!(phy_r(DSI_PHY_REGULATOR_CAL_STATUS0) & 0x10))
			break;
	}

	/* The lanes (dsi_io_v2.c:304-321). */
	for (int ln = 0; ln < 5; ln++) {
		uintptr_t off = DSI_PHY_BASE + DSI_PHY_LANE_BASE +
				ln * DSI_PHY_LANE_STRIDE;

		wr(off + 0, disp_lane_cfg[ln][0]);
		wr(off + 4, disp_lane_cfg[ln][1]);
		wr(off + 8, disp_lane_cfg[ln][2]);
		wr(off + 12, disp_lane_cfg[ln][3]);
		wr(off + 20, disp_lane_cfg[ln][4]);
		wr(off + 24, disp_lane_cfg[ln][5]);
	}

	/* The timing (dsi_io_v2.c:323-332). */
	for (int i = 0; i < 12; i++)
		phy_w(DSI_PHY_TIMING_CTRL_0 + 4 * i, disp_phy_timing[i]);
}

/*
 * The DSI controller in command mode, for this panel.
 * The values are the ones the phone's own kernel had when the panel was
 * working (docs/display-2026-11-10.txt) and the ones the msm-3.18 code
 * computes for this device tree node.
 */
static void __attribute__((unused)) dsi_ctrl_init_cmd_mode(void)
{
	uint32_t d;

	/* The soft reset (dsi_host_v2.c:528-548) on BOTH DSI0 and DSI1. */
	d = dsi0_r(DSI_CTRL) & ~DSI_EN;
	dsi0_w(DSI_CTRL, d);
	dsi0_w(DSI_CLK_CTRL, 0x23f);
	dsi0_w(DSI_SOFT_RESET, 0x01);
	dsi0_w(DSI_SOFT_RESET, 0x00);

	d = dsi1_r(DSI_CTRL) & ~DSI_EN;
	dsi1_w(DSI_CTRL, d);
	dsi1_w(DSI_CLK_CTRL, 0x23f);
	dsi1_w(DSI_SOFT_RESET, 0x01);
	dsi1_w(DSI_SOFT_RESET, 0x00);

	/* DSI_COMMAND_MODE_MDP_CTRL (dsi_host_v2.c:431-442):
	 * dst_format = RGB888 (8), no interleave, no rgb swap. */
	d = 0x00000008UL;
	dsi_both_w(DSI_COMMAND_MODE_MDP_CTRL, d);

	/* DSI_COMMAND_MODE_MDP_DCS_CMD_CTRL (dsi_host_v2.c:445-450):
	 * the DCS commands that write and continue a memory write. */
	d = ((0x3cUL & 0xff) << 8) | (0x2cUL & 0xff) | BIT(16);
	dsi_both_w(DSI_COMMAND_MODE_MDP_DCS_CMD_CTRL, d);

	/* DSI_COMMAND_MODE_DMA_CTRL: the data comes from the frame buffer
	 * and the power mode is low power. */
	dsi_both_w(DSI_COMMAND_MODE_DMA_CTRL, 0x14000000UL);

	/*
	 * DSI_TRIG_CTRL: no MDP trigger, software trigger for DMA.
	 * Matches phone's live dump: 0x80000004
	 */
	d = 0x04UL | BIT(31);
	dsi_both_w(DSI_TRIG_CTRL, d);

	/* The lane swap: none. */
	dsi_both_w(DSI_LANE_SWAP_CTRL, 0x00000000UL);

	/* The clock out timing: t_clk_post and t_clk_pre (0x2e and 0x2c). */
	d = ((0x2eUL & 0x3f) << 8) | (0x2cUL & 0x3f);
	dsi_both_w(DSI_CLKOUT_TIMING_CTRL, d);

	/* The end of the packet. */
	dsi_both_w(DSI_EOT_PACKET_CTRL, 0x00000000UL);

	/* Only the acknowledge errors may raise an interrupt. */
	dsi_both_w(DSI_ERR_INT_MASK0, 0x13ff3fe0UL);

	/* The clocks of the controller itself. */
	dsi_both_w(DSI_CLK_CTRL, 0x23f);

	/*
	 * The lanes, the command mode and the clock enable:
	 * Must match the live dump: 0x000001f5!
	 */
	uint32_t ctrl_val = DSI_CLK_EN | DSI_CMD_MODE_EN |
			    DSI_LANE0_EN | DSI_LANE1_EN | DSI_LANE2_EN | DSI_LANE3_EN |
			    DSI_EN;
	dsi_both_w(DSI_CTRL, ctrl_val);

	/* The geometry of the two streams (dsi_host_v2.c:1159-1180):
	 * the stride of a line is width * bpp + 1 = 720 * 3 + 1. */
	d = ((uint32_t)(G4_PANEL_W * 3 + 1) << 16) | (0UL << 8) | DTYPE_DCS_LWRITE;
	dsi_both_w(DSI_COMMAND_MODE_MDP_STREAM0_CTRL, d);
	dsi_both_w(DSI_COMMAND_MODE_MDP_STREAM1_CTRL, d);

	d = ((uint32_t)G4_PANEL_H << 16) | G4_PANEL_W;
	dsi_both_w(DSI_COMMAND_MODE_MDP_STREAM0_TOTAL, d);
	dsi_both_w(DSI_COMMAND_MODE_MDP_STREAM1_TOTAL, d);
}

/*
 * Send one MIPI DCS command through the DSI DMA engine.
 *
 * The sequence is the one of the kernel (dsi_host_v2.c:623-675,
 * mdss_dsi_cmd.c:226-333):
 *
 *   - the 4 byte host header and the payload go into a buffer in RAM;
 *   - DSI_DMA_CMD_OFFSET takes the address of the buffer;
 *   - DSI_DMA_CMD_LENGTH takes its length, aligned to 4;
 *   - DSI_CMD_MODE_DMA_SW_TRIGGER is written with 1;
 *   - the completion is the CMD_DMA_DONE bit of DSI_INT_CTRL.
 *
 * The buffer is in the scratch RAM (0x50010000), which is plain RAM in
 * the lower bank and is not used by anything else.
 */
/*
 * CMD_DMA_DONE (bit 0 of DSI_INT_CTRL) is write-one-to-clear. The kernel
 * clears it before every transfer and again in the timeout path
 * (mdss_dsi_host.c:1810-1816, dsi_host_v2.h:27). Without the clear, a
 * stale bit left behind by LK makes EVERY following command look
 * instantly complete, and the whole "is the DSI link alive?" diagnostic
 * is worthless. The mask bits [1],[9],[17],[21] are enables and must be
 * preserved: DSI_INTR_ALL_MASK comes from dsi_host_v2.h:28.
 */
#define DSI_INTR_ALL_MASK	0x02220202UL

static void dsi_dma_done_clear(void)
{
	uint32_t v0 = dsi0_r(DSI_INT_CTRL);
	uint32_t v1 = dsi1_r(DSI_INT_CTRL);

	dsi0_w(DSI_INT_CTRL, (v0 & DSI_INTR_ALL_MASK) | BIT(0));
	dsi1_w(DSI_INT_CTRL, (v1 & DSI_INTR_ALL_MASK) | BIT(0));
	__asm__ volatile("dsb sy" ::: "memory");
}

static int dsi_dcs_write(uint8_t cmd, const uint8_t *payload, unsigned len,
			 unsigned wait_ms)
{
	volatile uint8_t *buf = (volatile uint8_t *)DSI_CMD_BUF;
	uint32_t hdr;
	unsigned total;

	if (len == 0) {
		/* A short write with no parameter. */
		hdr = DSI_HDR_DTYPE(DTYPE_DCS_WRITE) | DSI_HDR_DATA1(cmd) |
		      DSI_HDR_VC(0) | DSI_HDR_LAST;
		buf[0] = hdr & 0xff;
		buf[1] = (hdr >> 8) & 0xff;
		buf[2] = (hdr >> 16) & 0xff;
		buf[3] = (hdr >> 24) & 0xff;
		total = 4;
	} else if (len == 1) {
		/* A short write with one parameter. */
		hdr = DSI_HDR_DTYPE(DTYPE_DCS_WRITE1) | DSI_HDR_DATA1(cmd) |
		      DSI_HDR_DATA2(payload[0]) | DSI_HDR_VC(0) | DSI_HDR_LAST;
		buf[0] = hdr & 0xff;
		buf[1] = (hdr >> 8) & 0xff;
		buf[2] = (hdr >> 16) & 0xff;
		buf[3] = (hdr >> 24) & 0xff;
		total = 4;
	} else {
		/* A long write: the word count counts the command byte too,
		 * and the payload is padded with 0xff. This is exactly
		 * mdss_dsi_generic_lwrite() / mdss_dsi_cmd.c:96-135. */
		unsigned n = len + 1;
		unsigned padded = (n + 3) & ~3u;

		/* Third latent bug: the 256 byte buffer was never bounds
		 * checked. A 0x2C pixel packet would have scribbled over
		 * whatever follows 0x50010000. */
		if (padded > DSI_CMD_MAX_PAYLOAD) {
			con_hex2("DCS", "payload too big cmd=", cmd, "len=", len);
			return -1;
		}
		hdr = DSI_HDR_WC(n) | DSI_HDR_LONG_PKT |
		      DSI_HDR_DTYPE(DTYPE_DCS_LWRITE) | DSI_HDR_VC(0) |
		      DSI_HDR_LAST;
		buf[0] = hdr & 0xff;
		buf[1] = (hdr >> 8) & 0xff;
		buf[2] = (hdr >> 16) & 0xff;
		buf[3] = (hdr >> 24) & 0xff;
		buf[4] = cmd;
		/* 2026-11-15: the old loop wrote payload[0..len-2] into
		 * buf[5..3+len] and skipped the LAST payload byte. Long DCS
		 * writes (0x2A/0x2B column/page address, 0x2C pixel data)
		 * were therefore corrupt. buf[4] holds the command, so the
		 * payload starts at buf[5]. */
		for (unsigned i = 0; i < len; i++)
			buf[5 + i] = payload ? payload[i] : 0;
		for (unsigned i = 1 + len; i < padded; i++)
			buf[4 + i] = 0xff;
		total = 4 + padded;
	}

	/* 2026-11-16 (alignment fix): the DMA command buffer is plain DRAM,
	 * not an MMIO window. Push the freshly built packet out of any data
	 * cache before the host reads it. Harmless with the MMU off, and it
	 * removes a content-dependent failure: a uniform solid colour hides
	 * a stale payload, text does not. */
	for (uintptr_t a = DSI_CMD_BUF; a < DSI_CMD_BUF + total; a += 64)
		__asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");

	/* Dual-DSI split panel: BOTH links must receive the command.
	 * The old code used dsi_w() (= DSI0 only), so only the left half
	 * of the panel ever saw sleep-out / display-on. The kernel does
	 * the same broadcast in mdss_dsi_cmd_dma_tx() (lines 1769-1781):
	 * the other controller gets the SAME dma_addr and its own trigger. */
	dsi_dma_done_clear();
	dsi_both_w(DSI_DMA_CMD_OFFSET, (uint32_t)DSI_CMD_BUF);
	dsi_both_w(DSI_DMA_CMD_LENGTH, total);
	__asm__ volatile("dsb sy" ::: "memory");
	dsi_both_w(DSI_CMD_MODE_DMA_SW_TRIGGER, 0x01);

	/* Wait for BOTH controllers. 2026-11-16: the old loop broke on
	 * DSI0 alone. Both links DMA from the SAME buffer; returning early
	 * let the next packet overwrite DSI_CMD_BUF while DSI1 was still
	 * streaming it. That produced the deterministic row shear: a solid
	 * red frame hid it, HELLO did not. 200 ms is DMA_TX_TIMEOUT. */
	unsigned waited = 0;
	uint32_t done0 = 0, done1 = 0;

	for (; waited < 200; waited++) {
		done0 = dsi0_r(DSI_INT_CTRL) & BIT(0);
		done1 = dsi1_r(DSI_INT_CTRL) & BIT(0);
		if (done0 && done1)
			break;
		g4_wdt_pet();
		delay_ms(1);
	}
	/* One line per short command, not one per 0x2C pixel packet: a full
	 * screen is ~200 packets and would bury the useful part of pstore.
	 * log the WAIT of the first three LONG packets too, so the cost of
	 * a 64 KiB transfer is measurable instead of guessed. */
	static unsigned long_logged;
	if (total == 4 || waited == 200 ||
	    (total > 4 && long_logged++ < 3))
		con_hex2("DCS", "wait_ms=", waited, "links=",
			 (done0 ? 1u : 0u) | (done1 ? 2u : 0u));
	if (waited == 200) {
		con_hex1("DCS", "timeout cmd=", cmd);
		/* A hang after this point is possible: flush the log now. */
		pstore_console_write(con_log, (unsigned)con_log_len);
		return -1;
	}

	if (wait_ms)
		delay_ms(wait_ms);
	return 0;
}

/*
 * The panel: the DCS commands that bring the JDI NT36850 up.
 *
 * The list comes from the panel's own device tree node
 * ("arch/arm64/boot/dts/lge/dsi-panel-jdi-nt36850-dualmipi0-qhd-cmd.dtsi",
 * qcom,mdss-dsi-on-command). The kernel's format is
 * <dtype> <wait_ms> <payload...>, and the full list is a page select and
 * a hundred register writes: the shim only sends what a display that has
 * already been initialised by the bootloader needs to come back:
 *
 *   0x11  sleep out            (with the 100 ms the panel asks for)
 *   0x29  display on           (with the 20 ms the panel asks for)
 *
 * The full sequence can be turned on with DISP_PANEL_FULL_INIT.
 */
static int dsi_panel_init(void)
{
	/* Try multiple times with longer delays. A link that never answers
	 * is a measurement, not a reason to keep hammering it: stop on the
	 * first timeout and say so in the pstore. */
	for (int attempt = 0; attempt < 3; attempt++) {
		if (dsi_dcs_write(0x11, 0, 0, 0) < 0)  /* sleep out */
			goto fail;
		delay_ms(120);                     /* give it extra time */
		if (dsi_dcs_write(0x29, 0, 0, 0) < 0)  /* display on */
			goto fail;
		delay_ms(50);                      /* give it extra time */
	}
	return 0;
fail:
	disp_mark("DCS: sleep-out / display-on did not complete");
	return -1;
}

/* ------------------------------------------------------------------ */
/* The MDP (qcom,mdss_mdp@fd900000)                                    */
/* ------------------------------------------------------------------ */

/*
 * The block offsets come from the phone's own device tree
 * (msm8992-lg-h815-boot.dts:784-815):
 *
 *   qcom,mdss-ctl-off       = <0x2000 0x2200 0x2400>
 *   qcom,mdss-pipe-rgb-off  = <0x15000 0x17000 0x19000>
 *   qcom,mdss-mixer-intf-off= <0x45000 0x46000>
 *   qcom,mdss-intf-off      = <0x6b000 0x6b800 0x6c000 0x6c800>
 *
 * and the register offsets from the kernel of this phone
 * (drivers/video/msm/mdss/mdss_mdp_hwio.h and
 *  drivers/gpu/drm/msm/sde/sde_hw_ctl.c).
 */
#define MDP_BASE	((uintptr_t)G4_MDP_BASE)
#define CTL_BASE	(MDP_BASE + G4_MDP_CTL_OFF)
#define RGB2_BASE	(MDP_BASE + G4_MDP_RGB2_OFF)
#define LM0_BASE	(MDP_BASE + 0x45000UL)
#define INTF2_BASE	(MDP_BASE + G4_MDP_INTF2_OFF)
#define INTF1_BASE	(MDP_BASE + 0x6b000UL)
#define PP0_BASE	(MDP_BASE + 0x71000UL)

/* mdss_mdp_hwio.h:531 -> INTF_FRAME_COUNT, :578 -> PP_AUTOREFRESH_CONFIG.
 * A frame counter that advances is numeric proof that frames are being
 * delivered, independent of what the eye can see on the glass. */
#define INTF_FRAME_COUNT	0x0ac
#define PP_TEAR_CONFIG		0x000
#define PP_AUTOREFRESH_CONFIG	0x030

/* The CTL block. */
#define CTL_LAYER(lm)		(((lm) == 5) ? 0x024 : ((lm) * 0x004))
#define CTL_TOP			0x014
#define CTL_FLUSH		0x018
#define CTL_START		0x01c
#define CTL_SW_RESET		0x030
#define CTL_LAYER_EXTN		0x040

/* The bits of CTL_TOP (mdss_mdp_hwio.h:144-157): BIT(17) selects the
 * command mode, bits [7:4] the interface. */
#define CTL_OP_CMD_MODE		BIT(17)
#define CTL_OP_VIDEO_MODE	0

/* The flush bits (sde_hw_ctl.c:108-233). */
#define FLUSH_SSPP_RGB2		BIT(5)
#define FLUSH_LM0		BIT(6)
#define FLUSH_CTL		BIT(17)
#define FLUSH_INTF2		BIT(29)

/* The layer mixer configuration (mdss_mdp_hwio.h:358-359, mdss_mdp_ctl.c:3182). */
#define LM_BORDER_COLOR		BIT(24)

/* The SSPP (the RGB2 pipe). */
#define SSPP_SRC_SIZE		0x000
#define SSPP_SRC_IMG_SIZE	0x004
#define SSPP_SRC_XY		0x008
#define SSPP_OUT_SIZE		0x00c
#define SSPP_OUT_XY		0x010
#define SSPP_SRC0_ADDR		0x014
#define SSPP_SRC1_ADDR		0x018
#define SSPP_SRC2_ADDR		0x01c
#define SSPP_SRC3_ADDR		0x020
#define SSPP_SRC_YSTRIDE0	0x024
#define SSPP_SRC_YSTRIDE1	0x028
#define SSPP_SRC_FORMAT		0x030
#define SSPP_SRC_UNPACK		0x034
#define SSPP_SRC_OP_MODE	0x038
#define SSPP_FETCH_CONFIG	0x048
#define SSPP_SRC_ADDR_SW_STATUS	0x070
#define SSPP_DECIMATION_CFG	0x0b4

/* The INTF (the INTF2 block). */
#define INTF_TIMING_ENGINE_EN	0x000
#define INTF_BORDER_COLOR	0x044
#define INTF_UNDERFLOW_COLOR	0x048
#define INTF_POLARITY_CTL	0x050
#define INTF_DSI_CMD_TRIGGER_EN	0x084
#define INTF_PANEL_FORMAT	0x090
#define INTF_FRAME_LINE_COUNT_EN	0x0a8

/* RGB888, 24 bits per pixel on the wire (mdss_mdp_hwio.h:553). */
#define PANEL_FORMAT_RGB888	0x213f

/* The global interface select: one byte per interface, 1 = DSI
 * (mdss_mdp_hwio.h:59, mdss_mdp_ctl.c:2652-2658). */
#define MDP_DISP_INTF_SEL	0x004
#define INTF_TYPE_DSI		1

/*
 * The values the phone's own kernel had when the panel was working
 * (docs/display-2026-11-10.txt):
 *
 *   SRC_SIZE     = 0x0A0005A0   (2560 << 16 | 1440)
 *   SRC_IMG_SIZE = 0x0A0005A0
 *   SRC_FORMAT   = 0x000236FF   (RGBA8888, see mdss_mdp_pipe.c:1579-1601)
 *   SRC_UNPACK   = 0x03010002
 *   YSTRIDE0     = 0x00001680   (1440 * 4)
 *   PANEL_FORMAT = 0x0000213F
 */
#define MDP_SRC_FORMAT_RGBA8888	0x000236ffUL
#define MDP_SRC_UNPACK_RGBA8888	0x03010002UL

/*
 * Bring the MDP up with the shim's own buffer.
 *
 * The order is the one of the kernel
 * (mdss_mdp_ctl.c:2652-2667 and 3968-3977, mdss_mdp_pipe.c:1533-1541,
 *  1612-1615 and 1678-1681, sde_encoder.c:874-896):
 *
 *   1. the interface select says that INTF2 is a DSI interface;
 *   2. the CTL block is reset;
 *   3. the RGB2 pipe is programmed: the geometry, then the format, then
 *      the address (the address LAST, like the kernel does, so that the
 *      pipe never reads a half programmed buffer);
 *   4. the mixer is told the size of the output;
 *   5. the layer mixer is pointed at the RGB2 pipe;
 *   6. the interface is put in command mode (the timing engine stays OFF:
 *      in command mode the DSI controller pulls the frame out);
 *   7. CTL_TOP says "command mode, INTF2";
 *   8. CTL_FLUSH arms the pipe, the mixer, the interface and the CTL,
 *      and CTL_START kicks the frame out.
 */
static int __attribute__((unused)) disp_mdp_init(void)
{
	uint32_t v;

	/* 1. Interface select: INTF1 (DSI0) and INTF2 (DSI1) are DSI interfaces. */
	v = rd(MDP_BASE + MDP_DISP_INTF_SEL);
	v |= (INTF_TYPE_DSI << 8);  /* INTF1 = DSI (DSI0) */
	v |= (INTF_TYPE_DSI << 16); /* INTF2 = DSI (DSI1) */
	wr(MDP_BASE + MDP_DISP_INTF_SEL, v);

	/*
	 * DO NOT do CTL_SW_RESET!
	 * Keep the bootloader's working dual-pipe split configuration (RGB0+RGB1).
	 */

	/* Split display controls matching bootloader */
	wr(MDP_BASE + 0x12f4, 0x00000001);  /* SPLIT_DISPLAY_EN */
	wr(MDP_BASE + 0x12f8, 0x00000106);  /* UPPER_PIPE_CTRL */
	wr(MDP_BASE + 0x13f0, 0x00000106);  /* LOWER_PIPE_CTRL */

	/* PPB (Pingpong Buffer) split config */
	wr(MDP_BASE + 0x1334, 0x00000020);  /* PPB0_CTL */
	wr(MDP_BASE + 0x1338, 0x00310000);  /* PPB0_CFG */

	/* PINGPONG0 tear check (NO autorefresh like kernel) */
	wr(MDP_BASE + 0x71000, 0x00000001);
	wr(MDP_BASE + 0x71004, 0x0018007c);
	wr(MDP_BASE + 0x71008, 0x0000fff0);
	wr(MDP_BASE + 0x7100c, 0x00000a05);
	wr(MDP_BASE + 0x71010, 0x00000a00);
	wr(MDP_BASE + 0x71014, 0x012f0a02);
	wr(MDP_BASE + 0x71018, 0x00040004);
	wr(MDP_BASE + 0x7101c, 0x00000a00);
	
	/* NO autorefresh - kernel has 0x71030 = 0x00000000 */
	wr(MDP_BASE + 0x71030, 0x00000000);

	/*
	 * Configure RGB2 pipe with 0x20000000 (plain RAM, proven writable).
	 */
	uint32_t fb_addr = G4_DISP_FB_BASE;  /* 0x20000000 */
	
	wr(RGB2_BASE + SSPP_SRC_IMG_SIZE, 0x0A0005A0UL);      /* 1440×2560 */
	wr(RGB2_BASE + SSPP_SRC_SIZE, 0x0A0005A0UL);          /* 1440×2560 */
	wr(RGB2_BASE + SSPP_SRC_XY, 0x00000000);
	wr(RGB2_BASE + SSPP_OUT_SIZE, 0x0A0005A0UL);          /* 1440×2560 */
	wr(RGB2_BASE + SSPP_OUT_XY, 0x00000000);
	wr(RGB2_BASE + SSPP_SRC_FORMAT, 0x000236FFUL);        /* RGBA8888 like kernel */
	wr(RGB2_BASE + SSPP_SRC_UNPACK, 0x03010002UL);        /* like kernel */
	wr(RGB2_BASE + SSPP_SRC_YSTRIDE0, 0x00001680UL);      /* 1440×4 */
	wr(RGB2_BASE + SSPP_SRC_YSTRIDE1, 0x00000000);
	wr(RGB2_BASE + SSPP_DECIMATION_CFG, 0x00000000);
	wr(RGB2_BASE + SSPP_SRC_OP_MODE, 0x00000000);
	wr(RGB2_BASE + SSPP_FETCH_CONFIG, 0x00000087UL);      /* like kernel: 0x87 */
	wr(RGB2_BASE + SSPP_SRC_ADDR_SW_STATUS, 0x00000000);
	wr(RGB2_BASE + SSPP_SRC0_ADDR, fb_addr);
	wr(RGB2_BASE + SSPP_SRC1_ADDR, 0x00000000);
	wr(RGB2_BASE + SSPP_SRC2_ADDR, 0x00000000);
	wr(RGB2_BASE + SSPP_SRC3_ADDR, 0x00000000);
	
	con_hex1("MDP", "RGB2.fb_addr=", fb_addr);

	/* 4. The mixer: the size of the output. */
	wr(LM0_BASE + 0x004, (G4_FB_H << 16) | G4_FB_W);

	/*
	 * 5. Layer mixer: RGB2 at stage 1 with border (EXACTLY like kernel).
	 */
	v = 0x01008000UL;  /* RGB2 (pipe 5) at stage 1 + border - KERNEL VALUE */
	wr(CTL_BASE + CTL_LAYER(0), v);
	wr(CTL_BASE + CTL_LAYER_EXTN, 0x00000000);

	/*
	 * 6. INTF2 only in command mode (kernel uses INTF2, not INTF1).
	 */
	wr(INTF2_BASE + INTF_TIMING_ENGINE_EN, 0x00000000);
	wr(INTF2_BASE + INTF_BORDER_COLOR, 0x00000000);
	wr(INTF2_BASE + INTF_UNDERFLOW_COLOR, 0x000000ff);
	wr(INTF2_BASE + INTF_FRAME_LINE_COUNT_EN, 0x00000003);
	wr(INTF2_BASE + INTF_PANEL_FORMAT, PANEL_FORMAT_RGB888);

	/*
	 * 7. CTL_TOP: kernel value (INTF2, command mode).
	 */
	wr(CTL_BASE + CTL_TOP, 0x00020020UL);  /* kernel value */

	trail(0x2730, 0x01);
	return 0;
}

/*
 * A frame on a command-mode panel only reaches the panel when the DSI
 * controller is told to pull it. The software trigger is one write per
 * controller, after the buffer already holds the picture. Both links
 * are needed (dual-DSI split panel).
 */
static void dsi_soft_trigger(void)
{
	dsi0_w(DSI_CMD_MODE_MDP_SW_TRIGGER, 0x01);
	dsi1_w(DSI_CMD_MODE_MDP_SW_TRIGGER, 0x01);
	__asm__ volatile("dsb sy" ::: "memory");
}

/*
 * 2026-11-16: re-arm the two bits that the BOOT dump showed to be in the
 * WRONG state compared with the working kernel. Both are one read-modify-
 * write per link, no soft reset, no MDP reconfiguration: LK's pipeline
 * (RGB0+RGB1 -> cont_splash -> LM0 -> INTF1/2 -> DSI0/1) is left exactly
 * as it was measured.
 */
static void __attribute__((unused)) dsi_force_hs_mode(void)
{
	uint32_t a = dsi0_r(DSI_COMMAND_MODE_DMA_CTRL);
	uint32_t b = dsi1_r(DSI_COMMAND_MODE_DMA_CTRL);

	con_hex2("HS", "dmactrl0=", a, "dmactrl1=", b);
	dsi0_w(DSI_COMMAND_MODE_DMA_CTRL, a & ~DSI_DMA_CTRL_LP);
	dsi1_w(DSI_COMMAND_MODE_DMA_CTRL, b & ~DSI_DMA_CTRL_LP);
	con_hex2("HS", "after0=", dsi0_r(DSI_COMMAND_MODE_DMA_CTRL),
		 "after1=", dsi1_r(DSI_COMMAND_MODE_DMA_CTRL));
}

static void __attribute__((unused)) dsi_select_te(void)
{
	uint32_t a = dsi0_r(DSI_TRIG_CTRL);
	uint32_t b = dsi1_r(DSI_TRIG_CTRL);

	con_hex2("TE", "trig0=", a, "trig1=", b);
	dsi0_w(DSI_TRIG_CTRL, a | DSI_TRIG_TE_SEL);
	dsi1_w(DSI_TRIG_CTRL, b | DSI_TRIG_TE_SEL);
	con_hex2("TE", "after0=", dsi0_r(DSI_TRIG_CTRL),
		 "after1=", dsi1_r(DSI_TRIG_CTRL));
}

/* Read the MDP frame counters. If they advance after a kick, frames are
 * really leaving MDP: the display problem is then on the DSI/panel side,
 * not in the framebuffer. */
static void __attribute__((unused)) disp_frame_count(const char *tag)
{
	con_hex2(tag, "intf1=", rd(INTF1_BASE + INTF_FRAME_COUNT),
		 "intf2=", rd(INTF2_BASE + INTF_FRAME_COUNT));
	con_hex2(tag, "pp0tear=", rd(PP0_BASE + PP_TEAR_CONFIG),
		 "autorf=", rd(PP0_BASE + PP_AUTOREFRESH_CONFIG));
}

void disp_mdp_kick(void)
{
	dsi_soft_trigger();
	delay_ms(10);
}

/*
 * Full bring-up only: arm the pipeline and let CTL start it.
 *
 * CTL_FLUSH is a write-one strobe, NOT a status register. Reading it back
 * returns the bits still pending, and on this SoC some bits stay pending
 * while the panel works: the live kernel dump has 0x40000018 with 27
 * frames played and 0 underruns (docs/display-2026-11-10.txt). The poll
 * below is INFORMATION, never a pass/fail criterion: the criterion is
 * what the eye sees on the panel.
 *
 * The old test polled with "v != kick_value", which is true on the first
 * read whatever happens. Here we mask the bits we wrote and wait for
 * those bits only.
 */
static uint32_t __attribute__((unused)) disp_mdp_commit(void)
{
	uint32_t mask = FLUSH_SSPP_RGB2 | FLUSH_LM0 | FLUSH_CTL | FLUSH_INTF2;
	uint32_t residual = mask;
	unsigned ms;

	wr(CTL_BASE + CTL_FLUSH, mask);
	wr(CTL_BASE + CTL_START, 0x1);
	__asm__ volatile("dsb sy" ::: "memory");

	for (ms = 0; ms < DISP_FLUSH_POLL_MS; ms++) {
		residual = rd(CTL_BASE + CTL_FLUSH) & mask;
		if (residual == 0)
			break;
		g4_wdt_pet();
		delay_ms(1);
	}
	con_hex2("KICK", "poll_ms=", ms, "residual=", residual);
	trail(0x2738, residual);
	dsi_soft_trigger();
	return residual;
}

/* ------------------------------------------------------------------ */
/* The picture and the on-screen progress console                      */
/* ------------------------------------------------------------------ */

/*
 * Two buffers, two formats, and why the distinction matters.
 *
 *   cont_splash_mem  0x03400000, 12 MiB, CARVED OUT. The bootloader's
 *                    RGB0+RGB1 pipes read it as 720x2560 RGB888 halves.
 *                    12 MiB fits 1440*2560*3 (10.5 MiB) but NOT
 *                    1440*2560*4 (14.1 MiB). Never write past 0x04000000.
 *
 *   shim framebuffer 0x20000000, plain DRAM in the lower bank. Mode 3
 *                    points RGB2 at it with ARGB8888 / stride 5760. It is
 *                    plain DRAM, so a full 14.1 MiB frame is safe there.
 *
 * The old disp_draw() wrote 0xE10000 bytes at 0x03400000: 2.06 MiB past
 * the end of the window, into memory nobody had reserved. Fixed here.
 *
 * NOTE: console.c already paints every log line into con_fb (con_str ->
 * put_str). The on-screen LOG therefore exists already; what has always
 * been missing is the scan-out and an agreed pixel format. This file
 * only adds the stage number and the heartbeat on top of it.
 */
#define DISP_SPLASH_BASE	0x03400000UL
#define DISP_SPLASH_SIZE	0x00c00000UL
#define DISP_FB_BASE		G4_DISP_FB_BASE
#define DISP_FB_W		1440
#define DISP_FB_H		2560

#if DISP_FB_BPP == 32
#define DISP_FB_BYTES		4
#elif DISP_FB_BPP == 24
#define DISP_FB_BYTES		3
#else
#error "DISP_FB_BPP must be 24 or 32"
#endif

/* Every byte of a stage colour is equal, so the fill is a uniform colour
 * whether the pipes read 3 or 4 bytes per pixel. Deliberate: a stage has
 * to be readable before the inherited pixel format is known. */
static uint32_t disp_stage_color(unsigned stage)
{
	static const uint8_t gray[8] = {
		0x18, 0x30, 0x48, 0x60, 0x78, 0x90, 0xa8, 0xc0
	};
	uint8_t c = gray[stage & 7];

	return ((uint32_t)c << 24) | ((uint32_t)c << 16) |
	       ((uint32_t)c << 8) | c;
}

static void disp_cache_clean(uintptr_t base, uintptr_t len)
{
	for (uintptr_t a = base; a < base + len; a += 64)
		__asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

/* ------------------------------------------------------------------ */
/* MODE 4: DCS straight into the panel's own GRAM, with NO MDP         */
/* ------------------------------------------------------------------ */
/*
 * Why this exists (the D0/D1 result of 2026-11-15):
 *
 *   D0: zero display reads/writes -> the LG logo stays on the panel.
 *   D1: 10.5 MiB written into cont_splash + cache clean + 40 heartbeats
 *       -> NOT ONE PIXEL changed, CPU alive, no fault.
 *
 * So the visible logo is NOT being scanned out of 0x03400000 by a live
 * MDP at handover. On a DSI command-mode panel the picture lives in the
 * panel's OWN GRAM: LK pushed it once and the panel self-refreshes with
 * the MDSS/DSI clocks off. Writing DDR can never change it.
 *
 * The shortest possible path to a visible pixel is therefore not MDP ->
 * CTL -> LM -> INTF -> DSI (five blocks, an SMMU and an IOVA to get
 * right): it is ONE DCS memory write over the DSI link that is already
 * there. DCS 0x2A/0x2B set the window, 0x2C streams RGB888 pixels.
 * No CTL, no RGB pipe, no LM, no INTF, no SMMU, no framebuffer address.
 *
 * Dual-DSI split: each link drives a 720 column half and every command
 * is broadcast to both, so a 640 column window appears TWICE (one HELLO
 * per half). That is deliberate: it is unmistakable and it avoids having
 * to guess how LK mapped the split.
 */
#define GRAM_W			640
#define GRAM_H			200
#define GRAM_STRIDE		(GRAM_W * 3)		/* RGB888 */
#define GRAM_BYTES		((uintptr_t)GRAM_H * GRAM_STRIDE)
#define GRAM_ROWS_PER_PKT	8			/* 8*1920 = 15360 < 16380 */
/* 2026-11-16: put the test pattern at the TOP of the panel. The previous
 * y=1200 put it in the middle of a 2560-line screen, where a partial or
 * misaligned frame is easy to miss. */
#define GRAM_Y0			0
#define GRAM_FB			G4_DISP_FB_BASE	/* 0x20000000, plain DRAM */
#define GRAM_BEAT_W		720	/* full panel width, or rows shear */
#define GRAM_BEAT_H		32
#define GRAM_SQUARE_Y0		240

static int s_gram_ready;

/* Set a column (0x2A) or page (0x2B) address window. */
static int dsi_dcs_addr(uint8_t cmd, unsigned a0, unsigned a1)
{
	uint8_t p[4];

	p[0] = (uint8_t)(a0 >> 8);
	p[1] = (uint8_t)a0;
	p[2] = (uint8_t)(a1 >> 8);
	p[3] = (uint8_t)a1;
	return dsi_dcs_write(cmd, p, 4, 0);
}

static int gram_window(unsigned x1, unsigned y0, unsigned y1)
{
	if (dsi_dcs_addr(0x2a, 0, x1) < 0)
		return -1;
	return dsi_dcs_addr(0x2b, y0, y1);
}

/*
 * MIPI DCS 0x3A (set_pixel_format). LK leaves the panel with a pixel
 * format we never measured; a 3-byte-per-pixel stream interpreted as
 * RGB565 or RGB666 produces exactly the salt-and-pepper noise seen on
 * 2026-11-16. 0x77 is DCS_PIXFMT_24BPP, matching qcom,mdss-dsi-bpp=<24>
 * in the panel DTS. Sent as a short write with one parameter on both
 * DSI links.
 */
static int __attribute__((unused)) gram_set_format(void)
{
	const uint8_t pixfmt = 0x77;
	return dsi_dcs_write(0x3a, &pixfmt, 1, 20);
}

/*
 * Render HELLO into a small RGB888 scratch framebuffer in plain DRAM,
 * then push it into the panel's GRAM through DCS.
 *
 * 2026-11-16: the first version set a FRESH 0x2A/0x2B window before every
 * 8-row packet and used 0x2C for all of them. Two problems with that, both
 * from the phone's own DTS:
 *   - on this LGD panel 0x2A/0x2B/0x3A are also MANUFACTURER REGISTERS on
 *     a selected command page (they appear as register writes in
 *     qcom,mdss-dsi-on-command, e.g. "15 ... 02 2A 03"), so a DCS long
 *     write with 4 address bytes may be reinterpreted, not set a window;
 *   - the kernel never sends 0x2A/0x2B at all: it declares
 *     wr_mem_start=0x2c and wr_mem_continue=0x3c, and the DSI host inserts
 *     those into the MDP stream (DSI.dcsctl = 0x00013C2C measured at boot).
 *
 * So: ONE window, first packet 0x2C, all following packets 0x3C (memory
 * write continue), exactly like the kernel's stream. Fewer commands and no
 * dependence on the window registers being DCS-standard on this page.
 */
static int __attribute__((unused)) gram_hello(void)
{
	volatile uint32_t *scr = (volatile uint32_t *)GRAM_FB;
	const unsigned words = (unsigned)(GRAM_BYTES / 4);

	for (unsigned i = 0; i < words; i++)
		scr[i] = 0x10101010U;		/* R=G=B=0x10, dark grey */

	con_draw_text_bpp(GRAM_FB, GRAM_STRIDE, 24, 8, "HELLO",
			  0x00ffffffU, 0x00101010U, 8, 24);
	con_draw_text_bpp(GRAM_FB, GRAM_STRIDE, 24, 144, "D4 GRAM",
			  0x00ff6040U, 0x00101010U, 3, 24);
	disp_cache_clean(GRAM_FB, GRAM_BYTES);

	if (gram_window(GRAM_W - 1, GRAM_Y0,
			GRAM_Y0 + GRAM_H - 1) < 0)
		return -1;

	for (unsigned y = 0; y < GRAM_H; y += GRAM_ROWS_PER_PKT) {
		unsigned rows = GRAM_H - y;
		unsigned n;
		uint8_t cmd = (y == 0) ? 0x2c : 0x3c;

		if (rows > GRAM_ROWS_PER_PKT)
			rows = GRAM_ROWS_PER_PKT;
		n = rows * GRAM_STRIDE;
		if (dsi_dcs_write(cmd,
		    (const uint8_t *)GRAM_FB + (uintptr_t)y * GRAM_STRIDE,
		    n, 0) < 0)
			return -1;
	}
	con_hex2("GRAM", "packets=", GRAM_H / GRAM_ROWS_PER_PKT,
		 "bytes=", (uint32_t)GRAM_BYTES);
	return 0;
}

/*
 * Fill the ENTIRE panel GRAM with one solid colour. 720x2560 RGB888 is
 * 5,529,600 bytes, about 338 DCS packets. D6 proved that DCS 0x28 reaches
 * the panel; a solid full-screen colour is the next unambiguous step: no
 * font, no window arithmetic, no MDP, no framebuffer address to guess.
 */
#define GRAM_FULL_W		720
#define GRAM_FULL_H		2560
#define GRAM_FULL_ROW		(GRAM_FULL_W * 3)	/* 2160 bytes */
#define GRAM_FULL_BYTES		((uintptr_t)GRAM_FULL_W * GRAM_FULL_H * 3)
/*
 * 30 whole rows = 64800 bytes. One DCS long packet can carry 65532, so a
 * full frame takes 86 packets instead of the 366 needed with 16 KiB
 * packets. Whole rows matter: a payload that is not a multiple of 2160
 * would start the next packet in the middle of a row and shear the image.
 */
#define GRAM_ROWS_PER_FULL_PKT	30
#define GRAM_FULL_CHUNK		(GRAM_ROWS_PER_FULL_PKT * GRAM_FULL_ROW)
#define GRAM_BEAT_Y0		520
#define GRAM_BEAT_ROWS		64
#define GRAM_SCRATCH		(GRAM_FB + GRAM_FULL_BYTES)
/* Must sit AFTER the full frame and after the beat scratch: GRAM_BEAT_FB
 * used to be GRAM_FB + 384000, which is INSIDE the 5529600-byte frame and
 * silently overwrote row ~178 whenever the old beat helper ran. */
#define GRAM_BEAT_FB		(GRAM_SCRATCH + \
				 (uintptr_t)GRAM_BEAT_ROWS * GRAM_FULL_ROW)

static void disp_hex8(char *buf, uint32_t v);
static int gram_stream_buffer(const uint8_t *src);
static int gram_stream_rows(const uint8_t *src, unsigned y0, unsigned rows);

static int __attribute__((unused)) gram_full_fill(uint8_t r, uint8_t g,
						   uint8_t b)
{
	volatile uint32_t *src = (volatile uint32_t *)GRAM_FB;
	uintptr_t words = GRAM_FULL_BYTES / 4;
	/* RGB888 repeats every 3 bytes, a 32-bit word holds 4, so the
	 * combined pattern repeats every 12 bytes = 3 words. GRAM_FULL_BYTES
	 * is divisible by 12. */
	uint32_t pat[3];

	pat[0] = (uint32_t)r | ((uint32_t)g << 8) |
		 ((uint32_t)b << 16) | ((uint32_t)r << 24);
	pat[1] = (uint32_t)g | ((uint32_t)b << 8) |
		 ((uint32_t)r << 16) | ((uint32_t)g << 24);
	pat[2] = (uint32_t)b | ((uint32_t)r << 8) |
		 ((uint32_t)g << 16) | ((uint32_t)b << 24);
	for (uintptr_t i = 0; i < words; i += 3) {
		src[i + 0] = pat[0];
		src[i + 1] = pat[1];
		src[i + 2] = pat[2];
	}
	disp_cache_clean(GRAM_FB, GRAM_FULL_BYTES);
	return gram_stream_buffer((const uint8_t *)GRAM_FB);
}

/*
 * Stream an arbitrary 720x2560 RGB888 buffer into the panel GRAM.
 *
 * D10 filled the whole panel with solid red and worked. Its window was
 * 0..719 columns, 0..2559 rows. The earlier HELLO attempts used a
 * 640-column window but kept sending 640*3 bytes per row; the panel
 * advances its GRAM pointer across the full 720-column width, so every
 * row was displaced by 80*3 = 240 bytes. That is the deterministic
 * "random pixel" picture seen on every boot.
 *
 * Each packet carries an integral number of 720-pixel rows (7 rows =
 * 15120 bytes), so the byte stream never starts in the middle of a pixel.
 */
static int gram_stream_rows(const uint8_t *src, unsigned y0, unsigned rows)
{
	uintptr_t off = 0;
	unsigned packets = 0;
	uint8_t cmd = 0x2c;

	if (gram_window(GRAM_FULL_W - 1, y0, y0 + rows - 1) < 0)
		return -1;

	for (unsigned y = 0; y < rows; y += GRAM_ROWS_PER_FULL_PKT) {
		unsigned nrows = rows - y;
		unsigned n;

		if (nrows > GRAM_ROWS_PER_FULL_PKT)
			nrows = GRAM_ROWS_PER_FULL_PKT;
		n = nrows * GRAM_FULL_ROW;
		if (dsi_dcs_write(cmd, src + off, n, 0) < 0)
			return -1;
		off += n;
		cmd = 0x3c;
		packets++;
	}
	return (int)packets;
}

static int __attribute__((unused)) gram_stream_buffer(const uint8_t *src)
{
	int packets = gram_stream_rows(src, 0, GRAM_FULL_H);

	if (packets < 0)
		return -1;
	con_hex2("FULL", "packets=", (uint32_t)packets,
		 "bytes=", (uint32_t)GRAM_FULL_BYTES);
	return 0;
}

/* ------------------------------------------------------------------ */
/* MODE 12 - ALIGNED, FAST CONSOLE (2026-11-16)                       */
/* ------------------------------------------------------------------ */
/*
 * Two problems were left after mode 11:
 *
 *   1. SPEED. Mode 11 streamed 720x2560x3 = 5,529,600 bytes on every
 *      boot. The panel renders the GRAM as it is written, so the user
 *      watched a slow top-to-bottom fill. Only the first ~600 rows hold
 *      the console; the remaining 1960 rows carried a flat background.
 *
 *   2. ALIGNMENT. Mode 11 set ONE 0x2A/0x2B window and then sent 85
 *      packets of 0x3C (write_memory_continue). Any byte the panel
 *      drops, inserts or leaves pending between packets shifts every
 *      following row a little more. A solid red frame hides that
 *      completely; a 12x-scaled glyph turns into chevrons. This is the
 *      deterministic "random pixels" seen on every boot.
 *
 * The fix: NEVER depend on carried GRAM state. Every packet gets a FRESH
 * 0x2A/0x2B window covering exactly its own rows, and always starts with
 * 0x2C (write_memory_start). A lost byte can corrupt at most one packet;
 * it can no longer accumulate. Each packet still carries an integral
 * number of 720-pixel rows, so no row starts mid-pixel.
 *
 * Data: 600 rows x 2160 bytes = 1,296,000 bytes (4.3x less than mode 11),
 * 20 packets instead of 86. The heartbeat repaints 8 rows, one packet.
 */
#define GRAM_BAND_H		600
#define GRAM_BAND_BYTES		((uintptr_t)GRAM_BAND_H * GRAM_FULL_ROW)
#define GRAM_BEAT_BAND_Y0	568
#define GRAM_BEAT_BAND_ROWS	8
#define GRAM_BAND_SCRATCH	(GRAM_FB + GRAM_BAND_BYTES)

/* Every packet is self-contained: fresh window, 0x2C, whole rows only. */
static int gram_stream_fresh(const uint8_t *src, unsigned rows)
{
	uint64_t t0 = timer_us();
	unsigned packets = 0;

	for (unsigned y = 0; y < rows; y += GRAM_ROWS_PER_FULL_PKT) {
		unsigned nrows = rows - y;

		if (nrows > GRAM_ROWS_PER_FULL_PKT)
			nrows = GRAM_ROWS_PER_FULL_PKT;
		if (gram_window(GRAM_FULL_W - 1, y, y + nrows - 1) < 0)
			return -1;
		if (dsi_dcs_write(0x2c,
		    src + (uintptr_t)y * GRAM_FULL_ROW,
		    nrows * GRAM_FULL_ROW, 0) < 0)
			return -1;
		packets++;
	}
	con_hex2("BAND", "packets=", packets, "ms=",
		 (uint32_t)((timer_us() - t0) / 1000u));
	return 0;
}

/* One-pixel rulers: if rows are still shifted, these become diagonals
 * instead of straight lines, and the photo says by how much. */
static void gram_band_rulers(void)
{
	uint32_t stride = GRAM_FULL_ROW;

	for (unsigned y = 0; y < GRAM_BAND_H; y++) {
		volatile uint8_t *row = (volatile uint8_t *)
			(GRAM_FB + (uintptr_t)y * stride);
		for (unsigned x = 0; x < 2; x++) {
			row[3 * x + 0] = 0x20;
			row[3 * x + 1] = 0xe0;
			row[3 * x + 2] = 0xe0;
		}
		for (unsigned x = GRAM_FULL_W - 2; x < GRAM_FULL_W; x++) {
			row[3 * x + 0] = 0x20;
			row[3 * x + 1] = 0xe0;
			row[3 * x + 2] = 0xe0;
		}
	}
	for (unsigned y = 180; y < GRAM_BAND_H; y += 160) {
		volatile uint8_t *row = (volatile uint8_t *)
			(GRAM_FB + (uintptr_t)y * stride);
		for (unsigned x = 0; x < GRAM_FULL_W; x++) {
			row[3 * x + 0] = 0x60;
			row[3 * x + 1] = 0x60;
			row[3 * x + 2] = 0x60;
		}
	}
}

static int __attribute__((unused)) gram_console_band(unsigned stage,
						      unsigned beat,
						      const char *label)
{
	volatile uint32_t *scr = (volatile uint32_t *)GRAM_FB;
	uint32_t stride = GRAM_FULL_ROW;
	char st[3], bt[24] = "BEAT ";
	char b[16];

	/* All four bytes equal: 3 bytes per pixel. */
	for (uintptr_t i = 0; i < GRAM_BAND_BYTES / 4; i++)
		scr[i] = 0x10101010U;

	for (unsigned y = 0; y < 16; y++) {
		volatile uint8_t *row = (volatile uint8_t *)
			(GRAM_FB + (uintptr_t)y * stride);
		for (unsigned x = 0; x < GRAM_FULL_W; x++) {
			row[3 * x + 0] = 0xff;
			row[3 * x + 1] = 0x20;
			row[3 * x + 2] = 0x20;
		}
	}
	gram_band_rulers();

	con_draw_text_bpp(GRAM_FB, stride, 40, 40, "HELLO",
			  0x00ffffffU, 0x00101010U, 8, 24);
	st[0] = (char)('0' + (stage % 10));
	st[1] = 0;
	con_draw_text_bpp(GRAM_FB, stride, 40, 200, st,
			  0x00ffff00U, 0x00101010U, 8, 24);
	con_draw_text_bpp(GRAM_FB, stride, 40, 350, label,
			  0x0040e0d0U, 0x00101010U, 4, 24);

	disp_hex8(b, beat);
	for (int i = 0; i < 8; i++)
		bt[5 + i] = b[i];
	bt[13] = 0;
	con_draw_text_bpp(GRAM_FB, stride, 40, 440, bt,
			  0x00ffffffU, 0x00101010U, 4, 24);

	disp_cache_clean(GRAM_FB, GRAM_BAND_BYTES);
	return gram_stream_fresh((const uint8_t *)GRAM_FB, GRAM_BAND_H);
}

/* Heartbeat: ONE 8-row packet (17,280 bytes), not 64 rows x 3 packets. */
static void __attribute__((unused)) gram_band_beat(unsigned beat)
{
	volatile uint8_t *p = (volatile uint8_t *)GRAM_BAND_SCRATCH;
	uintptr_t n = (uintptr_t)GRAM_BEAT_BAND_ROWS * GRAM_FULL_ROW;
	uint8_t r = (beat & 1) ? 0xff : 0x20;
	uint8_t g = (beat & 1) ? 0x20 : 0xff;

	for (uintptr_t i = 0; i < n; i += 3) {
		p[i + 0] = r;
		p[i + 1] = g;
		p[i + 2] = 0x20;
	}
	disp_cache_clean(GRAM_BAND_SCRATCH, n);
	if (gram_window(GRAM_FULL_W - 1, GRAM_BEAT_BAND_Y0,
			GRAM_BEAT_BAND_Y0 + GRAM_BEAT_BAND_ROWS - 1) < 0)
		return;
	(void)dsi_dcs_write(0x2c, (const uint8_t *)GRAM_BAND_SCRATCH, n, 0);
}

/* ------------------------------------------------------------------ */
/* MODE 13 - HARDWARE (MDP) CONSOLE + BOOT DIAGNOSTICS                 */
/* ------------------------------------------------------------------ */
/*
 * All data for this block is in display-data.h. Read it first.
 *
 * WHY. Modes 4..12 push pixels with the CPU: for every 64 KiB packet the
 * CPU copies 64 KiB from DRAM into DD_DSI_CMD_BUF, clears/waits
 * CMD_DMA_DONE on two links and strobes DMA_SW_TRIGGER. 86 packets for a
 * full frame. The panel renders its GRAM as bytes arrive, hence the slow
 * top-to-bottom fill; a lost byte between 0x3C packets shifts all later
 * rows, hence the sheared HELLO. Narrow 0x2B windows are ignored by this
 * panel (D12), so the CPU path cannot be made both fast and correct.
 *
 * THE HARDWARE PATH. LK already programmed RGB0/RGB1 -> 0x03400000 with
 * stride 4320 (1440x2560 RGB888), LM0 1440x2560, INTF1/INTF2 -> DSI0/1.
 * The pipeline is ALIVE but FROZEN (INTF frame counters = 0). The CPU
 * writes ONE frame into that framebuffer and issues ONE MDP_SW_TRIGGER.
 * MDP/DSI DMA then moves the whole frame with hardware timing: no CPU
 * byte loop, no per-packet state, no shear, no progressive fill.
 *
 * GPU? No Adreno driver exists in a bare-metal shim. The engine is the
 * MDP display processor + DSI DMA (hardware scan-out). "GPU" is shorthand
 * for that. Prove it numerically with CMD_MDP_DONE = BIT(8) of
 * DSI_INT_CTRL on BOTH links. DD_INTF_FRAME_COUNT is NOT a valid proof:
 * in command mode the timing engine is off and that counter stays 0 even
 * when the frame appears instantly (measured, 2026-11-16).
 *
 * STEP-BY-STEP BOOT LOGS. disp_boot_init() runs at the very start of
 * shim_main; every shim_checkpoint() calls disp_boot_step(). Each step
 * paints its own line and kicks ONE hardware frame BEFORE the next
 * operation. If the phone hangs, the last visible line is the last step
 * that completed -- that is the point of the whole diagnostic.
 */

#define DD_STEP_FIRST_Y		240
#define DD_STEP_PITCH		52
#define DD_STEP_MAX		20
#define DD_PROGRESS_Y		200
#define DD_PROGRESS_H		8
#define DD_LOG_FIRST_Y		320
#define DD_LOG_PITCH		36
#define DD_LOG_LINES		14

static int s_boot_ready;
static unsigned s_step_n;
static unsigned s_gpu_kick_mode;	/* 0 trigger only, 1 +CTL, 2 +full CTL */

static void fb_put_line(uint32_t y, const char *s, uint32_t fg, int scale)
{
	con_draw_text_bpp(DD_FB_BASE, DD_FB_STRIDE, 40, y, s,
			  fg, 0x00101010U, scale, 24);
	disp_cache_clean(DD_FB_BASE + (uintptr_t)y * DD_FB_STRIDE,
			 (uintptr_t)16 * scale * DD_FB_STRIDE);
}

static void fb_progress(unsigned n)
{
	uint32_t w = (n + 1) * 60;

	if (w > DD_FB_W)
		w = DD_FB_W;
	for (unsigned y = DD_PROGRESS_Y; y < DD_PROGRESS_Y + DD_PROGRESS_H; y++) {
		volatile uint8_t *row = (volatile uint8_t *)
			(DD_FB_BASE + (uintptr_t)y * DD_FB_STRIDE);
		for (unsigned x = 0; x < w; x++) {
			row[3 * x + 0] = 0x20;
			row[3 * x + 1] = 0xff;
			row[3 * x + 2] = 0x40;
		}
	}
	disp_cache_clean(DD_FB_BASE + (uintptr_t)DD_PROGRESS_Y * DD_FB_STRIDE,
			 (uintptr_t)DD_PROGRESS_H * DD_FB_STRIDE);
}

/* Read the two frame counters. They only advance when MDP hardware played
 * a frame; a CPU/DCS write never touches them. */
static uint32_t gpu_f1(void) { return rd(INTF1_BASE + DD_GPU_FRAME_COUNT_OFF); }
static uint32_t gpu_f2(void) { return rd(INTF2_BASE + DD_GPU_FRAME_COUNT_OFF); }

/* A frame can still be in flight after CMD_MDP_DONE: the panel GRAM is
 * written by the DSI engine over several milliseconds. Repainting the
 * framebuffer while that DMA reads it produces the brief salt-and-pepper
 * flashes seen on 2026-11-16. Wait for BOTH command engines to go idle
 * before any CPU write to cont_splash. */
static void disp_gpu_wait_idle(void)
{
	for (unsigned i = 0; i < 200; i++) {
		uint32_t s = dsi0_r(DSI_STATUS) | dsi1_r(DSI_STATUS);

		if (!(s & (DSI_STATUS_CMD_ENGINE_BUSY | DSI_STATUS_CMD_DMA_BUSY)))
			break;
		g4_wdt_pet();
		delay_ms(1);
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

/* Clear CMD_MDP_DONE (BIT(8)) on both links. W1C status register: the
 * enable/mask bits must be preserved, exactly like CMD_DMA_DONE. */
static void dsi_mdp_done_clear(void)
{
	uint32_t v0 = dsi0_r(DSI_INT_CTRL);
	uint32_t v1 = dsi1_r(DSI_INT_CTRL);

	dsi0_w(DSI_INT_CTRL,
	       (v0 & DD_DSI_INTR_ALL_MASK) | DD_DSI_INT_CTRL_MDP_DONE);
	dsi1_w(DSI_INT_CTRL,
	       (v1 & DD_DSI_INTR_ALL_MASK) | DD_DSI_INT_CTRL_MDP_DONE);
	__asm__ volatile("dsb sy" ::: "memory");
}

/* One kick, one measurement. mode selects how much CTL arming is done.
 * The verdict is CMD_MDP_DONE on BOTH links, NOT INTF_FRAME_COUNT: in
 * command mode the timing engine is off and that counter never moves. */
static int disp_gpu_try(unsigned mode, const char *tag)
{
	uint32_t a1 = gpu_f1(), a2 = gpu_f2();
	uint32_t st0 = dsi0_r(DSI_STATUS);
	unsigned waited = 0;
	uint32_t done0 = 0, done1 = 0;

	dsi_mdp_done_clear();
	if (mode == 1) {
		/* Strobe exactly the bits LK left pending. */
		wr(CTL_BASE + CTL_FLUSH, DD_BOOT_CTL_FLUSH);
		wr(CTL_BASE + CTL_START, 0x1);
	} else if (mode == 2) {
		/* Arm the inherited pipes explicitly (RGB0, RGB1, LM0, CTL,
		 * INTF1, INTF2) and start the CTL. No pipe reconfiguration. */
		wr(CTL_BASE + CTL_FLUSH,
		   BIT(3) | BIT(4) | BIT(6) | BIT(17) | BIT(28) | BIT(29));
		wr(CTL_BASE + CTL_START, 0x1);
	}
	__asm__ volatile("dsb sy" ::: "memory");
	dsi_soft_trigger();			/* MDP_SW_TRIGGER on both links */

	for (; waited < 200; waited++) {
		done0 = dsi0_r(DSI_INT_CTRL) & DD_DSI_INT_CTRL_MDP_DONE;
		done1 = dsi1_r(DSI_INT_CTRL) & DD_DSI_INT_CTRL_MDP_DONE;
		if (done0 && done1)
			break;
		g4_wdt_pet();
		delay_ms(1);
	}

	/* Do not let the caller repaint cont_splash while this frame is
	 * still being pushed into the panel GRAM. */
	disp_gpu_wait_idle();
	delay_ms(2);
	uint32_t b1 = gpu_f1(), b2 = gpu_f2();
	/* INTF counters are informational: 0 is EXPECTED in command mode. */
	con_hex2(tag, "mdp_done=", (done0 ? 1u : 0u) | (done1 ? 2u : 0u),
		 "wait_ms=", waited);
	con_hex2(tag, "d_intf1=", b1 - a1, "d_intf2=", b2 - a2);
	con_hex2(tag, "dsi_status=", dsi0_r(DSI_STATUS) ^ st0,
		 "ack_err=", dsi0_r(DSI_ACK_ERR_STATUS));
	return (done0 && done1) ? 1 : 0;
}

/* Explicit GPU-vs-CPU verification. Never assert: measure. */
static void disp_gpu_probe(void)
{
	unsigned winner = 0;
	int ok = 0;
	/* Try the known-good kick first (display-data.h), then bisect if a
	 * future device or bootloader state changes the requirement. */
	static const unsigned order[3] = { DD_GPU_KICK_MODE, 0, 2 };

	for (unsigned k = 0; k < 3; k++) {
		unsigned m = order[k];

		if (disp_gpu_try(m, "GPUPROBE")) {
			winner = m;
			ok = 1;
			break;
		}
	}
	s_gpu_kick_mode = winner;
	if (ok) {
		con_str("GPU", "verdict=HW CMD_MDP_DONE set on BOTH links");
		con_str("GPU", "engine=" DD_GPU_ENGINE_NAME " (no Adreno driver)");
		con_str("GPU", "CPU writes ONE framebuffer, then ONE trigger");
		con_str("GPU", "per-frame CPU byte loop = 0 bytes");
	} else {
		con_str("GPU", "verdict=CPU CMD_MDP_DONE never set");
		con_str("GPU", "fallback=DCS byte stream is the only visible path");
	}
	con_hex2("GPU", "kick_mode=", winner,
		 "cpu_bytes_per_frame=", ok ? 0u : (uint32_t)DD_T_FULL_BYTES);
	con_str("GPU", "INTF_FRAME_COUNT=0 is EXPECTED in command mode");
}

/* Called once, at the very start of shim_main (mode 13 only). */
void disp_boot_init(void)
{
	volatile uint32_t *fb = (volatile uint32_t *)DD_FB_BASE;

	con_hex1("DATA", "id=", DISPLAY_DATA_ID);
	con_str("DATA", "source=shim/display-data.h " DISPLAY_DATA_ID_STR);
	if (clocks_display_core() < 0) {
		con_str("BOOT", "display init FAILED: clocks");
		return;
	}
	dsi_force_hs_mode();
	if (dsi_dcs_write(DD_DCS_SLEEP_OUT, 0, 0, 120) < 0)
		return;
	if (dsi_dcs_write(DD_DCS_DISPLAY_ON, 0, 0, 20) < 0)
		return;
	if (gram_set_format() < 0)
		return;

	/* ONE full framebuffer write. Geometry is exactly what LK's RGB0/1
	 * are already programmed for: 1440x2560 RGB888, stride 4320. */
	for (uintptr_t i = 0; i < DD_FB_BYTES / 4; i++)
		fb[i] = 0x10101010U;
	disp_cache_clean(DD_FB_BASE, DD_FB_BYTES);

	fb_put_line(40, "BOOT DIAGNOSTICS", 0x00ffffffU, 8);
	s_boot_ready = 1;
	con_str("BOOT", "framebuffer console ready at 0x03400000");
	disp_gpu_probe();
	disp_boot_step("BOOT display init");
}

static void fb_clear_band(uint32_t y0, uint32_t rows)
{
	volatile uint32_t *p = (volatile uint32_t *)
		(DD_FB_BASE + (uintptr_t)y0 * DD_FB_STRIDE);
	uintptr_t words = (uintptr_t)rows * DD_FB_STRIDE / 4;

	for (uintptr_t i = 0; i < words; i++)
		p[i] = 0x10101010U;
	disp_cache_clean(DD_FB_BASE + (uintptr_t)y0 * DD_FB_STRIDE,
			 (uintptr_t)rows * DD_FB_STRIDE);
}

/* Render the LAST DD_LOG_LINES lines of con_log. Future-proof: any new
 * con_str()/con_hex() line in the shim automatically shows up at the next
 * checkpoint, with no change to this function. */
static void fb_log_tail(void)
{
	int n = con_log_len;
	int start = n, lines = 0, i, row = 0;

	while (start > 0 && lines <= DD_LOG_LINES)
		if (con_log[--start] == '\n')
			lines++;
	if (start > 0)
		start++;

	fb_clear_band(DD_LOG_FIRST_Y - 4,
		      DD_LOG_LINES * DD_LOG_PITCH + 8);
	i = start;
	while (i < n && row < DD_LOG_LINES) {
		char line[64];
		int j = 0;

		while (i < n && con_log[i] != '\n' && j < 63)
			line[j++] = con_log[i++];
		line[j] = 0;
		if (i < n && con_log[i] == '\n')
			i++;
		if (j)
			fb_put_line(DD_LOG_FIRST_Y +
				    (unsigned)row * DD_LOG_PITCH,
				    line, 0x00ffffffU, 2);
		row++;
	}
}

/* Called by every shim_checkpoint(): paint the step AND the recent log
 * lines, then kick ONE hardware frame. If the next operation hangs, the
 * glass keeps showing the last state that completed. */
void disp_boot_step(const char *name)
{
	unsigned n;
	char buf[28];
	unsigned i = 0;

	if (!s_boot_ready)
		return;
	n = s_step_n++;
	if (n >= DD_STEP_MAX)
		return;
	disp_gpu_wait_idle();
	buf[i++] = (char)('0' + (n / 10) % 10);
	buf[i++] = (char)('0' + (n % 10));
	buf[i++] = ' ';
	while (i < sizeof(buf) - 1 && name && *name)
		buf[i++] = *name++;
	buf[i] = 0;

	fb_put_line(DD_STEP_FIRST_Y, buf, 0x0040e0d0U, 3);
	fb_log_tail();
	fb_progress(n);
	disp_gpu_try(s_gpu_kick_mode, "STEP");
	con_hex1("STEP", "n=", n);
}

/* The hold loop: one small bar repaint + one hardware frame, no DCS. */
static void __attribute__((unused)) gpu_console_beat(unsigned beat)
{
	uint32_t y = 1300;

	/* One frame every 2 s, not every 500 ms: fewer kicks, no visible
	 * refresh flutter between step updates. */
	if (beat & 3u)
		return;
	disp_gpu_wait_idle();
	uint32_t w = 40 + (beat & 1) * 600;
	uint32_t r = (beat & 1) ? 0xff : 0x20;
	uint32_t g = (beat & 1) ? 0x20 : 0xff;

	for (unsigned row = y; row < y + 16; row++) {
		volatile uint8_t *p = (volatile uint8_t *)
			(DD_FB_BASE + (uintptr_t)row * DD_FB_STRIDE);
		for (unsigned x = 0; x < w; x++) {
			p[3 * x + 0] = (uint8_t)r;
			p[3 * x + 1] = (uint8_t)g;
			p[3 * x + 2] = 0x20;
		}
	}
	disp_cache_clean(DD_FB_BASE + (uintptr_t)y * DD_FB_STRIDE,
			 16 * DD_FB_STRIDE);
	disp_gpu_try(s_gpu_kick_mode, "BEATFR");
}

/*
 * Render the on-screen console into a full-size RGB888 buffer: HELLO, the
 * stage number, a label and a colour bar at the top. Then stream it once.
 * This is the visible console the project has been after; no MDP, no CTL,
 * no SMMU, no framebuffer address translation is involved.
 */
static int __attribute__((unused)) gram_console(unsigned stage,
						  unsigned beat,
						  const char *label)
{
	volatile uint32_t *scr = (volatile uint32_t *)GRAM_FB;
	uint32_t stride = GRAM_FULL_ROW;
	char st[3];
	char bt[24] = "BEAT ";

	/* All four bytes of the fill word must be equal: the framebuffer is
	 * 3 bytes per pixel, so a word with different bytes produces coloured
	 * vertical stripes instead of a flat background. */
	for (uintptr_t i = 0; i < GRAM_FULL_BYTES / 4; i++)
		scr[i] = 0x10101010U;

	/* A bright bar makes the top of the frame unmistakable. */
	for (unsigned y = 0; y < 24; y++) {
		volatile uint8_t *row = (volatile uint8_t *)
			(GRAM_FB + (uintptr_t)y * stride);
		for (unsigned x = 0; x < GRAM_FULL_W; x++) {
			row[3 * x + 0] = 0xff;
			row[3 * x + 1] = 0x20;
			row[3 * x + 2] = 0x20;
		}
	}

	con_draw_text_bpp(GRAM_FB, stride, 40, 100, "HELLO",
			  0x00ffffffU, 0x00101010U, 12, 24);

	st[0] = (char)('0' + (stage % 10));
	st[1] = 0;
	con_draw_text_bpp(GRAM_FB, stride, 40, 340, st,
			  0x00ffff00U, 0x00101010U, 12, 24);
	con_draw_text_bpp(GRAM_FB, stride, 180, 360, label,
			  0x0040e0d0U, 0x00101010U, 4, 24);

	{
		char b[16];
		disp_hex8(b, beat);
		for (int i = 0; i < 8; i++)
			bt[5 + i] = b[i];
		bt[13] = 0;
	}
	con_draw_text_bpp(GRAM_FB, stride, 40, 520, bt,
			  0x00ffffffU, 0x00101010U, 4, 24);

	disp_cache_clean(GRAM_FB, GRAM_FULL_BYTES);
	return gram_stream_buffer((const uint8_t *)GRAM_FB);
}

/* Heartbeat: repaint ONLY the 64 rows holding the BEAT line and stream
 * them as three DCS packets. The rest of the frame is left in the panel
 * GRAM, so the number advances on screen without repainting 5.5 MiB. */
static void __attribute__((unused)) gram_console_beat(unsigned beat)
{
	volatile uint32_t *scr = (volatile uint32_t *)GRAM_SCRATCH;
	uintptr_t bytes = (uintptr_t)GRAM_BEAT_ROWS * GRAM_FULL_ROW;
	uint32_t stride = GRAM_FULL_ROW;
	char b[16], bt[24] = "BEAT ";
	uint8_t r = (beat & 1) ? 0xff : 0x20;
	uint8_t g = (beat & 1) ? 0x20 : 0xff;

	for (uintptr_t i = 0; i < bytes / 4; i++)
		scr[i] = 0x10101010U;

	disp_hex8(b, beat);
	for (int i = 0; i < 8; i++)
		bt[5 + i] = b[i];
	bt[13] = 0;
	con_draw_text_bpp(GRAM_SCRATCH, stride, 40, 0, bt,
			  0x00ffffffU, 0x00101010U, 4, 24);

	/* Blinking strip at the bottom of the beat region. */
	for (unsigned y = 56; y < GRAM_BEAT_ROWS; y++) {
		volatile uint8_t *row = (volatile uint8_t *)
			(GRAM_SCRATCH + (uintptr_t)y * stride);
		for (unsigned x = 0; x < GRAM_FULL_W; x++) {
			row[3 * x + 0] = r;
			row[3 * x + 1] = g;
			row[3 * x + 2] = 0x20;
		}
	}
	disp_cache_clean(GRAM_SCRATCH, bytes);
	(void)gram_stream_rows((const uint8_t *)GRAM_SCRATCH,
			       GRAM_BEAT_Y0, GRAM_BEAT_ROWS);
}

/* Blinking full-width band for the older modes. 720 columns, otherwise
 * the panel advances 720 pixels per row and a narrow window shears. */
static void gram_beat(unsigned beat)
{
	volatile uint32_t *p = (volatile uint32_t *)GRAM_BEAT_FB;
	uintptr_t n = (uintptr_t)GRAM_BEAT_W * GRAM_BEAT_H * 3;
	uint8_t r = (beat & 1) ? 0xff : 0x20;
	uint8_t g = (beat & 1) ? 0x20 : 0xff;
	uint32_t pat[3];

	pat[0] = (uint32_t)r | ((uint32_t)g << 8) |
		 ((uint32_t)0x20 << 16) | ((uint32_t)r << 24);
	pat[1] = (uint32_t)g | ((uint32_t)0x20 << 8) |
		 ((uint32_t)r << 16) | ((uint32_t)g << 24);
	pat[2] = (uint32_t)0x20 | ((uint32_t)r << 8) |
		 ((uint32_t)g << 16) | ((uint32_t)0x20 << 24);
	for (uintptr_t i = 0; i < n / 4; i += 3) {
		p[i + 0] = pat[0];
		p[i + 1] = pat[1];
		p[i + 2] = pat[2];
	}
	disp_cache_clean(GRAM_BEAT_FB, n);
	(void)gram_stream_rows((const uint8_t *)GRAM_BEAT_FB,
			       GRAM_SQUARE_Y0, GRAM_BEAT_H);
}

/* Fill AT MOST size bytes: rows are clamped to what the window holds. */
static void disp_fill_bounded(uintptr_t base, uintptr_t size,
			      uint32_t stride, uint32_t color)
{
	volatile uint32_t *p = (volatile uint32_t *)base;
	uint32_t rows = size / stride;
	uintptr_t words;

	if (rows > DISP_FB_H)
		rows = DISP_FB_H;
	words = ((uintptr_t)rows * stride) / 4;
	for (uintptr_t i = 0; i < words; i++)
		p[i] = color;
	con_hex2("FB", "rows=", rows, "bytes=", (uint32_t)(words * 4));
}

static void disp_text(uintptr_t fb, uint32_t stride, int x, int y,
		      const char *s, uint32_t fg, int scale, int bpp)
{
	con_draw_text_bpp(fb, stride, x, y, s, fg, 0xff000000U, scale, bpp);
}

static void disp_hex8(char *buf, uint32_t v)
{
	static const char hex[] = "0123456789ABCDEF";

	for (int i = 0; i < 8; i++)
		buf[i] = hex[(v >> (28 - 4 * i)) & 0xf];
	buf[8] = 0;
}

static void disp_beat_line(uintptr_t fb, uint32_t stride,
			   unsigned beat, int bpp)
{
	char b[16], line[24] = "BEAT ";

	disp_hex8(b, beat);
	for (int i = 0; i < 8; i++)
		line[5 + i] = b[i];
	line[13] = 0;
	disp_text(fb, stride, 40, 200, line, 0xffffffffU, 4, bpp);
}

/*
 * Paint the whole screen for a stage, BEFORE the risky step runs. A bus
 * hang inside that step leaves the number of the step on the panel: this
 * is the on-screen answer to "step 3 or step 7".
 */
static void __attribute__((unused)) disp_screen_paint(unsigned stage,
			unsigned beat, const char *label)
{
	uintptr_t fb, size;
	uint32_t stride;
	int bpp;
	char st[3];

	if (SHIM_DISPLAY_MODE == 3) {
		fb = DISP_FB_BASE;
		stride = DISP_FB_W * 4;
		bpp = 32;
		size = (uintptr_t)DISP_FB_H * stride;
	} else {
		fb = DISP_SPLASH_BASE;
		stride = DISP_FB_W * DISP_FB_BYTES;
		bpp = DISP_FB_BPP;
		size = DISP_SPLASH_SIZE;
	}

	disp_fill_bounded(fb, size, stride, disp_stage_color(stage));
	disp_text(fb, stride, 40, 40, "HELLO", 0xffffffffU, 6, bpp);

	st[0] = (char)('0' + (stage % 10));
	st[1] = 0;
	disp_text(fb, stride, 40, 140, st, 0xffffff00U, 6, bpp);
	disp_text(fb, stride, 140, 140, label, 0xff40e0d0U, 3, bpp);
	disp_beat_line(fb, stride, beat, bpp);

	disp_cache_clean(fb, size);
	con_hex2("SCR", "stage=", stage, "beat=", beat);
}

/*
 * Eight full-width colour bars, 320 lines each, RGB888 with the stride LK
 * left in RGB0/RGB1 (0x10E0 = 4320). A garbled frame still shows bands, so
 * this separates "MDP reads our buffer" from "MDP reads some other memory".
 */
static void __attribute__((unused)) disp_bars(uintptr_t base)
{
	static const uint8_t colors[8][3] = {
		{0xff, 0x00, 0x00}, {0x00, 0xff, 0x00}, {0x00, 0x00, 0xff},
		{0xff, 0xff, 0xff}, {0x00, 0x00, 0x00}, {0x00, 0xff, 0xff},
		{0xff, 0x00, 0xff}, {0xff, 0xff, 0x00}
	};
	volatile uint8_t *p = (volatile uint8_t *)base;
	uint32_t stride = DISP_FB_W * 3;

	for (unsigned band = 0; band < 8; band++) {
		for (unsigned y = 0; y < 320; y++) {
			volatile uint8_t *row = p +
				(uintptr_t)(band * 320 + y) * stride;
			for (unsigned x = 0; x < DISP_FB_W; x++) {
				row[3 * x + 0] = colors[band][0];
				row[3 * x + 1] = colors[band][1];
				row[3 * x + 2] = colors[band][2];
			}
		}
	}
	disp_cache_clean(base, (uintptr_t)DISP_FB_H * stride);
	con_hex2("BARS", "addr=", (uint32_t)base,
		 "bytes=", (uint32_t)((uintptr_t)DISP_FB_H * stride));
}

/* Called from the 20 s hold loop: only the beat line is redrawn. */
void disp_screen_beat(unsigned beat)
{
	uintptr_t fb;
	uint32_t stride;
	int bpp;

	if (SHIM_DISPLAY_MODE == 0)
		return;			/* baseline: no writes at all */

	/* Mode 4 does not have a DDR framebuffer at all: the heartbeat is
	 * a DCS pixel packet pushed straight into the panel GRAM. If the
	 * probe failed there is nothing to blink, so stay quiet. */
	if (SHIM_DISPLAY_MODE == 13) {
		gpu_console_beat(beat);
		con_hex1("BEAT", "n=", beat);
		return;
	}

	if (SHIM_DISPLAY_MODE == 12) {
		gram_band_beat(beat);
		con_hex1("BEAT", "n=", beat);
		return;
	}

	if (SHIM_DISPLAY_MODE == 11) {
		gram_console_beat(beat);
		con_hex1("BEAT", "n=", beat);
		return;
	}

	if (SHIM_DISPLAY_MODE == 4 || SHIM_DISPLAY_MODE == 7 ||
	    SHIM_DISPLAY_MODE == 9 || SHIM_DISPLAY_MODE == 10) {
		if (s_gram_ready)
			gram_beat(beat);
		con_hex1("BEAT", "n=", beat);
		return;
	}

	/* Mode 5 keeps BOTH candidate paths alive during the hold: repaint
	 * cont_splash (MDP), kick a frame, and blink the DCS square (GRAM).
	 * The FRB lines say whether MDP actually played any frame. */
	if (SHIM_DISPLAY_MODE == 5) {
		uint32_t stride = DISP_FB_W * DISP_FB_BYTES;

		disp_beat_line(DISP_SPLASH_BASE, stride, beat, DISP_FB_BPP);
		disp_cache_clean(DISP_SPLASH_BASE + (uintptr_t)200 * stride,
				 (uintptr_t)80 * stride);
		dsi_soft_trigger();
		if (s_gram_ready)
			gram_beat(beat);
		if ((beat & 7) == 0)
			disp_frame_count("FRB");
		con_hex1("BEAT", "n=", beat);
		return;
	}

	/* Mode 6: the panel is held OFF for the whole inspection. Restore
	 * display-on at the very last beat so the reboot into recovery does
	 * not inherit a blank panel if LK does not re-init it. */
	if (SHIM_DISPLAY_MODE == 6) {
		if (beat == 39)
			(void)dsi_dcs_write(0x29, 0, 0, 20);
		con_hex1("BEAT", "n=", beat);
		return;
	}

	if (SHIM_DISPLAY_MODE == 3) {
		fb = DISP_FB_BASE;
		stride = DISP_FB_W * 4;
		bpp = 32;
	} else {
		fb = DISP_SPLASH_BASE;
		stride = DISP_FB_W * DISP_FB_BYTES;
		bpp = DISP_FB_BPP;
	}

	disp_beat_line(fb, stride, beat, bpp);
	disp_cache_clean(fb + 200 * stride, 80 * stride);
	if (SHIM_DISPLAY_MODE >= 2)
		dsi_soft_trigger();
	con_hex1("BEAT", "n=", beat);
}

/*
 * The readback.
 *
 * The values of these registers on a phone that is driving the panel are
 * known (docs/display-2026-11-10.txt), so reading them back after the
 * bring-up says, in one line per register, whether the shim programmed
 * the hardware the way the kernel does it. The reads are safe (the
 * clocks are on) and the values go both into the ASCII log and into the
 * trail, so they survive even a run that hangs afterwards.
 */
static void disp_dump(const char *tag)
{
	/*
	 * The table is built on the stack, not in the data: the image runs
	 * wherever the bootloader puts it, with no relocation pass, so an
	 * absolute address in .data is not allowed (the build fails on it).
	 */
	struct reg {
		const char *name;
		uintptr_t addr;
	};
	/* 2026-11-15: this table had 24 slots but 34 REG() calls. The last
	 * ten writes went past the end of the stack frame (gcc warned
	 * -Waggressive-loop-optimizations). Bounds-check every entry. */
#define DISP_DUMP_MAX	64
	struct reg regs[DISP_DUMP_MAX];
	unsigned n = 0;

#define REG(nm, a)	do { \
		if (n < DISP_DUMP_MAX) { \
			regs[n].name = (nm); \
			regs[n].addr = (a); \
			n++; \
		} \
	} while (0)

	REG("DSI.ctrl",	DSI_BASE + DSI_OFF(DSI_CTRL));
	/* 2026-11-15: the BOOT dump only ever read DSI0. On a dual-DSI
	 * panel the second link is half the picture: if DSI1 is disabled
	 * or clocked off, no amount of MDP work will show a full frame.
	 * Both are read AFTER clocks_display_core(), so the AHB access
	 * completes (a read with the clock off wedges the bus). */
	REG("DSI1.ctrl",  DSI1_BASE + DSI_OFF(DSI_CTRL));
	REG("DSI1.clkctl", DSI1_BASE + DSI_OFF(DSI_CLK_CTRL));
	REG("DSI1.trig",  DSI1_BASE + DSI_OFF(DSI_TRIG_CTRL));
	REG("DSI.mdpctl", DSI_BASE + DSI_OFF(DSI_COMMAND_MODE_MDP_CTRL));
	REG("DSI.dcsctl", DSI_BASE + DSI_OFF(DSI_COMMAND_MODE_MDP_DCS_CMD_CTRL));
	REG("DSI.strm0c", DSI_BASE + DSI_OFF(DSI_COMMAND_MODE_MDP_STREAM0_CTRL));
	REG("DSI.strm0t", DSI_BASE + DSI_OFF(DSI_COMMAND_MODE_MDP_STREAM0_TOTAL));
	REG("DSI.trig",	DSI_BASE + DSI_OFF(DSI_TRIG_CTRL));
	REG("DSI.clkctl", DSI_BASE + DSI_OFF(DSI_CLK_CTRL));
	REG("DSI.timing", DSI_BASE + DSI_OFF(DSI_CLKOUT_TIMING_CTRL));
	/* 2026-11-16: the D4 run proved the DCS DMA engine completes on both
	 * links but NOTHING appeared on the glass. These are the registers
	 * that say whether the packet actually left the host: LP/HS mode,
	 * command engine busy, ACK errors from the panel, and the interrupt
	 * status we poll. One line each, for both links. */
	REG("DSI.dmactl", DSI_BASE + DSI_OFF(DSI_COMMAND_MODE_DMA_CTRL));
	REG("DSI.status", DSI_BASE + DSI_OFF(DSI_STATUS));
	REG("DSI.fifo",   DSI_BASE + DSI_OFF(DSI_FIFO_STATUS));
	REG("DSI.ackerr", DSI_BASE + DSI_OFF(DSI_ACK_ERR_STATUS));
	REG("DSI.intctl", DSI_BASE + DSI_OFF(DSI_INT_CTRL));
	REG("DSI.rdbk0",  DSI_BASE + DSI_OFF(DSI_RDBK_DATA0));
	REG("DSI1.dmactl", DSI1_BASE + DSI_OFF(DSI_COMMAND_MODE_DMA_CTRL));
	REG("DSI1.status", DSI1_BASE + DSI_OFF(DSI_STATUS));
	REG("DSI1.ackerr", DSI1_BASE + DSI_OFF(DSI_ACK_ERR_STATUS));
	REG("DSI1.intctl", DSI1_BASE + DSI_OFF(DSI_INT_CTRL));
	REG("PHY.str0",	DSI_PHY_BASE + DSI_PHY_STRENGTH_CTRL_0);
	REG("PHY.reg0",	DSI_PHY_BASE + DSI_PHY_REGULATOR_CTRL_0);
	REG("PHY.lane0", DSI_PHY_BASE + DSI_PHY_LANE_BASE);
	REG("PLL.rsm",	DSI_PHY_BASE + PLL_RESET_SM);
	REG("CTL.top",	CTL_BASE + CTL_TOP);
	REG("CTL.flush", CTL_BASE + CTL_FLUSH);
	REG("CTL.layer0", CTL_BASE + CTL_LAYER(0));
	
	/* RGB0 and RGB1 - CRITICAL to see where bootloader left them pointing */
	REG("RGB0.size", MDP_BASE + 0x15000UL + SSPP_SRC_SIZE);
	REG("RGB0.fmt",  MDP_BASE + 0x15000UL + SSPP_SRC_FORMAT);
	REG("RGB0.addr", MDP_BASE + 0x15000UL + SSPP_SRC0_ADDR);
	REG("RGB0.strd", MDP_BASE + 0x15000UL + SSPP_SRC_YSTRIDE0);
	REG("RGB1.size", MDP_BASE + 0x17000UL + SSPP_SRC_SIZE);
	REG("RGB1.fmt",  MDP_BASE + 0x17000UL + SSPP_SRC_FORMAT);
	REG("RGB1.addr", MDP_BASE + 0x17000UL + SSPP_SRC0_ADDR);
	REG("RGB1.strd", MDP_BASE + 0x17000UL + SSPP_SRC_YSTRIDE0);
	
	REG("RGB2.size", RGB2_BASE + SSPP_SRC_SIZE);
	REG("RGB2.fmt",	RGB2_BASE + SSPP_SRC_FORMAT);
	REG("RGB2.unpk", RGB2_BASE + SSPP_SRC_UNPACK);
	REG("RGB2.addr", RGB2_BASE + SSPP_SRC0_ADDR);
	REG("RGB2.strd", RGB2_BASE + SSPP_SRC_YSTRIDE0);
	
	/* INTF1 - might be the real interface for dual-DSI */
	REG("INTF1.tgen", MDP_BASE + 0x6b000UL + INTF_TIMING_ENGINE_EN);
	REG("INTF1.pfmt", MDP_BASE + 0x6b000UL + INTF_PANEL_FORMAT);
	REG("INTF1.under", MDP_BASE + 0x6b000UL + INTF_UNDERFLOW_COLOR);
	
	REG("INTF2.tgen", INTF2_BASE + INTF_TIMING_ENGINE_EN);
	REG("INTF2.pfmt", INTF2_BASE + INTF_PANEL_FORMAT);
	REG("INTF1.frmc", INTF1_BASE + INTF_FRAME_COUNT);
	REG("INTF2.frmc", INTF2_BASE + INTF_FRAME_COUNT);
	REG("PP0.tear",   PP0_BASE + PP_TEAR_CONFIG);
	REG("PP0.autorf", PP0_BASE + PP_AUTOREFRESH_CONFIG);
	REG("LM0.outsz", LM0_BASE + 0x004);
#undef REG

	for (unsigned i = 0; i < n; i++) {
		uint32_t v = rd(regs[i].addr);

		con_hex1(tag, regs[i].name, v);
		trail(0x2740 + (uint32_t)i, v);
	}
}

/* ------------------------------------------------------------------ */
/* The test                                                            */
/* ------------------------------------------------------------------ */

uint32_t display_test(void)
{
	uint32_t done = 0;

	con_hex2("BUILD", "id=", (uint32_t)G4_BUILD_ID, "mode=", SHIM_DISPLAY_MODE);
	con_hex2("BUILD", "stage=", SHIM_DISPLAY_STAGE, "bpp=", DISP_FB_BPP);
	disp_mark("display test: start");

	/*
	 * MODE 0 - BASELINE. Zero display MMIO reads and zero writes. If the
	 * LG logo is not stable here, nothing that follows is interpretable.
	 */
	if (SHIM_DISPLAY_MODE == 0) {
		disp_mark("D0 baseline: no display reads, no display writes");
		return DISP_OK_BASELINE;
	}

	/*
	 * MODE 1/2 - MINIMAL TOUCH. Only DRAM inside the 12 MiB window, plus
	 * the text. No clock, no PLL, no MDP register is touched. The one
	 * question: is LK still scanning cont_splash at handoff?
	 */
	if (SHIM_DISPLAY_MODE == 1 || SHIM_DISPLAY_MODE == 2) {
		if (SHIM_DISPLAY_MODE == 2) {
			/* 2026-11-15: D1b used to write DSI_CMD_MODE_MDP_SW_TRIGGER
			 * with the MDSS clocks still OFF. A write to a window whose
			 * clock is off does not complete and the CPU stops inside
			 * the access (two hangs on 2026-11-09). Turn the core clocks
			 * on FIRST, and take the long-missing BOOT dump while we are
			 * here: it says which pipe and which address LK left live.
			 * D1b is therefore no longer "zero MMIO" - it is D1 + core
			 * clocks + read-only dump + one frame trigger. */
			disp_mark("D1b: MMSS core clocks before any DSI write");
			if (clocks_display_core() < 0)
				return done;
			done |= DISP_OK_CLOCKS;
			disp_dump("BOOT");
			disp_mark("D1b: BOOT dump done");
		}
		disp_screen_paint(1, 0, "D1 minimal touch");
		done |= DISP_OK_DRAWN;
		disp_mark("D1: bounded fill + HELLO in cont_splash");
		if (SHIM_DISPLAY_MODE == 2) {
			disp_mark("D1b: DSI software trigger");
			dsi_soft_trigger();
			done |= DISP_OK_DSI;
		}
		disp_mark("D1: done, waiting for the eye");
		return done;
	}

	/*
	 * MODE 4 - THE DECISIVE EXPERIMENT (added after D0/D1, 2026-11-15).
	 *
	 * D1 proved that the panel is NOT scanning cont_splash: 10.5 MiB of
	 * DDR changed, 40 heartbeats ran, and not one pixel moved. On a DSI
	 * command-mode panel the picture lives in the panel's own GRAM and
	 * the panel self-refreshes with the MDSS clocks off.
	 *
	 * So instead of rebuilding MDP -> CTL -> LM -> INTF -> DSI (five
	 * blocks plus an SMMU/IOVA), go straight to the panel with ONE DCS
	 * memory write. If it shows up, we have a display channel and the
	 * whole MDP problem becomes optional. If it does not, the DSI link
	 * itself is off at handover and the full bring-up is unavoidable.
	 *
	 * DISPLAY_STAGE bisects it:
	 *   1  core clocks + read-only BOOT dump (pure measurement)
	 *   2  + PLL/pixel clocks + DCS 0x11/0x29 probe on both links
	 *   3  + 0x2A/0x2B window and HELLO via 0x2C into the GRAM
	 */
	if (SHIM_DISPLAY_MODE == 4) {
		disp_mark("D4: DCS direct to panel GRAM, MDP untouched");
#if SHIM_DISPLAY_STAGE >= 1
		disp_mark("D4 step 1: MMSS core clocks + BOOT dump");
		if (clocks_display_core() < 0)
			return done;
		done |= DISP_OK_CLOCKS;
		disp_dump("BOOT");
		disp_mark("D4 step 1 done");
#endif
#if SHIM_DISPLAY_STAGE >= 2
		disp_mark("D4 step 2: read the links, then probe with DCS");
		{
			const uint32_t want = DSI_EN | DSI_CMD_MODE_EN | DSI_CLK_EN;
			uint32_t c0 = dsi0_r(DSI_CTRL);
			uint32_t c1 = dsi1_r(DSI_CTRL);

			con_hex2("D4", "ctrl0=", c0, "ctrl1=", c1);
			/*
			 * If LK left both controllers enabled in command mode,
			 * do NOT touch the PLL or the pixel/byte clocks: re-
			 * configuring a live RCG can blank the panel and would
			 * mix two variables into one experiment. Only repair
			 * the clock tree when the links are actually down.
			 */
			if ((c0 & want) == want && (c1 & want) == want) {
				disp_mark("D4: both links live, no clock reconfig");
			} else {
				disp_mark("D4: links down, enabling PLL + pixel clocks");
				if (dsi_pll_enable() < 0)
					return done;
				if (clocks_display_pixel() < 0)
					return done;
			}
		}
		done |= DISP_OK_PLL;
		if (dsi_dcs_write(0x11, 0, 0, 120) < 0)
			return done;
		if (dsi_dcs_write(0x29, 0, 0, 20) < 0)
			return done;
		done |= DISP_OK_PANEL_ON;
		disp_mark("D4 step 2 done: DSI command engine answers");
#endif
#if SHIM_DISPLAY_STAGE >= 3
		disp_mark("D4 step 3: HELLO via DCS 0x2C into GRAM");
		if (gram_hello() < 0)
			return done;
		s_gram_ready = 1;
		done |= DISP_OK_DRAWN | DISP_OK_GRAM;
		disp_mark("D4 step 3 done: no MDP/CTL/LM/INTF written");
#endif
		return done;
	}

	/*
	 * MODE 5 - RE-ARM THE PIPELINE LK LEFT BEHIND (2026-11-16).
	 *
	 * The D4 BOOT dump was the measurement that was missing from all
	 * seven earlier tests, and it says something unexpected: the MDP
	 * pipeline is NOT dead. It is fully configured and pointing at the
	 * very buffer D1 filled:
	 *
	 *   RGB0.addr = RGB1.addr = 0x03400000, strd 0x10E0 (4320),
	 *   size 0x0A0002D0 (720x2560), fmt 0x0002243F; DSI.ctrl 0x1F5
	 *   on BOTH links; clkctl 0x23F; mdpctl 0x08; dcsctl 0x00013C2C.
	 *
	 * Those are the working kernel's values. What is DIFFERENT is
	 * DSI.trig = 0x00000004 on both links, versus 0x80000004 in the
	 * working kernel (old/displayTest.md:262-263). BIT(31) is te_sel
	 * (mdss_dsi_host.c:369-370, DTS te-pin-select=<1>): LK left the TE
	 * source deselected and the MDP frame path frozen. The panel keeps
	 * showing the last frame in its GRAM.
	 *
	 * So instead of rebuilding MDP (what tests 1-7 did, breaking LK's
	 * good configuration), this mode only:
	 *   1. paints cont_splash AND the panel GRAM with the same HELLO;
	 *   2. clears BIT(26) of DSI_COMMAND_MODE_DMA_CTRL (LP -> HS) so
	 *      15 KiB DCS pixel packets can actually be delivered;
	 *   3. sets BIT(31) of DSI_TRIG_CTRL on both links (kernel value);
	 *   4. kicks MDP_SW_TRIGGER on both links, once and every 500 ms.
	 *
	 * No CTL_FLUSH, no CTL_START, no RGB/LM/INTF writes: LK's pipeline
	 * is preserved. Whichever path comes alive, it shows HELLO. The FR*
	 * lines give a numeric readout of frames actually played.
	 */
	if (SHIM_DISPLAY_MODE == 5) {
		disp_mark("D5: re-arm LK pipeline + HS DCS, ZERO MDP writes");
#if SHIM_DISPLAY_STAGE >= 1
		if (clocks_display_core() < 0)
			return done;
		done |= DISP_OK_CLOCKS;
		disp_dump("BOOT");
		disp_frame_count("FR0");
		disp_mark("D5 step 1 done: extended BOOT dump");
#endif
#if SHIM_DISPLAY_STAGE >= 2
		dsi_force_hs_mode();
		dsi_select_te();
		done |= DISP_OK_DSI;
		disp_mark("D5 step 2 done: HS + TE select on both links");
#endif
#if SHIM_DISPLAY_STAGE >= 3
		disp_screen_paint(1, 0, "D5 MDP+GRAM");
		done |= DISP_OK_DRAWN;
		if (gram_hello() == 0) {
			s_gram_ready = 1;
			done |= DISP_OK_GRAM;
		} else {
			disp_mark("D5: DCS GRAM path failed, MDP path still armed");
		}
		disp_frame_count("FR1");
		dsi_soft_trigger();
		delay_ms(30);
		disp_frame_count("FR2");
		disp_mark("D5 done: both sources painted and kicked");
#endif
		return done;
	}

	/*
	 * MODE 6 - THE CONTACT PROBE (fallback if D5 is still dark).
	 *
	 * CMD_DMA_DONE is a HOST-side completion: it says the DMA engine
	 * moved the packet into the DSI FIFO, NOT that the panel obeyed it.
	 * D4 logged 25 packets + 40 beats with done0=done1=1 and zero visual
	 * change, so this is the one question left: does the panel obey ANY
	 * DCS command? DCS 0x28 (display off) is the panel's own off-command
	 * in the DTS and is sent in HS mode. If the LG logo goes BLACK for
	 * the whole 20 s hold, the panel is listening and the problem is
	 * confined to the GRAM window / page / pixel format. If the logo
	 * stays, the packets never leave the host and the DSI command path
	 * (LP/HS, DMA address, trigger) is where to look.
	 */
	if (SHIM_DISPLAY_MODE == 6) {
		const uint8_t off_param = 0x00;

		disp_mark("D6: DCS contact probe, display OFF for the hold");
		if (clocks_display_core() < 0)
			return done;
		done |= DISP_OK_CLOCKS;
		disp_dump("BOOT");
		dsi_force_hs_mode();
		if (dsi_dcs_write(0x28, &off_param, 1, 0) < 0)
			return done;
		disp_mark("D6: 0x28 sent - screen must go BLACK if panel obeys DCS");
		return done;
	}

	/*
	 * MODE 7 - CLEAN HELLO (2026-11-16, after D5 produced visible noise).
	 *
	 * D5 stage 3 was the first run that changed the glass: a full screen
	 * of salt-and-pepper static. That proves the DCS path reaches the
	 * panel. Static is the classic symptom of a pixel-format mismatch:
	 * 3 bytes per pixel fed to a panel configured for RGB565/RGB666.
	 * LK never told us the format, and the DTS on-command writes 0x3A
	 * with several different values on manufacturer pages.
	 *
	 * This mode does NOT set te_sel: that keeps the MDP frame path
	 * untouched, so anything visible comes from DCS alone. It forces HS,
	 * wakes the panel, explicitly sets DCS 0x3A = 0x77 (24bpp), and
	 * writes HELLO at page address 0 (top of the display).
	 */
	if (SHIM_DISPLAY_MODE == 7) {
		disp_mark("D7: HS DCS + 0x3A 24bpp + HELLO at top, no TE change");
		if (clocks_display_core() < 0)
			return done;
		done |= DISP_OK_CLOCKS;
		disp_dump("BOOT");
		dsi_force_hs_mode();
		if (dsi_dcs_write(0x11, 0, 0, 120) < 0)
			return done;
		if (dsi_dcs_write(0x29, 0, 0, 20) < 0)
			return done;
		if (gram_set_format() < 0)
			return done;
		done |= DISP_OK_PANEL_ON;
		disp_mark("D7: pixel format set to 24bpp on both links");

		disp_screen_paint(1, 0, "D7 clean");
		done |= DISP_OK_DRAWN;
		if (gram_hello() == 0) {
			s_gram_ready = 1;
			done |= DISP_OK_GRAM;
		}
		con_hex2("ACK", "dsi0=", dsi0_r(DSI_ACK_ERR_STATUS),
			 "dsi1=", dsi1_r(DSI_ACK_ERR_STATUS));
		disp_mark("D7 done: HELLO at page 0");
		return done;
	}

	/*
	 * MODE 13 - HARDWARE (MDP) CONSOLE + BOOT DIAGNOSTICS (2026-11-16).
	 *
	 * Framebuffer console at DD_FB_BASE in LK's geometry, hardware frame
	 * kicks, explicit GPU-vs-CPU verdict from the INTF frame counters,
	 * and a step line painted at every shim_checkpoint() from the very
	 * start of boot. See display-data.h for every constant.
	 */
	if (SHIM_DISPLAY_MODE == 13) {
		disp_mark("D13: hardware MDP console + boot diagnostics");
		if (!s_boot_ready)
			disp_boot_init();
		if (!s_boot_ready)
			return done;
		done |= DISP_OK_CLOCKS | DISP_OK_PANEL_ON;
		/* No HELLO: the framebuffer console IS the diagnostic. One
		 * final frame makes the last step visible and stable. */
		disp_gpu_try(s_gpu_kick_mode, "FINAL");
		done |= DISP_OK_DRAWN | DISP_OK_GRAM;
		con_str("DISP", "D13: boot diagnostics only, final frame kicked");
		disp_mark("D13 done");
		return done;
	}

	/*
	 * MODE 12 - ALIGNED, FAST CONSOLE (2026-11-16).
	 *
	 * Same DCS channel that mode 11 proved, but every packet sets its
	 * own 0x2A/0x2B window and starts with 0x2C, so no GRAM pointer
	 * state is carried between packets. Only the 600 rows that hold
	 * the console are streamed: 1,296,000 bytes instead of 5,529,600.
	 */
	if (SHIM_DISPLAY_MODE == 12) {
		disp_mark("D12: aligned band console, fresh window per packet");
		if (clocks_display_core() < 0)
			return done;
		done |= DISP_OK_CLOCKS;
		dsi_force_hs_mode();
		if (dsi_dcs_write(0x11, 0, 0, 120) < 0)
			return done;
		if (dsi_dcs_write(0x29, 0, 0, 20) < 0)
			return done;
		if (gram_set_format() < 0)
			return done;
		done |= DISP_OK_PANEL_ON;
		if (gram_console_band(1, 0, "SHIM CONSOLE") == 0) {
			done |= DISP_OK_DRAWN | DISP_OK_GRAM;
			con_str("DISP", "D12: aligned console streamed");
		}
		con_hex2("ACK", "dsi0=", dsi0_r(DSI_ACK_ERR_STATUS),
			 "dsi1=", dsi1_r(DSI_ACK_ERR_STATUS));
		disp_mark("D12 done");
		return done;
	}

	/*
	 * MODE 11 - THE VISIBLE CONSOLE (2026-11-16).
	 *
	 * D10 filled the whole panel with red, top to bottom, left to right:
	 * the GRAM is writable through DCS. The earlier static came from a
	 * 640-column window on a 720-column panel, which displaced every
	 * row by 240 bytes and produced a deterministic diagonal mess.
	 *
	 * Mode 11 renders HELLO + stage + label + heartbeat into a full
	 * 720x2560 RGB888 buffer and streams it in packets of 7 whole rows.
	 * No MDP register, no CTL_FLUSH, no MDP_SW_TRIGGER, no SMMU address.
	 */
	if (SHIM_DISPLAY_MODE == 11) {
		disp_mark("D11: full-frame HELLO console over DCS");
		if (clocks_display_core() < 0)
			return done;
		done |= DISP_OK_CLOCKS;
		disp_dump("BOOT");
		dsi_force_hs_mode();
		if (dsi_dcs_write(0x11, 0, 0, 120) < 0)
			return done;
		if (dsi_dcs_write(0x29, 0, 0, 20) < 0)
			return done;
		if (gram_set_format() < 0)
			return done;
		done |= DISP_OK_PANEL_ON;
		if (gram_console(1, 0, "SHIM CONSOLE") == 0) {
			done |= DISP_OK_DRAWN | DISP_OK_GRAM;
			con_str("DISP", "D11: HELLO streamed to panel GRAM");
		}
		con_hex2("ACK", "dsi0=", dsi0_r(DSI_ACK_ERR_STATUS),
			 "dsi1=", dsi1_r(DSI_ACK_ERR_STATUS));
		disp_mark("D11 done");
		return done;
	}

	/*
	 * MODE 10 - FULL-SCREEN SOLID COLOUR OVER DCS (2026-11-16).
	 *
	 * D6 kept the panel BLACK for 20 s after a single 0x28, so DCS
	 * commands do reach the panel. D7/D9 wrote a 640x200 HELLO region
	 * and produced a deterministic static picture instead. That points
	 * at the GRAM window / pixel format, not at the link.
	 *
	 * Mode 10 removes every remaining variable: one 720x2560 window,
	 * one solid colour, ~338 0x2C/0x3C packets, no text, no MDP frame
	 * kick, no TE change. If the whole glass turns red, the panel GRAM
	 * is writable and HELLO is a font problem only.
	 */
	if (SHIM_DISPLAY_MODE == 10) {
		disp_mark("D10: full GRAM solid red, no MDP kicks");
		if (clocks_display_core() < 0)
			return done;
		done |= DISP_OK_CLOCKS;
		disp_dump("BOOT");
		dsi_force_hs_mode();
		if (dsi_dcs_write(0x11, 0, 0, 120) < 0)
			return done;
		if (dsi_dcs_write(0x29, 0, 0, 20) < 0)
			return done;
		if (gram_set_format() < 0)
			return done;
		done |= DISP_OK_PANEL_ON;
		if (gram_full_fill(0xff, 0x00, 0x00) == 0)
			done |= DISP_OK_DRAWN | DISP_OK_GRAM;
		con_hex2("ACK", "dsi0=", dsi0_r(DSI_ACK_ERR_STATUS),
			 "dsi1=", dsi1_r(DSI_ACK_ERR_STATUS));
		disp_mark("D10 done: full GRAM written");
		return done;
	}

	/*
	 * MODE 9 - DCS ONLY (2026-11-16).
	 *
	 * D7 proved that HS + 0x3A + DCS reaches the panel: the screen
	 * changed. But D7 still let the hold loop call MDP_SW_TRIGGER every
	 * 500 ms (it does that for every mode >= 2). A command-mode MDP frame
	 * pushes a whole new picture into the panel GRAM and would overwrite
	 * the HELLO we just wrote with DCS. That is the likely source of the
	 * deterministic static.
	 *
	 * Mode 9 is D7 without a single MDP frame kick: no cont_splash paint,
	 * no MDP_SW_TRIGGER anywhere, only DCS 0x3A + 0x2C/0x3C into the GRAM
	 * and the DCS blinking square for the heartbeat. If HELLO appears,
	 * the display channel is solved.
	 */
	if (SHIM_DISPLAY_MODE == 9) {
		disp_mark("D9: DCS only, ZERO MDP frame kicks");
		if (clocks_display_core() < 0)
			return done;
		done |= DISP_OK_CLOCKS;
		disp_dump("BOOT");
		dsi_force_hs_mode();
		if (dsi_dcs_write(0x11, 0, 0, 120) < 0)
			return done;
		if (dsi_dcs_write(0x29, 0, 0, 20) < 0)
			return done;
		if (gram_set_format() < 0)
			return done;
		done |= DISP_OK_PANEL_ON;
		if (gram_hello() == 0) {
			s_gram_ready = 1;
			done |= DISP_OK_DRAWN | DISP_OK_GRAM;
		}
		con_hex2("ACK", "dsi0=", dsi0_r(DSI_ACK_ERR_STATUS),
			 "dsi1=", dsi1_r(DSI_ACK_ERR_STATUS));
		disp_mark("D9 done: no MDP register or trigger touched");
		return done;
	}

	/*
	 * MODE 8 - BIG BARS (2026-11-16).
	 *
	 * D5 and D7 both changed the glass to the SAME deterministic static
	 * picture on every boot. D1 and D4 did not. The only action common
	 * to D5/D7 and absent in D1/D4 is the MDP_SW_TRIGGER that the hold
	 * loop issues for every mode >= 2. So MDP is playing frames again,
	 * but what it fetches is not what this shim paints: either a different
	 * address (SMMU/IOVA) or a different pixel format/stride.
	 *
	 * This mode changes NO register except the core clocks and the frame
	 * kick. It fills cont_splash with eight unmistakable colour bars using
	 * the exact stride LK left in RGB0/RGB1, and also fills 0x20000000 in
	 * case MDP reads the shim framebuffer instead. If the bars appear, the
	 * MDP path is ours and the console can be built on it. If the static
	 * is unchanged, MDP is reading a third address and the SMMU mapping
	 * has to be measured.
	 */
	if (SHIM_DISPLAY_MODE == 8) {
		disp_mark("D8: big colour bars + MDP frame kick, no DCS");
		if (clocks_display_core() < 0)
			return done;
		done |= DISP_OK_CLOCKS;
		disp_dump("BOOT");
		disp_bars(DISP_SPLASH_BASE);
		disp_bars(DISP_FB_BASE);
		done |= DISP_OK_DRAWN;
		disp_frame_count("FR0");
		dsi_soft_trigger();
		delay_ms(30);
		disp_frame_count("FR1");
		disp_mark("D8 done: bars painted, frame kicked");
		return done;
	}

	/*
	 * MODE 3 - FULL BRING-UP, bisected by SHIM_DISPLAY_STAGE.
	 * Every stage paints its number BEFORE touching the hardware and
	 * writes the same line into ramoops: a hang is then readable both on
	 * the panel and in the log.
	 */
#if SHIM_DISPLAY_STAGE >= 1
	disp_screen_paint(1, 0, "clocks");
	disp_mark("step 1: MMSS core clocks");
	if (clocks_display_core() < 0)
		return done;
	done |= DISP_OK_CLOCKS;
#endif
#if SHIM_DISPLAY_STAGE >= 2
	disp_screen_paint(2, 0, "dsi pll");
	disp_mark("step 2: DSI PHY PLL + pixel clocks");
	if (dsi_pll_enable() < 0)
		return done;
	if (clocks_display_pixel() < 0)
		return done;
	done |= DISP_OK_PLL;
	/* First safe register read: the clocks are on and no display
	 * register has been written yet. This is the BOOT dump that was
	 * missing from all seven earlier tests. */
	disp_dump("BOOT");
#endif
#if SHIM_DISPLAY_STAGE >= 3
	disp_screen_paint(3, 0, "dsi ctrl");
	disp_mark("step 3: DSI PHY + controller");
	dsi_phy_init();
	dsi_ctrl_init_cmd_mode();
	done |= DISP_OK_DSI;
#endif
#if SHIM_DISPLAY_STAGE >= 4
	disp_screen_paint(4, 0, "mdp");
	disp_mark("step 4: MDP CTL/RGB2/INTF2");
	if (disp_mdp_init() < 0)
		return done;
	done |= DISP_OK_MDP;
#endif
#if SHIM_DISPLAY_STAGE >= 5
	disp_screen_paint(5, 0, "frame");
	disp_mark("step 5: frame + commit");
	done |= DISP_OK_DRAWN;
	disp_mdp_commit();
	disp_screen_paint(6, 0, "panel");
	disp_mark("step 6: panel sleep-out / display-on");
	dsi_panel_init();
	done |= DISP_OK_PANEL_ON;
	disp_screen_paint(7, 0, "kick");
	disp_mark("step 7: kick and hold");
	disp_mdp_kick();
#endif
	disp_dump("FINAL");
	disp_mark("full bring-up: complete");
	return done;
}

int display_panel_on(void)
{
	return dsi_panel_init();
}
