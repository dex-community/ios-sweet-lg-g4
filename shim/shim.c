/*
 * LG G4 (H815) boot shim.
 *
 * The sweet shim was written for the Redmi Note 10 Pro (Qualcomm ABL,
 * SM8150). The G4 is an MSM8992 with an LK bootloader, so the hardware
 * constants all changed (see g4.h) and the boot flow differs:
 *
 *   - LK parses the QCDT v3 device-tree table that is appended to the boot
 *     image, copies the entry that matches the board id into the tags
 *     address and jumps to the kernel through scm_elexec_call(), which
 *     puts the DTB pointer in x0. The DTB hand-off is therefore the same
 *     as on ABL and start.S needs no change.
 *   - LK patches /chosen (bootargs, linux,initrd-start/end) and /memory
 *     before the jump, so the shim can read the real RAM map from the
 *     live FDT instead of guessing it.
 *
 * Any unexpected exception paints a blue panic screen.
 */
#include <stdint.h>
#include "console.h"
#include "g4.h"
#include "fdt.h"
#include "mmu.h"
#include "loader.h"
#include "trail.h"
#include "pstore.h"
#include "mmio.h"
#include "wdt.h"

int emmc_write_log(const char *text, unsigned len);
extern unsigned emmc_last_stage;
extern int emmc_last_error;
extern int emmc_last_cmd_error;
/* The result of the FIRST bring-up attempt, kept apart from the second
 * one that reset() triggers. See emmc.c. */
extern unsigned emmc_first_stage;
extern int emmc_first_error;
extern int emmc_first_cmd_error;
/* The fine grained progress counter of the eMMC driver. */
extern unsigned emmc_step;
/* Why the card did not answer (emmc.c). */
extern unsigned emmc_class;
/* The diagnostic build: which bring-up variant got the furthest, and how
 * far it got (shim/emmc_diag.inc). */
extern unsigned emmc_diag_variant;
extern unsigned emmc_diag_stage;

/* The QEMU model of the controller (only in the SHIM_EMMC_MODEL build). */
void emmc_model_config(uintptr_t dtb);

/* The self-test of the MMIO allowlist (shim/mmio_selftest.c). */
void mmio_selftest(void);

#if SHIM_SD_INIT
/* The bring-up of the microSD slot (shim/sd.c). */
#include "sd.h"
#endif
#if SHIM_SD_PAYLOAD
#include "sd_payload.h"
#endif

/* The T2 read-only probe of the eMMC host (shim/sd_probe.inc). */
#if SHIM_SD_PROBE
void sd_probe_before(void);
void sd_probe_after(void);
int sd_probe_should_run(unsigned phase);
#endif

/* The E0 (eMMC card) and D0 (MDP) probes (shim/emmc_probe.c). */
#if SHIM_EMMC_PROBE
void emmc_probe_e0(void);
void emmc_probe_d0(void);
#endif
void emmc_model_finish(void);

/*
 * The stage of the eMMC bring-up is encoded in the time the phone stays up
 * before the reset: stage * STAGE_STEP_S seconds after the shim started.
 *
 * It is a fallback for the case where the card cannot be written to, which
 * is exactly the case where the stage is needed. The computer measures the
 * time between "fastboot reboot" and the phone coming back in recovery.
 *
 * The stage alone is not enough: emmc.c renumbered the error codes, and the
 * old "err * 3 seconds" encoding no longer tells the two apart (both the
 * raw command error -1..-4 and the higher level error -1..-10 were in play).
 * The error is therefore encoded in the remainder of the step:
 *
 *   wait = stage * STAGE_STEP_S + (error_code + 1) * STAGE_ERR_STEP_S
 *
 * with STAGE_ERR_STEP_S smaller than STAGE_STEP_S, so the stage is still
 * readable from the coarse part and the error from the fine part.
 */
#define STAGE_STEP_S	20
#define STAGE_ERR_STEP_S	2
#ifndef SHIM_FORCE_WAIT_S
#define SHIM_FORCE_WAIT_S	0
#endif
#ifndef SHIM_RAW_TIMING
#define SHIM_RAW_TIMING		0
#endif
/*
 * Fine timing: the diagnostic event, not the raw step.
 *
 *   wait = SHIM_FINE_BASE_S + 20 s * event index
 *
 * The event index comes from a fixed table, so the host can decode the
 * wait time without knowing anything about the driver. The unit is 20 s
 * because the boot of the phone (the bootloader plus TWRP) varies by a few
 * seconds between runs: with a smaller unit the jitter would move the
 * reading into the neighbouring slot.
 *
 * The base is larger than the time reset() spends in its own second
 * emmc_write_log() call, otherwise the wait would clamp to 0 and the
 * reading would be meaningless.
 */
#ifndef SHIM_FINE_TIMING
#define SHIM_FINE_TIMING	0
#endif
#ifndef SHIM_FINE_BASE_S
#define SHIM_FINE_BASE_S	40
#endif
/*
 * A crash is reported with a base of its own, above the highest event
 * slot (40 + 20 * 14 = 320), so the two encodings never overlap.
 */
#define SHIM_CRASH_BASE		500

/*
 * The event table. The index is what the host decodes; keep it in sync
 * with tools/run-shim.sh.
 */
enum {
	EV_ALL_OK = 0,	/* the log was written to the card and read back  */
	EV_WAKE_FAIL,	/* the card answered to neither CMD5 nor CMD7        */
	EV_CMD16_TIMEOUT,	/* CMD16 timed out                          */
	EV_CMD16_ERR,		/* the controller reported an error         */
	EV_CMD16_FATAL,	/* the card answered with a fatal R1          */
	EV_EXTCSD_TIMEOUT,	/* the EXT_CSD read timed out              */
	EV_EXTCSD_ERR,		/* the controller reported an error         */
	EV_EXTCSD_OTHER,	/* the EXT_CSD read failed in another way   */
	EV_WRITE_TIMEOUT,	/* the sector write timed out               */
	EV_WRITE_ERR,		/* the controller reported an error         */
	EV_OTHER,		/* the driver did not get that far          */
	EV_READBACK_FAIL,	/* the read back failed or did not match    */
	EV_MMU_NO_16K,		/* the core has no 16K granule              */
	EV_MMU_FAIL,		/* mmu_enable() refused                     */
	EV_MMU_OK,		/* the MMU is on with the 16K page tables   */
	EV_INACCESSIBLE,	/* the controller does not answer at all    */
	EV_NO_CAPS,		/* the capability registers are empty        */
	EV_CLOCK_OFF,		/* the card clock is not enabled              */
	EV_CLOCK_UNSTABLE,	/* the internal clock never stabilised        */
	EV_CMD_INHIBIT,		/* the command line is stuck busy             */
	EV_CARD_ABSENT,		/* the controller is fine, the card is not    */
	EV_REINIT_FAIL,		/* the card does not answer to CMD0/1/2/3/7   */
	EV_CLOCK_GATED,		/* the controller was gating the card clock   */
	EV_COUNT
};
/* The eMMC stage values, mirrored from emmc.c. */
#define EV_STAGE_WAKE		4
#define EV_STAGE_BLKLEN		3
#define EV_STAGE_BLKLEN2	5
#define EV_STAGE_EXTCSD		6
#define EV_STAGE_WRITE		7
#define EV_STAGE_READBACK	8
#define EV_STAGE_DONE		9
#define EV_STAGE_REINIT		10

