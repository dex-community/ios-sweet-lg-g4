/*
 * sd.c -- bring up of the microSD slot (SDCC2) from the shim.
 *
 * WHY A SEPARATE FILE
 * -------------------
 * The eMMC path (shim/emmc.c) is proven: it runs at every boot and it is
 * the one that writes the log. Nothing here touches it.
 *
 * THE ORDER THAT MATTERS
 * ----------------------
 * On 2026-11-09 the phone hung twice because the shim wrote to a
 * controller whose clock was off: the write never completed and the AXI bus
 * stalled (GUIDE-05-MMIO-SAFE.md). The rule that comes out of it:
 *
 *     THE CLOCK GOES ON FIRST, THE CONTROLLER SECOND.
 *
 * So this file does, in this order:
 *
 *   1. the GCC clock branches of SDCC2: the AHB branch first (it is the
 *      window itself), then the apps branch;
 *   2. the card clock, at 400 kHz, through the GCC RCG (the SDHCI divider
 *      is ignored on MSM: SDHCI_QUIRK2_ALWAYS_USE_BASE_CLOCK, sdhci-msm.c:3489)
 *      -- 400 kHz comes from the CAF table ftbl_sdcc2_4_apps_clk_src:
 *      F(400000, gcc_xo, 12, 1, 4), that is CFG[4:0]=23, CFG[10:8]=0 (XO),
 *      M=1, N=~3=0xFFFFFFFC, D=~4=0xFFFFFFFB;
 *   3. only then the controller: HC mode, core reset, software reset, bus
 *      power;
 *   4. the commands, in the order of the CAF core (drivers/mmc/core/sd.c
 *      and sd_ops.c): CMD0, CMD8 (0x1AA), CMD55+ACMD41 (the OCR, up to 100
 *      times with 10 ms in between), CMD2, CMD3, CMD7, CMD9, CMD16, CMD17.
 *
 * The QEMU model of the controller REPRODUCES the stall (its windows are
 * not served until the clock branch is on), so tools/qemu-sd.py proves that
 * the order above is respected.
 *
 * IT IS OFF BY DEFAULT: the build flag SHIM_SD_INIT turns it on, and it is
 * only turned on once the measurements from the recovery (GUIDE-06) say
 * that the SDCC2 pins are muxed on SDC2.
 */
#include <stdint.h>
#include "g4.h"
#include "console.h"
#include "trail.h"
#include "mmio.h"
#include "sd.h"

/* The register offsets of the host (the same as the eMMC host: the two
 * controllers are the same part). */
#define SD_HC_BLK_SIZE	0x04
#define SD_HC_BLK_COUNT	0x06
#define SD_HC_ARGUMENT	0x08
#define SD_HC_TRANSFER	0x0c
#define SD_HC_COMMAND	0x0e
#define SD_HC_RESPONSE	0x10
#define SD_HC_BUF_PORT	0x20
#define SD_HC_PRESENT	0x24
#define SD_HC_CTRL1	0x28
#define SD_HC_POWER	0x29
#define SD_HC_CLK_CTRL	0x2c
#define SD_HC_SWRST	0x2f
#define SD_HC_INT_STATUS 0x30
#define SD_HC_INT_ENABLE 0x34
#define SD_HC_INT_SIGNAL 0x38
#define SD_HC_CTRL2	0x3c
#define SD_HC_CAPS1	0x40
#define SD_HC_CAPS2	0x44
#define SD_HC_VENDOR	0x10c

#define SD_CORE_POWER	0x00
#define SD_CORE_HC_MODE	0x78
#define SD_CORE_STATUS	0xdc
#define SD_CORE_MASK	0xe0
#define SD_CORE_CLEAR	0xe4
#define SD_CORE_CTL	0xe8

#define SD_HC_MODE_EN	(1u << 0)
#define SD_FF_CLK_SW_RST_DIS (1u << 13)

/* The global clock controller (DTS: qcom,gcc@fc400000, reg = <0xfc400000
 * 0x2000>). */
#define G4_GCC_BASE	0xfc400000UL

/* The GCC registers of SDCC2 (clock-gcc-8992.c:91-93). */
#define SD_GCC_RCG	0x510
#define SD_GCC_APPS_CBCR 0x504
#define SD_GCC_AHB_CBCR	0x508

