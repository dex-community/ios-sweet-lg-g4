/*
 * A software model of the SDCC1 host controller and of a hypothetical eMMC
 * card, used ONLY in the QEMU build (SHIM_EMMC_MODEL=1).
 *
 * *** THIS IS NOT THE MSM8992 SDCC1 ***
 * There is no model of this controller anywhere: what follows is a made up
 * register file that behaves the way the code under test expects, so that
 * the shim's eMMC logic (state machine, order of the register accesses,
 * timeout handling, the (variant, stage) encoding, absence of crashes and
 * loops) can be exercised under QEMU, where no real controller exists.
 *
 * A green result here says NOTHING about the phone. Only a run on the real
 * hardware can say whether the card answers. See T2-RAPPORTO.md.
 *
 * What the model provides:
 *   - the two register windows, CORE (0xf9824000, 4 KB) and HC (0xf9824900,
 *     512 B), served from byte arrays;
 *   - the initial register values measured on the phone (DATA-RUN-6.txt
 *     section 2.1), so the shim starts from the same state it finds there;
 *   - a log of every access (order, direction, window, offset, size, value)
 *     in a block of RAM that the host tool extracts from the RAM dump;
 *   - a card that can be told to stay mute, to answer only after the PWRCTL
 *     acknowledgement, or to answer only after N milliseconds.
 *
 * The card is evaluated lazily: every access first runs model_poll(), which
 * decides whether the command the host has just written is answered yet.
 * That is what makes the "answer only after N ms" mode possible without an
 * interrupt-driven model.
 */

/*
 * The sizes come from the device tree:
 *   reg = <0xf9824900 0x1a0 0xf9824000 0x800>
 * so HC is 0x1a0 bytes at 0xf9824900 and CORE is 0x800 bytes at
 * 0xf9824000. They do NOT overlap: CORE ends at 0xf9824800.
 */
#define MODEL_HC_SIZE		0x1a0
#define MODEL_CORE_SIZE		0x800
/* The SDCC2 windows (the microSD slot): a SECOND host, served only when
 * its GCC clock branch is on. See model_window(). */
#define MODEL_SD2_HC_SIZE	0x100
#define MODEL_SD2_CORE_SIZE	0x800
/* The GCC (global clock controller) window: the first 8 KB is kept, which is
 * the whole window of the real device (qcom,gcc@fc400000, reg size 0x2000).
 * The shim touches SDCC1_APPS_CBCR (0x4c4), SDCC1_AHB_CBCR (0x4c8) and the
 * SDCC1 apps RCG (0x4d0..0x4e0); the SDCC2 ones are at 0x504..0x520.
 *
 * It must be the REAL size: an access outside the model's window would go
 * to the real address, which does not exist under QEMU (the first version
 * of the model kept 4 KB and the self-test panicked with
 * FAR=0xfc4004c4). */
#define MODEL_GCC_SIZE		0x2000
/* The global clock controller (same value as in emmc_lk_bringup.inc, which
 * is included after this file). */
#ifndef G4_GCC_BASE
#define G4_GCC_BASE		0xfc400000UL
#endif

/* The magic that lets the host tool find the log in the RAM dump. */
#define MODEL_LOG_MAGIC		"G4MMLOG1"

/* The header of the payload on the card (shim/sd_payload.h). */
#define SD_PAYLOAD_MAGIC	"G4PAYLD1"


/* Card modes. */
#define MODEL_MUTE		0	/* never answers anything            */
#define MODEL_ACK		1	/* answers only after the PWRCTL ack */
#define MODEL_DELAY		2	/* answers only after N ms from power */
#define MODEL_ALWAYS		3	/* always answers (positive control) */

/* Registers inside the model, relative to the window base. */
#define M_CORE_POWER	0x00
#define M_CORE_MCIVER	0x50
#define M_CORE_HCMODE	0x78
#define M_CORE_STATUS	0xdc	/* PWRCTL status  */
#define M_CORE_MASK	0xe0	/* PWRCTL mask    */
#define M_CORE_CLEAR	0xe4	/* PWRCTL clear   */
#define M_CORE_CTL	0xe8	/* PWRCTL ctl     */

#define M_HC_ARGUMENT	0x08
#define M_HC_TRANSFER	0x0c
#define M_HC_COMMAND	0x0e
#define M_HC_RESPONSE	0x10
#define M_HC_BUFPORT	0x20
#define M_HC_PRESENT	0x24
#define M_HC_CTRL1	0x28
#define M_HC_POWER	0x29
#define M_HC_CLKCTL	0x2c
#define M_HC_SWRST	0x2f
#define M_HC_INTSTATUS	0x30
#define M_HC_INTENABLE	0x34
#define M_HC_INTSIGNAL	0x38
#define M_HC_CTRL2	0x3c
#define M_HC_CAPS1	0x40
#define M_HC_CAPS2	0x44
#define M_HC_VENDOR	0x10c