static unsigned reset_stage = 0xffffffffU;	/* 0xffffffff: not set */
static uint64_t shim_t0;
/* Set when the shim took a fault: the stage of the fault is reported
 * through the wait time, and the card write is skipped. */
static int panic_stage;
/* emmc_step right after the FIRST emmc_write_log(): the second call (from
 * reset()) overwrites emmc_step, so it is captured here. */
static unsigned first_step = 0xffffffffU;
/* What mmu_enable() returned, for the event table. */
static int shim_mmu_rc = -1;

/*
 * Which event the phone reports through the wait time: how far the eMMC
 * bring-up got and, if it got all the way, what the MMU did.
 */
static unsigned shim_event_index(void)
{
	unsigned stage = emmc_first_stage;
	int ce = emmc_first_cmd_error;
	unsigned ev;

	switch (stage) {
	case EV_STAGE_DONE:
		ev = EV_ALL_OK;
		break;
	case EV_STAGE_WAKE:
		/* The card did not answer: the class says why. */
		switch (emmc_class) {
		case 1: ev = EV_INACCESSIBLE; break;
		case 2: ev = EV_NO_CAPS; break;
		case 3: ev = EV_CLOCK_OFF; break;
		case 4: ev = EV_CLOCK_UNSTABLE; break;
		case 5: ev = EV_CMD_INHIBIT; break;
		case 6: ev = EV_CARD_ABSENT; break;
		case 7: ev = EV_CLOCK_GATED; break;
		default: ev = EV_WAKE_FAIL; break;
		}
		break;
	case EV_STAGE_BLKLEN:
	case EV_STAGE_BLKLEN2:
		ev = (ce == -3) ? EV_CMD16_ERR :
		     ((ce == -4 || ce == -1) ? EV_CMD16_FATAL : EV_CMD16_TIMEOUT);
		break;
	case EV_STAGE_EXTCSD:
		ev = (ce == -3) ? EV_EXTCSD_ERR :
		     (ce == -2) ? EV_EXTCSD_TIMEOUT : EV_EXTCSD_OTHER;
		break;
	case EV_STAGE_WRITE:
		ev = (ce == -3) ? EV_WRITE_ERR :
		     (ce == -2) ? EV_WRITE_TIMEOUT : EV_OTHER;
		break;
	case EV_STAGE_READBACK:
		ev = EV_READBACK_FAIL;
		break;
	case EV_STAGE_REINIT:
		ev = EV_REINIT_FAIL;
		break;
	default:
		ev = EV_OTHER;
		break;
	}

	if (ev == EV_ALL_OK) {
		/* The card worked, so report the MMU instead. */
		if (shim_mmu_rc == -2)
			ev = EV_MMU_NO_16K;
		else if (shim_mmu_rc)
			ev = EV_MMU_FAIL;
		else
			ev = EV_MMU_OK;
	}
	return ev;
}

#define RESTART_BOOTLOADER	0x77665500U

#define GICD_BASE		G4_GICD_BASE
#define GICR_BASE		G4_GICR_BASE
#define GICR_SIZE		G4_GICR_SIZE
#define GICR_PE_SIZE		0x20000UL
#define GICR_TYPER		0x08
#define GICR_WAKER		0x14
#define GICR_SGI		0x10000
#define GICR_IGROUPR0		(GICR_SGI + 0x080)
#define GICR_ISENABLER0		(GICR_SGI + 0x100)
#define GICR_ICENABLER0		(GICR_SGI + 0x180)
#define GICR_IPRIORITYR		(GICR_SGI + 0x400)
#define GICR_ISPENDR0		(GICR_SGI + 0x200)
#define GICR_ISACTIVER0		(GICR_SGI + 0x300)
#define GICR_ICFGR1		(GICR_SGI + 0xc04)
#define VTIMER_INTID		G4_VTIMER_INTID
#define PTIMER_INTID		G4_PTIMER_INTID
#define SGI_INTID		1

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_SUSPEND	0xc4000001UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_MIGRATE_INFO_TYPE	0x84000006UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_SYSTEM_SUSPEND	0xc400000eUL
#define PSCI_SYSTEM_RESET2	0xc4000012UL
#define SMCCC_VERSION		0x80000000UL

#define NR_CPUS			G4_NR_CPUS

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")

/* Guarded by the MMIO allowlist (shim/mmio.h). */
static inline uint32_t rd32(uintptr_t a) { return mmio_rd32(a); }
static inline uint64_t rd64(uintptr_t a) { return mmio_rd64(a); }
static inline void wr32(uintptr_t a, uint32_t v) { mmio_wr32(a, v); }
static inline void wr8(uintptr_t a, uint8_t v) { mmio_wr8(a, v); }

extern char secondary_entry[];
void shim_main(uint64_t x0, uint64_t x1, uint64_t x2);

static const char *stage;	/* set at runtime: no absolute pointers in data */
/* A numeric twin of "stage", so that the phase can be reported through
 * the wait time when the shim faults (the console is not readable). */
static unsigned phase_no;
#define PHASE(n)	(phase_no = (n), trail(0x1000, (n)))
static volatile uint64_t sec_mpidr[NR_CPUS];
static volatile uint64_t sec_probe[4];	/* CPU1 redistributor write test */
static volatile uint32_t irq_count, irq_other, irq_last;
static volatile int irq_src;

/* ICC_SGI1R_EL1 value targeting the calling core only. */
static uint64_t sgi_self(unsigned id)
{
	uint64_t m = rsr("mpidr_el1");
	return ((m >> 16) & 0xff) << 32 |	/* Aff2 */
	       ((m >> 8) & 0xff) << 16 |	/* Aff1 */
	       (uint64_t)id << 24 |
	       (1U << (m & 0xf));		/* target list: Aff0 */
}

/*
 * PSCI call.
 *
 * On this device PSCI does not exist: the H815 device tree deletes the
 * psci node ("/* psci is broken *\/") and the Android kernel falls back
 * to the qcom,pshold driver for the reboot. Calling "smc #0" traps, so
 * the call is not made at all and the function reports "not supported".
 *
 * The signature is kept so that the callers (test_smp, psci_line) stay
 * compilable and can be re-enabled if a device with a real PSCI is ever
 * targeted.
 */
static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	(void)fn;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0xffffffffffffffffULL;	/* not supported */
}

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	while (now() < end)
		;
}

/*
 * Copy the ASCII console log into scratch RAM. A reset does not scrub
 * DRAM on this SoC, so the log is still there after the phone comes back
 * up, and a recovery with root can read it out of /dev/mem.
 *
 * 0x50000000 is inside the 0x10000000-0x5fffffff RAM bank (see
 * /proc/iomem) and is not used by the bootloader, which lives higher up.
 */
#define LOG_SCRATCH	0x50000000UL
#define LOG_MARKER	"G4SHIMLG"

