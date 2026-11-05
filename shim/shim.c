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
static unsigned reset_stage = 0xffffffffU;	/* 0xffffffff: not set */
static uint64_t shim_t0;
/* Set when the shim took a fault: the stage of the fault is reported
 * through the wait time, and the card write is skipped. */
static int panic_stage;

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

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t rd64(uintptr_t a) { return *(volatile uint64_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

extern char secondary_entry[];

static const char *stage;	/* set at runtime: no absolute pointers in data */
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

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
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
#if !SHIM_HW_DIAG
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
			 * A fault: the wait is the fine grained eMMC step,
			 * in seconds, so the computer reads it directly.
			 */
			uint64_t hz = rsr("cntfrq_el0");
			uint64_t elapsed_s = (now() - shim_t0) / hz;
			uint64_t deadline_s = (uint64_t)reset_stage;

			wait_s = (deadline_s > elapsed_s) ?
				 (unsigned)(deadline_s - elapsed_s) : 0;
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
#ifndef SHIM_HW_DIAG
	/* QEMU build: no watchdog to kick, so just park and let the monitor
	 * dump the framebuffer. */
	(void)wait_s;
	for (;;)
		__asm__ volatile("wfe");
#else
	delay_ms(wait_s * 1000);
#if SHIM_HW_DIAG
	/*
	 * Ask the bootloader to come up in recovery, so the log that was just
	 * written to the card can be read back with dd. LK reads this
	 * register at boot (platform/msm_shared/reboot.c) and boots the
	 * recovery partition when it finds RECOVERY_MODE.
	 */
	wr32(G4_RESTART_REASON, LK_RECOVERY_MODE);
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	wr32(G4_PSHOLD, 0);
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
#if SHIM_HW_DIAG
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

	/*
	 * A fault while handling a fault: the framebuffer may well be the
	 * thing that faulted, so do not touch it again. The eMMC log is
	 * written by reset(), and it only needs the console for its own
	 * progress messages, so switching the console off keeps the log.
	 */
	if (in_exc) {
		con_disable();
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
	 * Report where the fault happened through the wait time: the wait
	 * is set to the fine grained eMMC step, so the computer can read
	 * it straight off the clock.
	 */
	if (SHIM_RAW_TIMING)
		reset_stage = emmc_step;
	else if (reset_stage == 0xffffffffU)
		reset_stage = emmc_first_stage != 0xff ?
					emmc_first_stage : 0;
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
	uint64_t sre = rsr("icc_sre_el1");
	if (!(sre & 1)) {
		wsr("icc_sre_el1", sre | 1);
		isb();
	}
	if (!(rsr("icc_sre_el1") & 1)) {
		con_str("GIC", "no sysreg iface, skip");
		return;
	}

	stage = "gic dist";
	uint32_t dctlr = rd32(GICD_BASE);
	/* Non-secure view (DS=0): bit0 EnableGrp1, bit1 EnableGrp1A, bit4 ARE_NS; Linux writes 0x13 */
	wr32(GICD_BASE, dctlr | 0x13);
	gicd_wait_rwp();
	con_hex2("GICD", "was=", dctlr, "now=", rd32(GICD_BASE));

	stage = "gic redist";
	uintptr_t r = find_gicr();
	if (!r) {
		con_str("GICR", "not found");
		return;
	}
	uint32_t waker = rd32(r + GICR_WAKER);
	if (waker & 2) {
		wr32(r + GICR_WAKER, waker & ~2U);
		for (int i = 0; i < 100000 && (rd32(r + GICR_WAKER) & 4); i++)
			;
	}
	con_hex2("GICR", "base=", r, "waker=", rd32(r + GICR_WAKER));

	stage = "gic config";
	uint32_t grp = rd32(r + GICR_IGROUPR0);
	wr32(r + GICR_IGROUPR0, 0xffffffff);	/* NS view: secure bits are WI */
	uint32_t en_before = rd32(r + GICR_ISENABLER0);
	uint32_t pri_before = rd32(r + GICR_IPRIORITYR + 16);	/* INTIDs 16..19 */
	for (int id = 0; id < 32; id++)
		wr8(r + GICR_IPRIORITYR + id, 0xa0);
	wr32(r + GICR_ISENABLER0, (1U << VTIMER_INTID) | (1U << PTIMER_INTID) | (1U << SGI_INTID));
	con_hex2("ENABLE", "was=", en_before, "now=", rd32(r + GICR_ISENABLER0));
	(void)grp;
	(void)pri_before;

	uint64_t ctlr = rsr("icc_ctlr_el1");
	wsr("icc_ctlr_el1", ctlr & ~2ULL);	/* EOImode 0: EOIR also deactivates */
	wsr("icc_pmr_el1", 0xf0);
	wsr("icc_bpr1_el1", 0);
	wsr("icc_igrpen1_el1", 1);
	isb();
	con_hex2("ICC", "ctlr=", ctlr, "grpen1=", rsr("icc_igrpen1_el1"));

	stage = "gic vtimer";
	irq_window("VTIMER", r, VTIMER_INTID);
	stage = "gic sgi";
	irq_window("SGI1", r, SGI_INTID);
	wr32(r + GICR_ICENABLER0, 0xffffffff);
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
	stage = "boot";
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

	stop_watchdog();

	/*
	 * The eMMC first: it is the only log channel that survives the reset,
	 * so it has to be written even if something later goes wrong. The
	 * trail it leaves behind says how far the card bring-up got.
	 */
#if SHIM_HW_DIAG
	stage = "emmc";
	emmc_write_log(con_log, (unsigned)con_log_len);
#endif

	/* An xnu payload in the ramdisk means: boot it (MMU stays off). */
	stage = "loader";
	const char *why = loader_boot(dtb);
	con_str("LOADER", why);
	con_skip();

	/* No payload: run the diagnostics so the screen shows what we learned. */
	stage = "fill off";
	uint64_t fill_off = ticks_fill(RED);
	uint64_t mem_off = ticks_mem();

	stage = "mmu enable";
	int rc = mmu_enable(&mi);

	stage = "fill on";
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
	con_hex2("PARANGE", "tg0=", (uint32_t)(rsr("id_aa64mmfr0_el1") & 0xf),
		 "tg1=", (uint32_t)((rsr("id_aa64mmfr0_el1") >> 20) & 0xf));
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
	stage = "psci";
	con_hex2("PSCI", "ver=", smc(PSCI_VERSION, 0, 0, 0), "smccc=", smc(SMCCC_VERSION, 0, 0, 0));
	con_skip();

	stage = "gic";
	test_gic();
#else
	con_str("PSCI", "skipped (not a phone)");
	con_str("GIC", "skipped (not a phone)");
#endif

	/*
	 * The diagnostics above clear the screen, so the device tree is
	 * dumped last: on the phone this is what tells us the real RAM map.
	 */
	stage = "fdt";
	dump_fdt(dtb);

	stage = "done";
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