/* PWRCTL bits, as in the CAF source (sdhci.h). */
#define PWRCTL_BUS_ON		0x1u
#define PWRCTL_BUS_OFF		0x2u
#define PWRCTL_IO_LOW		0x4u
#define PWRCTL_IO_HIGH		0x8u
#define PWRCTL_BUS_SUCCESS	0x1u
#define PWRCTL_IO_SUCCESS	0x4u

/* Present state bits the shim looks at. */
#define MPS_CMD_INHIBIT		0x00000001u
#define MPS_DATA_INHIBIT	0x00000002u
#define MPS_SPACE_AVAILABLE	0x00000400u
#define MPS_DATA_AVAILABLE	0x00000800u
#define MPS_DAT0_LEVEL		0x00100000u

/* Interrupt status bits. */
#define MINT_CMD_DONE		0x00000001u
#define MINT_XFER_DONE		0x00000002u

/* Command flags. */
#define MCMD_RESP_MASK		0x03u
#define MCMD_DATA_PRESENT	0x20u

/* SD commands the model answers (the microSD of the model). */
#define MCMD_SD_APP		55	/* CMD55: the prefix of ACMD41   */
#define MCMD_SD_IF_COND		8	/* CMD8: 0x1AA                 */
#define MCMD_SD_OP_COND		41	/* ACMD41                      */
#define MCMD_SD_ALL_SEND_CID	2	/* CMD2                        */
#define MCMD_SD_SET_REL_ADDR	3	/* CMD3                        */
#define MCMD_SD_SEND_CSD	9	/* CMD9                        */
#define MCMD_SD_SELECT		7	/* CMD7                        */
#define MCMD_SD_SET_BLOCKLEN	16	/* CMD16                       */
#define MCMD_SD_READ_BLOCK	17	/* CMD17                       */

/* MMC commands the model answers. */
#define MCMD_GO_IDLE		0
#define MCMD_SET_REL_ADDR	3
#define MCMD_SLEEP_AWAKE	5
#define MCMD_SELECT		7
#define MCMD_SEND_EXT_CSD	8
#define MCMD_SET_BLOCKLEN	16

/* The log entry. Fixed size, little endian, so the host tool can parse it
 * straight out of the RAM dump. */
struct model_log_entry {
	uint32_t seq;
	uint32_t dir;		/* 0 = read, 1 = write */
	uint32_t win;		/* 0 = CORE, 1 = HC, 2 = GCC */
	uint32_t off;
	uint32_t size;
	uint32_t val;
	uint32_t tsc;		/* low 32 bits of cntvct_el0 */
	uint32_t repeat;	/* how many times the very same access repeated */
};

struct model_log_hdr {
	char magic[8];
	uint32_t n;		/* number of entries */
	uint32_t mode;
	uint32_t delay_ms;
	uint32_t checksum;	/* sum of all the entries' words */
};

#define MODEL_LOG_MAX	4096

/*
 * NOT static: the host reads these out of the RAM dump, so the compiler
 * must not decide that the stores are dead. With "static" GCC removed
 * them entirely (the magic was not in the dump).
 *
 * The header and the entries are ONE block, so that the entries follow
 * the header with no gap: that is how the host tool finds them.
 */
struct model_log_block {
	struct model_log_hdr hdr;
	uint32_t xfer_words;	/* bulk accesses to the buffer port */
	uint32_t pad;
	struct model_log_entry e[MODEL_LOG_MAX];
};
struct model_log_block emmc_model_log;
static unsigned model_log_n;

static uint8_t model_core[MODEL_CORE_SIZE];
static uint8_t model_hc[MODEL_HC_SIZE];
static uint8_t model_gcc[MODEL_GCC_SIZE];
static uint8_t model_sd2_core[MODEL_SD2_CORE_SIZE];
static uint8_t model_sd2_hc[MODEL_SD2_HC_SIZE];

/*
 * Is the SDCC2 controller clocked?
 *
 * This is the point of the model for SDCC2: a write to a controller whose
 * clock is off NEVER COMPLETES (that is what hung the phone twice on
 * 2026-11-09). The model reproduces it: while the clock is off, the
 * SDCC2 windows are NOT served, so the access goes to the real address,
 * which under QEMU does not exist -> the guest faults and the test
 * fails. So a test that passes has PROVEN that the shim turns the clock on
 * before touching the controller.
 */
static uint32_t model_sd2_clocked;
/* The command the host has pending on the SDCC2 host. */
static uint32_t model_sd2_cmd_pending;
static uint32_t model_sd2_cmd_index;
static uint32_t model_sd2_cmd_flags;
static uint32_t model_sd2_cmd_arg;
static uint32_t model_sd2_app_prefix;	/* CMD55 has been seen            */
static uint32_t model_sd2_rsp[4];