static void save_log(void)
{
#if !SHIM_HW_DIAG || SHIM_EMMC_MODEL
	/* QEMU has no RAM at the scratch address; skip it. */
	return;
#endif
	volatile uint8_t *dst = (volatile uint8_t *)LOG_SCRATCH;
	const char *src = con_log;
	int n = con_log_len;

	if (n > CON_LOG_SIZE - 1)
		n = CON_LOG_SIZE - 1;
	for (int i = 0; i < 8; i++)
		dst[i] = (uint8_t)LOG_MARKER[i];
	for (int i = 0; i < n; i++)
		dst[8 + i] = (uint8_t)src[i];
	dst[8 + n] = 0;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void __attribute__((noreturn)) reset(unsigned after_s)
{
	trail(0x3000, after_s);
	save_log();
	con_skip();
	{
		char msg[24];
		int n = 0;
		const char *p = "RESET in ";
		while (*p)
			msg[n++] = *p++;
		if (after_s >= 100)
			msg[n++] = (char)('0' + after_s / 100);
		if (after_s >= 10)
			msg[n++] = (char)('0' + (after_s / 10) % 10);
		msg[n++] = (char)('0' + after_s % 10);
		msg[n++] = 's';
		msg[n] = 0;
		con_str("RESET", msg);
	}
#if SHIM_HW_DIAG
	/*
	 * Put the log on the eMMC as well: the panel is in command mode and
	 * the RAM copy does not survive the trip through Android. The card
	 * does survive, and a recovery reads it back with dd.
	 *
	 * NOT after a fault: the fault usually comes from the eMMC driver
	 * itself, so a second attempt would fault again and recurse.
	 */
	if (!panic_stage)
		emmc_write_log(con_log, (unsigned)con_log_len);
	/*
	 * And into the ramoops console zone, which the next Android boot
	 * reads back: /sys/fs/pstore/console-ramoops-0. This is the channel
	 * that works when the card is mute.
	 *
	 * The write is made here even after a fault: the log of the fault is
	 * exactly what is worth keeping. What keeps it from looping is the
	 * attempt counter in pstore.c (at most two attempts).
	 */
	pstore_console_write(con_log, (unsigned)con_log_len);
#if SHIM_EMMC_MODEL
	emmc_model_finish();
#endif
#endif
	/*
	 * How long to stay up before the reset. Normally this is a fixed
	 * short wait, but when the eMMC bring-up reported a stage, the wait
	 * is stretched so that the stage can be read off the clock: stage
	 * * STAGE_STEP_S seconds after the shim started.
	 */
	{
		unsigned wait_s = after_s;

#if SHIM_HW_DIAG
		if (panic_stage) {
			/*
			 * A fault: the wait is the phase number, in
			 * seconds, so the computer reads it directly.
			 */
			uint64_t hz = rsr("cntfrq_el0");
			uint64_t elapsed_s = (now() - shim_t0) / hz;
			/* reset_stage is 0xffffffff when the fault came before
			 * anything set it (double fault): never wait ~4e9 s. */
			uint64_t deadline_s = (reset_stage != 0xffffffffU) ?
					      (uint64_t)reset_stage :
					      (uint64_t)phase_no;

			wait_s = (deadline_s > elapsed_s) ?
				 (unsigned)(deadline_s - elapsed_s) : 0;
		} else if (SHIM_DIAG_TIMING) {
			/*
			 * Diagnostic build: the wait carries the matrix
			 * variant x stage (see shim/emmc_diag.inc):
			 *
			 *   wait = 60 s + 25 s * variant + 5 s * stage
			 *
			 * The stage unit divides the variant unit, so the
			 * decoding is unique. The units are far bigger than
			 * the jitter of the boot time. The largest value
			 * (60 + 25*15 + 5*3 = 450 s) stays below the crash
			 * base (500 s).
			 */
			uint64_t hz = rsr("cntfrq_el0");
			uint64_t elapsed_s = (now() - shim_t0) / hz;
			uint64_t deadline_s = 60ULL +
					      25ULL * emmc_diag_variant +
					      5ULL * emmc_diag_stage;

			wait_s = (deadline_s > elapsed_s) ?
				 (unsigned)(deadline_s - elapsed_s) : 0;
			con_hex2("DIAG", "v=", emmc_diag_variant,
				 "s=", emmc_diag_stage);
		} else if (SHIM_FINE_TIMING) {
			uint64_t hz = rsr("cntfrq_el0");
			uint64_t elapsed_s = (now() - shim_t0) / hz;
			unsigned ev = shim_event_index();
			uint64_t deadline_s = SHIM_FINE_BASE_S + 20ULL * ev;

			if (deadline_s <= elapsed_s)
				con_hex("FINE", 0xbad0);	/* base too small */
			wait_s = (deadline_s > elapsed_s) ?
				 (unsigned)(deadline_s - elapsed_s) : 0;
			con_hex2("FINE", "ev=", ev, "stage=", emmc_first_stage);
			con_hex("STEP", first_step);
			con_hex("WAIT", wait_s);
		} else if (reset_stage != 0xffffffffU) {
			uint64_t hz = rsr("cntfrq_el0");
			uint64_t elapsed_s = (now() - shim_t0) / hz;
			unsigned stage = reset_stage > 12 ? 12 : reset_stage;
			/*
			 * Prefer the higher level error code (it names the
			 * step that failed); fall back to the raw command
			 * error if there is no higher level one.
			 */
			int err = emmc_first_error ? emmc_first_error :
						emmc_first_cmd_error;
			unsigned err_s = (err < 0) ?
				 (unsigned)(-err + 1) * STAGE_ERR_STEP_S : 0;
			uint64_t deadline_s = (uint64_t)stage * STAGE_STEP_S + err_s;

			wait_s = (deadline_s > elapsed_s) ?
				 (unsigned)(deadline_s - elapsed_s) : 0;
			/*
			 * DIAGNOSTIC ENCODING: report the raw values so
			 * they can be read straight off the clock without
			 * having to invert a formula. stage is in the
			 * minutes part, error in the seconds part.
			 */
			if (SHIM_RAW_TIMING) {
				unsigned raw = (stage & 0xf) * 60 +
					       (unsigned)(-err) * 2;
				uint64_t raw_deadline = (uint64_t)raw;

				wait_s = (raw_deadline > elapsed_s) ?
					 (unsigned)(raw_deadline - elapsed_s) : 0;
			}
			con_hex2("STAGE", "n=", stage, "err=", (uint32_t)err);
			con_hex("WAIT", wait_s);
		}
		if (SHIM_FORCE_WAIT_S > 0)
			wait_s = SHIM_FORCE_WAIT_S;
#endif
#if !SHIM_HW_DIAG
	/* QEMU build: no watchdog to kick, so just park and let the monitor
	 * dump the framebuffer. */
	(void)wait_s;
	for (;;)
		__asm__ volatile("wfe");
#else
	delay_ms(wait_s * 1000);
#if SHIM_HW_DIAG && !SHIM_EMMC_MODEL
	/*
	 * Ask the bootloader to come up in recovery, so the log that was just
	 * written to the card can be read back with dd. LK reads this
	 * register at boot (platform/msm_shared/reboot.c) and boots the
	 * recovery partition when it finds RECOVERY_MODE.
	 */
	wr32(G4_RESTART_REASON, LK_RECOVERY_MODE);
	__asm__ volatile("dsb sy" ::: "memory");
	/*
	 * Reset the phone by dropping PS_HOLD, NOT with PSCI.
	 *
	 * The H815 has no PSCI: its device tree says so explicitly
	 * ("/* psci is broken *\/" and "/delete-node/ psci;"), the CPU
	 * nodes use enable-method = "qcom,8994-arm-cortex-acc", and the
	 * Android kernel's arm_pm_restart is the qcom,pshold driver
	 * (drivers/power/reset/msm-poweroff.c), not psci_sys_reset.
	 *
	 * Calling smc(PSCI_SYSTEM_RESET) faults (the phase counter says
	 * the crash is in smc(PSCI_VERSION)), so the PSCI call is gone.
	 * Deasserting PS_HOLD is what the kernel does for the same job.
	 */
	wr32(G4_PSHOLD, 0);
	__asm__ volatile("dsb sy" ::: "memory");
#endif
	for (;;)
		__asm__ volatile("wfe");
#endif
	}
}

/*
 * The apps watchdog is armed when the bootloader hands over. Left alone it
 * resets the phone in about a minute, which kills every long bring-up test.
 * Stop it before anything else.
 */
static void stop_watchdog(void)
{
#if SHIM_HW_DIAG && !SHIM_EMMC_MODEL
	uint32_t en = rd32(G4_WDT_BASE + G4_WDT_EN);

	if (!(en & 1))
		return;
	wr32(G4_WDT_BASE + G4_WDT_DISABLE, 1);
	__asm__ volatile("dsb sy" ::: "memory");
	con_hex2("WDT", "was=", en, "now=", rd32(G4_WDT_BASE + G4_WDT_EN));
#else
	/* QEMU build: there is no watchdog there. */
	con_str("WDT", "skipped (not a phone)");
#endif
}

/* ---- exceptions ---- */

static void irq_handler(void)
{
	uint64_t iar = rsr("icc_iar1_el1");
	uint32_t id = iar & 0xffffff;

	if (id >= 1020 && id <= 1023)
		return;	/* spurious */
	irq_last = id;
	if (id == (uint32_t)irq_src)
		irq_count++;
	else
		irq_other++;
	if (id == VTIMER_INTID)
		wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);	/* 10 ms */
	else if (id == PTIMER_INTID)
		wsr("cntp_tval_el0", rsr("cntfrq_el0") / 100);
	wsr("icc_eoir1_el1", iar);
	isb();
}

