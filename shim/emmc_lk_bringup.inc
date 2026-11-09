/*
 * emmc_lk_bringup(): bring the SDCC1 host and the eMMC back from the state
 * LK leaves them in, by replaying what the factory LK does at boot.
 *
 * State at shim entry (from the LK binary, research/lk-g4/dis-4.*):
 *   target_uninit -> mmc_put_card_to_sleep(): CMD7(deselect), CMD5(RCA<<16|0x8000)
 *   sdhci_mode_disable():                      CORE+0x78 &= ~HC_MODE_EN
 * so the card sleeps (possibly in HS400) and the SDHCI register block is
 * switched off.
 *
 * Why the earlier "re-enable HC mode and talk" approach gives a mute card:
 * the core reset resets the SDHCI side to its power-on state: SD clock off,
 * no divider, 1 bit, status enables cleared. LK itself never talks to the
 * card right after that reset; mmc_init() (0x0f9155f0) first does, in order,
 * (a) clocks, (b) sdhci_msm_init, (c) caps -> base clock, (d) 400 kHz clock,
 * (e) CMD0, CMD1.., CMD2, CMD3, CMD7. This function is that sequence.
 *
 * Provenance of each step (F = fact read from the LK disassembly, I = inferred
 * from the standard SDHCI spec / Linux sdhci-msm, not seen in the LK dump):
 *   GCC CBCR enable +0x4c8 / +0x4c4 ........ F (clk_enable, 0x0f9184b8); the
 *                                              register numbers are confirmed
 *                                              by the LK clock table
 *                                              (dis-4.9-k: sdc1_core_clk ->
 *                                              0x4c4, sdc1_iface_clk -> 0x4c8)
 *                                              and by clock-gcc-8992.c
 *   CORE+0x78 off, CORE+0x00|=0x80, wait,
 *     CORE+0x78 on, |=0x2000 ................ F (sdhci_msm_init)
 *   HC+0x2f = 1, poll <= 100 x 1 ms ......... F (0x0f912e20, r1 == 1)
 *   PWRCTL: CLEAR=STATUS, CTL|=1/4, MASK=0xf  F (sdhci_msm_init)
 *   HC+0x11c = HC+0x40 | 1<<26 ............. F (when MCI version major != 0 and
 *                                              minor not 0x11/0x12); the address
 *                                              is HC+0x11c, NOT CORE+0x11c:
 *                                              the CAF driver reaches that
 *                                              register through host->ioaddr
 *                                              (the HC base) too
 *   base clock = caps[15:8] MHz ............ F (0x0f913954)
 *   divider loop, clock reg value .......... F (0x0f912ec4, up to the point
 *                                              where the dump is cut)
 *   write clock reg, wait stable, |= 4 ..... I (standard SDHCI)
 *   HC+0x2e = 0xf ........................... F (0x0f913200, busy commands)
 *   HC+0x29 power on 1.8 V .................. I (LK writes 0x0e then 0x0f, that
 *                                              is 3.3 V, at every boot)
 *   CMD5 awake .............................. I (spec; LK uses the sleep bit)
 *   CMD0, CMD1(0x40ff0000 the first time), CMD2, CMD3(RCA 2<<16), CMD7 .. F
 *
 * The "v" parameter carries the few points where the LK, the working Linux
 * driver and this file disagree; the diagnostic build (EMMC_DIAG_VARIANTS)
 * sweeps them one at a time to find out which one decides whether the card
 * answers. The default variant is the one that mimics the LK.
 */
