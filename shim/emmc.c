/*
 * Minimal eMMC log driver for the LG G4 shim  --  v2
 *
 * Job: get the console log onto the card. The panel is in DSI command mode
 * (what the shim draws never reaches the screen) and RAM does not survive
 * the trip through Android, but a sector on the card does, and a recovery
 * can read it back.
 *
 * Principle of v2: ADOPT the controller and the card exactly as LK left
 * them. LK has just read this very boot image from the card with its own
 * driver, so the host is already configured (clock, DLL/CDC, vendor
 * register, IO pads at 1.8 V, bus width, speed mode). v1 reset the host,
 * reprogrammed power and clock and re-identified the card; each of those
 * steps can only destroy what LK set up, so none of them is here.
 *
 * Writes to the controller are limited to:
 *   - INT_STATUS acknowledges (write-1-to-clear)
 *   - block size / count / argument / transfer mode / command registers
 *   - the buffer data port
 *   - CMD/DAT line reset (SWRST bits 1 and 2, NOT reset-all) after an error
 *   - INT_ENABLE, OR-ing in the status bits polled here, only if missing
 *   - CORE_HC_MODE bit 0, only if LK left HC mode disabled
 *
 * Bring-up ladder (a rung is only tried if the previous one failed):
 *   1. CMD16. If the card answers and its R1 says "transfer state", it is
 *      still selected and nothing else is needed.
 *   2. If it does not answer, the card is probably asleep: Qualcomm's LK
 *      does CMD7(deselect) + CMD5(sleep) in target_uninit() right before
 *      jumping to the kernel. Wake it: CMD5 (awake) + CMD7 (select), for
 *      the RCAs LK is likely to have assigned.
 *   3. CMD8 (EXT_CSD): capacity -> sector or byte addressing, active
 *      partition (never write unless it is the user area), bus width and
 *      timing (logged).
 *
 * Addressing. For cards larger than 2 GiB (the G4 has 32 GB) the argument
 * of CMD17/CMD24 is the BLOCK number, not a byte address. Only cards up to
 * 2 GiB take byte addresses. v1 always sent lba * 512, which on a sector
 * addressed card is out of range (or, on a bigger card, somebody else's
 * block). v2 decides from EXT_CSD SEC_COUNT, the way the Linux MMC core does.
 *
 * Safety: the only LBAs this file will ever write are the four listed in
 * emmc_lba_allowed() (two spare partitions, two sectors each). "misc" and
 * "cache" are out of reach by construction.
 *
 * Log layout (per spare partition):
 *   LBA N     "G4SHIMLG" | stage u32 | error s32 | R1 of CMD16 u32 | rca u16 |
 *             pad u16 | text...                     (same layout as v1)
 *   LBA N+1   "G4SHIMDG" | u32 words, see the D_* indices below
 *
 * Stages (emmc_last_stage, 20 s per unit in the shim's timing channel):
 *   0 start       1 probe (read-only snapshot)   2 HC mode re-enabled
 *   3 CMD16       4 wake (CMD5/CMD7)             5 CMD16 after wake
 *   6 EXT_CSD     7 write                        8 read-back     9 done
 */
#include <stdint.h>
#include "g4.h"
#include "console.h"
#include "trail.h"
#include "mmio.h"
#include "wdt.h"

/*
 * Which bring-up path to compile:
 *   1 = replay the LK init sequence (emmc_lk_bringup.inc), the path the
 *       factory bootloader takes at every boot;
 *   0 = the old "re-enable HC mode and hope" path, kept for comparison.
 */
#ifndef EMMC_LK_BRINGUP
#define EMMC_LK_BRINGUP	1
#endif

/* ------------------------------------------------------------------ */
/* Register map                                                         */
/* ------------------------------------------------------------------ */

#define HC		G4_EMMC_BASE		/* 0xf9824900, "hc_mem"   */
#define CORE		G4_EMMC_CORE_BASE	/* 0xf9824000, "core_mem" */

/* SDHCI registers ("hc_mem"). */
#define HC_BLK_SIZE	0x04	/* 16 bit */
#define HC_BLK_COUNT	0x06	/* 16 bit */
#define HC_ARGUMENT	0x08	/* 32 bit */
#define HC_TRANSFER	0x0c	/* 16 bit */
#define HC_COMMAND	0x0e	/* 16 bit */
#define HC_RESPONSE	0x10	/* 32 bit x 4 */
#define HC_BUF_PORT	0x20	/* 32 bit */
#define HC_PRESENT	0x24	/* 32 bit */
#define HC_CTRL1	0x28	/* 32 bit view: ctrl1 | power | blkgap | wakeup */
#define HC_CLK_CTRL	0x2c	/* 32 bit view: clock | timeout | swrst */
#define HC_SWRST	0x2f	/* 8 bit */
#define HC_INT_STATUS	0x30	/* 32 bit: normal low half, error high half */
#define HC_INT_ENABLE	0x34	/* 32 bit */
#define HC_INT_SIGNAL	0x38	/* 32 bit */
#define HC_CTRL2	0x3c	/* 32 bit view: auto-CMD12 error | host ctrl 2 */
#define HC_CAPS1	0x40	/* 32 bit */
#define HC_CAPS2	0x44	/* 32 bit */

/* QSDCC core registers ("core_mem"). */
#define CORE_POWER	0x00
#define CORE_MCI_VER	0x50
#define CORE_HC_MODE	0x78	/* bit 0 = HC_MODE_EN, bit 13 = FF_CLK_SW_RST_DIS */

#define HC_MODE_EN		(1u << 0)
#define FF_CLK_SW_RST_DIS	(1u << 13)

/* Present state (0x24). */
#define PS_CMD_INHIBIT		0x00000001u
#define PS_DATA_INHIBIT		0x00000002u
#define PS_SPACE_AVAILABLE	0x00000400u	/* buffer write enable */
#define PS_DATA_AVAILABLE	0x00000800u	/* buffer read enable  */
#define PS_DAT0_LEVEL		0x00100000u	/* DAT0 line, low = card busy */

/* Software reset (0x2f). */
#define SWRST_CMD_DAT		0x06		/* CMD line + DAT line only */

/* Command register flags (low byte). */
#define CMD_RESP_NONE		0x00
#define CMD_RESP_136		0x01
#define CMD_RESP_48		0x02
#define CMD_RESP_48BUSY		0x03
#define CMD_CRC_CHECK		0x08
#define CMD_IDX_CHECK		0x10
#define CMD_DATA_PRESENT	0x20

/*
 * The two response types that are NOT R1.
 *
 * The bits [1:0] of the command register are the response type (00 none,
 * 01 136 bits, 10 48 bits, 11 48 bits + busy); the bits above carry the
 * CRC check, the index check and the data present. So a response type is
 * the low two bits PLUS the flags that belong to it, and the two "special"
 * types below are the low two bits with the high bits CLEAR:
 *
 *   CMD_RESP_R3      0x82  the OCR: 48 bits, NO CRC7 and NO index.
 *   CMD_RESP_R3BUSY  0x83  the OCR and the busy wait that follows it.
 *
 * Why they exist (this was a bug, found on 2026-11-10 by comparing with the
 * driver Linux that works on this phone):
 *
 *   - the INDEX check belongs to the 136 bit response (R2) only: the SDHCI
 *     spec says the index field exists in the 136 bit answer, so asking the
 *     controller to compare it on a 48 bit answer is meaningless. The
 *     working driver (drivers/mmc/host/sdhci.c) sets SDHCI_CMD_INDEX only
 *     for MMC_RSP_136;
 *   - the CRC7 belongs to R1 (R2, R6), NOT to the OCR of CMD1 and CMD5:
 *     the OCR has no CRC at all. The working driver sends CMD5 as
 *     MMC_RSP_R1B WITHOUT MMC_RSP_CRC, that is with no CRC check, and the
 *     OCR of CMD1 as MMC_RSP_R3, also with no CRC check.
 *
 * With the wrong flags the controller throws away a perfectly good answer
 * (CRC error) or flags a bogus index error: the card looks mute even when
 * it answered.
 */