static volatile int in_exc;

void exc_handler(uint64_t slot, uint64_t *regs)
{
	if (slot == 5) {	/* IRQ from current EL, SPx */
		irq_handler();
		return;
	}
	/* First thing, before anything that could fault again. */
	trail(0x2000, (uint32_t)slot);
	trail(0x2001, (uint32_t)rsr("elr_el1"));
	trail(0x2002, (uint32_t)rsr("esr_el1"));
	trail(0x2003, (uint32_t)rsr("far_el1"));

	/*
	 * A fault while handling a fault: the framebuffer may well be the
	 * thing that faulted, so do not touch it again. The eMMC log is
	 * written by reset(), and it only needs the console for its own
	 * progress messages, so switching the console off keeps the log.
	 */
	if (in_exc) {
		con_disable();
		/* Without this, reset() calls emmc_write_log() again; if that
		 * is what faulted, it faults again, forever. */
		panic_stage = 1;
		reset(5);
	}
	in_exc = 1;

#if SHIM_HW_DIAG
	/*
	 * The splash framebuffer (cont_splash_mem, 0x03400000) sits inside
	 * the 150 MiB "secure_region" the kernel reserves for the secure
	 * world, and the XPU blocks the non-secure EL1 world from writing
	 * it. So on the phone the console never touches the framebuffer:
	 * the log lives in RAM and on the eMMC card.
	 */
	con_disable();
#endif
	con_clear(BLUE);
	switch (slot & 3) {
	case 0: con_str("PANIC", "SYNC"); break;
	case 1: con_str("PANIC", "IRQ"); break;
	case 2: con_str("PANIC", "FIQ"); break;
	default: con_str("PANIC", "SERROR"); break;
	}
	con_hex2("SLOT", "n=", slot, "cpu=", rsr("mpidr_el1"));
	con_str("STAGE", stage);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("SPSR", rsr("spsr_el1"));
	con_hex("X0", regs[0]);
	con_hex("X1", regs[1]);
	con_hex("X30", regs[29]);	/* stashed lr is just above the frame */
	/*
	 * The log is already in scratch RAM; do not wait long.
	 *
	 * The panic path must NOT try to write to the card again: the
	 * fault very likely came from the eMMC driver, and a second
	 * attempt would fault again and recurse (the "fault while
	 * handling a fault" branch above). So the card write is skipped
	 * here and the wait is used to report the stage of the fault.
	 */
	panic_stage = 1;
	/*
	 * Report where the fault happened through the wait time.
	 *
	 * The wait is the phase number, in seconds, so the computer reads
	 * it directly. The encoding is unambiguous by construction:
	 *   1..10   the phase of the shim
	 *   11..69  the step inside the eMMC driver (phase 2)
	 *   70..109 the phase of the shim again (psci, gic, fdt)
	 *   +128    the fault was on a secondary core
	 *
	 * The fine grained eMMC step is a fallback. The precise answer is
	 * the faulting instruction, and the AArch64 exception link register
	 * holds exactly that: elr_el1 is the address of the instruction
	 * that took the fault. When the fault happened inside our own
	 * image the offset from the image base is small, so it fits in
	 * the wait time and pins the crash to a single instruction
	 * instead of a whole step.
	 */
	if (SHIM_RAW_TIMING) {
		uint64_t mpidr = rsr("mpidr_el1");
		uint64_t elr = rsr("elr_el1");
		uintptr_t img = (uintptr_t)shim_main;
		unsigned n;

		if (phase_no == 2)
			n = 10 + emmc_step;
		else
			n = phase_no;

		/*
		 * If elr points into our image, report the offset from
		 * shim_main instead: it is more precise than the step.
		 * The wait time is one byte, so the offset is counted in
		 * 32 byte units (8 instructions): that covers 8 KiB of
		 * code, which is the whole text section, and keeps the
		 * number inside 0..255.
		 */
		if (elr >= img && elr < img + 0x8000)
			n = (unsigned)((elr - img) / 32);
		else if (elr >= img && elr < img + 0x100000)
			n = (unsigned)((elr - img) / 32) & 0x7f;

		reset_stage = SHIM_CRASH_BASE + n +
			      (((mpidr & 0xffffff) != 0) ? 100 : 0);
	} else if (reset_stage == 0xffffffffU)
		reset_stage = SHIM_CRASH_BASE +
			      (emmc_first_stage != 0xff ? emmc_first_stage : 0);
	reset(15);
}

/* ---- SMP ---- */

static uintptr_t find_gicr(void);

void secondary_main(uint64_t idx)
{
	sec_mpidr[idx] = rsr("mpidr_el1") | (1ULL << 63);
	if (idx == 1) {
		uintptr_t r = find_gicr();
		if (r) {
			sec_probe[0] = rd32(r + GICR_IGROUPR0);
			wr32(r + GICR_IGROUPR0, 0xffffffff);
			sec_probe[1] = rd32(r + GICR_IGROUPR0);
			wr8(r + GICR_IPRIORITYR + 19, 0xa0);
			sec_probe[2] = rd32(r + GICR_IPRIORITYR + 16);
		}
		sec_probe[3] = r ? 1 : 0xdead;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_CPU_OFF, 0, 0, 0);
}

