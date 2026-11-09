/*
 * wdt.h -- the watchdog guard.
 *
 * WHY
 * ---
 * On 2026-11-09 the shim hung the phone twice: a write to a controller whose
 * clock was off blocked the AXI bus, the CPU stopped inside the transaction
 * and NOTHING reset the phone: the watchdog was not armed (the log of the
 * successful runs has no "WDT" line, so WDT0_EN was 0 at the shim's entry),
 * and the fault handler never ran because a bus hang is not a fault.
 *
 * The guard closes that hole: arm the watchdog before the risky part, pet it
 * in the delay loops, disarm it before the final wait. If the CPU hangs, the
 * watchdog barks and then bites, and the bite is a hardware reset: the phone
 * comes back on its own instead of staying on the LG logo for ever.
 *
 * THE REGISTERS (not from memory: drivers/soc/qcom/watchdog_v2.c:40-46 of
 * the CAF tree, the same tree the phone's kernel is built from):
 *
 *   +0x04  WDT0_RST        write 1 = restart the counter (the "pet")
 *   +0x08  WDT0_EN         bit 0 = the watchdog is running
 *   +0x0c  WDT0_STS        the counter
 *   +0x10  WDT0_BARK_TIME  the bark timeout, in ticks of WDT_HZ
 *   +0x14  WDT0_BITE_TIME  the bite timeout, in ticks of WDT_HZ
 *
 * The bark/bite times are written by the driver as
 *
 *   timeout = (bark_time_ms * WDT_HZ) / 1000              (watchdog_v2.c:598)
 *   bite    = timeout + 3 * WDT_HZ                        (watchdog_v2.c:600)
 *
 * The phone's device tree asks for qcom,bark-time = 0x3e80 and
 * qcom,pet-time = 0x2710 (DTS:3726-3727): 16000 ms and 10000 ms.
 *
 * NOTE: the DTS also sets qcom,ipi-ping, so the secure side may pet the
 * watchdog by itself. If it does, the guard never barks: it is a net, not a
 * guarantee. The real guarantee is that the shim does not touch hardware it
 * has not already driven successfully (see tools/check-mmio-writes.py).
 */
#pragma once
#include <stdint.h>

/* The tick rate of the watchdog counter (Hz). */
#ifndef G4_WDT_HZ
#define G4_WDT_HZ		19000u
#endif

/* Arm the watchdog with the given bark time in milliseconds. Returns 0 on
 * success. */
int g4_wdt_arm(uint32_t bark_ms);

/* Stop the watchdog (WDT0_EN = 0). */
void g4_wdt_disarm(void);

/* Restart the counter (the "pet"). A no-op when the guard is off. */
void g4_wdt_pet(void);

/* Is the guard armed? */
int g4_wdt_is_armed(void);
