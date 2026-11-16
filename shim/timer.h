/*
 * timer.h -- the timebase of the shim, and the fix for the hang of 2026-11-15.
 *
 * THE HANG, AND WHY
 * -----------------
 * On 2026-11-15 a run of the shim stopped between emmc_write_log() and
 * pstore_console_write(): the pstore record ended with "EMMC the card is
 * identified; the writes are off", with no "PSTORE hdr=" and no "WDT
 * disarmed" after it. The phone did not crash: it sat there until the
 * 60 s watchdog rebooted it (the "fastboot boot" command came back after
 * 58.5 s).
 *
 * The cause is the timebase of the eMMC driver:
 *
 *   - emmc.c measures its timeouts with the VIRTUAL timer
 *     (cntvct_el0), through emmc_ticks() and emmc_deadline();
 *   - the EL0 counters are NOT running at the shim's entry. Nothing in
 *     the boot path (shim/start.S) programs CNTKCTL_EL1, CNTV_CTL_EL0 or
 *     CNTP_CTL_EL0, and the LK leaves them as it found them;
 *   - a frozen counter reads a constant, so emmc_deadline() returns a
 *     constant too, and every polling loop that waits for the deadline
 *     (emmc_wait, emmc_wait_idle, emmc_wait_busy, emmc_delay_us) waits
 *     for ever.
 *
 * So the phone was not stuck on the eMMC controller: it was stuck in a
 * wait loop whose clock does not run. The fix has two halves and both
 * are needed:
 *
 *   1. the counters are switched ON at the entry (timer_init()), so the
 *      timebase exists;
 *   2. every wait is bounded by that timebase AND pets the watchdog, so
 *      a wait that never ends becomes a reset instead of a hang.
 *
 * WHY THE PHYSICAL TIMER
 * ----------------------
 * The timebase is CNTPCT_EL0 (the physical timer), not CNTVCT_EL0 (the
 * virtual one). The virtual timer of EL0 is the one the hypervisor would
 * own; on a phone without a hypervisor it is unused, but the physical
 * timer is the one that is always there and the one the ARM timer DT
 * node of this phone describes (qcom,armv8-timer, 19.2 MHz).
 *
 * NOTE ON THE INTERRUPTS
 * -----------------------
 * Switching a counter on does NOT raise an interrupt: the timer only
 * interrupts when its TVAL is set, and both TVALs are left at zero (a
 * TVAL of zero means "no expiry"). The IRQ test of the shim
 * (shim/shim.c) programs the TVALs on purpose, and only in its own
 * window.
 */
#pragma once
#include <stdint.h>
#include "g4.h"

/* The watchdog guard: the wait loops pet it (shim/wdt.h). */
void g4_wdt_pet(void);

#ifndef BIT
#define BIT(n)		(1UL << (n))
#endif

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) ({ uint64_t _v = (v); __asm__ volatile("msr " r ", %0" :: "r"(_v)); })

/*
 * The registers of the generic timer (ARM DDI0487):
 *
 *   CNTKCTL_EL1   0x000   [0] EL0VCTEN  [1] EL0PCTEN
 *   CNTP_CTL_EL0  0x010   [0] ENABLE    [1] ISTATUS
 *   CNTV_CTL_EL0  0x018   [0] ENABLE    [1] ISTATUS
 *
 * The shim runs at EL1, so it has to open the door for EL0 (the counters
 * it reads) before it may use them.
 */
#define CNTKCTL_EL1_EL0VCTEN	BIT(0)
#define CNTKCTL_EL1_EL0PCTEN	BIT(1)
#define CNTV_CTL_EL0_ENABLE	BIT(0)

/*
 * Switch the counters on and make sure they are really running.
 *
 * It is called by shim_main() before anything else: every module that
 * waits uses these counters, and a module that waits before the
 * timebase exists waits for ever.
 */
static inline void timer_init(void)
{
	uint64_t kctl = rsr("cntkctl_el1");
	uint64_t vctl = rsr("cntv_ctl_el0");
	uint64_t pctl = rsr("cntp_ctl_el0");

	kctl |= CNTKCTL_EL1_EL0VCTEN | CNTKCTL_EL1_EL0PCTEN;
	wsr("cntkctl_el1", kctl);

	vctl |= CNTV_CTL_EL0_ENABLE;
	wsr("cntv_ctl_el0", vctl);

	pctl |= CNTV_CTL_EL0_ENABLE;
	wsr("cntp_ctl_el0", pctl);

	__asm__ volatile("isb" ::: "memory");
}

/* The raw counter (ticks of the 19.2 MHz clock). */
static inline uint64_t timer_ticks(void)
{
	__asm__ volatile("isb" ::: "memory");
	return rsr("cntpct_el0");
}

/* The tick rate, in Hz. */
static inline uint32_t timer_hz(void)
{
	return (uint32_t)rsr("cntfrq_el0");
}

/* The counter, in microseconds. It is monotonic: the shim never changes
 * the frequency of the clock, so the difference of two readings is a
 * duration. */
static inline uint64_t timer_us(void)
{
	return timer_ticks() * 1000000u / timer_hz();
}

/* The instant at which a wait of "us" microseconds ends. */
static inline uint64_t timer_deadline_us(uint64_t us)
{
	return timer_ticks() + (uint64_t)timer_hz() * us / 1000000u;
}

/*
 * Wait for "us" microseconds.
 *
 * The watchdog is peted on the way: the wait loops are exactly where a
 * hang hides, and a hang that the watchdog cannot end is a brick
 * (shim/wdt.h).
 */
static inline void timer_delay_us(uint32_t us)
{
	uint64_t end = timer_deadline_us(us);
	unsigned n = 0;

	while (timer_ticks() < end) {
		if ((++n & 0x3ffu) == 0)
			g4_wdt_pet();
	}
}

static inline void timer_delay_ms(uint32_t ms)
{
	timer_delay_us(ms * 1000u);
}

/*
 * The same wait, but with a way out: the function returns 0 when the
 * deadline passed and -1 when the condition became true first.
 *
 *   timer_wait_until(deadline_us, condition) -> 0 on timeout, -1 on the
 *                                               condition
 *
 * "condition" is a function pointer, so that the caller can poll a
 * register without the timebase knowing anything about it.
 */
typedef int (*timer_cond_fn)(void *ctx);

static inline int timer_wait_until(uint64_t deadline, timer_cond_fn cond,
				   void *ctx)
{
	while (timer_ticks() < deadline) {
		if (cond(ctx))
			return -1;
	}
	return 0;
}