/* The commands. */
#define SD_CMD_GO_IDLE		0
#define SD_CMD_ALL_SEND_CID	2
#define SD_CMD_SET_REL_ADDR	3
#define SD_CMD_SLEEP_AWAKE	5
#define SD_CMD_SELECT		7
#define SD_CMD_IF_COND		8
#define SD_CMD_SEND_CSD		9
#define SD_CMD_SET_BLOCKLEN	16
#define SD_CMD_READ_BLOCK	17
#define SD_CMD_APP		55
#define SD_CMD_APP_OP_COND	41

/* The command flags of the SDHCI command register. */
#define SD_RESP_NONE	0x00
#define SD_RESP_136	0x01
#define SD_RESP_48	0x02
#define SD_RESP_48BUSY	0x03
#define SD_RESP_R7	0x05	/* R7: like R1 but 4 bytes */
#define SD_CRC_CHECK	0x08
#define SD_IDX_CHECK	0x10
#define SD_DATA_PRESENT	0x20

/* The interrupt status bits. */
#define SD_INT_CMD_DONE	0x00000001u
#define SD_INT_XFER_DONE	0x00000002u
#define SD_INT_ERR_MASK	0xffff8000u

/* The timeouts. */
#define SD_CMD_TIMEOUT_US	1000000u
#define SD_XFER_TIMEOUT_US	2000000u

/* The OCR we ask for: HCS (bit 30), 2.7-3.6 V (0xFF0000), S4 (bit 15) and
 * S18 (bit 7). The CAF core builds it from the voltages of the device
 * tree (qcom,vdd-io-voltage-level = 1.8-2.95 V) and sets the CCS bit
 * only after CMD8 has answered (sd.c:829-831). */
#define SD_OCR_REQ	0x40ff8080u

/* The card state (R1): the state is bits 12:9, READY_FOR_DATA is bit 8. */
#define SD_R1_STATE(r)		(((r) >> 9) & 0xfu)
#define SD_STATE_STANDBY	1u
#define SD_STATE_TRANSFER	4u

/* The result of the bring-up. */
int sd_init_stage;
int sd_init_error;
uint16_t sd_rca;
uint32_t sd_cid[4];
uint32_t sd_ocr;

/* The steps, for the log. */
enum {
	SD_STEP_CLOCK_AHB = 1,
	SD_STEP_CLOCK_APPS,
	SD_STEP_CLOCK_RATE,
	SD_STEP_HC_MODE,
	SD_STEP_SW_RESET,
	SD_STEP_POWER,
	SD_STEP_CMD0,
	SD_STEP_CMD8,
	SD_STEP_ACMD41,
	SD_STEP_CMD2,
	SD_STEP_CMD3,
	SD_STEP_CMD7,
	SD_STEP_CMD16,
	SD_STEP_CMD17,
	SD_STEP_COUNT
};

static const char *const sd_step_names[SD_STEP_COUNT] = {
	"entry", "clock-ahb", "clock-apps", "clock-rate", "hc-mode",
	"sw-reset", "power", "cmd0", "cmd8", "acmd41", "cmd2", "cmd3",
	"cmd7", "cmd16", "cmd17",
};

#define SD_STEP(n)	do { sd_init_stage = (n); } while (0)

/* ------------------------------------------------------------------ */
/* The accessors                                                        */
/* ------------------------------------------------------------------ */

/*
 * They go through the MMIO allowlist (shim/mmio.h): the SDCC2 windows are
 * allowed, but only AFTER the clock branch is on. The QEMU model refuses
 * to serve them before that, so a wrong order is a test failure and not a
 * hung phone.
 *
 * In the QEMU build the accesses must ALSO go through the software model
 * (shim/emmc_model.inc), like the ones of emmc.c: without it they would
 * reach the real addresses, which do not exist there (the first version
 * of this file did that and the guest panicked with FAR=0xfc400508).
 */
#if SHIM_EMMC_MODEL
uint32_t emmc_model_rd(uintptr_t a, unsigned size);
void emmc_model_wr(uintptr_t a, unsigned size, uint32_t v);

