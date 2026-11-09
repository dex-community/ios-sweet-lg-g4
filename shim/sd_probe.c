/*
 * sd_probe.c -- the T2 read-only probe of the two SD controllers.
 *
 * The probe lives in its own translation unit so that the eMMC code
 * (shim/emmc.c and friends) stays exactly as it was. It needs the low level
 * accessors of emmc.c (rd32/wr32/...), the delay helper and the register
 * offsets, so those are re-declared here instead of shared through a header
 * that would have to be touched.
 *
 * What it does and why: see shim/sd_probe.inc.
 */

#include <stdint.h>
#include "g4.h"
#include "console.h"
#include "trail.h"
#include "mmio.h"

/* ------------------------------------------------------------------ */
/* The accessors, copied from emmc.c (the QEMU model is not used here: */
/* the probe only runs on the phone, where the windows are real).       */
/* ------------------------------------------------------------------ */

/* Guarded by the MMIO allowlist, like the accessors of emmc.c. */
static inline uint32_t rd32(uintptr_t a) { return mmio_rd32(a); }
static inline void wr32(uintptr_t a, uint32_t v) { mmio_wr32(a, v); }
static inline uint16_t rd16(uintptr_t a) { return mmio_rd16(a); }
static inline void wr16(uintptr_t a, uint16_t v) { mmio_wr16(a, v); }
static inline uint8_t rd8(uintptr_t a) { return mmio_rd8(a); }
static inline void wr8(uintptr_t a, uint8_t v) { mmio_wr8(a, v); }

static uint64_t sd_ticks(void)
{
	uint64_t v;

	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

static uint32_t sd_tick_hz(void)
{
	uint64_t v;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return (uint32_t)v;
}

static void emmc_delay_us(uint32_t us)
{
	uint64_t end = sd_ticks() + (uint64_t)sd_tick_hz() * us / 1000000u;

	while (sd_ticks() < end)
		;
}

/* The register offsets, from emmc.c (the names follow the CAF driver, so
 * they are ipotesi(nome Linux)). */
#define HC				G4_EMMC_BASE
#define CORE				G4_EMMC_CORE_BASE

#define HC_PRESENT	0x24
#define HC_CTRL1	0x28
#define HC_POWER_CTRL	0x29
#define HC_CLK_CTRL	0x2c
#define HC_SWRST	0x2f
#define HC_INT_STATUS	0x30
#define HC_CTRL2	0x3c
#define HC_CAPS1	0x40

#define CORE_POWER	0x00
#define CORE_MCI_VER	0x50
#define CORE_HC_MODE	0x78
#define HC_MODE_EN	(1u << 0)
#define FF_CLK_SW_RST_DIS (1u << 13)
/* The vendor spec register, reached through the HC window (the CAF driver
 * uses host->ioaddr, which is the HC base: sdhci-msm.c:96). */
#define HC_VENDOR_SPEC	0x10c

#define G4_GCC_BASE	0xfc400000UL

#include "sd_probe.inc"

/*
 * The probe is called from shim.c, a different translation unit: the
 * functions have to have external linkage.
 */
