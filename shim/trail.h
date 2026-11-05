/*
 * trail: a crash-proof breadcrumb log in IMEM.
 *
 * The console is invisible on the phone and timing is too noisy, so every
 * interesting event is appended here, as {tag, value} pairs, in a small ring
 * in IMEM (SRAM). IMEM already has to survive the final reset: G4_RESTART_REASON
 * lives in it and LK reads it after the reset. A recovery with root reads the
 * ring back with tools/read-trail-imem.py, no eMMC driver involved.
 *
 * Layout at TRAIL_BASE:
 *   +0  magic  TRAIL_MAGIC          (written LAST by trail_init: valid flag)
 *   +4  count  total events written (ring: slot = count % TRAIL_N)
 *   +8  n      TRAIL_N
 *   +12 reserved
 *   +16 {tag, value} x TRAIL_N
 * tag = (ms_since_boot & 0xffff) << 16 | code
 *
 * SAFETY: IMEM also holds cookies of the bootloader and of Linux. The
 * offset below is a guess until tools/imem-probe.sh says the area is free.
 * trail_init() refuses to touch the area unless it is all zero (or already
 * one of our own rings): on a busy area the trail simply stays off and
 * nothing is written. Override with -DG4_TRAIL_OFF=0x... -DG4_TRAIL_SIZE=0x...
 *
 * Everything is a no-op in the QEMU build (SHIM_HW_DIAG=0).
 */
#pragma once
#include <stdint.h>
#include "g4.h"

#ifndef G4_TRAIL_OFF
#define G4_TRAIL_OFF	0x700
#endif
#ifndef G4_TRAIL_SIZE
#define G4_TRAIL_SIZE	0x200
#endif
#ifndef TRAIL_BASE
#define TRAIL_BASE	(G4_IMEM_BASE + G4_TRAIL_OFF)
#endif
#define TRAIL_MAGIC	0x47345452u	/* "G4TR" */
#define TRAIL_HDR	16u
#define TRAIL_N		((G4_TRAIL_SIZE - TRAIL_HDR) / 8u)

#if SHIM_HW_DIAG

#ifndef TRAIL_MS
static inline uint32_t trail_ms(void)
{
	uint64_t v;

	__asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
	return (uint32_t)(v / (G4_TIMEBASE / 1000));
}
#define TRAIL_MS()	trail_ms()
#endif

#ifndef TRAIL_DSB
#define TRAIL_DSB()	__asm__ volatile("dsb sy" ::: "memory")
#endif

static inline void trail_init(void)
{
	volatile uint32_t *p = (volatile uint32_t *)TRAIL_BASE;
	int ok = (p[0] == TRAIL_MAGIC);

	if (!ok) {
		ok = 1;
		for (uint32_t i = 0; i < G4_TRAIL_SIZE / 4; i++)
			if (p[i]) {
				ok = 0;
				break;
			}
	}
	if (!ok)
		return;		/* busy area: stay off, write nothing */
	for (uint32_t i = 0; i < G4_TRAIL_SIZE / 4; i++)
		p[i] = 0;
	p[2] = TRAIL_N;
	TRAIL_DSB();
	p[0] = TRAIL_MAGIC;
	TRAIL_DSB();
}

static inline void trail(uint32_t code, uint32_t val)
{
	volatile uint32_t *p = (volatile uint32_t *)TRAIL_BASE;
	uint32_t n, slot;

	if (p[0] != TRAIL_MAGIC)
		return;
	n = p[1];
	slot = n % TRAIL_N;
	p[4 + slot * 2] = ((TRAIL_MS() & 0xffffu) << 16) | (code & 0xffffu);
	p[5 + slot * 2] = val;
	TRAIL_DSB();
	p[1] = n + 1;		/* publish after the entry is in place */
	TRAIL_DSB();
}

#else
static inline void trail_init(void) {}
static inline void trail(uint32_t code, uint32_t val) { (void)code; (void)val; }
#endif
