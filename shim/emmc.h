/*
 * emmc.h -- the eMMC of the LG G4 (SDCC1, the host controller at
 * 0xf9824900 and the MCI core at 0xf9824000).
 *
 * WHAT THIS MODULE DOES
 * ----------------------
 *   1. it switches the clocks of the SDCC1 on (the LK turns them off
 *      before it jumps), and only then it touches the controller;
 *   2. it brings the host back to the state the LK left it in
 *      (HC mode, the core reset, the vendor registers, the bus power,
 *      the identification clock);
 *   3. it identifies the card (CMD0, CMD1, CMD2, CMD3, CMD7) and reads
 *      its EXT_CSD;
 *   4. it can write the log on the card (OFF by default: there is no
 *      sector of this card that is safe to scribble on).
 *
 * THE HANG OF 2026-11-15
 * ----------------------
 * A run stopped between emmc_write_log() and pstore_console_write(),
 * and the phone sat there until the watchdog rebooted it. The cause was
 * NOT the controller: it was the timebase. The waits of this driver are
 * measured with the generic timer, and the EL0 counters were not
 * running at the shim's entry, so every deadline was a constant and
 * every wait loop waited for ever (shim/timer.h has the whole story).
 *
 * Two things were done about it:
 *
 *   - the counters are switched on at the entry (timer_init()), and
 *   - every wait here is bounded by that timebase and pets the watchdog,
 *     so a wait that cannot end becomes a reset instead of a hang.
 *
 * THE CMD2 QUESTION
 * -----------------
 * CMD2 (SEND_ARG) is the one command this card used to refuse: the
 * answer came back with an interrupt error and the controller kept the
 * command line. The working Linux driver asks for a 136 bit answer with
 * the CRC7 check and WITHOUT the index check, while this shim used to
 * ask for it with the index check as well.
 *
 * emmc_cmd2() does not guess: it tries the four possible answers in the
 * order of the drivers (136 bit with CRC first, the way the working
 * Linux driver does it) and remembers the one that works, so the second
 * boot of the phone does not have to sweep again. The answer it found
 * is in the log ("C2OK").
 */
#pragma once
#include <stdint.h>

/* The result of the bring-up: 0 when the card is identified. */
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
#define EMMC_E_BRINGUP		-11	/* the host did not come back */
#define EMMC_E_IDENT		-12	/* the card did not identify */

/*
 * Bring the eMMC up: the clocks, the host, the card.
 *
 * It never writes to the card: it identifies it and reads its EXT_CSD.
 * Returns EMMC_OK or one of the errors above.
 */
int emmc_init(void);

/*
 * Write the log on the card.
 *
 * OFF by default (the build flag SHIM_EMMC_WRITE): the two addresses
 * that are used are inside the 'persist' and 'laf' partitions, so the
 * writes are only for somebody who knows what they are doing.
 */
int emmc_log_write(const char *text, unsigned len);

/* What the bring-up found. */
unsigned emmc_get_sec_count(void);	/* the sectors of the EXT_CSD */
int emmc_get_sector_mode(void);		/* 1 = the addresses are 32 bit */
uint16_t emmc_get_rca(void);		/* the relative card address */
unsigned emmc_get_stage(void);		/* how far the identification got */
const char *emmc_get_stage_name(void);	/* the same, in words */
int emmc_get_cmd_error(void);		/* the last command error */

/* The classes of failure, for the log. */
#define EMMC_CLASS_NONE		0
#define EMMC_CLASS_INACCESSIBLE	1	/* the controller did not come back */
#define EMMC_CLASS_NO_CAPS	2	/* the capabilities read as zero */
#define EMMC_CLASS_CLOCK_OFF	3	/* the card clock did not come on */
#define EMMC_CLASS_CLOCK_UNSTABLE 4	/* the clock did not settle */
int emmc_get_class(void);