#ifndef G4_GCC_BASE
#define G4_GCC_BASE		0xfc400000UL
#endif
#ifndef GCC_SDCC1_APPS_CBCR
#define GCC_SDCC1_APPS_CBCR	0x4c4
#endif
#ifndef GCC_SDCC1_AHB_CBCR
#define GCC_SDCC1_AHB_CBCR	0x4c8
#endif
#define CORE_PWRCTL_STATUS	0xdc
#define CORE_PWRCTL_MASK	0xe0
#define CORE_PWRCTL_CLEAR	0xe4
#define CORE_PWRCTL_CTL		0xe8
/*
 * The vendor spec register is reached through the HC window: the CAF driver
 * accesses it as host->ioaddr + CORE_VENDOR_SPEC, and host->ioaddr is the
 * SDHCI base (0xf9824900). The LK writes the same address (0xf9824a0c,
 * dis-4.9-d 0x0f913f98).
 *
 * CORE_CLK_PWRSAVE: with this bit the controller gates the card clock by
 * itself when there is no traffic (the Linux driver clears it while it
 * talks to the card).
 * CORE_HC_MCLK_SEL: which clock the MCI uses (2 = free running MCLK,
 * 3 = the HS400 one). CORE_HC_SELECT_IN: the CDC input select (6 = HS400).
 * The Linux driver puts both back to the default before it talks to a card
 * that is not in HS400.
 */
#define HC_VENDOR_SPEC		0x10c
#define CORE_CLK_PWRSAVE	(1u << 1)
#define CORE_HC_MCLK_SEL_MASK	(3u << 8)
#define CORE_HC_MCLK_SEL_DFLT	(2u << 8)
#define CORE_HC_SELECT_IN_EN	(1u << 18)
#define CORE_HC_SELECT_IN_MASK	(7u << 19)
#define HC_CAPS_OVERRIDE	0x11c
#define HC_POWER_CTRL		0x29
#define HC_TIMEOUT_CTRL		0x2e
#define HC_HOST_CTRL2		0x3e
#define LK_OCR			0x40ff8000u	/* LK: 0x40ff0000 the first time */
#define LK_OCR_FIRST		0x40ff0000u
#define LK_RCA			2u
/* R3 (OCR): 48 bit, no CRC and no index check. emmc_cmd() only adds the
 * checks for the exact values CMD_RESP_48 / CMD_RESP_48BUSY. */
#define CMD_RESP_R3		0x82

#define EMMC_E_BRINGUP		-11	/* host bring-up failed */
#define EMMC_E_IDENT		-12	/* card did not identify */

/*
 * How far the identification got, so that the host can tell a mute card
 * from a card that talks but is not ready:
 *   0 nothing answered (CMD0/CMD1)
 *   1 CMD1 answered, the card never became ready
 *   2 CMD1 ready, CMD2/CMD3/CMD7 failed
 *   3 identified (the card is selected and in the transfer state)
 */
unsigned emmc_lk_stage;

/*
 * Which variant of the sweep is being tried. The diagnostic build sets it
 * before every try (shim/emmc_diag.inc); the log lines below carry it in
 * their label, so that the readings of the six variants stay apart in the
 * text the host reads back.
 */
unsigned emmc_diag_var_no;

/*
 * The label that tells the log which variant the readings belong to. It is
 * a buffer and not a table of pointers on purpose: the image runs wherever
 * LK puts it, so it cannot contain a single absolute address.
 */
static char pwr_lbl[4] = "PV0";

static void pwr_set_label(unsigned n)
{
	pwr_lbl[0] = 'P';
	pwr_lbl[1] = 'V';
	pwr_lbl[2] = (char)('0' + (n < 10 ? n : 9));
	pwr_lbl[3] = 0;
}

#define PWR_LABEL	(pwr_lbl)

/*
 * The registers the factory LK touches around the bus power, read at the
 * points where it does something with them. The card does not answer at
 * all, so these are the only direct measurements of the controller that
 * can be brought back from a run: they go into the console log, and the
 * log goes into the ramoops console zone (shim/pstore.c).
 *
 *   CORE+0xdc  PWRCTL status  (BUS_ON, BUS_OFF, IO_LOW, IO_HIGH)
 *   CORE+0xe0  PWRCTL mask
 *   CORE+0xe4  PWRCTL clear
 *   CORE+0xe8  PWRCTL control (the LK's handler writes 5 here)
 *   HC+0x29    bus power (bit 0 = on, bits 3:1 = the voltage)
 */
