/*
 * The diagnostic build: instead of one bring-up, a sweep of variants.
 *
 * The card does not answer, and there is no way to read the registers of the
 * controller from the host (the trail in IMEM is not readable from the
 * recovery), so the only channel is the time the shim waits before it resets
 * the phone. This file turns that channel into a matrix:
 *
 *   every variant is tried in the same boot, in order, and the one that got
 *   the furthest is reported through the wait time:
 *
 *       wait = 40 s + 50 s * variant + 10 s * stage
 *
 *   variant 0..7  which variant (see the table below)
 *   stage   0..3  how far it got (0 nothing, 1 CMD1 answered, 2 CMD1 ready,
 *                 3 identified)
 *
 * The units (50 s and 10 s) are much larger than the jitter of the boot
 * time (about 1 s between runs), so the decoding is unambiguous.
 *
 * The variants cover every point where the factory LK, the working Linux
 * driver and this shim disagree:
 */
/* The result, read by shim.c to encode the wait time. Always defined, so
 * that the timing code can link in every build. */
unsigned emmc_diag_variant;
unsigned emmc_diag_stage;

#if EMMC_DIAG_VARIANTS

static const struct lk_var lk_diag_variants[] = {
	/*
	 * The experiment of 2026-11-08, preregistered before the run.
	 *
	 * What T1 established (FATTO, see research/lk-g4/T1-RAPPORTO.md):
	 *   - with CAPS1 = 0x342dc8b2 the factory LK writes 0x0a then 0x0b in
	 *     HC+0x29, that is 1.8 V, NOT the 0x0e/0x0f (3.3 V) of the run 5;
	 *   - after those two writes the LK BLOCKS (event_wait) until the PWR
	 *     interrupt handler (0x0f913ac0) acknowledges the bus power
	 *     transaction by writing CORE+0xe8 = 5;
	 *   - the shim today acknowledges with 1, derived from the status
	 *     register, not 5, derived from the mask register as the LK does.
	 *
	 * The variants, in the order they are tried (the sweep stops at the
	 * first one that identifies the card):
	 *
	 *   V0  control: exactly the run 5 winner (3.3 V, no extra ack)
	 *   V1  V0 + the LK's PWRCTL acknowledgement (CORE+0xe8 = 5)
	 *   V2  V1 + 10 s of wait before CMD5/CMD0
	 *   V3  V0 but HC+0x29 is not written at all (covers H4: the bus
	 *       power is driven by the PMIC, not by the SDHCI register)
	 *   V4  the LK's voltage on this CAPS1 (1.8 V), otherwise like V0
	 *   V5  1.8 V + the LK's PWRCTL acknowledgement
	 *
	 * Preregistered reading of the result (variant, stage):
	 *   only V1/V2 answer   -> the PWRCTL acknowledgement is the factor
	 *   only V2 answers     -> it is the time (H1)
	 *   V0 and V1 answer, V3 does not -> HC+0x29 matters
	 *   only V3 answers     -> H4 (the power comes from the PMIC)
	 *   V4/V5 answer, V0/V1 do not -> the voltage was the problem
	 *   V5 answers, V1 does not -> both the voltage and the ack are needed
	 *   V6/V7 answer         -> the registers the LK writes (CTRL1 bit 4:
	 *                           the bus power comes from the PMIC)
	 *   nobody answers      -> the factor of the run 5 is not reproduced
	 */
	/* 0: the run 5 winner, unchanged (control). */
	{ 0x0f, 400000, 0 },
	/* 1: the control + the LK's PWRCTL acknowledgement. */
	{ 0x0f, 400000, LK_F_PWR_ACK },
	/* 2: the same + 10 s before the identification. */
	{ 0x0f, 400000, LK_F_PWR_ACK | LK_F_DELAY_10S },
	/* 3: the control without writing HC+0x29 at all. */
	{ LK_PWR_KEEP, 400000, 0 },
	/* 4: the voltage the factory LK really uses on this CAPS1 (1.8 V). */
	{ 0x0b, 400000, 0 },
	/* 5: 1.8 V + the LK's PWRCTL acknowledgement. */
	{ 0x0b, 400000, LK_F_PWR_ACK },
	/*
	 * 6 and 7: the registers the factory LK writes (INT_ENABLE 0x000b /
	 * 0xffff and CTRL1 = 0x10).
	 *
	 * CTRL1 bit 4 is "SD Bus Power Control Mode": with it set the SD Bus
	 * Power bit is read-only and the bus power is driven by the PMIC, not
	 * by the SDHCI register (that is the quirk the working Linux driver
	 * mentions: "Bus power control is handled by interacting with PMIC").
	 * The shim, like the old run 5, writes HC+0x29 and keeps the default
	 * mode, where the two ways of driving the power fight each other.
	 */
	/* 6: the LK's registers, at 3.3 V. */
	{ 0x0f, 400000, LK_F_LK_REGS },
	/* 7: the LK's registers, at 1.8 V (the voltage the LK really uses). */
	{ 0x0b, 400000, LK_F_LK_REGS },
};

#define LK_DIAG_NVARS	(sizeof(lk_diag_variants) / sizeof(lk_diag_variants[0]))

/*
 * Try every variant and keep the one that got the furthest. Returns 0 when
 * some variant identified the card (the caller then goes on with the normal
 * path), -1 when none of them got anything.
 */
static int emmc_diag_sweep(void)
{
	unsigned best_var = 0;
	unsigned best_stage = 0;
	unsigned i;

	for (i = 0; i < LK_DIAG_NVARS; i++) {
		int rc;

		/* The readings the bring-up takes carry this number in their
		 * label, so that the six variants stay apart in the log. */
		emmc_diag_var_no = i;
		pwr_set_label(i);
		trail(0x0080 + i, lk_diag_variants[i].flags);
		rc = emmc_lk_bringup(&lk_diag_variants[i]);
		trail(0x00a0 + i, (uint32_t)emmc_lk_stage);
		/* What this variant did, in the text the host reads back. */
		con_hex2("VAR", "n=", i, "stage=", emmc_lk_stage);
		con_hex2("VAR", "n=", i, "rc=", (uint32_t)rc);
		con_hex2("VAR", "n=", i, "fl=", lk_diag_variants[i].flags);
		if (emmc_lk_stage > best_stage) {
			best_stage = emmc_lk_stage;
			best_var = i;
			if (best_stage >= 3) {
				emmc_diag_variant = best_var;
				emmc_diag_stage = best_stage;
				return 0;	/* identified: stop here */
			}
		}
		(void)rc;
	}
	emmc_diag_variant = best_var;
	emmc_diag_stage = best_stage;
	return -1;
}
#endif /* EMMC_DIAG_VARIANTS */