/* The SD card of the model: it answers the SD commands. */
static uint32_t model_sd2_rca;
static uint32_t model_sd2_cid[4];
static uint8_t model_sd2_csd[16];
static uint8_t model_sd2_data[512];
static uint32_t model_sd2_data_len;
static uint32_t model_sd2_data_pos;
static uint8_t model_sd2_disk[512];
static uint32_t model_sd2_disk_valid;
/*
 * The blocks of the card, as the layout of shim/sd_payload.h describes:
 * the payload header is at block 2048 and the payload itself follows. The
 * model serves them so that the test can check the whole path (header,
 * CRC, copy into RAM).
 */
#define MODEL_SD2_DISK_BLOCKS	8
static uint8_t model_sd2_blocks[MODEL_SD2_DISK_BLOCKS][512];
static uint32_t model_sd2_disk_blocks;
#define MODEL_SD2_PAYLOAD_LBA	2048

/* Card state. */
static uint32_t model_mode = MODEL_MUTE;
/* The device tree can ask for a test payload on the card (the QEMU test
 * of shim/sd_payload.c). */
static uint32_t model_sd_payload;
static uint32_t model_delay_ms;
static uint32_t model_pwr_on;		/* the bus power has been turned on  */
static uint32_t model_pwr_tsc;		/* when that happened               */
static uint32_t model_ack_seen;		/* the PWRCTL ack has been written  */
static uint32_t model_cmd_pending;
static uint32_t model_cmd_index;
static uint32_t model_cmd_flags;
static uint32_t model_cmd_arg;
static uint32_t model_rsp[4];
static uint8_t model_data[512];
static uint8_t model_disk[512];	/* the last sector written */
static int model_disk_valid;
static uint32_t model_data_len;
static uint32_t model_data_pos;
static uint32_t model_wr_words;
static uint32_t model_xfer_words;