static void emmc_pwr_probe(unsigned step)
{
	con_hex2(PWR_LABEL, "dc=", rd32(CORE + CORE_PWRCTL_STATUS),
		 "e0=", rd32(CORE + CORE_PWRCTL_MASK));
	con_hex2(PWR_LABEL, "e4=", rd32(CORE + CORE_PWRCTL_CLEAR),
		 "e8=", rd32(CORE + CORE_PWRCTL_CTL));
	con_hex2(PWR_LABEL, "hc29=", rd8(HC + HC_POWER_CTRL),
		 "step=", step);
	con_hex2(PWR_LABEL, "pres=", rd32(HC + HC_PRESENT),
		 "clk=", rd32(HC + HC_CLK_CTRL));
}

/* rd16 sta in emmc.c, insieme agli altri accessori. */

static void emmc_gcc_enable(uint32_t off, uint32_t tag)
{
	uintptr_t a = G4_GCC_BASE + off;
	uint32_t v = rd32(a);

	trail(tag, v);
	if (!(v & 1u)) {
		wr32(a, v | 1u);
		for (int i = 0; i < 1000 && (rd32(a) & 0x80000000u); i++)
			emmc_delay_us(10);	/* CLK_OFF clears when it runs */
	}
	trail(tag + 1, rd32(a));
}

/* sdhci_msm_init(): CLEAR = STATUS; CTL |= BUS_SUCCESS / IO_SUCCESS. */
static void emmc_pwrctl_ack(void)
{
	uint32_t st = rd32(CORE + CORE_PWRCTL_STATUS);
	uint32_t ctl;

	wr32(CORE + CORE_PWRCTL_CLEAR, st);
	ctl = rd32(CORE + CORE_PWRCTL_CTL);
	if (st & 0x3u)
		ctl |= 0x1u;
	if (st & 0xcu)
		ctl |= 0x4u;
	wr32(CORE + CORE_PWRCTL_CTL, ctl);
}

/* sdhci set clock: divided clock mode, 10 bit divider, as in 0x0f912ec4. */
static int emmc_set_clock(uint32_t base_hz, uint32_t target_hz)
{
	uint32_t clk;

	if (base_hz <= target_hz) {
		clk = 1;
	} else {
		uint32_t d = 2, n;

		for (; d < 0x7fe; d += 2)
			if (target_hz >= base_hz / d)
				break;
		if (d >= 0x7fe)
			return -1;
		n = d >> 1;
		clk = ((n & 0xffu) << 8) | (((n >> 8) & 3u) << 6) | 1u;
	}
	wr16(HC + HC_CLK_CTRL, (uint16_t)clk);
	trail(0x006d, clk);
	for (int i = 0; i < 200; i++) {		/* internal clock stable: bit 1 */
		if (rd16(HC + HC_CLK_CTRL) & 2u)
			break;
		emmc_delay_us(100);
	}
	trail(0x006e, rd16(HC + HC_CLK_CTRL));
	if (!(rd16(HC + HC_CLK_CTRL) & 2u))
		return -2;
	wr16(HC + HC_CLK_CTRL, (uint16_t)(clk | 4u));	/* SD clock enable */
	return 0;
}

/*
 * The identification did not finish. "answered" says whether the card said
 * anything at all: that is the difference between a mute card and a card
 * that talks but never becomes ready. The two land on different events of
 * the table, which is the only thing the host can read when the card
 * cannot be written to.
 */
static int emmc_ident_fail(int answered)
{
	if (answered) {
		emmc_last_stage = STAGE_REINIT;
		emmc_pwr_probe(90);
		return EMMC_E_IDENT;
	}
	emmc_last_stage = STAGE_WAKE;
	emmc_class = CLASS_CARD_ABSENT;
	emmc_pwr_probe(91);
	return EMMC_E_IDENT;
}