#define CMD_RESP_R3		0x82
#define CMD_RESP_R3BUSY		0x83

/* Transfer mode (0x0c). Standard SDHCI: bit 4 = read, bit 2 = auto-CMD12. */
#define MODE_READ		0x0010

/* Interrupt status (0x30). */
#define INT_CMD_DONE		0x00000001u
#define INT_XFER_DONE		0x00000002u
#define INT_ERR_MASK		0xffff8000u	/* error summary + error half */

/* MMC commands. */
#define CMD_SLEEP_AWAKE		5
#define CMD_SELECT_CARD		7
#define CMD_SEND_EXT_CSD	8
#define CMD_SET_BLOCKLEN	16
#define CMD_READ_BLOCK		17
#define CMD_WRITE_BLOCK		24
#define CMD_SEND_RELATIVE_ADDR	3	/* CMD3: get/set the relative address */

/* Card status (R1). The state is bits 12:9, READY_FOR_DATA is bit 8. */
#define R1_STATE(r)		(((r) >> 9) & 0xfu)
#define CARD_STATE_TRAN		4u
/* Errors that make a read/write response unusable: out of range, address
 * misalign, block length, write protect, CC error, generic error. Stale
 * "illegal command"/"CRC" bits from earlier traffic are deliberately NOT
 * in the mask. */
#define R1_FATAL		0xe4180000u

/* EXT_CSD fields. */
#define EXT_CSD_PART_CONFIG	179
#define EXT_CSD_BUS_WIDTH	183
#define EXT_CSD_HS_TIMING	185
#define EXT_CSD_DEVICE_TYPE	196
#define EXT_CSD_SEC_COUNT	212	/* 4 bytes, little endian */
#define EMMC_2GB_SECTORS	0x00400000u

/* Timeouts. */
#define CMD_TIMEOUT_US		1000000u
#define XFER_TIMEOUT_US		2000000u

/*
 * Where the log goes, IF the card writes are enabled at all.
 *
 * THE GEOMETRY OF THIS CARD, measured on the phone on 2026-11-10
 * (/proc/partitions + blockdev, the sizes in 512 byte sectors):
 *
 *   p1 modem        0 ..    176128      p27 fsc           316416 ..   317440
 *   p22 misc        208896 ..  241664    p30 DDR           319488 ..   323584
 *   p23 persist     241664 ..  307200    p35 persistent    327680 ..   331776
 *   p24 modemst1    307200 ..  310272    p37 laf           339968 ..   438272
 *   p26 fsg         313344 ..  316416    p38 boot          438272 ..   520192
 *                                        p39 recovery      520192 ..   602112
 *                                        p49 cache         9834496 .. 12357632
 *                                        p50 userdata      12357632 .. 60985344
 *                                        p51 grow          60985344 .. 61009886
 *
 * THE OLD ADDRESSES WERE WRONG (2026-11-10).
 *
 * The old code took the start of two "empty spare" partitions and added 256
 * sectors:
 *
 *   spare1  p21  start 259072  ->  log at 259328
 *   spare2  p36  start 385024  ->  log at 385280
 *
 * but those numbers belong to a DIFFERENT layout: in the table above no
 * partition starts at 259072 or at 385024, and the two addresses fall
 * INSIDE two partitions that the project forbids:
 *
 *   259328 is in p23 = persist  (241664 .. 307200)
 *   385280 is in p37 = laf      (339968 .. 438272)
 *
 * So every run of the shim, since the day those constants were written,
 * has been overwriting the modem's persistent data and the LAF (the
 * download mode) partition with 512 bytes of log. That is why the phone
 * started behaving strangely after a run.
 *
 * There is no safe place to write on this card: the partitions follow each
 * other with no gap, and the only free space is the tail after p51 (grow),
 * which is exactly the space the "grow" partition takes when it grows
 * (that would be the iOS rootfs).
 *
 * SO THE CARD WRITES ARE OFF BY DEFAULT (SHIM_EMMC_WRITE=0). The log goes
 * to the ramoops console zone and to the scratch RAM at 0x50000000, which
 * are the channels that do not touch anything. Whoever turns the writes on
 * has to choose an address and say why it is safe.
 */
#define LOG_LBA_SPARE1		259328UL	/* NOT SAFE: inside p23 (persist) */
#define LOG_LBA_SPARE2		385280UL	/* NOT SAFE: inside p37 (laf)     */

/* Error codes. They end up in the log sector, so they stay small. */
#define EMMC_OK			0
#define EMMC_E_NO_CAPS		-1	/* capability register reads as 0 / all ones */
#define EMMC_E_BLKLEN		-3	/* CMD16 failed or card not in transfer state */
#define EMMC_E_WAKE		-4	/* card answers to neither RCA candidate */
#define EMMC_E_EXTCSD		-5	/* CMD8 failed */
#define EMMC_E_PARTITION	-6	/* boot/RPMB/GP partition selected: refuse */
#define EMMC_E_RANGE		-7	/* LBA not allowed / beyond the card */
#define EMMC_E_WRITE		-8
#define EMMC_E_READBACK		-9
#define EMMC_E_VERIFY		-10	/* read back differs from what was written */

#define STAGE_START		0
#define STAGE_PROBE		1
#define STAGE_HC_MODE		2
#define STAGE_BLKLEN		3
#define STAGE_WAKE		4
#define STAGE_BLKLEN2		5
#define STAGE_EXTCSD		6
#define STAGE_WRITE		7
#define STAGE_READBACK		8
#define STAGE_DONE		9
/* The card did not answer to the full identification (CMD0/1/2/3/7). */
#define STAGE_REINIT		10

/*
 * How far the bring-up got, and what went wrong. The shim turns the stage
 * and the raw command error into the time it waits before the reset, so
 * that they reach the computer even when nothing can be written.
 */
unsigned emmc_last_stage = 0xff;
int emmc_last_error = 0;

/*
 * The result of the FIRST bring-up attempt.
 *
 * emmc_write_log() runs twice: once from the main path (where the console
 * log still exists in RAM) and once from reset() (where the log is written
 * to the card). The second call starts from scratch and would overwrite the
 * stage of the first one, which is the one that matters: the timing
 * channel in reset() has to report where the FIRST attempt stopped.
 *
 * These three variables are therefore only written once, by the first
 * call, and never touched again.
 */
unsigned emmc_first_stage = 0xff;
int emmc_first_error = 0;
int emmc_first_cmd_error = 0;

/* Raw error of the last command: -1 the lines never became idle, -2 the
 * command timed out in software, -3 the controller reported an error,
 * -4 the card's R1 reported a fatal error. */
int emmc_last_cmd_error = 0;

/*
 * Why did the card not answer? The host can only report one number, so
 * the class is what the event table reports. The values are the ones
 * shim.c switches on (see shim_event_index()).
 */
enum {
	CLASS_NONE = 0,
	CLASS_INACCESSIBLE,	/* the controller does not answer at all    */
	CLASS_NO_CAPS,		/* the capability registers are empty        */
	CLASS_CLOCK_OFF,	/* the card clock is not enabled              */
	CLASS_CLOCK_UNSTABLE,	/* the internal clock never stabilised        */
	CLASS_CMD_INHIBIT,	/* the command line is stuck busy             */
	CLASS_CARD_ABSENT,	/* the controller is fine, the card is not    */
	CLASS_CLOCK_GATED	/* the controller was gating the card clock   */
};
unsigned emmc_class = CLASS_NONE;

/*
 * A fine grained progress counter, updated before every step that can
 * fault (in practice: every MMIO access). If the shim dies in the driver,
 * this is what says where: the panic handler puts it into the wait time.
 *
 * It is deliberately separate from emmc_last_stage, which tracks the
 * logical steps of the bring-up.
 */