static void __attribute__((unused)) test_smp(void)
{
	stage = "smp";
	uint64_t self = rsr("mpidr_el1") & 0xffffff;
	int64_t ret[NR_CPUS];

	for (int i = 1; i < NR_CPUS; i++)
		ret[i] = smc(PSCI_CPU_ON, (uint64_t)i << 8, (uintptr_t)secondary_entry, i);
	delay_ms(500);
	for (int i = 1; i < NR_CPUS; i++) {
		char label[] = "CPU0";
		label[3] = '0' + i;
		if (((uint64_t)i << 8) == self)
			con_str(label, "is boot cpu");
		else
			con_hex2(label, "ret=", (uint32_t)ret[i], "mpidr=",
				 (uint32_t)sec_mpidr[i] | (sec_mpidr[i] ? 0 : 0xdead0000));
	}
}

/* ---- GIC + timer ---- */

static uintptr_t find_gicr(void)
{
	uint64_t aff = rsr("mpidr_el1");
	uint32_t want = (aff & 0xffffff) | ((aff >> 8) & 0xff000000);

	for (uintptr_t off = 0; off < GICR_SIZE; off += GICR_PE_SIZE) {
		uint64_t typer = rd64(GICR_BASE + off + GICR_TYPER);
		if ((uint32_t)(typer >> 32) == want)
			return GICR_BASE + off;
		if (typer & (1 << 4))	/* Last */
			break;
	}
	return 0;
}

static void gicd_wait_rwp(void)
{
	for (int i = 0; i < 1000000 && (rd32(GICD_BASE) & (1U << 31)); i++)
		;
}

/* Run one interrupt source for ~300 ms and report what the GIC saw. */
static void irq_window(const char *label, uintptr_t r, int src)
{
	uint64_t f = rsr("cntfrq_el0");
	uint32_t pend = 0, act = 0, isr = 0, hppi = 0x3ff, ist = 0;

	irq_count = irq_other = 0;
	irq_last = 0x3ff;
	irq_src = src;
	if (src == VTIMER_INTID) {
		wsr("cntv_tval_el0", f / 100);
		wsr("cntv_ctl_el0", 1);
	} else if (src == PTIMER_INTID) {
		wsr("cntp_tval_el0", f / 100);
		wsr("cntp_ctl_el0", 1);
	}
	isb();
	/* sample state with IRQs still masked, after the first period */
	uint64_t t = now() + f / 50;
	while (now() < t)
		;
	if (src == SGI_INTID)
		wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
	isb();
	ist = src == PTIMER_INTID ? rsr("cntp_ctl_el0") : rsr("cntv_ctl_el0");
	pend = rd32(r + GICR_ISPENDR0);
	act = rd32(r + GICR_ISACTIVER0);
	isr = rsr("isr_el1");
	hppi = rsr("icc_hppir1_el1");

	__asm__ volatile("msr daifclr, #2" ::: "memory");
	t = now() + f * 3 / 10;
	/* SGI: one already sent above, send 9 more 10 ms apart -> expect 10 */
	uint64_t next = now() + f / 100;
	for (int sent = 1; now() < t;)
		if (src == SGI_INTID && sent < 10 && now() >= next) {
			wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
			isb();
			sent++;
			next += f / 100;
		}
	__asm__ volatile("msr daifset, #2" ::: "memory");
	wsr("cntv_ctl_el0", 0);
	wsr("cntp_ctl_el0", 0);
	isb();

	con_hex2(label, "irqs=", irq_count, "other=", irq_other);
	con_hex2(" pre", "pend=", pend, "act=", act);
	con_hex2(" pre", "isr=", isr, "hppi=", hppi);
	con_hex2(" pre", "tctl=", ist, "last=", irq_last);
}

static void test_gic(void)
{
	stage = "gic sre";
	/*
	 * The MSM8992 has a GICv2 (DTS: qcom,msm-qgic2). 0xf9002000 is the
	 * GICv2 CPU interface, not a GICv3 redistributor, and the ICC_* system
	 * registers are UNDEFINED when ID_AA64PFR0_EL1.GIC is 0: the first
	 * icc_sre_el1 read below would take a sync exception, turn the wait
	 * time into a panic code and skip the final log write to the card.
	 */
	if (((rsr("id_aa64pfr0_el1") >> 24) & 0xf) == 0) {
		con_str("GIC", "no GICv3 sysreg iface (GICv2 SoC), skip");
		return;
	}
	/*
	 * Sub-phases: the GIC is the part of this device that is most
	 * likely to be wrong (wrong base, wrong interface version, or an
	 * address the XPU does not let EL1 touch). Each step gets its own
	 * phase number, so the wait time says exactly which one faults.
	 */
	PHASE(81);
	uint64_t sre = rsr("icc_sre_el1");
	PHASE(82);
	if (!(sre & 1)) {
		wsr("icc_sre_el1", sre | 1);
		isb();
	}
	PHASE(83);
	if (!(rsr("icc_sre_el1") & 1)) {
		con_str("GIC", "no sysreg iface, skip");
		return;
	}

	stage = "gic dist";
	PHASE(84);
	uint32_t dctlr = rd32(GICD_BASE);
	PHASE(85);
	/* Non-secure view (DS=0): bit0 EnableGrp1, bit1 EnableGrp1A, bit4 ARE_NS; Linux writes 0x13 */
	wr32(GICD_BASE, dctlr | 0x13);
	PHASE(86);
	gicd_wait_rwp();
	PHASE(87);
	con_hex2("GICD", "was=", dctlr, "now=", rd32(GICD_BASE));

	stage = "gic redist";
	PHASE(88);
	uintptr_t r = find_gicr();
	if (!r) {
		con_str("GICR", "not found");
		return;
	}
	PHASE(89);
	uint32_t waker = rd32(r + GICR_WAKER);
	PHASE(90);
	if (waker & 2) {
		wr32(r + GICR_WAKER, waker & ~2U);
		for (int i = 0; i < 100000 && (rd32(r + GICR_WAKER) & 4); i++)
			;
	}
	PHASE(91);
	con_hex2("GICR", "base=", r, "waker=", rd32(r + GICR_WAKER));

	stage = "gic config";
	PHASE(92);
	uint32_t grp = rd32(r + GICR_IGROUPR0);
	PHASE(93);
	wr32(r + GICR_IGROUPR0, 0xffffffff);	/* NS view: secure bits are WI */
	PHASE(94);
	uint32_t en_before = rd32(r + GICR_ISENABLER0);
	PHASE(95);
	uint32_t pri_before = rd32(r + GICR_IPRIORITYR + 16);	/* INTIDs 16..19 */
	PHASE(96);
	for (int id = 0; id < 32; id++)
		wr8(r + GICR_IPRIORITYR + id, 0xa0);
	PHASE(97);
	wr32(r + GICR_ISENABLER0, (1U << VTIMER_INTID) | (1U << PTIMER_INTID) | (1U << SGI_INTID));
	PHASE(98);
	con_hex2("ENABLE", "was=", en_before, "now=", rd32(r + GICR_ISENABLER0));
	(void)grp;
	(void)pri_before;

	PHASE(99);
	uint64_t ctlr = rsr("icc_ctlr_el1");
	PHASE(100);
	wsr("icc_ctlr_el1", ctlr & ~2ULL);	/* EOImode 0: EOIR also deactivates */
	PHASE(101);
	wsr("icc_pmr_el1", 0xf0);
	PHASE(102);
	wsr("icc_bpr1_el1", 0);
	PHASE(103);
	wsr("icc_igrpen1_el1", 1);
	PHASE(104);
	isb();
	PHASE(105);
	con_hex2("ICC", "ctlr=", ctlr, "grpen1=", rsr("icc_igrpen1_el1"));

	stage = "gic vtimer";
	PHASE(106);
	irq_window("VTIMER", r, VTIMER_INTID);
	PHASE(107);
	stage = "gic sgi";
	irq_window("SGI1", r, SGI_INTID);
	PHASE(108);
	wr32(r + GICR_ICENABLER0, 0xffffffff);
	PHASE(109);
	isb();
}