static int emmc_lk_bringup(const struct lk_var *v)
{
	uint32_t rsp = 0, caps, ver, base_mhz, ocr;
	int rc;

	emmc_lk_stage = 0;

	/* Where did LK leave things? (HC reads are meaningless with HC mode off
	 * but harmless; they show whether the SDHCI block kept its state.) */
	trail(0x0064, rd32(CORE + CORE_HC_MODE));
	trail(0x0072, rd32(HC + HC_CLK_CTRL));
	emmc_pwr_probe(30);

	/* (a) clocks */
	STEP(30);
	emmc_gcc_enable(GCC_SDCC1_AHB_CBCR, 0x0060);
	emmc_gcc_enable(GCC_SDCC1_APPS_CBCR, 0x0062);

	/* (b) sdhci_msm_init() */
	STEP(31);
	if (!(v->flags & LK_F_NO_CORE_RESET)) {
		wr32(CORE + CORE_HC_MODE, rd32(CORE + CORE_HC_MODE) & ~HC_MODE_EN);
		wr32(CORE + CORE_POWER, rd32(CORE + CORE_POWER) | (1u << 7));
		emmc_delay_us(10000);		/* LK: delay(1); be generous */
	}
	wr32(CORE + CORE_HC_MODE, rd32(CORE + CORE_HC_MODE) | HC_MODE_EN);
	wr32(CORE + CORE_HC_MODE, rd32(CORE + CORE_HC_MODE) | FF_CLK_SW_RST_DIS);
	trail(0x0065, rd32(CORE + CORE_HC_MODE));
	trail(0x0066, rd32(CORE + CORE_POWER));

	/*
	 * The MSM controller can gate the clock to the card by itself: with
	 * CORE_CLK_PWRSAVE set in the vendor spec register the card clock
	 * stops as soon as there is no traffic, and then no command ever
	 * completes. The Linux driver clears the bit while it talks to the
	 * card (and puts it back afterwards).
	 *
	 * The same register also selects the clock path: after an HS400
	 * session the MCI is left with MCLK_SEL = HS400 and the CDC select
	 * in HS400 mode, and the identification clock then does not reach
	 * the card. The Linux driver puts both back to the default before it
	 * talks to a card that is not in HS400.
	 */
	{
		uint32_t vs = rd32(HC + HC_VENDOR_SPEC);

		trail(0x0077, vs);
		if (v->flags & LK_F_VENDOR_RESET) {
			vs = (vs & ~CORE_HC_MCLK_SEL_MASK) |
			     CORE_HC_MCLK_SEL_DFLT;
			vs &= ~CORE_HC_SELECT_IN_EN;
			vs &= ~CORE_HC_SELECT_IN_MASK;
		}
		if ((v->flags & LK_F_CLR_PWRSAVE) && (vs & CORE_CLK_PWRSAVE))
			vs &= ~CORE_CLK_PWRSAVE;
		wr32(HC + HC_VENDOR_SPEC, vs);
		trail(0x0078, rd32(HC + HC_VENDOR_SPEC));
	}

	STEP(32);
	if (!(v->flags & LK_F_NO_SW_RST)) {
		wr8(HC + HC_SWRST, 1);		/* SW reset ALL */
		{
			int i;

			for (i = 0; i < 100 && (rd8(HC + HC_SWRST) & 1u); i++)
				emmc_delay_us(1000);
			trail(0x0067, (uint32_t)i);	/* 100 = never cleared */
			if (i >= 100) {
				/* The controller did not come back: nothing else
				 * can work. Report it as unreachable. */
				emmc_last_stage = STAGE_WAKE;
				emmc_class = CLASS_INACCESSIBLE;
			}
		}
	}

	STEP(33);
	trail(0x0068, rd32(CORE + CORE_PWRCTL_STATUS));
	emmc_pwrctl_ack();
	wr32(CORE + CORE_PWRCTL_MASK, 0xf);
	trail(0x0069, rd32(CORE + CORE_PWRCTL_CTL));

	ver = rd32(CORE + CORE_MCI_VER);
	trail(0x006a, ver);
	if ((ver >> 28) != 0 && (ver & 0xffu) != 0x11 && (ver & 0xffu) != 0x12)
		wr32(HC + HC_CAPS_OVERRIDE, rd32(HC + HC_CAPS1) | (1u << 26));

	/* (c) base clock from the capabilities */
	STEP(34);
	caps = rd32(HC + HC_CAPS1);
	trail(0x006b, caps);
	trail(0x006c, rd32(HC + HC_CAPS_OVERRIDE));
	if (caps == 0 || caps == 0xffffffffu) {
		emmc_last_stage = STAGE_WAKE;
		emmc_class = (caps == 0xffffffffu) ? CLASS_INACCESSIBLE :
						     CLASS_NO_CAPS;
		return EMMC_E_NO_CAPS;
	}
	base_mhz = (caps >> 8) & 0xffu;
	if (!base_mhz)
		base_mhz = 200;

	if (v->flags & LK_F_LK_REGS) {
		/* What the LK writes (0x0b normal / 0xffff error). */
		wr16(HC + HC_INT_ENABLE, 0x000b);
		wr16(HC + HC_INT_ENABLE + 2, 0xffff);
		wr16(HC + HC_INT_SIGNAL, 0x000b);
		wr16(HC + HC_INT_SIGNAL + 2, 0xffff);
		wr8(HC + HC_CTRL1, 0x10);
	} else {
		/* Status bits that emmc_cmd() polls must be latched; no signal
		 * enable. */
		wr32(HC + HC_INT_ENABLE, 0x03ff00ffu);
		wr32(HC + HC_INT_SIGNAL, 0);
		wr8(HC + HC_CTRL1, rd8(HC + HC_CTRL1) & ~0x26u);	/* 1 bit */
	}
	wr8(HC + HC_TIMEOUT_CTRL, 0xf);
	wr16(HC + HC_HOST_CTRL2, rd16(HC + HC_HOST_CTRL2) & ~7u);	/* legacy */

	/* Bus power.
	 *
	 * Measured on the phone (2026-11-05, run 5): at the shim entry
	 * HC+0x29 reads 0x00, that is the bus power is OFF (the LK gave it
	 * away before jumping to the kernel) and the voltage field says
	 * nothing. The card only answers when the bus is powered up at
	 * **3.3 V** (0x0f), the same value the LK writes at every boot
	 * (0x0e then 0x0f, see dis-4.9-m-0f913954): powering it up at
	 * 1.8 V (0x0b) leaves the card mute.
	 *
	 * As in CAF, the voltage is selected with the power off and the
	 * power is turned on afterwards.
	 */
	if (v->power_val != LK_PWR_KEEP) {
		uint8_t p = rd8(HC + HC_POWER_CTRL);

		trail(0x006f, p);
		if (!(p & 1u) || (v->flags & LK_F_PWR_FORCE)) {
			wr8(HC + HC_POWER_CTRL, (uint8_t)(v->power_val & ~1u));
			emmc_delay_us(1000);
			emmc_pwr_probe(33);
			wr8(HC + HC_POWER_CTRL, (uint8_t)v->power_val);
			for (int i = 0; i < 100 && !rd32(CORE + CORE_PWRCTL_STATUS); i++)
				emmc_delay_us(100);
			trail(0x0070, rd32(CORE + CORE_PWRCTL_STATUS));
			emmc_pwr_probe(34);
			emmc_pwrctl_ack();
			emmc_pwr_probe(35);
		} else {
			/* The bus power was already on: the value comes from an
			 * earlier try (the diagnostic sweep tries six of them in
			 * the same boot). */
			emmc_pwr_probe(36);
		}
	}
	trail(0x0071, rd8(HC + HC_POWER_CTRL));

	/*
	 * The PWRCTL acknowledgement.
	 *
	 * The LK does not just write HC+0x29: it waits for the bus power
	 * transaction with the PMIC to complete. The completion raises the
	 * PWR interrupt, whose handler (0x0f913ac0 in the factory LK) writes
	 * CORE+0xe8 with BUS_ON_OFF_SUCC (bit 0) | IO_SIG_SUCC (bit 2) = 5,
	 * and only then does sdhci_init() carry on (event_wait).
	 *
	 * The variant LK_F_PWR_ACK reproduces that handshake without the
	 * interrupt: wait for the bus-on status in CORE+0xdc, clear it with
	 * CORE+0xe4 and acknowledge with CORE+0xe8 = 5.
	 */
	if (v->flags & LK_F_PWR_ACK) {
		uint32_t st = 0;

		for (int i = 0; i < 1000; i++) {
			st = rd32(CORE + CORE_PWRCTL_STATUS);
			if (st & 0x3u)
				break;
			emmc_delay_us(100);
		}
		trail(0x0072, st);
		wr32(CORE + CORE_PWRCTL_CLEAR, st);
		wr32(CORE + CORE_PWRCTL_CTL, 0x5);
		trail(0x0073, rd32(CORE + CORE_PWRCTL_CTL));
		emmc_pwr_probe(37);
	}

	/* (d) the identification clock */
	STEP(35);
	rc = emmc_set_clock(base_mhz * 1000000u, v->clk_hz);
	if (rc) {
		emmc_last_stage = STAGE_WAKE;
		emmc_class = (rc == -2) ? CLASS_CLOCK_UNSTABLE : CLASS_CLOCK_OFF;
		return EMMC_E_BRINGUP;
	}
	emmc_delay_us(2000);
	g_have_dat0 = (rd32(HC + HC_PRESENT) & PS_DAT0_LEVEL) != 0;
	trail(0x0073, rd32(HC + HC_PRESENT));

	/* (e) identification */
	STEP(36);
	if (v->flags & LK_F_DELAY_10S) {
		/* The card may need time after the bus has been powered up
		 * before it answers. The LK does not wait here (it waits for
		 * the power interrupt instead), so this is a hypothesis, not
		 * a replay. */
		trail(0x0074, 0x0a);
		for (int i = 0; i < 10000; i++)
			emmc_delay_us(1000);
		trail(0x0075, 0x0a);
	}
	if (!(v->flags & LK_F_NO_WAKE)) {
		rc = emmc_cmd(CMD_SLEEP_AWAKE, LK_RCA << 16, CMD_RESP_48BUSY, &rsp);
		trail(0x0074, (uint32_t)rc);	/* a timeout means it was awake */
		if (!rc)
			emmc_delay_us(10000);
	}

	STEP(37);
	if (emmc_cmd(0, 0, CMD_RESP_NONE, 0))
		return emmc_ident_fail(0);
	STEP(38);
	ocr = (v->flags & LK_F_LK_OCR) ? LK_OCR_FIRST : LK_OCR;
	{
		int i;

		for (i = 0; i < 1000; i++) {
			if (emmc_cmd(1, ocr, CMD_RESP_R3, &rsp))
				return emmc_ident_fail(0);
			emmc_lk_stage = 1;	/* the card answered */
			if (rsp & 0x80000000u)
				break;
			emmc_delay_us(1000);
		}
		trail(0x0075, (uint32_t)i);
		trail(0x0076, rsp);
		if (i == 1000)
			return emmc_ident_fail(1);
		g_sector_mode = (rsp & 0x40000000u) != 0;
	}
	STEP(39);
	emmc_lk_stage = 2;		/* the card is ready, not identified yet */
	if (emmc_cmd(2, 0, CMD_RESP_136, &rsp))
		return emmc_ident_fail(1);
	if (emmc_cmd(CMD_SEND_RELATIVE_ADDR, LK_RCA << 16, CMD_RESP_48, &rsp))
		return emmc_ident_fail(1);
	g_rca = (uint16_t)LK_RCA;
	if (emmc_cmd(CMD_SELECT_CARD, LK_RCA << 16, CMD_RESP_48BUSY, &rsp))
		return emmc_ident_fail(1);
	emmc_lk_stage = 3;
	return EMMC_OK;
}
