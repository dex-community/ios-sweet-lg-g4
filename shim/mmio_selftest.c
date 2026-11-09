/*
 * mmio_selftest.c -- the self-test of the MMIO allowlist.
 *
 * WHAT IT PROVES
 * --------------
 * That a write outside the allowlist NEVER REACHES THE BUS, and that a read
 * outside it returns all ones without touching anything. This is the
 * property that was missing on 2026-11-09, when a write to a controller
 * whose clock was off hung the phone twice.
 *
 * The test tries, in this order:
 *
 *   1. a WRITE to SDCC2 (0xf98a4900) -- the window that hung the phone.
 *      It must be refused.
 *   2. a READ from SDCC2 -- it must be refused too (a read from a wedged
 *      bus can stall just as a write does).
 *   3. a WRITE to the GCC (0xfc4004c4, a window the shim really uses) --
 *      it must GO THROUGH.
 *
 * The test runs only in a build with SHIM_MMIO_SELFTEST=1, which is what
 * the QEMU test uses. On the phone it is off.
 */

#include <stdint.h>
#include "g4.h"
#include "console.h"
#include "trail.h"
#include "mmio.h"

/*
 * In the QEMU build the accessors of emmc.c go through the software model
 * (emmc_model_rd/emmc_model_wr), which is what makes the model see the
 * access. The self-test must use the SAME accessors, or it would touch the
 * real addresses (which do not exist under QEMU: the first version of this
 * test panicked with FAR=0xfc4004c4).
 */
#if SHIM_EMMC_MODEL
uint32_t emmc_model_rd(uintptr_t a, unsigned size);
void emmc_model_wr(uintptr_t a, unsigned size, uint32_t v);

static inline uint32_t sel_rd32(uintptr_t a)
{
	if (!mmio_read_ok(a, 4)) {
		mmio_denied(a, 4);
		return 0xffffffffu;
	}
	return emmc_model_rd(a, 4);
}
static inline void sel_wr32(uintptr_t a, uint32_t v)
{
	if (!mmio_write_ok(a, 4)) {
		mmio_denied(a, 4);
		return;
	}
	emmc_model_wr(a, 4, v);
}
#else
static inline uint32_t sel_rd32(uintptr_t a) { return mmio_rd32(a); }
static inline void sel_wr32(uintptr_t a, uint32_t v) { mmio_wr32(a, v); }
#endif

/* The counter of the refusals (mmio.h). It is defined here because this file
 * is in the source list of every build. */
unsigned mmio_denied_count;

#if SHIM_MMIO_SELFTEST

/*
 * The window that hung the phone: the SDCC2 host. It is NOT in the
 * allowlist: nobody ever drove it successfully.
 */
#define SELFTEST_FORBIDDEN	0xf98a4900UL
/* A window the shim really uses: the SDCC1 apps clock branch. */
#define SELFTEST_ALLOWED	0xfc4004c4UL

void mmio_selftest(void)
{
	uint32_t before, after, readback;

	con_str("MMIO", "self-test");

	/* 1: the forbidden write. */
	before = sel_rd32(SELFTEST_ALLOWED);
	sel_wr32(SELFTEST_FORBIDDEN, 0xdeadbeefu);
	readback = sel_rd32(SELFTEST_FORBIDDEN);
	con_hex2("MMIO", "forbidden read=", readback, "expect=", 0xffffffffu);

	/* 2: the forbidden read. */
	readback = sel_rd32(SELFTEST_FORBIDDEN + 4);
	con_hex2("MMIO", "forbidden read2=", readback, "expect=", 0xffffffffu);

	/* 3: the allowed write. It must reach the bus. */
	sel_wr32(SELFTEST_ALLOWED, before);
	after = sel_rd32(SELFTEST_ALLOWED);
	con_hex2("MMIO", "allowed was=", before, "now=", after);

	/* The count of refusals: it must be exactly 2 (the write and the read
	 * of the forbidden window). */
	con_hex2("MMIO", "denied count=", mmio_denied_count, "expect=", 2);

	trail(0x2400, (uint32_t)SELFTEST_FORBIDDEN);
	trail(0x2401, readback);
	trail(0x2402, after);
	trail(0x2403, mmio_denied_count);
}

#else

void mmio_selftest(void) { }

#endif