static uint64_t model_ticks(void)
{
	uint64_t v;

	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

static uint32_t model_tick_hz(void)
{
	uint64_t v;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return (uint32_t)v;
}

static uint32_t model_get(const uint8_t *p, unsigned size)
{
	uint32_t v = 0;

	for (unsigned i = 0; i < size; i++)
		v |= (uint32_t)p[i] << (8 * i);
	return v;
}

static void model_set(uint8_t *p, unsigned size, uint32_t v)
{
	for (unsigned i = 0; i < size; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

/* ------------------------------------------------------------------ */
/* The card                                                            */
/* ------------------------------------------------------------------ */

/* A synthetic EXT_CSD: SEC_COUNT at byte 212 (little endian), the rest
 * zeros. The value is the one measured on the phone (run 5). */
static void model_prepare_ext_csd(void)
{
	uint32_t sec = 0x03a3e000u;

	for (unsigned i = 0; i < 512; i++)
		model_data[i] = 0;
	model_data[212] = (uint8_t)sec;
	model_data[213] = (uint8_t)(sec >> 8);
	model_data[214] = (uint8_t)(sec >> 16);
	model_data[215] = (uint8_t)(sec >> 24);
	model_data_len = 512;
	model_data_pos = 0;
}

static void model_build_response(void)
{
	/* No data phase unless the command below asks for one. */
	model_data_len = 0;
	model_data_pos = 0;

	switch (model_cmd_index) {
	case MCMD_GO_IDLE:
		model_rsp[0] = 0;
		break;
	case 1:
		/* CMD1 (SEND_OP_COND): R3. Bit 31 = the card is ready, bit 30
		 * = sector mode. 0xc0ff8000 is what a card that is ready
		 * answers. */
		model_rsp[0] = 0xc0ff8000u;
		break;
	case 2:
		/* CMD2 (ALL_SEND_CID): R2, the first word of the CID that was
		 * read on the phone. */
		model_rsp[0] = 0x11010030u;
		break;
	case MCMD_SET_REL_ADDR:
		model_rsp[0] = 0x00000900u;	/* R1, state = transfer */
		break;
	case MCMD_SLEEP_AWAKE:
		model_rsp[0] = 0x40ff8000u;	/* R3, the card is awake */
		break;
	case MCMD_SELECT:
		model_rsp[0] = 0x00000900u;	/* R1 */
		break;
	case MCMD_SET_BLOCKLEN:
		model_rsp[0] = 0x00000900u;	/* R1 */
		break;
	case MCMD_SEND_EXT_CSD:
		model_rsp[0] = 0x00000900u;	/* R1 */
		model_prepare_ext_csd();
		break;
	case 17:
		/* CMD17 (READ_BLOCK): give back the sector that was written
		 * last, so that the read-back check has something to
		 * compare. */
		model_rsp[0] = 0x00000900u;	/* R1 */
		for (unsigned i = 0; i < 512; i++)
			model_data[i] = model_disk_valid ? model_disk[i] : 0;
		model_data_len = 512;
		model_data_pos = 0;
		break;
	default:
		model_rsp[0] = 0x00000900u;
		break;
	}

	/* The host reads the response from HC+0x10. */
	model_set(model_hc + M_HC_RESPONSE, 4, model_rsp[0]);
}

/* ------------------------------------------------------------------ */
/* The card of the SDCC2 slot                                          */
/* ------------------------------------------------------------------ */

/*
 * The response of the model SD card. The sequence is the one of the CAF
 * core (drivers/mmc/core/sd_ops.c): CMD8 answers R7 with 0x1AA, ACMD41
 * answers R3 with the OCR, CMD2 the CID, CMD3 the RCA, CMD7/CMD9/CMD16 R1.
 */
static void model_sd2_build_response(void)
{
	model_sd2_data_len = 0;
	model_sd2_data_pos = 0;

	switch (model_sd2_cmd_index) {
	case 0:		/* CMD0: no response */
		model_sd2_rsp[0] = 0;
		break;
	case MCMD_SD_IF_COND:	/* CMD8: R7, the pattern in the low byte */
		model_sd2_rsp[0] = 0x000001aa;
		break;
	case MCMD_SD_APP:	/* CMD55: R1, it only opens the APP prefix */
		model_sd2_rsp[0] = 0x00000900;
		break;
	case MCMD_SD_OP_COND:	/* ACMD41: R3, bit 31 = power-up complete */
		model_sd2_rsp[0] = 0xc0ff8080;
		break;
	case MCMD_SD_ALL_SEND_CID:	/* CMD2: R2 */
		model_sd2_rsp[0] = model_sd2_cid[0];
		break;
	case MCMD_SD_SET_REL_ADDR:	/* CMD3: R1, the RCA */
		model_sd2_rsp[0] = 0x00000900;
		break;
	case MCMD_SD_SELECT:		/* CMD7: R1b */
		model_sd2_rsp[0] = 0x00000900;
		break;
	case MCMD_SD_SEND_CSD:		/* CMD9: R1 + the CSD */
		model_sd2_rsp[0] = 0x00000900;
		for (unsigned i = 0; i < 16; i++)
			model_sd2_data[i] = model_sd2_csd[i];
		model_sd2_data_len = 16;
		break;
	case MCMD_SD_SET_BLOCKLEN:	/* CMD16: R1 */
		model_sd2_rsp[0] = 0x00000900;
		break;
	case MCMD_SD_READ_BLOCK: {	/* CMD17: R1 + one sector */
		uint32_t lba = model_sd2_cmd_arg;
		uint32_t idx = (lba >= MODEL_SD2_PAYLOAD_LBA) ?
				lba - MODEL_SD2_PAYLOAD_LBA : lba;

		model_sd2_rsp[0] = 0x00000900;
		for (unsigned i = 0; i < 512; i++)
			model_sd2_data[i] = 0;
		if (idx < model_sd2_disk_blocks) {
			for (unsigned i = 0; i < 512; i++)
				model_sd2_data[i] = model_sd2_blocks[idx][i];
		} else if (idx == 0 && model_sd2_disk_valid) {
			for (unsigned i = 0; i < 512; i++)
				model_sd2_data[i] = model_sd2_disk[i];
		}
		model_sd2_data_len = 512;
		break;
	}
	default:
		model_sd2_rsp[0] = 0x00000900;
		break;
	}

	/* The host reads the response from HC+0x10. */
	model_set(model_sd2_hc + M_HC_RESPONSE, 4, model_sd2_rsp[0]);
}

/* Is the SD card ready to answer the command that is pending? */
static int model_sd2_card_ready(void)
{
	if (model_mode == MODEL_MUTE)
		return 0;
	return 1;
}

/* Called at every access: answers the pending SD command, if the card is
 * ready to. */
static void model_sd2_poll(void)
{
	uint32_t intstat;

	if (!model_sd2_cmd_pending)
		return;
	if (!model_sd2_card_ready())
		return;

	model_sd2_cmd_pending = 0;
	model_sd2_build_response();

	/* A read command with a data phase also makes the data available. */
	if ((model_sd2_cmd_flags & MCMD_DATA_PRESENT) &&
	    (model_get(model_sd2_hc + M_HC_TRANSFER, 2) & 0x10u))
		model_set(model_sd2_hc + M_HC_PRESENT, 4,
			  model_get(model_sd2_hc + M_HC_PRESENT, 4) |
			  MPS_DATA_AVAILABLE);

	intstat = model_get(model_sd2_hc + M_HC_INTSTATUS, 4) | MINT_CMD_DONE;
	model_set(model_sd2_hc + M_HC_INTSTATUS, 4, intstat);
}

/* Is the card ready to answer the command that is pending? */
static int model_card_ready(void)
{
	uint64_t now;

	switch (model_mode) {
	case MODEL_ACK:
		return model_ack_seen != 0;
	case MODEL_DELAY:
		if (!model_pwr_on)
			return 0;
		now = model_ticks();
		return (now - model_pwr_tsc) * 1000ull / model_tick_hz() >=
		       model_delay_ms;
	case MODEL_ALWAYS:
		return 1;
	case MODEL_MUTE:
	default:
		return 0;
	}
}

/* Called at every access: decides whether the pending command is answered. */
static void model_poll(void)
{
	uint32_t intstat;

	if (!model_cmd_pending)
		return;
	if (!model_card_ready())
		return;

	model_cmd_pending = 0;
	model_build_response();

	/* A read command with a data phase also makes the data available. */
	if ((model_cmd_flags & MCMD_DATA_PRESENT) &&
	    (model_get(model_hc + M_HC_TRANSFER, 2) & 0x10u))
		model_set(model_hc + M_HC_PRESENT, 4,
			  model_get(model_hc + M_HC_PRESENT, 4) |
			  MPS_DATA_AVAILABLE);

	intstat = model_get(model_hc + M_HC_INTSTATUS, 4) | MINT_CMD_DONE;
	model_set(model_hc + M_HC_INTSTATUS, 4, intstat);
}

/* ------------------------------------------------------------------ */
/* The register windows                                                */
/* ------------------------------------------------------------------ */

static uint32_t model_read_reg(unsigned win, unsigned off, unsigned size)
{
	const uint8_t *p;

	if (win == 0)
		p = model_core + off;
	else if (win == 1)
		p = model_hc + off;
	else if (win == 3)
		p = model_sd2_core + off;
	else if (win == 4)
		p = model_sd2_hc + off;
	else
		p = model_gcc + off;

	if (win == 0) {
		if (off == M_CORE_MCIVER)
			return 0x1000003e;	/* measured on the phone */
		return model_get(p, size);
	}

	switch (off) {
	case M_HC_CAPS1:
		return 0x342dc8b2;	/* measured on the phone */
	case M_HC_CAPS2:
		return 0x00008007;	/* measured on the phone */
	case M_HC_PRESENT: {
		uint32_t v = 0x01f80000u | MPS_SPACE_AVAILABLE | MPS_DAT0_LEVEL;

		if (win == 4) {
			if (model_sd2_data_len &&
			    model_sd2_data_pos < model_sd2_data_len)
				v |= MPS_DATA_AVAILABLE;
			return v;
		}
		if (model_data_len && model_data_pos < model_data_len)
			v |= MPS_DATA_AVAILABLE;
		return v;
	}
	case M_HC_SWRST:
		return 0;		/* the reset is always complete */
	case M_HC_BUFPORT: {
		uint32_t pos, v = 0;

		if (win == 4) {
			pos = model_sd2_data_pos;
			if (!model_sd2_data_len)
				return 0;
			for (unsigned i = 0; i < 4 && pos + i < model_sd2_data_len; i++)
				v |= (uint32_t)model_sd2_data[pos + i] << (8 * i);
			pos += 4;
			if (pos >= model_sd2_data_len) {
				model_sd2_data_pos = model_sd2_data_len;
				model_set(model_sd2_hc + M_HC_PRESENT, 4,
					  model_get(model_sd2_hc + M_HC_PRESENT, 4) &
					  ~MPS_DATA_AVAILABLE);
				model_set(model_sd2_hc + M_HC_INTSTATUS, 4,
					  model_get(model_sd2_hc + M_HC_INTSTATUS, 4) |
					  MINT_XFER_DONE);
			} else {
				model_sd2_data_pos = pos;
			}
			return v;
		}
		pos = model_data_pos;
		if (!model_data_len)
			return 0;
		for (unsigned i = 0; i < 4 && pos + i < model_data_len; i++)
			v |= (uint32_t)model_data[pos + i] << (8 * i);
		pos += 4;
		if (pos >= model_data_len) {
			model_data_pos = model_data_len;
			model_set(model_hc + M_HC_PRESENT, 4,
				  model_get(model_hc + M_HC_PRESENT, 4) &
				  ~MPS_DATA_AVAILABLE);
			model_set(model_hc + M_HC_INTSTATUS, 4,
				  model_get(model_hc + M_HC_INTSTATUS, 4) |
				  MINT_XFER_DONE);
		} else {
			model_data_pos = pos;
		}
		return v;
	}
	default:
		break;
	}
	return model_get(p, size);
}

static void model_write_reg(unsigned win, unsigned off, unsigned size,
			    uint32_t val)
{
	uint8_t *p;

	if (win == 0)
		p = model_core + off;
	else if (win == 1)
		p = model_hc + off;
	else if (win == 3)
		p = model_sd2_core + off;
	else if (win == 4)
		p = model_sd2_hc + off;
	else {
		/* The clock controller: writing the enable bit starts the
		 * clock, and the CLK_OFF bit (31) clears when it runs. The
		 * shim polls for it, so it has to clear right away.
		 *
		 * The SDCC2 branches are the ones that bring the SDCC2
		 * controller to life: until they are on, the controller's
		 * windows are NOT served, so a write to it does not
		 * complete (the stall that hung the phone). */
		p = model_gcc + off;
		model_set(p, size, (val & ~0x80000000u) | 1u);
		if (off == 0x504 || off == 0x508) {
			if (val & 1u)
				model_sd2_clocked = 1;
		}
		return;
	}

	switch (off) {
	case M_CORE_POWER:
		/* The core reset bit clears itself when the reset is over. */
		model_set(p, size, val & ~(uint32_t)(1u << 7));
		return;
	case M_CORE_CLEAR:
		/* Write-1-to-clear on the PWRCTL status. */
		model_set(model_core + M_CORE_STATUS, 4,
			  model_get(model_core + M_CORE_STATUS, 4) & ~val);
		return;
	case M_CORE_CTL:
		model_set(p, size, val);
		return;
	case M_HC_POWER:
		model_set(p, size, val);
		if (val & 1u) {
			/* The bus goes up: the controller latches the bus-on
			 * transaction in the PWRCTL status. */
			if (win == 4) {
				model_set(model_sd2_core + M_CORE_STATUS, 4,
					  model_get(model_sd2_core + M_CORE_STATUS, 4) |
					  PWRCTL_BUS_ON);
				return;
			}
			model_set(model_core + M_CORE_STATUS, 4,
				  model_get(model_core + M_CORE_STATUS, 4) |
				  PWRCTL_BUS_ON);
		}
		return;
	case M_HC_CLKCTL:
		/* The clock is stable as soon as it is programmed. */
		model_set(p, size, val | 2u);
		return;
	case M_HC_SWRST:
		/* The software reset completes immediately. */
		model_set(p, size, 0);
		return;
	case M_HC_INTSTATUS:
		/* Write-1-to-clear. */
		model_set(p, size, model_get(p, size) & ~val);
		return;
	case M_HC_COMMAND:
		model_set(p, size, val);
		if (win == 4) {
			model_sd2_cmd_index = (val >> 8) & 0xffu;
			model_sd2_cmd_flags = val & 0xffu;
			model_sd2_cmd_arg = model_get(model_sd2_hc + M_HC_ARGUMENT, 4);
			model_sd2_cmd_pending = 1;
			return;
		}
		model_cmd_index = (val >> 8) & 0xffu;
		model_cmd_flags = val & 0xffu;
		model_cmd_arg = model_get(model_hc + M_HC_ARGUMENT, 4);
		model_cmd_pending = 1;
		return;
	case M_HC_BUFPORT:
		/* The host is pushing a block to the card: keep it (so that a
		 * read-back can find it) and report the transfer as complete
		 * when the 512 bytes are in. */
		if (model_wr_words < 128) {
			for (unsigned i = 0; i < 4; i++)
				model_disk[model_wr_words * 4 + i] =
					(uint8_t)(val >> (8 * i));
		}
		model_wr_words++;
		if (model_wr_words >= 128) {
			model_wr_words = 0;
			model_disk_valid = 1;
			model_set(model_hc + M_HC_INTSTATUS, 4,
				  model_get(model_hc + M_HC_INTSTATUS, 4) |
				  MINT_XFER_DONE);
		}
		return;
	default:
		model_set(p, size, val);
		return;
	}
}

/* ------------------------------------------------------------------ */
/* The access log                                                      */
/* ------------------------------------------------------------------ */

static void model_log_add(unsigned dir, unsigned win, unsigned off,
			  unsigned size, uint32_t val)
{
	struct model_log_entry *e;

	/* Consecutive identical accesses (the polling loops) are collapsed
	 * into one entry with a repeat counter, otherwise the log is all
	 * noise. */
	if (model_log_n) {
		e = &emmc_model_log.e[model_log_n - 1];
		if (e->dir == dir && e->win == win && e->off == off &&
		    e->size == size && e->val == val) {
			e->repeat++;
			return;
		}
	}
	/* The bulk data port is not logged word by word: it would bury
	 * everything else. It is counted instead. */
	if (win == 1 && off == M_HC_BUFPORT) {
		model_xfer_words++;
		return;
	}
	if (model_log_n >= MODEL_LOG_MAX)
		return;
	e = &emmc_model_log.e[model_log_n++];
	e->seq = model_log_n;
	e->dir = dir;
	e->win = win;
	e->off = off;
	e->size = size;
	e->val = val;
	e->tsc = (uint32_t)model_ticks();
	e->repeat = 1;
}

/* ------------------------------------------------------------------ */
/* The entry points used by the shim                                   */
/* ------------------------------------------------------------------ */

uint32_t emmc_model_rd(uintptr_t a, unsigned size);
void emmc_model_wr(uintptr_t a, unsigned size, uint32_t v);

static int model_window(uintptr_t a, unsigned *win, unsigned *off)
{
	uintptr_t core = G4_EMMC_CORE_BASE;
	uintptr_t hc = G4_EMMC_BASE;
	uintptr_t gcc = G4_GCC_BASE;
	uintptr_t sd2_core = G4_SD_CORE_BASE;
	uintptr_t sd2_hc = G4_SD_BASE;

	if (a >= core && a < core + MODEL_CORE_SIZE) {
		*win = 0;
		*off = (unsigned)(a - core);
		return 1;
	}
	if (a >= hc && a < hc + MODEL_HC_SIZE) {
		*win = 1;
		*off = (unsigned)(a - hc);
		return 1;
	}
	if (a >= gcc && a < gcc + MODEL_GCC_SIZE) {
		*win = 2;
		*off = (unsigned)(a - gcc);
		return 1;
	}
	/*
	 * The SDCC2 windows are served ONLY when the clock branch is on:
	 * without it an access would not complete, exactly like on the phone.
	 */
	if (model_sd2_clocked) {
		if (a >= sd2_core && a < sd2_core + MODEL_SD2_CORE_SIZE) {
			*win = 3;
			*off = (unsigned)(a - sd2_core);
			return 1;
		}
		if (a >= sd2_hc && a < sd2_hc + MODEL_SD2_HC_SIZE) {
			*win = 4;
			*off = (unsigned)(a - sd2_hc);
			return 1;
		}
	}
	return 0;
}

uint32_t emmc_model_rd(uintptr_t a, unsigned size)
{
	unsigned win, off;
	uint32_t v;

	if (!model_window(a, &win, &off))
		return *(volatile uint32_t *)a;

	model_poll();
	model_sd2_poll();
	v = model_read_reg(win, off, size);
	model_log_add(0, win, off, size, v);
	return v;
}

void emmc_model_wr(uintptr_t a, unsigned size, uint32_t v)
{
	unsigned win, off;

	if (!model_window(a, &win, &off)) {
		*(volatile uint32_t *)a = v;
		return;
	}

	model_poll();
	model_sd2_poll();
	model_write_reg(win, off, size, v);
	model_log_add(1, win, off, size, v);
}

/* ------------------------------------------------------------------ */
/* Configuration and initial values                                    */
/* ------------------------------------------------------------------ */

static void model_put(unsigned win, unsigned off, unsigned size, uint32_t v)
{
	model_set((win == 0) ? model_core + off : model_hc + off, size, v);
}

/*
 * Read the mode from the device tree:
 *   /chosen/emmc-model     0 mute, 1 ack, 2 delay, 3 always
 *   /chosen/emmc-delay-ms  how long the delay mode waits (default 100)
 *
 * The initial register values are the ones measured on the phone at the
 * shim entry (DATA-RUN-6.txt section 2.1).
 */
void emmc_model_config(uintptr_t dtb)
{
	uint64_t v;

	/* The defaults first, then the device tree over them: the tree is
	 * read BEFORE anything else, because what it asks for (the card
	 * mode, the delay, the test payload of the card) is used by the
	 * code below. */
	model_mode = MODEL_MUTE;
	model_delay_ms = 100;
	model_sd_payload = 0;
	if (dtb) {
		if (fdt_prop_u64(dtb, "/chosen", "emmc-model", &v))
			model_mode = (uint32_t)v;
		if (fdt_prop_u64(dtb, "/chosen", "emmc-delay-ms", &v))
			model_delay_ms = (uint32_t)v;
		if (fdt_prop_u64(dtb, "/chosen", "sd-payload", &v))
			model_sd_payload = (uint32_t)v;
	}

	/*
	 * A test payload on the card, so that the whole path of
	 * shim/sd_payload.c can be exercised: the header at block 2048, the
	 * payload at 2049, with the CRC that the shim computes.
	 */
	if (model_sd_payload) {
		uint32_t len = 512;
		uint32_t crc;

		for (unsigned i = 0; i < 512; i++)
			model_sd2_blocks[1][i] = (uint8_t)(0x5a ^ i);
		for (unsigned i = 0; i < 8; i++)
			model_sd2_blocks[0][i] = SD_PAYLOAD_MAGIC[i];
		{
			uint32_t *h = (uint32_t *)(model_sd2_blocks[0] + 8);

			h[0] = 1;			/* version */
			h[1] = len;			/* len    */
			h[2] = 0;			/* crc, below */
			h[3] = 0;			/* flags */
		}
		crc = crc32_le(0xffffffffu, model_sd2_blocks[1], len) ^ 0xffffffffu;
		{
			uint32_t *h = (uint32_t *)(model_sd2_blocks[0] + 8);

			h[2] = crc;
		}
		model_sd2_disk_blocks = 2;
		trail(0x2800, crc);
	}

	model_put(0, M_CORE_POWER, 4, 0x41);
	model_put(0, M_CORE_MCIVER, 4, 0x1000003e);
	model_put(0, M_CORE_HCMODE, 4, 0x01);
	model_put(0, M_CORE_STATUS, 4, 0);
	model_put(0, M_CORE_MASK, 4, 0);
	model_put(0, M_CORE_CLEAR, 4, 0);
	model_put(0, M_CORE_CTL, 4, 0);
	model_put(0, 0x100, 4, 0x60006400);
	model_put(0, 0x10c, 4, 0);

	model_put(1, M_HC_ARGUMENT, 4, 0);
	model_put(1, M_HC_TRANSFER, 2, 0);
	model_put(1, M_HC_COMMAND, 2, 0);
	model_put(1, M_HC_RESPONSE, 4, 0);
	model_put(1, M_HC_PRESENT, 4, 0x01f80000);
	model_put(1, M_HC_CTRL1, 4, 0x00000f00);
	model_put(1, M_HC_POWER, 1, 0x00);
	model_put(1, M_HC_CLKCTL, 4, 0x000ffa07);
	model_put(1, M_HC_SWRST, 1, 0);
	model_put(1, M_HC_INTSTATUS, 4, 0);
	model_put(1, M_HC_INTENABLE, 4, 0x03ff80ff);
	model_put(1, M_HC_INTSIGNAL, 4, 0);
	model_put(1, M_HC_CTRL2, 4, 0);
	model_put(1, M_HC_CAPS1, 4, 0x342dc8b2);
	model_put(1, M_HC_CAPS2, 4, 0x00008007);
	model_put(1, 0x100, 4, 0x60006400);
	model_put(1, M_HC_VENDOR, 4, 0x00000a1c);

	model_sd2_clocked = 0;
	model_sd2_cmd_pending = 0;
	model_sd2_app_prefix = 0;
	model_sd2_rca = 1;
	model_sd2_data_len = 0;
	model_sd2_data_pos = 0;
	model_sd2_disk_valid = 0;
	/* The CID and the CSD of the model card: the CID is the one of the
	 * microSD of the phone (SS32G), the CSD is a plausible SDHC one. */
	model_sd2_disk_blocks = 0;

	model_sd2_cid[0] = 0x03534453;
	model_sd2_cid[1] = 0x53333247;
	model_sd2_cid[2] = 0x80a46843;
	model_sd2_cid[3] = 0x0e010a00;
	for (unsigned i = 0; i < 16; i++)
		model_sd2_csd[i] = 0;
	model_sd2_csd[0] = 0x40;		/* the structure of the CSD */
	model_sd2_csd[1] = 0x0e;		/* CSD version 2.0 (SDHC)  */
	model_pwr_on = 0;
	model_ack_seen = 0;
	model_cmd_pending = 0;
	model_data_len = 0;
	model_data_pos = 0;
	model_wr_words = 0;
	model_xfer_words = 0;
	model_disk_valid = 0;
	model_log_n = 0;

	for (unsigned i = 0; i < 8; i++)
		emmc_model_log.hdr.magic[i] = MODEL_LOG_MAGIC[i];


	emmc_model_log.hdr.n = 0;
	emmc_model_log.hdr.mode = model_mode;
	emmc_model_log.hdr.delay_ms = model_delay_ms;
	emmc_model_log.hdr.checksum = 0;
}

/* Called at the end of the run: close the log so that the host tool can
 * find it in the RAM dump. */
void emmc_model_finish(void)
{
	uint32_t sum = 0;

	for (unsigned i = 0; i < model_log_n; i++) {
		const struct model_log_entry *e = &emmc_model_log.e[i];

		sum += e->seq + e->dir + e->win + e->off + e->size + e->val +
		       e->tsc + e->repeat;
	}
	emmc_model_log.xfer_words = model_xfer_words;
	emmc_model_log.hdr.n = model_log_n;
	emmc_model_log.hdr.mode = model_mode;
	emmc_model_log.hdr.delay_ms = model_delay_ms;
	emmc_model_log.hdr.checksum = sum;
}