static inline uint32_t sd_rd32(uintptr_t a)
{
	if (!mmio_read_ok(a, 4)) {
		mmio_denied(a, 4);
		return 0xffffffffu;
	}
	return emmc_model_rd(a, 4);
}
static inline void sd_wr32(uintptr_t a, uint32_t v)
{
	if (!mmio_write_ok(a, 4)) {
		mmio_denied(a, 4);
		return;
	}
	emmc_model_wr(a, 4, v);
}
static inline uint16_t sd_rd16(uintptr_t a)
{
	if (!mmio_read_ok(a, 2)) {
		mmio_denied(a, 2);
		return 0xffffu;
	}
	return (uint16_t)emmc_model_rd(a, 2);
}
static inline void sd_wr16(uintptr_t a, uint16_t v)
{
	if (!mmio_write_ok(a, 2)) {
		mmio_denied(a, 2);
		return;
	}
	emmc_model_wr(a, 2, v);
}
static inline uint8_t sd_rd8(uintptr_t a)
{
	if (!mmio_read_ok(a, 1)) {
		mmio_denied(a, 1);
		return 0xffu;
	}
	return (uint8_t)emmc_model_rd(a, 1);
}
static inline void sd_wr8(uintptr_t a, uint8_t v)
{
	if (!mmio_write_ok(a, 1)) {
		mmio_denied(a, 1);
		return;
	}
	emmc_model_wr(a, 1, v);
}
#else
static inline uint32_t sd_rd32(uintptr_t a) { return mmio_rd32(a); }
static inline void sd_wr32(uintptr_t a, uint32_t v) { mmio_wr32(a, v); }
static inline uint16_t sd_rd16(uintptr_t a) { return mmio_rd16(a); }
static inline void sd_wr16(uintptr_t a, uint16_t v) { mmio_wr16(a, v); }
static inline uint8_t sd_rd8(uintptr_t a) { return mmio_rd8(a); }
static inline void sd_wr8(uintptr_t a, uint8_t v) { mmio_wr8(a, v); }
#endif

#define SD_HC	G4_SD_BASE
#define SD_CORE	G4_SD_CORE_BASE