/* ---- main ---- */

static void __attribute__((unused)) psci_line(const char *label, uint64_t fn)
{
	con_hex2(label, "feat=", smc(PSCI_FEATURES, fn, 0, 0), 0, 0);
}

static uint64_t scratch[8192];	/* 64KB, lives in our (later cacheable) image */

static uint64_t ticks_fill(uint32_t c)
{
	uint64_t t = now();
	con_clear(c);
	return now() - t;
}

static uint64_t ticks_mem(void)
{
	uint64_t t = now();
	for (int pass = 0; pass < 16; pass++)
		for (int i = 0; i < 8192; i++)
			scratch[i] = scratch[i] * 3 + i;
	return now() - t;
}

static uint32_t to_us(uint64_t ticks)
{
	return ticks * 1000000 / rsr("cntfrq_el0");
}

/* ---- device tree ---- */

static int put_hex8(char *buf, int n, uint32_t v)
{
	static const char hex[] = "0123456789ABCDEF";

	for (int i = 0; i < 2; i++)
		buf[n + i] = hex[(v >> (4 * (1 - i))) & 0xf];
	return n + 2;
}

/*
 * Print one device-tree property. Text properties are printed as text,
 * everything else as hex. The node path is part of the label, so the
 * /memory carve-out list stays readable.
 */
static int dump_prop(const char *node_path, const char *name, uint32_t len,
		     const uint8_t *val, void *ctx)
{
	char buf[112];
	char label[48];
	int n = 0, i = 0, printable = len > 0;
	const char *base = node_path;

	(void)ctx;
	for (const char *s = name; *s && i < (int)sizeof(label) - 1; s++)
		label[i++] = *s;
	label[i] = 0;

	for (uint32_t k = 0; k < len; k++)
		if (val[k] < 0x20 || val[k] > 0x7e)
			printable = 0;

	if (printable) {
		for (uint32_t k = 0; k < len && n < (int)sizeof(buf) - 1; k++)
			buf[n++] = (char)val[k];
	} else {
		for (uint32_t k = 0; k < len && k < 12; k++) {
			n = put_hex8(buf, n, val[k]);
			buf[n++] = ' ';
		}
		if (len > 12) {
			buf[n++] = '.';
			buf[n++] = '.';
		}
	}
	buf[n] = 0;

	/* label: "<node> <prop>" */
	i = 0;
	for (const char *s = base; *s && i < (int)sizeof(label) - 2; s++)
		label[i++] = *s;
	label[i++] = ' ';
	label[i++] = ' ';
	label[i] = 0;
	con_str(label, buf);
	return 0;
}

/* ---- device tree discovery ---- */

#define FDT_MAGIC	0xd00dfeed

static int fdt_ok(uint64_t a)
{
	if (!a || (a & 3))
		return 0;
	return __builtin_bswap32(rd32(a)) == FDT_MAGIC;
}

/*
 * Where is the device tree?
 *
 * LK's scm_elexec_call() puts the DTB pointer in x0, which is what the
 * sweet shim assumed and what the V60 port uses. ABL for arm64 uses a
 * different convention, and this phone's bootloader is called "aboot" in
 * the partition table, so do not trust a single register: check x0, x1 and
 * x2, then the addresses bootloaders usually park the tree at, then
 * sweep the low memory.
 */
static uint64_t find_dtb(uint64_t x0, uint64_t x1, uint64_t x2)
{
	static const uint64_t candidates[] = {
		0x00000100UL,	/* LG's own stock tags_addr */
		0x00400000UL,	/* the tags_addr our boot image asks for */
		0x00800000UL, 0x01000000UL, 0x02000000UL, 0x04000000UL,
		0x08000000UL, 0x10000000UL, 0x20000000UL, 0x30000000UL,
		0x38000000UL, 0x3c000000UL, 0x40000000UL, 0x50000000UL,
		0x60000000UL, 0x70000000UL, 0x78000000UL, 0x7f000000UL,
		0x80000000UL, 0x80080000UL, 0x80100000UL, 0x81000000UL,
		0x81800000UL, 0x82000000UL, 0x82200000UL, 0x82800000UL,
		0x83000000UL, 0x88000000UL, 0x8a000000UL, 0x90000000UL,
		0x9f900000UL, 0xc0000000UL, 0xd0000000UL,
	};

	if (fdt_ok(x0))
		return x0;
	if (fdt_ok(x1))
		return x1;
	if (fdt_ok(x2))
		return x2;
	for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
		if (fdt_ok(candidates[i]))
			return candidates[i];
	/*
	 * No sweep of the low memory here: on this SoC 0x0-0x9600000 is the
	 * secure region, protected by the XPU, and reading it from the
	 * non-secure EL1 world takes a data abort. The bootloader always
	 * hands the tree over somewhere in ordinary RAM.
	 */
	return 0;
}

/* ---- framebuffer discovery ---- */

struct fb_search {
	char path[128];
	int sfb;		/* a simple-framebuffer node */
	int splash;		/* the cont_splash_mem carve-out */
};