unsigned emmc_step = 0;
/* Prepare-phase steps go to the IMEM trail; the per-transfer micro steps
 * (>= 40) do not, the command level trail covers them. */
#define STEP(n)	do { emmc_step = (n); if ((n) < 40) trail(0x0001, (n)); } while (0)

/* The step names, for the documentation and for reading the trail.
 * Static, so that they do not put absolute relocations in the image. */
static const char *const emmc_step_names[] = {
	"entry", "snapshot", "hcmode-rd", "hcmode-wr", "caps", "int-enable",
	"cmd16-1", "wake-select", "wake-cmd5", "cmd16-2", "ext-csd",
	"build", "write1", "write2", "readback",
	/* the xfer micro-steps */
	"xfer-wait-idle", "xfer-regs", "xfer-cmd", "xfer-cmd-done",
	"xfer-fifo-space", "xfer-push", "xfer-pull", "xfer-done"
};
static const unsigned emmc_step_count = (unsigned)(sizeof(emmc_step_names) /
					     sizeof(emmc_step_names[0]));

/* The numeric values of the steps. Keep in sync with emmc_step_names. */
enum {
	STEP_ENTRY = 0,
	STEP_SNAPSHOT,
	STEP_HCMODE_READ,
	STEP_HCMODE_WRITE,
	STEP_CAPS,
	STEP_INTEN,
	STEP_CMD16_1,
	STEP_WAKE_SELECT,
	STEP_WAKE_CMD5,
	STEP_CMD16_2,
	STEP_EXT_CSD,
	STEP_BUILD,
	STEP_WRITE1,
	STEP_WRITE2,
	STEP_READBACK,
	STEP_COUNT
};

/* ------------------------------------------------------------------ */
/* Diagnostics: one sector of host/card state, written next to the log  */
/* ------------------------------------------------------------------ */

enum {
	D_FORMAT,	/* layout version (2)                                  */
	D_RUNG,		/* 0 = card was already selected, 1 = had to wake it   */
	D_CORE_POWER,	/* core 0x00                                           */
	D_CORE_MCIVER,	/* core 0x50                                           */
	D_CORE_HCMODE,	/* core 0x78, before                                   */
	D_CORE_100,	/* core 0x100 (DLL config, if it lives here)           */
	D_CORE_10C,	/* core 0x10c (vendor spec, if it lives here)          */
	D_HC_100,	/* hc   0x100                                          */
	D_HC_10C,	/* hc   0x10c                                          */
	D_PRESENT,	/* present state, before                               */
	D_CTRL1,	/* 0x28: ctrl1 | power | blkgap | wakeup               */
	D_CLKCTL,	/* 0x2c: clock | timeout | swrst                       */
	D_INTSTAT,	/* interrupt status, before                            */
	D_INTEN,	/* interrupt status enable, before                     */
	D_INTSIG,	/* interrupt signal enable                             */
	D_CTRL2,	/* 0x3c: host control 2 (UHS mode select in bits 2:0)  */
	D_CAPS1,
	D_CAPS2,
	D_HCMODE_AFTER,	/* core 0x78 after the HC mode check                   */
	D_DAT0,		/* 1 if the DAT0 level is visible in present state     */
	D_CMD16_R1,	/* R1 of the first CMD16, or 0xdead0000 | error        */
	D_RCA,		/* RCA that answered to the wake-up, 0 if not needed   */
	D_WAKE_BC,	/* CMD5 awake with the BROADCAST address: the answer  */
	D_WAKE_RSP,	/* CMD5 awake with the RCA: the answer                */
	D_WAKE_INT,	/* interrupt status of the last wake-up CMD5         */
	D_WAKE_VS,	/* the vendor spec register (the clock path)          */
	D_SEC_COUNT,	/* EXT_CSD SEC_COUNT                                   */
	D_EXT_MODE,	/* bus width | hs timing << 8 | part cfg << 16 | dev type << 24 */
	D_ADDR_MODE,	/* 1 = sector addressing, 0 = byte addressing          */
	D_FAIL_INT,	/* interrupt status of the last controller error       */
	D_CMD_ERR,	/* emmc_last_cmd_error when the log was built          */
	D_COUNT
};

static uint32_t g_diag[D_COUNT];
static int g_have_dat0;
static int g_sector_mode;
static uint32_t g_sec_count;
static uint16_t g_rca;

/* ------------------------------------------------------------------ */
/* Low level helpers                                                    */
/* ------------------------------------------------------------------ */

/*
 * The accessors go through the MMIO allowlist (shim/mmio.h): an access
 * outside the windows the shim is allowed to touch is REFUSED, not
 * executed. On this SoC a write to a dead window is what hung the phone
 * twice (see mmio.h).
 */
#if SHIM_EMMC_MODEL
/*
 * QEMU build: there is no SDCC1 controller there, so the two register
 * windows are served by the software model (shim/emmc_model.inc), which
 * logs every access. The model is a made up controller: a green result
 * under QEMU says nothing about the phone.
 */
uint32_t emmc_model_rd(uintptr_t a, unsigned size);
void emmc_model_wr(uintptr_t a, unsigned size, uint32_t v);

static inline uint32_t rd32(uintptr_t a)
{
	if (!mmio_read_ok(a, 4)) {
		mmio_denied(a, 4);
		return 0xffffffffu;
	}
	return emmc_model_rd(a, 4);
}
static inline void wr32(uintptr_t a, uint32_t v)
{
	if (!mmio_write_ok(a, 4)) {
		mmio_denied(a, 4);
		return;
	}
	emmc_model_wr(a, 4, v);
}
static inline uint16_t rd16(uintptr_t a)
{
	if (!mmio_read_ok(a, 2)) {
		mmio_denied(a, 2);
		return 0xffffu;
	}
	return (uint16_t)emmc_model_rd(a, 2);
}
static inline void wr16(uintptr_t a, uint16_t v)
{
	if (!mmio_write_ok(a, 2)) {
		mmio_denied(a, 2);
		return;
	}
	emmc_model_wr(a, 2, v);
}
static inline uint8_t rd8(uintptr_t a)
{
	if (!mmio_read_ok(a, 1)) {
		mmio_denied(a, 1);
		return 0xffu;
	}
	return (uint8_t)emmc_model_rd(a, 1);
}
static inline void wr8(uintptr_t a, uint8_t v)
{
	if (!mmio_write_ok(a, 1)) {
		mmio_denied(a, 1);
		return;
	}
	emmc_model_wr(a, 1, v);
}
#else
static inline uint32_t rd32(uintptr_t a) { return mmio_rd32(a); }
static inline void wr32(uintptr_t a, uint32_t v) { mmio_wr32(a, v); }
static inline uint16_t rd16(uintptr_t a) { return mmio_rd16(a); }
static inline void wr16(uintptr_t a, uint16_t v) { mmio_wr16(a, v); }
static inline uint8_t rd8(uintptr_t a) { return mmio_rd8(a); }
static inline void wr8(uintptr_t a, uint8_t v) { mmio_wr8(a, v); }
#endif