static uint64_t sd_ticks(void)
{
	uint64_t v;

	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

static uint32_t sd_tick_hz(void)
{
	uint64_t v;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return (uint32_t)v;
}

static void sd_delay_us(uint32_t us)
{
	uint64_t end = sd_ticks() + (uint64_t)sd_tick_hz() * us / 1000000u;

	while (sd_ticks() < end)
		;
}

static void sd_ack(void)
{
	sd_wr32(SD_HC + SD_HC_INT_STATUS, 0xffffffffu);
}

static void sd_reset_lines(void)
{
	uint64_t end = sd_ticks() + (uint64_t)sd_tick_hz() * 10000u / 1000000u;

	sd_wr8(SD_HC + SD_HC_SWRST, 0x06);	/* CMD and DAT lines only */
	while (sd_rd8(SD_HC + SD_HC_SWRST) & 0x06u) {
		if (sd_ticks() >= end)
			break;
	}
	sd_ack();
}

/*
 * One command. resp_type is one of the SD_RESP_* values; 'rsp' gets the
 * first word of the response. Returns 0 on success.
 */
static int sd_cmd(uint8_t index, uint32_t argument, uint8_t resp_type,
		  uint32_t *rsp)
{
	uint32_t flags = resp_type & 0x03u;
	uint32_t status;

	if (resp_type == SD_RESP_48 || resp_type == SD_RESP_48BUSY)
		flags |= SD_CRC_CHECK | SD_IDX_CHECK;

	/* Wait for the command line to be free. */
	{
		uint64_t end = sd_ticks() + (uint64_t)sd_tick_hz() *
			       SD_CMD_TIMEOUT_US / 1000000u;

		while (sd_rd32(SD_HC + SD_HC_PRESENT) & 0x3u) {
			if (sd_ticks() >= end) {
				sd_reset_lines();
				return -1;
			}
		}
	}

	sd_ack();
	sd_wr32(SD_HC + SD_HC_ARGUMENT, argument);
	sd_wr16(SD_HC + SD_HC_TRANSFER, 0);
	sd_wr16(SD_HC + SD_HC_COMMAND,
		(uint16_t)(((uint16_t)index << 8) | flags));

	{
		uint64_t end = sd_ticks() + (uint64_t)sd_tick_hz() *
			       SD_CMD_TIMEOUT_US / 1000000u;

		for (;;) {
			status = sd_rd32(SD_HC + SD_HC_INT_STATUS);
			if (status & SD_INT_ERR_MASK)
				return -3;
			if (status & SD_INT_CMD_DONE)
				break;
			if (sd_ticks() >= end)
				return -2;
		}
	}

	if (rsp)
		*rsp = sd_rd32(SD_HC + SD_HC_RESPONSE);
	sd_wr32(SD_HC + SD_HC_INT_STATUS, SD_INT_CMD_DONE);
	if (resp_type == SD_RESP_48BUSY) {
		/* The card holds DAT0 low while it is busy. */
		uint64_t end = sd_ticks() + (uint64_t)sd_tick_hz() * 3000000u /
			       1000000u;

		while (!(sd_rd32(SD_HC + SD_HC_PRESENT) & 0x100000u)) {
			if (sd_ticks() >= end)
				break;
		}
	}
	sd_ack();
	return 0;
}

/*
 * One 512 byte block, read through the FIFO (no DMA).
 *
 * The order is the one of emmc_xfer() (shim/emmc.c), which is the proven
 * one: the response of the command comes FIRST, then the data is read while
 * the controller signals it with DATA_AVAILABLE in the present state.
 * Waiting for XFER_DONE before reading the buffer would deadlock: the
 * transfer completes only as the data goes out.
 */
static int sd_read_block(uint32_t lba, uint8_t *out)
{
	uint32_t r1 = 0;
	uint32_t status;
	uint64_t end;
	unsigned i;

	sd_wr16(SD_HC + SD_HC_BLK_SIZE, 0x0200);
	sd_wr16(SD_HC + SD_HC_BLK_COUNT, 1);
	sd_wr16(SD_HC + SD_HC_TRANSFER, 0x0010);	/* read */
	sd_ack();
	sd_wr32(SD_HC + SD_HC_ARGUMENT, lba);
	sd_wr16(SD_HC + SD_HC_COMMAND,
		(uint16_t)((SD_CMD_READ_BLOCK << 8) |
			   (SD_RESP_48 | SD_CRC_CHECK | SD_IDX_CHECK |
			    SD_DATA_PRESENT)));

	/* The response of the command comes first. */
	end = sd_ticks() + (uint64_t)sd_tick_hz() * SD_CMD_TIMEOUT_US / 1000000u;
	for (;;) {
		status = sd_rd32(SD_HC + SD_HC_INT_STATUS);
		if (status & SD_INT_ERR_MASK)
			return -3;
		if (status & SD_INT_CMD_DONE)
			break;
		if (sd_ticks() >= end)
			return -2;
	}
	r1 = sd_rd32(SD_HC + SD_HC_RESPONSE);
	sd_wr32(SD_HC + SD_HC_INT_STATUS, SD_INT_CMD_DONE);
	if (r1 & 0xe4180000u)		/* a fatal R1 */
		return -4;

	/* And then the data, while the controller offers it. */
	end = sd_ticks() + (uint64_t)sd_tick_hz() * SD_XFER_TIMEOUT_US / 1000000u;
	for (i = 0; i < 512 / 4; i++) {
		uint32_t w;

		while (!(sd_rd32(SD_HC + SD_HC_PRESENT) & 0x00000800u)) {
			status = sd_rd32(SD_HC + SD_HC_INT_STATUS);
			if (status & SD_INT_ERR_MASK)
				return -3;
			if (sd_ticks() >= end)
				return -2;
		}
		w = sd_rd32(SD_HC + SD_HC_BUF_PORT);
		out[i * 4 + 0] = (uint8_t)(w >> 0);
		out[i * 4 + 1] = (uint8_t)(w >> 8);
		out[i * 4 + 2] = (uint8_t)(w >> 16);
		out[i * 4 + 3] = (uint8_t)(w >> 24);
	}
	sd_ack();
	return 0;
}

/* ------------------------------------------------------------------ */
/* The clock                                                            */
/* ------------------------------------------------------------------ */

/* Enable a clock branch: set bit 0 and wait for bit 31 to clear. */
static void sd_gcc_enable(uint32_t off)
{
	uintptr_t a = G4_GCC_BASE + off;
	uint32_t v = sd_rd32(a);

	if (!(v & 1u)) {
		sd_wr32(a, v | 1u);
		for (int i = 0; i < 1000 && (sd_rd32(a) & 0x80000000u); i++)
			sd_delay_us(10);
	}
}

/*
 * The card clock: 400 kHz from the XO, the entry F(400000, gcc_xo, 12, 1, 4)
 * of the CAF table ftbl_sdcc2_4_apps_clk_src (clock-gcc-8992.c:1125).
 *
 * The RCG layout (clock-local2.c:48-52): CMD_RCGR at +0, CFG_RCGR at +4,
 * M at +8, N at +0xc, D at +0x10. The values are the ones the macro F
 * computes (clock-gcc-8992.c:43-52):
 *
 *   CFG[4:0] = 2*div-1 = 23,  CFG[10:8] = 0 (XO)
 *   M = 1,  N = ~((N-M)) = ~3,  D = ~N = ~4
 */
static void sd_gcc_set_400k(void)
{
	uintptr_t r = G4_GCC_BASE + SD_GCC_RCG;

	sd_wr32(r + 0x08, 0x00000001u);		/* M = 1                */
	sd_wr32(r + 0x0c, 0xfffffffcu);		/* N = ~3               */
	sd_wr32(r + 0x10, 0xfffffffbu);		/* D = ~4               */
	sd_wr32(r + 0x04, 0x00000017u);		/* CFG: div 23, src XO   */
	sd_wr32(r + 0x00, 0x00000001u);		/* CMD_RCGR: enable     */
}

/* ------------------------------------------------------------------ */
/* The bring-up                                                         */
/* ------------------------------------------------------------------ */

int sd_init(uint8_t *block0)
{
	uint32_t rsp = 0;
	uint32_t caps;
	int rc;

	sd_init_stage = 0;
	sd_init_error = 0;
	sd_rca = 0;

	/* (1) THE CLOCK FIRST. */
	SD_STEP(SD_STEP_CLOCK_AHB);
	sd_gcc_enable(SD_GCC_AHB_CBCR);
	trail(0x2500, SD_GCC_AHB_CBCR);

	SD_STEP(SD_STEP_CLOCK_APPS);
	sd_gcc_enable(SD_GCC_APPS_CBCR);
	trail(0x2501, SD_GCC_APPS_CBCR);

	SD_STEP(SD_STEP_CLOCK_RATE);
	sd_gcc_set_400k();
	trail(0x2502, sd_rd32(G4_GCC_BASE + SD_GCC_RCG));

	/* (2) and only now the controller. */
	SD_STEP(SD_STEP_HC_MODE);
	sd_wr32(SD_CORE + SD_CORE_HC_MODE,
		sd_rd32(SD_CORE + SD_CORE_HC_MODE) & ~SD_HC_MODE_EN);
	sd_wr32(SD_CORE + SD_CORE_POWER,
		sd_rd32(SD_CORE + SD_CORE_POWER) | (1u << 7));
	sd_delay_us(10000);
	sd_wr32(SD_CORE + SD_CORE_HC_MODE,
		sd_rd32(SD_CORE + SD_CORE_HC_MODE) | SD_HC_MODE_EN);
	sd_wr32(SD_CORE + SD_CORE_HC_MODE,
		sd_rd32(SD_CORE + SD_CORE_HC_MODE) | SD_FF_CLK_SW_RST_DIS);

	SD_STEP(SD_STEP_SW_RESET);
	sd_wr8(SD_HC + SD_HC_SWRST, 1);
	{
		int i;

		for (i = 0; i < 100 && (sd_rd8(SD_HC + SD_HC_SWRST) & 1u); i++)
			sd_delay_us(1000);
		if (i >= 100) {
			sd_init_error = -10;	/* the reset never completed */
			return sd_init_error;
		}
	}

	/* The interrupts we poll have to be latched. */
	sd_wr32(SD_HC + SD_HC_INT_ENABLE, 0x03ff00ffu);
	sd_wr32(SD_HC + SD_HC_INT_SIGNAL, 0);
	sd_wr8(SD_HC + SD_HC_CTRL1, sd_rd8(SD_HC + SD_HC_CTRL1) & ~0x26u);
	sd_wr8(SD_HC + SD_HC_CTRL1, sd_rd8(SD_HC + SD_HC_CTRL1) & ~0x26u);

	caps = sd_rd32(SD_HC + SD_HC_CAPS1);
	if (caps == 0 || caps == 0xffffffffu) {
		sd_init_error = -11;		/* the host does not answer   */
		return sd_init_error;
	}
	trail(0x2503, caps);

	/* The bus power: the voltage comes from the capabilities of the host
	 * (the 1.8 V branch is bit 26 of the vendor spec capabilities,
	 * HC+0x11c: CORE_1_8V_SUPPORT, sdhci-msm.c:3240). */
	SD_STEP(SD_STEP_POWER);
	{
		uint32_t vcaps = sd_rd32(SD_HC + 0x11c);
		uint8_t p;

		p = sd_rd8(SD_HC + SD_HC_POWER);
		if (!(p & 1u)) {
			sd_wr8(SD_HC + SD_HC_POWER, 0x0e);	/* 3.3 V, power off */
			sd_delay_us(1000);
			sd_wr8(SD_HC + SD_HC_POWER, 0x0f);	/* power on        */
			for (int i = 0; i < 100 && !sd_rd32(SD_CORE + SD_CORE_STATUS); i++)
				sd_delay_us(100);
			sd_wr32(SD_CORE + SD_CORE_CLEAR,
				sd_rd32(SD_CORE + SD_CORE_STATUS));
			sd_wr32(SD_CORE + SD_CORE_CTL, 0x5);
		}
		trail(0x2504, vcaps);
		trail(0x2505, sd_rd8(SD_HC + SD_HC_POWER));
	}

	/* (3) the commands, in the order of the CAF core. */
	SD_STEP(SD_STEP_CMD0);
	rc = sd_cmd(SD_CMD_GO_IDLE, 0, SD_RESP_NONE, 0);
	if (rc) {
		sd_init_error = -20;
		return sd_init_error;
	}

	/* CMD8: 0x1AA. The R7 must answer 0xAA in the low byte. */
	SD_STEP(SD_STEP_CMD8);
	rc = sd_cmd(SD_CMD_IF_COND, 0x1aa, SD_RESP_R7, &rsp);
	if (rc || (rsp & 0xffu) != 0xaau) {
		sd_init_error = -21;		/* not an SD 2.0 card, or mute */
		return sd_init_error;
	}
	trail(0x2506, rsp);

	/* ACMD41: CMD55 with arg 41, then CMD41 with the OCR. Up to 100
	 * tries, 10 ms apart (sd_ops.c:150). */
	SD_STEP(SD_STEP_ACMD41);
	{
		int i;
		int ready = 0;

		for (i = 0; i < 100; i++) {
			rc = sd_cmd(SD_CMD_APP, SD_CMD_APP_OP_COND, SD_RESP_R7, &rsp);
			if (rc)
				break;
			rc = sd_cmd(SD_CMD_APP_OP_COND, SD_OCR_REQ, SD_RESP_48, &rsp);
			if (rc)
				break;
			sd_ocr = rsp;
			if (rsp & 0x80000000u) {	/* power-up complete */
				ready = 1;
				break;
			}
			sd_delay_us(10000);
		}
		trail(0x2507, (uint32_t)i);
		if (!ready) {
			sd_init_error = -22;
			return sd_init_error;
		}
	}

	/* CMD2: the CID. */
	SD_STEP(SD_STEP_CMD2);
	rc = sd_cmd(SD_CMD_ALL_SEND_CID, 0, SD_RESP_136, &rsp);
	if (rc) {
		sd_init_error = -23;
		return sd_init_error;
	}
	sd_cid[0] = rsp;

	/* CMD3: the relative address. */
	SD_STEP(SD_STEP_CMD3);
	rc = sd_cmd(SD_CMD_SET_REL_ADDR, 0, SD_RESP_R7, &rsp);
	if (rc) {
		sd_init_error = -24;
		return sd_init_error;
	}
	sd_rca = (uint16_t)(rsp >> 16);
	trail(0x2508, rsp);

	/* CMD7: select the card. */
	SD_STEP(SD_STEP_CMD7);
	rc = sd_cmd(SD_CMD_SELECT, (uint32_t)sd_rca << 16, SD_RESP_48BUSY, &rsp);
	if (rc) {
		sd_init_error = -25;
		return sd_init_error;
	}

	/*
	 * CMD9 (the CSD) is NOT sent here: it has a data phase, which this
	 * first version does not read. It is not needed to prove that the
	 * data path works (CMD17 on the first sector does that). It goes in
	 * with the CSD reader.
	 */

	/* CMD16: the block length (512). */
	SD_STEP(SD_STEP_CMD16);
	rc = sd_cmd(SD_CMD_SET_BLOCKLEN, 512, SD_RESP_R7, &rsp);
	if (rc) {
		sd_init_error = -27;
		return sd_init_error;
	}

	/* CMD17: the first sector, to prove that the data path works. */
	SD_STEP(SD_STEP_CMD17);
	if (block0) {
		rc = sd_read_block(0, block0);
		if (rc) {
			sd_init_error = -28;
			return sd_init_error;
		}
		trail(0x2509, 0x5a5a5a5au);
	}

	sd_init_stage = SD_STEP_COUNT;
	con_hex2("SD", "rca=", sd_rca, "cid=", sd_cid[0]);
	con_hex2("SD", "ocr=", sd_ocr, "stage=", (uint32_t)sd_init_stage);
	return 0;
}