static int eqstr(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static int contains(const uint8_t *buf, uint32_t len, const char *needle)
{
	uint32_t n = 0;

	while (needle[n])
		n++;
	for (uint32_t i = 0; i + n <= len; i++) {
		uint32_t j = 0;
		while (j < n && buf[i + j] == (uint8_t)needle[j])
			j++;
		if (j == n)
			return 1;
	}
	return 0;
}

static int fb_cb(const char *node_path, const char *name, uint32_t len,
		 const uint8_t *val, void *ctx)
{
	struct fb_search *s = ctx;
	int hit = 0;

	if (eqstr(name, "compatible") && contains(val, len, "simple-framebuffer"))
		hit = 1;
	else if (eqstr(name, "label") && contains(val, len, "cont_splash_mem"))
		hit = 1;
	if (!hit)
		return 0;
	s->sfb |= eqstr(name, "compatible");
	s->splash |= !eqstr(name, "compatible");
	for (int i = 0; node_path[i] && i < (int)sizeof(s->path) - 1; i++)
		s->path[i] = node_path[i];
	s->path[sizeof(s->path) - 1] = 0;
	return 1;
}

/*
 * Where is the framebuffer? The G4's device tree has no simple-framebuffer
 * node: the panel is driven by the MDP in command mode and the bootloader
 * leaves a 12 MiB cont_splash_mem window at 0x03400000 that the display
 * controller keeps scanning out. Prefer what the device tree says, fall
 * back to the compiled-in address.
 */
static void find_framebuffer(uint64_t dtb)
{
	struct fb_search s = {0};
	uint64_t cells[4];
	int n;

	if (!dtb)
		return;
	fdt_walk_props(dtb, "", fb_cb, &s);

	if (!s.path[0])
		return;
	n = fdt_prop_cells(dtb, s.path, "reg", cells, 4);
	if (n < 1)
		return;

	/* reg is <addr size> with one cell each, or <0 addr 0 size> when the
	 * node inherits the root's two address cells and two size cells. */
	uint64_t base, size = 0;
	if (n >= 4 && cells[0] == 0 && cells[2] == 0) {
		base = cells[1];
		size = cells[3];
	} else {
		base = cells[0];
		size = n > 1 ? cells[1] : 0;
	}

	uint64_t w = 0, h = 0, stride = 0;
	fdt_prop_u64(dtb, s.path, "width", &w);
	fdt_prop_u64(dtb, s.path, "height", &h);
	fdt_prop_u64(dtb, s.path, "stride", &stride);
	if (!stride)
		stride = (w ? w : FB_W) * FB_BPP / 8;

	/* Never draw outside the window the device tree reserved. */
	if (size && stride && size / stride < h)
		h = size / stride;

	con_set_fb((uintptr_t)base, (uint32_t)(w ? w : FB_W),
		   (uint32_t)(h ? h : FB_H), (uint32_t)stride);
	con_str("FB", s.sfb ? "simple-framebuffer" : "cont_splash_mem");
	con_hex2("FB", "at=", (uint32_t)base, "size=", (uint32_t)size);
	con_hex2("FB", "w=", (uint32_t)(w ? w : FB_W), "h=", (uint32_t)(h ? h : FB_H));
}

/*
 * LK fills in /memory (it is <0 0 0 0> in the stored DTB) and /chosen
 * (bootargs, initrd). Print both: the RAM map is what decides where XNU
 * can live, and we cannot guess it.
 */
static void dump_fdt(uint64_t dtb)
{
	uint64_t v;

	if (!dtb)
		return;
	con_str("FDT", "/chosen:");
	con_skip();
	fdt_walk_props(dtb, "/chosen", dump_prop, 0);
	con_str("FDT", "/memory + carve-outs:");
	con_skip();
	fdt_walk_props_under(dtb, "/memory", dump_prop, 0);
	con_skip();
	if (fdt_prop_u64(dtb, "/chosen", "linux,initrd-start", &v))
		con_hex("INITRD", v);
}

void shim_main(uint64_t x0, uint64_t x1, uint64_t x2)
{
	struct mmu_info mi;
	uint64_t dtb = find_dtb(x0, x1, x2);

	shim_t0 = now();
	trail_init();
	PHASE(1); stage = "boot";
#if SHIM_HW_DIAG
	/*
	 * On the phone the console starts switched off. The splash
	 * framebuffer lives inside the 150 MiB secure region that the XPU
	 * protects from the non-secure EL1 world, and a write there is a data
	 * abort: the log would die in the exception handler before it could
	 * be written anywhere. In RAM it is safe, and at the very end the
	 * log is replayed into the framebuffer to find out whether the
	 * region turns out to be writable after all.
	 */
	con_disable();
#endif
	/*
	 * Find the framebuffer before painting anything. con_fb starts at the
	 * compiled-in address, so a device tree that describes one wins, and
	 * otherwise the fallback is used. Painting first would fault on a
	 * machine whose real framebuffer is only in the device tree (QEMU).
	 */
	find_framebuffer(dtb);
	con_clear(RED);
	con_str("SHIM", "LG G4 (H815) bring-up shim");
	con_hex2("DTB", "at=", dtb, "magic=", dtb ? __builtin_bswap32(rd32(dtb)) : 0);
	con_hex2("DTBC", "x0=", x0, "x1=", x1);
	con_hex2("DTBD", "x2=", x2, "fdt0=", fdt_ok(x0));
	con_hex2("CPU", "cntfrq=", rsr("cntfrq_el0"), "mmfr0=", rsr("id_aa64mmfr0_el1"));
	con_skip();

	/*
	 * The log of the PREVIOUS run, read out of the ramoops console zone
	 * (2026-11-10).
	 *
	 * The pstore is a one shot channel: the zone holds the last writer
	 * only, and the recovery overwrites it with its own console as soon
	 * as it boots, so the log of the run that just finished is unreadable
	 * by the time anybody could look at it. The shim runs before anything
	 * else touches the zone, so it reads the record of the previous run
	 * and prints it: every run prints the one before it, with no
	 * external tool in between.
	 *
	 * This is what makes a BOOTLOOP readable: the run that loops prints
	 * the log of the run before it, and the one before that, and so on.
	 */
	pstore_read_previous();

	stop_watchdog();

#if SHIM_MMIO_SELFTEST
	/* The self-test of the allowlist: it tries the write that hung the
	 * phone and checks that it is refused. */
	mmio_selftest();
#endif

	/*
	 * The eMMC first: it is the only log channel that survives the reset,
	 * so it has to be written even if something later goes wrong. The
	 * trail it leaves behind says how far the card bring-up got.
	 */
#if SHIM_HW_DIAG
	PHASE(2); stage = "emmc";

	/*
	 * The watchdog guard (shim/wdt.c). It is armed BEFORE anything that
	 * touches the controller and disarmed once the log is safe. The
	 * watchdog was not armed at the shim's entry (the successful runs
	 * have no "WDT" line), so a bus hang had nothing to reset the
	 * phone: with the guard, a hang becomes a reset.
	 */
#if SHIM_WDT_GUARD
	g4_wdt_arm(G4_WDT_GUARD_MS);
#endif

#if SHIM_EMMC_MODEL
	/* The model takes its mode from the device tree and starts from the
	 * register values measured on the phone. */
	emmc_model_config(dtb);
#endif
#if SHIM_SD_PROBE
	/* The T2 measurement, BEFORE the bring-up: the eMMC controller has
	 * to be seen exactly as the LK left it. Reads only. */
	if (sd_probe_should_run(0))
		sd_probe_before();
#endif
#if SHIM_EMMC_PROBE
	/*
	 * The E0 and D0 probes (shim/emmc_probe.c), BEFORE anything touches a
	 * controller. The order matters:
	 *
	 *   E0 reads the eMMC host registers exactly as the LK left them, and
	 *      then sends ONE command (CMD5 awake, argument 0) to find out
	 *      whether the card answers at all. The bring-up that follows
	 *      resets the host and re-identifies the card, so the "before"
	 *      snapshot has to be taken first.
	 *
	 *   D0 reads the MDP registers (VIG0/CTL0) and the frame counter of
	 *      the timing engine. Pure reads, and the MDP window is in the
	 *      MMIO allowlist as READ-ONLY, so even a mistake here cannot
	 *      hang the phone on a write.
	 */
	PHASE(1); stage = "e0d0";
	emmc_probe_e0();
	emmc_probe_d0();
#endif
	emmc_write_log(con_log, (unsigned)con_log_len);
#if SHIM_SD_PROBE
	/* And again AFTER it: the bring-up resets the host, and the question
	 * is what that reset erases. Reads only. */
	sd_probe_after();
#endif
#if SHIM_SD_INIT
	/*
	 * The microSD slot. It comes after the eMMC (whose log is already
	 * safe) and before the pstore write (so its outcome is in the record
	 * that survives the reset).
	 *
	 * THE ORDER INSIDE sd_init() IS THE POINT: the clock of SDCC2 goes
	 * on BEFORE the controller is touched. The QEMU model refuses to
	 * serve the SDCC2 windows until the clock is on, so a wrong order
	 * fails the test instead of hanging the phone (GUIDE-05).
	 */
	PHASE(2); stage = "sd";
	{
		static uint8_t sd_block[512];
		int sd_rc = sd_init(sd_block);

		con_hex2("SD", "rc=", (uint32_t)sd_rc, "stage=",
			 (uint32_t)sd_init_stage);
		con_hex2("SD", "err=", (uint32_t)sd_init_error, "rca=",
			 sd_rca);
		trail(0x2600, (uint32_t)sd_rc);
		trail(0x2601, (uint32_t)sd_init_stage);
	}
#endif
#if SHIM_EMMC_MODEL
	emmc_model_finish();
#endif
	first_step = emmc_step;
	trail(0x1100, (uint32_t)emmc_first_error);
	trail(0x1101, emmc_first_stage);
	trail(0x1102, (uint32_t)emmc_first_cmd_error);
	trail(0x1103, first_step);
	/*
	 * The log goes into the ramoops console zone: DRAM that survives the
	 * reset and that the next Android boot reads back as
	 * /sys/fs/pstore/console-ramoops-0. This is the only channel that
	 * works when the card is mute, so it is written here, with the MMU
	 * still off, as soon as the eMMC attempt is over.
	 */
	pstore_console_write(con_log, (unsigned)con_log_len);
#if SHIM_WDT_GUARD
	/* The log is safe now: the guard has done its job. */
	g4_wdt_disarm();
#endif
#endif

	/* An xnu payload in the ramdisk means: boot it (MMU stays off). */
	PHASE(3); stage = "loader";
#if SHIM_SD_PAYLOAD
	/*
	 * The payload can also live on the microSD: the eMMC is mute, so
	 * the boot image carries only the shim. sd_payload_load() reads it
	 * from the card, checks the CRC and tells the loader to use that
	 * buffer (shim/loader.c). If the card has no payload, the loader
	 * falls back to the initrd of the boot image.
	 */
	con_hex2("SDPAY", "pre", (uint32_t)sd_init_error, "stage=",
		 (uint32_t)sd_init_stage);
	if (sd_init_error == 0) {
		int prc = sd_payload_load();

		con_hex2("SDPAY", "rc=", (uint32_t)prc, "err=",
			 (uint32_t)sd_payload_error);
	}
#endif
	const char *why = loader_boot(dtb);
	con_str("LOADER", why);
	con_skip();

	/* No payload: run the diagnostics so the screen shows what we learned. */
	PHASE(4); stage = "fill off";
	uint64_t fill_off = ticks_fill(RED);
	uint64_t mem_off = ticks_mem();

	PHASE(5); stage = "mmu enable";
	int rc = mmu_enable(&mi);
	shim_mmu_rc = rc;

	PHASE(6); stage = "fill on";
	uint64_t fill_on = rc ? 0 : ticks_fill(RED);
	uint64_t mem_on = rc ? 0 : ticks_mem();
	if (rc)
		con_clear(RED);

	con_str("SHIM", "diagnostics");
	con_skip();
	con_hex2("IMAGE", "lo=", mi.img_lo, "hi=", mi.img_hi);
	con_hex2("MMU", "rc=", rc, "l3=", mi.n_l3);
	con_hex("TTBR0", mi.ttbr);
	con_hex("SCTLR0", mi.sctlr_before);
	con_hex("SCTLR1", mi.sctlr_after);
	con_hex("TCR", rsr("tcr_el1"));
	/*
	 * PARange is bits [3:0] of ID_AA64MMFR0_EL1 (the physical address
	 * range), NOT a granule. The granule fields are TGran4 [15:12],
	 * TGran16 [19:16] and TGran64 [23:20], and each is a bitmask: bit 0
	 * means "4 KiB implemented", bit 1 "16 KiB", bit 2 "64 KiB".
	 */
	con_hex2("PARANGE", "pa=", (uint32_t)(rsr("id_aa64mmfr0_el1") & 0xf),
		 "tgran4=", (uint32_t)((rsr("id_aa64mmfr0_el1") >> 12) & 0xf));
	con_hex2("TGRAN", "16k=", (uint32_t)((rsr("id_aa64mmfr0_el1") >> 16) & 0xf),
		 "64k=", (uint32_t)((rsr("id_aa64mmfr0_el1") >> 20) & 0xf));
	con_hex2("GRANULE", "16k=", mmu_granule_ok() ? 1 : 0, "tg1=", (uint32_t)((rsr("id_aa64mmfr0_el1") >> 20) & 0xf));
	con_skip();
	con_hex("PAR img", mmu_probe((uintptr_t)shim_main));
	con_hex("PAR fb", mmu_probe(FB_PHYS + 0x1000));
#if SHIM_HW_DIAG
	con_hex("PAR gic", mmu_probe(GICD_BASE));
#endif
	con_skip();
	con_hex2("FILL us", "off=", to_us(fill_off), "on=", to_us(fill_on));
	con_hex2("MEM us", "off=", to_us(mem_off), "on=", to_us(mem_on));
	con_skip();

	/*
	 * The H815 device tree has no psci node ("psci is broken" in AOSP's
	 * h815 port) and the CPU nodes use enable-method =
	 * "qcom,8994-arm-cortex-acc", so PSCI CPU_ON is not available.
	 * Probe it anyway: if the TZ does answer, SMP becomes possible.
	 */
#if SHIM_HW_DIAG
	/*
	 * The H815 device tree has no psci node ("psci is broken" in AOSP's
	 * h815 port, and the node is explicitly deleted with
	 * /delete-node/ psci;) and the CPU nodes use enable-method =
	 * "qcom,8994-arm-cortex-acc", so PSCI is not available.
	 *
	 * Calling it faults: the phase counter puts the crash exactly in
	 * smc(PSCI_VERSION). The probe is therefore gone. SMP is not
	 * reachable through PSCI on this device; the CPU nodes would have
	 * to be started with the "qcom,8994-arm-cortex-acc" method, which
	 * is a different (and much deeper) piece of work.
	 */
	PHASE(70); stage = "psci";
	con_str("PSCI", "not available on this device (no psci node)");
	con_skip();

	PHASE(80); stage = "gic";
	PHASE(81);
	test_gic();
#else
	con_str("PSCI", "skipped (not a phone)");
	con_str("GIC", "skipped (not a phone)");
#endif

	/*
	 * The diagnostics above clear the screen, so the device tree is
	 * dumped last: on the phone this is what tells us the real RAM map.
	 */
	PHASE(9); stage = "fdt";
	dump_fdt(dtb);

	PHASE(10); stage = "done";
#if SHIM_HW_DIAG
	/*
	 * Last chance to show something on the panel. The bootloader left the
	 * DSI controller pointing at the splash window, so if that window is
	 * writable after all, the log appears here - for as long as the phone
	 * stays up before the reset into recovery.
	 */
	con_enable();
	con_repaint();
#endif
	/*
	 * The stage of the eMMC bring-up goes into the time the phone stays
	 * up, so that it can be read off the clock even when the card could
	 * not be written to.
	 *
	 * emmc_first_* holds the outcome of the first attempt, which is the
	 * one this has to report: reset() calls emmc_write_log() a second
	 * time and that call would otherwise replace the stage.
	 */
	reset_stage = emmc_first_stage;
	/* The log is in scratch RAM now, so there is no reason to linger. */
	reset(15);
}
