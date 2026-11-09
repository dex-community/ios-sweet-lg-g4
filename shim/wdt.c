/*
 * wdt.c -- the watchdog guard (see wdt.h for the why and the sources).
 *
 * On the phone the watchdog is a real device (qcom,msm-watchdog at
 * 0xf9017000, DTS:3721-3729). Under QEMU there is no watchdog at all, so
 * everything here is a no-op there: the QEMU build must not touch an address
 * that does not exist.
 */

#include <stdint.h>
#include "g4.h"
#include "wdt.h"
#include "console.h"
#include "trail.h"
#include "mmio.h"

/* The register offsets, from drivers/soc/qcom/watchdog_v2.c:40-46. */
#define WDT0_RST	0x04
#define WDT0_EN		0x08
#define WDT0_STS	0x0c
#define WDT0_BARK_TIME	0x10
#define WDT0_BITE_TIME	0x14

static int wdt_armed;

#if SHIM_HW_DIAG && !SHIM_EMMC_MODEL

int g4_wdt_arm(uint32_t bark_ms)
{
	uint32_t bark, bite;

	if (bark_ms < 1000)
		bark_ms = 1000;

	bark = (bark_ms * G4_WDT_HZ) / 1000u;
	bite = bark + 3u * G4_WDT_HZ;

	mmio_wr32(G4_WDT_BASE + WDT0_BARK_TIME, bark);
	mmio_wr32(G4_WDT_BASE + WDT0_BITE_TIME, bite);
	mmio_wr32(G4_WDT_BASE + WDT0_EN, 1);
	__asm__ volatile("dsb sy" ::: "memory");

	wdt_armed = 1;
	trail(0x2200, bark_ms);
	con_hex2("WDT", "armed bark_ms=", bark_ms, "bark=", bark);
	return 0;
}

void g4_wdt_disarm(void)
{
	if (!wdt_armed)
		return;
	mmio_wr32(G4_WDT_BASE + WDT0_EN, 0);
	__asm__ volatile("dsb sy" ::: "memory");
	wdt_armed = 0;
	trail(0x2201, 0);
	con_str("WDT", "disarmed");
}

void g4_wdt_pet(void)
{
	if (!wdt_armed)
		return;
	mmio_wr32(G4_WDT_BASE + WDT0_RST, 1);
}

int g4_wdt_is_armed(void)
{
	return wdt_armed;
}

#else	/* QEMU: no watchdog there */

int g4_wdt_arm(uint32_t bark_ms)
{
	(void)bark_ms;
	wdt_armed = 0;
	return 0;
}

void g4_wdt_disarm(void) { }
void g4_wdt_pet(void) { }

int g4_wdt_is_armed(void)
{
	return 0;
}

#endif