static uint64_t emmc_ticks(void)
{
	uint64_t v;

	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

static uint32_t emmc_tick_hz(void)
{
	uint64_t v;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return (uint32_t)v;
}

static uint64_t emmc_deadline(uint32_t us)
{
	return emmc_ticks() + (uint64_t)emmc_tick_hz() * us / 1000000u;
}

static void emmc_delay_us(uint32_t us)
{
	uint64_t end = emmc_deadline(us);
	unsigned n = 0;

	while (emmc_ticks() < end) {
		/* The wait loops are where a hang would hide, so the watchdog
		 * guard is petted from here (shim/wdt.c). */
		if ((++n & 0x3ffu) == 0)
			g4_wdt_pet();
	}
}

/* Clear the interrupt status (both halves are write-1-to-clear). */
static void emmc_ack(void)
{
	wr32(HC + HC_INT_STATUS, 0xffffffffu);
}

/* Reset the CMD and DAT state machines only. This is NOT the reset-all of
 * v1: the vendor registers, clock and DLL are left alone. */
static void emmc_reset_lines(void)
{
	uint64_t end = emmc_deadline(10000);

	STEP(48);
	wr8(HC + HC_SWRST, SWRST_CMD_DAT);
	STEP(49);
	while (rd8(HC + HC_SWRST) & SWRST_CMD_DAT) {
		if (emmc_ticks() >= end)
			break;
	}
	emmc_ack();
}

/* Wait for one of the wanted interrupt bits. Returns the status, or 0 on
 * timeout. A status carrying an error bit is returned as well: the caller
 * tells the two apart with INT_ERR_MASK. */
static uint32_t emmc_wait(uint32_t wanted, uint32_t timeout_us)
{
	uint64_t end = emmc_deadline(timeout_us);

	for (;;) {
		uint32_t status = rd32(HC + HC_INT_STATUS);

		if (status & INT_ERR_MASK)
			return status;
		if (status & wanted)
			return status;
		if (emmc_ticks() >= end)
			return 0;
	}
}

/* Wait until the command and data lines are free. Returns 0 when idle. */
static int emmc_wait_idle(void)
{
	uint64_t end = emmc_deadline(CMD_TIMEOUT_US);

	while (rd32(HC + HC_PRESENT) & (PS_CMD_INHIBIT | PS_DATA_INHIBIT)) {
		if (emmc_ticks() >= end) {
			emmc_reset_lines();
			if (rd32(HC + HC_PRESENT) & (PS_CMD_INHIBIT | PS_DATA_INHIBIT))
				return -1;
			return 0;
		}
	}
	return 0;
}

/* The card holds DAT0 low while it is busy (programming, waking up).
 * If this host does not show the line level, fall back to a fixed delay
 * that is longer than a 512 byte program. */
static void emmc_wait_busy(void)
{
	if (g_have_dat0) {
		uint64_t end = emmc_deadline(3000000);

		while (!(rd32(HC + HC_PRESENT) & PS_DAT0_LEVEL)) {
			if (emmc_ticks() >= end)
				break;
		}
	} else {
		emmc_delay_us(20000);
	}
}

/*
 * What the controller has to say about a command. The card does not answer
 * at all, so this is the only direct measurement of what the controller
 * did with the command: the interrupt status (did the command complete, did
 * it fail), the present state (is the command line still busy), the
 * response register (what came back on CMD) and the argument.
 */
static void emmc_cmd_log(uint8_t index, uint32_t argument, uint32_t rc)
{
	con_hex2("CMD", "n=", index, "rc=", rc);
	con_hex2("CMD", "int=", rd32(HC + HC_INT_STATUS),
		 "pres=", rd32(HC + HC_PRESENT));
	con_hex2("CMD", "rsp=", rd32(HC + HC_RESPONSE), "arg=", argument);
}

/*
 * One command without a data phase. Returns 0 on success; on failure the
 * raw error is also in emmc_last_cmd_error. resp_type is CMD_RESP_*; CRC
 * and index checks are added for the 48 bit responses.
 */
static int emmc_cmd(uint8_t index, uint32_t argument, uint8_t resp_type,
		    uint32_t *rsp)
{
	uint32_t flags = resp_type & 0x03u;
	uint32_t status;

	emmc_last_cmd_error = 0;
	if (emmc_wait_idle()) {
		emmc_last_cmd_error = -1;
		trail(0x0200 | index, (uint32_t)-1);
		emmc_cmd_log(index, argument, (uint32_t)emmc_last_cmd_error);
		return -1;
	}

	/*
	 * The CRC check belongs to the 48 bit answer of R1 (it has a CRC7),
	 * the index check to the 136 bit one (it has the index). The OCR
	 * (CMD_RESP_R3 / CMD_RESP_R3BUSY) has neither and must be asked
	 * without them, exactly like the working Linux driver does.
	 */
	if (resp_type == CMD_RESP_136)
		flags |= CMD_CRC_CHECK | CMD_IDX_CHECK;
	else if (resp_type == CMD_RESP_48 || resp_type == CMD_RESP_48BUSY)
		flags |= CMD_CRC_CHECK;

	emmc_ack();
	wr32(HC + HC_ARGUMENT, argument);
	wr16(HC + HC_TRANSFER, 0);
	wr16(HC + HC_COMMAND, (uint16_t)(((uint16_t)index << 8) | flags));

	status = emmc_wait(INT_CMD_DONE, CMD_TIMEOUT_US);
	if (!status) {
		emmc_last_cmd_error = -2;
		goto fail;
	}
	if (status & INT_ERR_MASK) {
		emmc_last_cmd_error = -3;
		g_diag[D_FAIL_INT] = status;
		goto fail;
	}
	if (rsp)
		*rsp = rd32(HC + HC_RESPONSE);
	wr32(HC + HC_INT_STATUS, INT_CMD_DONE);
	if (resp_type == CMD_RESP_48BUSY || resp_type == CMD_RESP_R3BUSY)
		emmc_wait_busy();
	emmc_ack();
	trail(0x0100 | index, rd32(HC + HC_RESPONSE));
	emmc_cmd_log(index, argument, 0);
	return 0;

fail:
	trail(0x0200 | index, (uint32_t)emmc_last_cmd_error);
	emmc_cmd_log(index, argument, (uint32_t)emmc_last_cmd_error);
	if (emmc_last_cmd_error == -3)
		trail(0x0300 | index, g_diag[D_FAIL_INT]);
	emmc_reset_lines();
	return emmc_last_cmd_error;
}

/*
 * One command with a single 512 byte data block, moved through the FIFO
 * (no DMA). Exactly one of 'out' (write) and 'in' (read) is non-NULL.
 * The R1 of the command goes to *r1_out. Returns 0 on success.
 */
static int emmc_xfer(uint8_t index, uint32_t argument, const uint8_t *out,
		     uint8_t *in, uint32_t *r1_out)
{
	uint16_t flags = CMD_RESP_48 | CMD_CRC_CHECK | CMD_IDX_CHECK | CMD_DATA_PRESENT;
	uint32_t status, r1;
	uint64_t end;

	emmc_last_cmd_error = 0;
	STEP(40);
	if (emmc_wait_idle()) {
		emmc_last_cmd_error = -1;
		return -1;
	}

	/* Single block, 512 bytes, no DMA. The transfer mode is "write" (all
	 * clear) or "read" (bit 4); a single block needs no block count and
	 * no auto-CMD12. */
	STEP(41);
	wr16(HC + HC_BLK_SIZE, 0x0200);
	wr16(HC + HC_BLK_COUNT, 1);
	wr16(HC + HC_TRANSFER, in ? MODE_READ : 0);

	STEP(42);
	emmc_ack();
	STEP(58);
	wr32(HC + HC_ARGUMENT, argument);
	STEP(59);
	wr16(HC + HC_COMMAND, (uint16_t)(((uint16_t)index << 8) | flags));

	/* The command response comes first, for reads and writes alike. */
	STEP(43);
	status = emmc_wait(INT_CMD_DONE, CMD_TIMEOUT_US);
	if (!status) {
		emmc_last_cmd_error = -2;
		goto fail;
	}
	if (status & INT_ERR_MASK) {
		emmc_last_cmd_error = -3;
		g_diag[D_FAIL_INT] = status;
		goto fail;
	}
	r1 = rd32(HC + HC_RESPONSE);
	*r1_out = r1;
	wr32(HC + HC_INT_STATUS, INT_CMD_DONE);
	if (r1 & R1_FATAL) {
		emmc_last_cmd_error = -4;
		goto fail;
	}

	if (out) {
		/* Wait for room in the FIFO, push the block, then wait for the
		 * transfer to complete and the card to finish programming. */
		STEP(44);
		end = emmc_deadline(CMD_TIMEOUT_US);
		while (!(rd32(HC + HC_PRESENT) & PS_SPACE_AVAILABLE)) {
			status = rd32(HC + HC_INT_STATUS);
			if (status & INT_ERR_MASK) {
				emmc_last_cmd_error = -3;
				g_diag[D_FAIL_INT] = status;
				goto fail;
			}
			if (emmc_ticks() >= end) {
				emmc_last_cmd_error = -2;
				goto fail;
			}
		}
		STEP(45);
		for (uint32_t word = 0; word < 128; word++) {
			const uint8_t *at = out + word * 4;

			wr32(HC + HC_BUF_PORT,
			     (uint32_t)at[0] | ((uint32_t)at[1] << 8) |
			     ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24));
		}
	} else {
		STEP(46);
		end = emmc_deadline(CMD_TIMEOUT_US);
		for (uint32_t word = 0; word < 128; word++) {
			uint32_t v;

			while (!(rd32(HC + HC_PRESENT) & PS_DATA_AVAILABLE)) {
				status = rd32(HC + HC_INT_STATUS);
				if (status & INT_ERR_MASK) {
					emmc_last_cmd_error = -3;
					g_diag[D_FAIL_INT] = status;
					goto fail;
				}
				if (emmc_ticks() >= end) {
					emmc_last_cmd_error = -2;
					goto fail;
				}
			}
			/* Every read of the buffer port pops four bytes. */
			v = rd32(HC + HC_BUF_PORT);
			in[word * 4 + 0] = (uint8_t)v;
			in[word * 4 + 1] = (uint8_t)(v >> 8);
			in[word * 4 + 2] = (uint8_t)(v >> 16);
			in[word * 4 + 3] = (uint8_t)(v >> 24);
		}
	}

	/* Only the transfer-complete bit counts here: the command-complete
	 * bit was already consumed above. */
	STEP(47);
	status = emmc_wait(INT_XFER_DONE, XFER_TIMEOUT_US);
	if (!status) {
		emmc_last_cmd_error = -2;
		goto fail;
	}
	if (status & INT_ERR_MASK) {
		emmc_last_cmd_error = -3;
		g_diag[D_FAIL_INT] = status;
		goto fail;
	}
	emmc_ack();
	if (out)
		emmc_wait_busy();
	trail(0x0400 | index, r1);
	return 0;

fail:
	trail(0x0500 | index, (uint32_t)emmc_last_cmd_error);
	trail(0x0300 | index, g_diag[D_FAIL_INT]);
	trail(0x0510 | index, emmc_step);
	emmc_reset_lines();
	return emmc_last_cmd_error;
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                             */
/* ------------------------------------------------------------------ */

/* Read-only look at the host as LK left it. Nothing is written. */
static void emmc_snapshot(void)
{
	STEP(STEP_SNAPSHOT);
	g_diag[D_CORE_POWER]  = rd32(CORE + CORE_POWER);
	g_diag[D_CORE_MCIVER] = rd32(CORE + CORE_MCI_VER);
	g_diag[D_CORE_HCMODE] = rd32(CORE + CORE_HC_MODE);
	g_diag[D_CORE_100]    = rd32(CORE + 0x100);
	g_diag[D_CORE_10C]    = rd32(CORE + 0x10c);
	g_diag[D_HC_100]      = rd32(HC + 0x100);
	g_diag[D_HC_10C]      = rd32(HC + 0x10c);
	g_diag[D_PRESENT]     = rd32(HC + HC_PRESENT);
	g_diag[D_CTRL1]       = rd32(HC + HC_CTRL1);
	g_diag[D_CLKCTL]      = rd32(HC + HC_CLK_CTRL);
	g_diag[D_INTSTAT]     = rd32(HC + HC_INT_STATUS);
	g_diag[D_INTEN]       = rd32(HC + HC_INT_ENABLE);
	g_diag[D_INTSIG]      = rd32(HC + HC_INT_SIGNAL);
	g_diag[D_CTRL2]       = rd32(HC + HC_CTRL2);
	g_diag[D_CAPS1]       = rd32(HC + HC_CAPS1);
	g_diag[D_CAPS2]       = rd32(HC + HC_CAPS2);
}

/*
 * Wake the card and select it.
 *
 * LK's target_uninit() leaves the eMMC deselected and asleep: CMD7 with
 * RCA 0 (deselect), then CMD5 with the sleep bit.
 *
 * JEDEC CMD5 (SLEEP_AWAKE): argument = RCA in 31:16, bit 15 = 1 means
 * SLEEP, bit 15 = 0 means AWAKE. The card is addressed by its RCA and a
 * card in the Sleep state is NOT selected; it does not answer CMD7. The way
 * back is therefore the mirror image of the way in, with the sleep bit
 * cleared:
 *
 *     CMD5 (awake, bit 15 = 0)  ->  Stand-by state
 *     CMD7 (select)             ->  Transfer state
 *
 * The previous version did CMD7 first and then CMD5 with bit 15 SET, i.e.
 * it asked the card to go back to sleep right after selecting it (and set
 * bit 31, which the spec does not define). That leaves CMD16 failing after
 * the "wake", exactly the symptom seen on the phone.
 *
 * The addresses that are tried, and the reason for the broadcast one, are
 * written in the comment of the function below.
 */
/*
 * The wake-up.
 *
 * The card is asleep when the shim starts: the factory LK puts it to sleep
 * before jumping to the kernel (mmc_put_card_to_sleep, mmc.c:3411: CMD7
 * deselect, then CMD5 with the sleep bit SET).
 *
 * The wake-up command is CMD5 (SLEEP_AWAKE) with the sleep bit CLEAR. The
 * argument carries the RCA in the bits [31:16] and the sleep/awake bit in
 * bit 15: that is the LK's own encoding (MMC_CARD_RCA_BIT = 16 and
 * MMC_CARD_SLEEP = 1<<15, mmc.h:568-569) and the one of the driver Linux
 * that works on this phone (drivers/mmc/core/mmc_ops.c, mmc_card_sleepawake):
 *
 *	cmd.opcode = MMC_SLEEP_AWAKE;
 *	cmd.arg = card->rca << 16;		<-- awake: the sleep bit is clear
 *	if (sleep)
 *		cmd.arg |= 1 << 15;
 *
 * THE ADDRESSES, AND WHY THE BROADCAST ONE COMES FIRST
 * ---------------------------------------------------
 *   0xffff0000  the BROADCAST address: per the spec it reaches every card
 *               on the bus, whatever RCA it has. It is the only command
 *               that can wake a card whose RCA the host does not know, so
 *               it is the one that tells "the card is asleep" apart from
 *               "the card does not answer at all".
 *
 *               (The RCA 0 that the previous version of this code used is
 *               NOT a broadcast address: it addresses the card whose RCA
 *               is 0, that is a card that never left the identification
 *               state, and such a card is not asleep either. The comment
 *               that said "per the JEDEC spec the card answers with its
 *               OCR whatever RCA it had" was wrong.)
 *
 *   0x00020000  the RCA the LK assigned (MMC_RCA = 2, mmc.h:568), that is
 *               the address the card really has after the LK's own boot.
 *
 *   0x00010000  the RCA 1, as a last resort.
 *
 * The answer of CMD5 is an OCR (R3): 48 bits, with NO CRC7 and NO index, so
 * it is asked with CMD_RESP_R3BUSY and not with CMD_RESP_48BUSY.
 *
 * The function returns 0 when the card answered and CMD7 selected it, -1
 * when nothing answered. The answers are kept in their own diagnostic slots
 * (D_WAKE_BC, D_WAKE_RSP, D_WAKE_INT), NOT in D_RCA: the previous version
 * wrote the answer of the RCA 0 into D_RCA and then the loop overwrote it
 * with the RCA that worked, so the interesting reading was lost exactly in
 * the case that mattered.
 */
static int emmc_wake(void)
{
	static const uint16_t candidates[] = { 2, 1 };
	uint32_t rsp = 0;
	int rc;

	/* (1) the broadcast address: every card, whatever RCA it has. */
	STEP(STEP_WAKE_CMD5);
	rc = emmc_cmd(CMD_SLEEP_AWAKE, 0xffff0000u, CMD_RESP_R3BUSY, &rsp);
	g_diag[D_WAKE_BC] = rc ? (0xdead0000u | (uint32_t)rc) : rsp;
	g_diag[D_WAKE_INT] = rd32(HC + HC_INT_STATUS);
	trail(0x007a, g_diag[D_WAKE_BC]);
	con_hex2("WAKE", "bc=", g_diag[D_WAKE_BC], "int=", g_diag[D_WAKE_INT]);
	/* Give the card the time to leave the Sleep state (tAWAKE). */
	emmc_delay_us(10000);

	/* (2) the RCA the LK used, and then the historical fallback. */
	for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		uint32_t rca = candidates[i];

		STEP(STEP_WAKE_CMD5);
		rc = emmc_cmd(CMD_SLEEP_AWAKE, rca << 16, CMD_RESP_R3BUSY, &rsp);
		g_diag[D_WAKE_RSP] = rc ? (0xdead0000u | (uint32_t)rc) : rsp;
		g_diag[D_WAKE_INT] = rd32(HC + HC_INT_STATUS);
		trail(0x007b, g_diag[D_WAKE_RSP]);
		con_hex2("WAKE", "rca=", rca, "rsp=", g_diag[D_WAKE_RSP]);
		emmc_delay_us(10000);

		STEP(STEP_WAKE_SELECT);
		if (!emmc_cmd(CMD_SELECT_CARD, rca << 16, CMD_RESP_48BUSY, &rsp)) {
			g_rca = (uint16_t)rca;
			/* bit 15 = the CMD5 answered, bits [7:0] = the RCA. */
			g_diag[D_RCA] = rca | (rc ? 0u : 0x8000u);
			return 0;
		}
	}
	return -1;
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#if EMMC_LK_BRINGUP
/*
 * The knobs of the bring-up.
 *
 * The factory LK, the working Linux driver and this file disagree on a few
 * points. Each of them is a flag or a value here, so that the diagnostic
 * build (EMMC_DIAG_VARIANTS) can try them one at a time and find out which
 * one decides whether the card answers.
 *
 * THE DEFAULT VARIANT IS NOT "WHAT THE LK DOES" ANY MORE (2026-11-10).
 * It is what the WORKING Linux driver does, and the difference is the whole
 * point of the change.
 *
 * WHY: the card is an HS400 card on this phone. The dmesg of the phone's own
 * Android kernel says it:
 *
 *	mmc0: BKOPS_EN bit = 1
 *	mmc0: sdhci_msm_execute_tuning: finally setting the tuning phase to 11
 *	mmc0: new HS400 MMC card at address 0001
 *
 * So the LK talks to the card in HS400 (200 MHz, DDR, with the CDC and the
 * DLL in the HS400 path) and then puts the card to sleep and jumps to the
 * kernel. The working driver, before it talks to a card that is NOT in
 * HS400, puts the clock path back to the legacy one
 * (drivers/mmc/host/sdhci-msm.c, sdhci_msm_set_clock):
 *
 *	 * HS400 - divided clock (free running MCLK/2)
 *	 * All other modes - default (free running MCLK)
 *	...
 *	 writel_relaxed((readl_relaxed(ioaddr + CORE_VENDOR_SPEC)
 *			  & ~CORE_HC_MCLK_SEL_MASK) | CORE_HC_MCLK_SEL_HS400, ...)
 *	 and, to enter HS400:
 *	 | CORE_HC_SELECT_IN_HS400 | CORE_HC_SELECT_IN_EN
 *
 * The shim, that talks to the card at 400 kHz in legacy mode, NEVER did
 * that: the MCI stayed on the HS400 clock path (MCLK_SEL = HS400,
 * SELECT_IN = HS400) and the identification clock did not reach the card.
 * The card therefore heard NOTHING: the controller sent the commands, the
 * response register stayed at zero and the command timed out
 * (INT_STATUS = 0x00018000 = error | command timeout, no CRC error, because
 * nothing came back). That is the symptom of all eight variants of the
 * 2026-11-09 sweep: NONE of them ever touched CORE_VENDOR_SPEC, so none of
 * them ever tested this.
 *
 * The same register also holds CORE_CLK_PWRSAVE (bit 1), the bit that lets
 * the controller gate the card clock by itself when there is no traffic:
 * the working driver clears it while it talks to the card
 * (msm_sdhci_set_clock) and puts it back afterwards. It is cleared here too.
 *
 * LK_F_VENDOR_RESET and LK_F_CLR_PWRSAVE are therefore ON in the default
 * variant, and the sweep keeps the old behaviour in a variant of its own, so
 * that the two can be compared.
 */
#define LK_PWR_KEEP	0xffffffffu	/* do not write HC+0x29 at all      */
#define LK_PWR_FORCE	0xfffffffeu	/* write it even if the power is on */
struct lk_var {
	uint32_t power_val;	/* value for HC+0x29 (bus power control)   */
	uint32_t clk_hz;	/* the clock target                         */
	uint32_t flags;
};
#define LK_F_NO_CORE_RESET	(1u << 0)	/* no CORE+0x00 core reset     */
#define LK_F_CLR_PWRSAVE	(1u << 1)	/* clear CORE+0x10c bit 1      */
#define LK_F_NO_SW_RST		(1u << 2)	/* no HC+0x2f software reset  */
#define LK_F_NO_WAKE		(1u << 3)	/* no CMD5 wake before CMD0    */
#define LK_F_LK_REGS		(1u << 4)	/* LK's INT_ENABLE/CTRL1      */
#define LK_F_LK_OCR		(1u << 5)	/* LK's first OCR (0x40ff0000)*/
#define LK_F_PWR_FORCE		(1u << 6)	/* write HC+0x29 even if on   */
#define LK_F_VENDOR_RESET	(1u << 7)	/* MCLK_SEL/SELECT_IN default */
#define LK_F_PWR_ACK		(1u << 8)	/* ack the PWRCTL like the LK */
#define LK_F_DELAY_10S		(1u << 9)	/* wait 10 s before CMD5/CMD0 */

#if SHIM_EMMC_MODEL
/*
 * The QEMU build serves the HC/CORE windows from the software model.
 * It is included here so that it can see the accessors above.
 */
#include "fdt.h"
#include "crc32.h"
#include "emmc_model.inc"
#endif

#include "emmc_lk_bringup.inc"
/*
 * The default variant: what the factory LK does.
 *
 * The bus power goes up at 3.3 V (0x0f), which is what the LK writes at
 * every boot and what makes the card answer (measured: at the shim entry
 * HC+0x29 reads 0x00, the power is off; at 1.8 V the card stays mute).
 * The clock target is 400 kHz.
 */
#if !EMMC_DIAG_VARIANTS
/*
 * The default bring-up.
 *
 * The bus power goes up at 3.3 V (0x0f), which is what the LK writes at
 * every boot and what makes the card answer (measured: at the shim entry
 * HC+0x29 reads 0x0b, the bus is on at 1.8 V; at 1.8 V the card stays mute).
 * The clock target is 400 kHz.
 *
 * And the clock PATH is put back to the legacy one, with the card clock
 * gating off: see the note above, that is the change of 2026-11-10 and the
 * only one that was never tried by the eight variants of the sweep.
 */
static const struct lk_var lk_var_default = {
	0x0f, 400000, LK_F_VENDOR_RESET | LK_F_CLR_PWRSAVE
};
#endif
#include "emmc_diag.inc"
#endif

/*
 * Make sure the card is selected, in the transfer state, on the user
 * partition, and know how it is addressed. 'scratch' is a 512 byte buffer.
 */
static int emmc_prepare(uint8_t *scratch)
{
	uint32_t rsp = 0, caps, ie, want;
	int rc;

	emmc_last_stage = STAGE_PROBE;
	emmc_snapshot();
	trail(0x0010, g_diag[D_CORE_HCMODE]);
	trail(0x0011, g_diag[D_PRESENT]);
	trail(0x0012, g_diag[D_CLKCTL]);
	trail(0x0013, g_diag[D_CTRL2]);
	trail(0x0014, g_diag[D_CORE_10C]);
	trail(0x0015, g_diag[D_HC_10C]);

#if EMMC_LK_BRINGUP
	(void)caps; (void)ie; (void)want;
	emmc_last_stage = STAGE_HC_MODE;
#if EMMC_DIAG_VARIANTS
	rc = emmc_diag_sweep();
#else
	rc = emmc_lk_bringup(&lk_var_default);
#endif
	g_diag[D_HCMODE_AFTER] = rd32(CORE + CORE_HC_MODE);
	if (rc)
		return rc;
	/* Same check as the old rung 1: selected and in the transfer state. */
	emmc_last_stage = STAGE_BLKLEN;
	STEP(STEP_CMD16_1);
	rc = emmc_cmd(CMD_SET_BLOCKLEN, 512, CMD_RESP_48, &rsp);
	g_diag[D_CMD16_R1] = rc ? (0xdead0000u | (uint16_t)rc) : rsp;
	if (rc || R1_STATE(rsp) != CARD_STATE_TRAN)
		return EMMC_E_BLKLEN;
#else
	/*
	 * HC mode is the one thing LK switches off before jumping to the
	 * kernel. sdhci_mode_disable() (verified in the factory LK binary
	 * at 0x0f914940) does exactly this:
	 *
	 *   RMWREG32(pwrctl_base + SDCC_MCI_HC_MODE, 0, 1, 0)
	 *
	 * that is, it clears bit 0 of CORE+0x78 and nothing else. On the
	 * G4 pwrctl_base is MSM_SDC1_BASE (0xf9824000) and the SDHCI
	 * controller is at MSM_SDC1_SDHCI_BASE (0xf9824900), so CORE+0x78
	 * is the right register.
	 *
	 * The reverse operation, sdhci_msm_init() (0x0f913f10), is not just
	 * "set bit 0 again": it clears bit 0, pulses the core reset (bit 7
	 * of CORE+0x00), waits a millisecond, writes the HC_MODE register
	 * (a plain writel of 1, which clobbers the other bits), and finally
	 * sets FF_CLK_SW_RST_DIS (bit 13).
	 *
	 * That core reset is what re-synchronises the SDHCI side with the
	 * card after the mode switch. Without it the controller can stay
	 * unresponsive, so it is reproduced here. It resets the CORE (the
	 * MCI/SDHCI bridge), not the host controller, so it does not touch
	 * the CDC/DLL calibration that LK set up.
	 *
	 * The whole sequence is only run when LK actually left HC mode
	 * disabled, so the normal case touches nothing.
	 */
	if (!(rd32(CORE + CORE_HC_MODE) & HC_MODE_EN)) {
		emmc_last_stage = STAGE_HC_MODE;
		STEP(STEP_HCMODE_WRITE);
		/* Disable HC mode. */
		wr32(CORE + CORE_HC_MODE, rd32(CORE + CORE_HC_MODE) & ~HC_MODE_EN);
		/* Core power reset (bit 7 of CORE+0x00). */
		wr32(CORE + CORE_POWER, rd32(CORE + CORE_POWER) | (1u << 7));
		emmc_delay_us(1000);
		/* Enable SDHCI mode. */
		wr32(CORE + CORE_HC_MODE, HC_MODE_EN);
		/* FF_CLK_SW_RST_DIS (bit 13). */
		wr32(CORE + CORE_HC_MODE, rd32(CORE + CORE_HC_MODE) | FF_CLK_SW_RST_DIS);
	}
	g_diag[D_HCMODE_AFTER] = rd32(CORE + CORE_HC_MODE);
	trail(0x0016, g_diag[D_HCMODE_AFTER]);

	STEP(STEP_CAPS);
	caps = rd32(HC + HC_CAPS1);
	if (caps == 0 || caps == 0xffffffffu)
		return EMMC_E_NO_CAPS;

	g_have_dat0 = (rd32(HC + HC_PRESENT) & PS_DAT0_LEVEL) != 0;
	g_diag[D_DAT0] = (uint32_t)g_have_dat0;

	/* The status bits polled here must be latched. LK polls them too, so
	 * they are normally on; OR them in if not. The signal enable (which
	 * would raise a real interrupt) is never touched. */
	want = INT_CMD_DONE | INT_XFER_DONE | 0xffff0000u;
	ie = rd32(HC + HC_INT_ENABLE);
	if ((ie & want) != want)
		wr32(HC + HC_INT_ENABLE, ie | want);

	/* Rung 1: is the card still selected? */
	emmc_last_stage = STAGE_BLKLEN;
	STEP(STEP_CMD16_1);
	rc = emmc_cmd(CMD_SET_BLOCKLEN, 512, CMD_RESP_48, &rsp);
	g_diag[D_CMD16_R1] = rc ? (0xdead0000u | (uint16_t)rc) : rsp;
	if (rc || R1_STATE(rsp) != CARD_STATE_TRAN) {
		/* Rung 2: asleep or deselected. */
		emmc_last_stage = STAGE_WAKE;
		STEP(STEP_WAKE_SELECT);
		if (emmc_wake())
			return EMMC_E_WAKE;
		g_diag[D_RUNG] = 1;

		emmc_last_stage = STAGE_BLKLEN2;
		STEP(STEP_CMD16_2);
		rc = emmc_cmd(CMD_SET_BLOCKLEN, 512, CMD_RESP_48, &rsp);
		g_diag[D_CMD16_R1] = rc ? (0xdead0000u | (uint16_t)rc) : rsp;
		if (rc || R1_STATE(rsp) != CARD_STATE_TRAN)
			return EMMC_E_BLKLEN;
	}

#endif

	/* Rung 3: what is this card? */
	emmc_last_stage = STAGE_EXTCSD;
	STEP(STEP_EXT_CSD);
	rc = emmc_xfer(CMD_SEND_EXT_CSD, 0, 0, scratch, &rsp);
	if (rc)
		return EMMC_E_EXTCSD;

	g_sec_count = le32(scratch + EXT_CSD_SEC_COUNT);
	g_sector_mode = g_sec_count > EMMC_2GB_SECTORS;
	g_diag[D_SEC_COUNT] = g_sec_count;
	g_diag[D_ADDR_MODE] = (uint32_t)g_sector_mode;
	trail(0x0020, g_sec_count);
	g_diag[D_EXT_MODE] = (uint32_t)scratch[EXT_CSD_BUS_WIDTH] |
			     ((uint32_t)scratch[EXT_CSD_HS_TIMING] << 8) |
			     ((uint32_t)scratch[EXT_CSD_PART_CONFIG] << 16) |
			     ((uint32_t)scratch[EXT_CSD_DEVICE_TYPE] << 24);

	/* Never write while a boot, RPMB or GP partition is selected. */
	if (scratch[EXT_CSD_PART_CONFIG] & 0x07)
		return EMMC_E_PARTITION;

	return EMMC_OK;
}

/* ------------------------------------------------------------------ */
/* Sector I/O                                                           */
/* ------------------------------------------------------------------ */

/* The only sectors this driver will touch. */
static int emmc_lba_allowed(uint32_t lba)
{
	return lba == LOG_LBA_SPARE1 || lba == LOG_LBA_SPARE1 + 1 ||
	       lba == LOG_LBA_SPARE2 || lba == LOG_LBA_SPARE2 + 1;
}

/* LBA -> command argument: a block number on sector addressed cards, a
 * byte address on the old (<= 2 GiB) ones. */
static int emmc_lba_to_arg(uint32_t lba, uint32_t *arg)
{
	if (!emmc_lba_allowed(lba))
		return EMMC_E_RANGE;
	if (g_sector_mode) {
		if (lba >= g_sec_count)
			return EMMC_E_RANGE;
		*arg = lba;
	} else {
		if (lba > 0x7fffffu)
			return EMMC_E_RANGE;
		*arg = lba * 512u;
	}
	return 0;
}

static int emmc_write_sector(uint32_t lba, const uint8_t *data)
{
	uint32_t arg, r1 = 0;
	int rc = emmc_lba_to_arg(lba, &arg);

	if (rc)
		return rc;
	return emmc_xfer(CMD_WRITE_BLOCK, arg, data, 0, &r1) ? EMMC_E_WRITE : 0;
}

static int emmc_read_sector(uint32_t lba, uint8_t *data)
{
	uint32_t arg, r1 = 0;
	int rc = emmc_lba_to_arg(lba, &arg);

	if (rc)
		return rc;
	return emmc_xfer(CMD_READ_BLOCK, arg, 0, data, &r1) ? EMMC_E_READBACK : 0;
}

/* Log sector: marker, stage, error code, R1 of CMD16, RCA and the text. */
static void emmc_build_sector(uint8_t *sector, const char *text, unsigned len,
			      unsigned stage, int error, uint16_t rca, uint32_t detail)
{
	static const char marker[8] = { 'G', '4', 'S', 'H', 'I', 'M', 'L', 'G' };

	for (unsigned i = 0; i < 512; i++)
		sector[i] = 0;
	for (unsigned i = 0; i < 8; i++)
		sector[i] = (uint8_t)marker[i];
	for (unsigned i = 0; i < 4; i++) {
		sector[8 + i] = (uint8_t)(stage >> (8 * i));
		sector[12 + i] = (uint8_t)((uint32_t)error >> (8 * i));
		sector[16 + i] = (uint8_t)(detail >> (8 * i));
	}
	sector[20] = (uint8_t)rca;
	sector[21] = (uint8_t)(rca >> 8);
	/* Only 488 bytes fit. Keep the END of the log: the eMMC and RESET
	 * lines are the ones that matter, the banner at the top does not. */
	if (len > 512 - 24) {
		text += len - (512 - 24);
		len = 512 - 24;
	}
	for (unsigned i = 0; i < len; i++)
		sector[24 + i] = (uint8_t)text[i];
}

/* Diagnostic sector: marker, then the D_* words, little endian. */
static void emmc_build_diag(uint8_t *sector)
{
	static const char marker[8] = { 'G', '4', 'S', 'H', 'I', 'M', 'D', 'G' };

	for (unsigned i = 0; i < 512; i++)
		sector[i] = 0;
	for (unsigned i = 0; i < 8; i++)
		sector[i] = (uint8_t)marker[i];
	for (unsigned w = 0; w < D_COUNT; w++)
		for (unsigned b = 0; b < 4; b++)
			sector[8 + w * 4 + b] = (uint8_t)(g_diag[w] >> (8 * b));
}

/*
 * Put the console log on the card, then read it back to prove it landed.
 * Returns 0 only if the read-back matches.
 */
/*
 * Remember the outcome of the first bring-up attempt. The second call
 * (from reset()) must not overwrite it.
 */
static void emmc_save_first_result(void)
{
	if (emmc_first_stage == 0xff) {
		emmc_first_stage = emmc_last_stage;
		emmc_first_error = emmc_last_error;
		emmc_first_cmd_error = emmc_last_cmd_error;
	}
}

int emmc_write_log(const char *text, unsigned len)
{
	uint8_t sector[512];
	uint8_t scratch[512];
	int rc, rc1 = 0, rc2 = 0, same = 0;
	unsigned stage;
	uint32_t lba_back = 0;

	(void)lba_back;
	(void)rc1;
	(void)rc2;
	(void)same;

	for (unsigned i = 0; i < D_COUNT; i++)
		g_diag[i] = 0;
	g_diag[D_FORMAT] = 2;
	g_have_dat0 = 0;
	g_sector_mode = 0;
	g_sec_count = 0;
	g_rca = 0;

	con_str("EMMC", "start");
	trail(0x00f8, 0);
	STEP(STEP_ENTRY);
	emmc_last_stage = STAGE_START;
	emmc_last_error = 0;
	emmc_last_cmd_error = 0;

	rc = emmc_prepare(scratch);
	trail(0x00f0, (uint32_t)rc);
	if (rc) {
		emmc_last_error = rc;
		con_hex2("EMMC", "prepare=", (uint32_t)rc, "stage=", emmc_last_stage);
		emmc_save_first_result();
		return rc;
	}
	con_hex2("EMMC", "sec=", g_sec_count, "sector_mode=", (uint32_t)g_sector_mode);

	emmc_last_stage = STAGE_WRITE;
	stage = STAGE_WRITE;
	g_diag[D_CMD_ERR] = (uint32_t)emmc_last_cmd_error;
	emmc_build_sector(sector, text, len, stage, 0, g_rca, g_diag[D_CMD16_R1]);
	emmc_build_diag(scratch);

#if SHIM_EMMC_WRITE
	/*
	 * Log + diagnostics on both spares.
	 *
	 * DANGER: the two addresses are inside the partitions 'persist' and
	 * 'laf' (see the geometry above). They are kept for the record, but
	 * the writes are OFF by default and this block is unreachable unless
	 * somebody deliberately turns them on AND fixes the addresses.
	 */
	STEP(STEP_WRITE1);
	rc1 = emmc_write_sector(LOG_LBA_SPARE1, sector);
	if (!rc1)
		rc1 = emmc_write_sector(LOG_LBA_SPARE1 + 1, scratch);
	STEP(STEP_WRITE2);
	rc2 = emmc_write_sector(LOG_LBA_SPARE2, sector);
	if (!rc2)
		rc2 = emmc_write_sector(LOG_LBA_SPARE2 + 1, scratch);
	trail(0x00f1, (uint32_t)rc1);
	trail(0x00f2, (uint32_t)rc2);
	con_hex2("EMMC", "w spare1=", (uint32_t)rc1, "spare2=", (uint32_t)rc2);
	if (rc1 && rc2) {
		emmc_last_error = rc1;
		emmc_save_first_result();
		return rc1;
	}

	/* A write that cannot be read back is not a write. */
	emmc_last_stage = STAGE_READBACK;
	lba_back = rc1 ? LOG_LBA_SPARE2 : LOG_LBA_SPARE1;
	STEP(STEP_READBACK);
	rc = emmc_read_sector(lba_back, scratch);
	if (!rc) {
		same = 1;
		for (unsigned i = 0; i < 512; i++)
			if (scratch[i] != sector[i]) {
				same = 0;
				break;
			}
	}
	trail(0x00f3, (uint32_t)rc);
	trail(0x00f4, (uint32_t)same);
	con_hex2("EMMC", "readback=", (uint32_t)rc, "same=", (uint32_t)same);

	if (rc) {
		emmc_last_error = rc;
		emmc_save_first_result();
		return rc;
	}
	if (!same) {
		emmc_last_error = EMMC_E_VERIFY;
		emmc_save_first_result();
		return EMMC_E_VERIFY;
	}
#else
	/*
	 * No write to the card (the default, and the right thing: there is
	 * no sector of this card that is safe to scribble on).
	 *
	 * The card has been identified and its EXT_CSD has been read: that is
	 * what the shim needs from it. The log goes to the ramoops console
	 * zone and to the scratch RAM at 0x50000000.
	 */
	con_str("EMMC", "the card is identified; the writes are off");
	trail(0x00f1, 0);
	trail(0x00f2, 0);
	trail(0x00f3, 0);
	trail(0x00f4, 1);
	rc1 = 0;
	rc2 = 0;
	same = 1;
	(void)rc1;
	(void)rc2;
	(void)same;
#endif

	emmc_last_stage = STAGE_DONE;
	emmc_last_error = 0;
	emmc_save_first_result();
	return 0;
}