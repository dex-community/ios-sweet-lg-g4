/*
 * mmio.h -- the MMIO allowlist and the guarded accessors.
 *
 * WHY THIS EXISTS
 * ---------------
 * On 2026-11-09 the shim hung the phone TWICE. Both times the cause was the
 * same shape: a write to a controller that nobody had ever initialized. The
 * controller's core clock was off, so the write never completed: the AXI bus
 * stalled with the transaction on it and the CPU stopped inside the MMIO
 * access. That is not a fault (no exception, no handler), the watchdog was
 * not armed (the successful runs have no "WDT" line in their log, so WDT0_EN
 * was 0), and the phone stayed on the LG logo for ever, unreachable from
 * USB. Recovering it needed the buttons (volume down + power) and a
 * re-flash of the boot partition.
 *
 * The lesson: on this SoC a write to a dead window is a brick, and a review
 * is not enough protection.
 *
 * THE RULE
 * --------
 * The shim may only touch:
 *
 *   - the DEVICE windows in the table below, for reads and writes. They are
 *     the windows this shim has already seen ALIVE on this phone:
 *     the eMMC host (the LK reads the boot image from the card with it, and
 *     the bring-up drives it at every run), the global clock controller (the
 *     LK enables the SDCC1 clocks in it), the watchdog (the register layout
 *     is from the CAF driver), the IMEM (the restart reason and the trail),
 *     the GIC, PSHOLD and the two SDCC2 windows;
 *
 *     On the SDCC2 windows the criterion is the same one, and it holds:
 *     at every boot with the microSD inserted, the Android kernel drives
 *     that controller (it identifies the card, reads its CSD and writes on
 *     it), so the window is alive. Its clock is the GCC branch that the
 *     shim turns on FIRST, before touching the controller: that order is
 *     exactly what the two hangs were missing, because a write to a
 *     controller whose clock is off never completes. The QEMU model of the
 *     controller REPRODUCES the stall (an access without the clock does
 *     not complete), so the order is verified offline by tools/qemu-sd.py;
 *   - the MEMORY windows, for reads and writes. They are plain RAM: the
 *     shim image, the device tree the bootloader hands over, the ramoops
 *     zone, the scratch copy of the log and the two RAM banks. Reading or
 *     writing RAM cannot wedge a bus;
 *   - the splash framebuffer, for READS only: the XPU refuses the writes
 *     (the console already avoids them with con_disable(), and the panic
 *     handler too).
 *
 * Everything else is REFUSED: the store never happens, the read returns all
 * ones, and the refusal is logged ("MMIO DENIED") so it shows up in the
 * pstore record.
 *
 * The addresses come from the phone itself, not from memory:
 *   - the shim image at 0x80000..0xd0000 and the device tree at 0x2000000
 *     (the log of the successful runs: "IMAGE lo=00080000 hi=000D0000",
 *     "DTB at=02000000 magic=D00DFEED"),
 *   - the splash window at 0x3400000 (the framebuffer the bootloader
 *     leaves), the ramoops at 0xff00000, the scratch log at 0x50000000
 *     (shim/pstore.c, shim/shim.c),
 *   - the two RAM banks 0x10000000..0x5fffffff and 0x80000000..0xdfffffff
 *     (the phone's /proc/iomem, see STATUS.md).
 */
#pragma once
#include <stdint.h>
#include "console.h"
#include "trail.h"

/*
 * The DEVICE windows: the only places where a write can wedge the bus.
 * Only numbers here: a table with pointers would need a relocation pass,
 * and the image runs wherever the bootloader puts it, with no relocation
 * pass at all.
 */
#define MMIO_DEV_N	10

static const uintptr_t mmio_dev_base[MMIO_DEV_N] = {
	0xf9824000UL,	/* 0: the eMMC core window ("core_mem", 0x800)  */
	0xf9824900UL,	/* 1: the eMMC host window ("hc_mem", 0x1a0)    */
	0xfc400000UL,	/* 2: the global clock controller (0x2000)       */
	0xf9017000UL,	/* 3: the watchdog (0x1000)                     */
	0xfe80f000UL,	/* 4: the IMEM (the restart reason, the trail)  */
	0xf9000000UL,	/* 5: the GIC (distributor + redistributor)     */
	0xfc4ab000UL,	/* 6: PSHOLD (the reset request)                */
	0xf98a4000UL,	/* 7: the SDCC2 core window (the microSD slot)  */
	0xf98a4900UL,	/* 8: the SDCC2 host window (the microSD slot)  */
	0xfd512000UL,	/* 9: the TLMM SDC pad configuration (0x100)   */
};

static const uint32_t mmio_dev_size[MMIO_DEV_N] = {
	0x00000800UL, 0x000001a0UL, 0x00002000UL, 0x00001000UL,
	0x00001000UL, 0x00003000UL, 0x00001000UL, 0x00000800UL,
	0x000001a0UL, 0x00000100UL,
};

/* The MEMORY windows: plain RAM, harmless to read and to write. */
#define MMIO_MEM_N	8

#if SHIM_EMMC_MODEL
/*
 * The QEMU build: the machine's RAM (the framebuffer and the model build's
 * ramoops live in it) and the device tree, which QEMU places at the top of
 * the memory.
 *
 * NOTE: the GCC window (0xfc400000) is in the DEVICE list, which is the
 * same in both builds, so it is allowed here too: the model serves it.
 */
static const uintptr_t mmio_mem_base[MMIO_MEM_N] = {
	0x40000000UL, 0x48000000UL, 0, 0, 0, 0, 0, 0,
};
static const uint32_t mmio_mem_size[MMIO_MEM_N] = {
	0x08000000UL, 0x00200000UL, 0, 0, 0, 0, 0, 0,
};
#else
/* The phone. */
static const uintptr_t mmio_mem_base[MMIO_MEM_N] = {
	0x00080000UL,	/* the shim image (0x80000..0xd0000)            */
	0x02000000UL,	/* the device tree the bootloader hands over    */
	0x0ff00000UL,	/* the ramoops zone (the pstore console)        */
	0x50000000UL,	/* the scratch copy of the log                  */
	0x10000000UL,	/* RAM bank 1 (0x10000000..0x5fffffff)          */
	0x80000000UL,	/* RAM bank 2 (0x80000000..0xdfffffff)          */
};
static const uint32_t mmio_mem_size[MMIO_MEM_N] = {
	0x00050000UL, 0x00200000UL, 0x00100000UL, 0x00100000UL,
	0x40000000UL, 0x60000000UL,
};
#endif

/* The READ-ONLY windows: the splash framebuffer.
 *
 * The splash framebuffer: the XPU refuses the writes, so they stay out of
 * the write list.
 *
 * The MDP window (0xfd900000, 0x90000) is NOT here on purpose. It was added
 * on 2026-11-10 for the D0 display probe and it caused a hang, twice: the
 * LK turns the display off before jumping to the kernel, so at the shim's
 * entry the MDSS/DSI clocks are OFF, and a read of a window whose clock is
 * off does not complete. The transaction stays on the bus, the CPU stops
 * inside the access, and the phone is gone.
 *
 * That is the same mechanism as the two hangs of 2026-11-09 (one of them a
 * READ of the TLMM). The allowlist says WHICH addresses may be touched; it
 * does NOT say that the window is powered. A new window needs the PROOF
 * that it is alive, and the MDP window has not got it.
 *
 * The display is measured from the recovery instead, where the real kernel
 * drives the panel:
 *   /sys/kernel/debug/mdp/ and /sys/kernel/debug/mdss_panel/dsi0/
 */
#define MMIO_RO_N	2

static const uintptr_t mmio_ro_base[MMIO_RO_N] = {
#if SHIM_EMMC_MODEL
	0x40000000UL,
#else
	0x03400000UL,	/* the splash window (0x3400000..0x4000000) */
#endif
	0,
};
static const uint32_t mmio_ro_size[MMIO_RO_N] = {
#if SHIM_EMMC_MODEL
	0x00020000UL,
#else
	0x00c00000UL,
#endif
	0,
};

static inline int mmio_in(uintptr_t a, unsigned size,
			  const uintptr_t *base, const uint32_t *sz,
			  unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		if (sz[i] && a >= base[i] && a + size <= base[i] + sz[i])
			return 1;
	}
	return 0;
}

/* May this address be read? */
static inline int mmio_read_ok(uintptr_t a, unsigned size)
{
	if (mmio_in(a, size, mmio_dev_base, mmio_dev_size, MMIO_DEV_N))
		return 1;
	if (mmio_in(a, size, mmio_mem_base, mmio_mem_size, MMIO_MEM_N))
		return 1;
	if (mmio_in(a, size, mmio_ro_base, mmio_ro_size, MMIO_RO_N))
		return 1;
	return 0;
}

/* May this address be written? */
static inline int mmio_write_ok(uintptr_t a, unsigned size)
{
	if (mmio_in(a, size, mmio_dev_base, mmio_dev_size, MMIO_DEV_N))
		return 1;
	if (mmio_in(a, size, mmio_mem_base, mmio_mem_size, MMIO_MEM_N))
		return 1;
	return 0;
}

/*
 * The refusal: it is logged and left in the trail, so a run that hits it is
 * visible in the pstore record instead of being silently different.
 *
 * IT MUST NOT USE THE CONSOLE. The console writes into the splash
 * framebuffer, which on the phone is inside the XPU-protected region: a
 * write there is a data abort, and the abort would happen INSIDE the
 * refusal path, turning a refused write into a fault. The first version of
 * this file did exactly that: the QEMU self-test panicked with
 * FAR=0xfc4004c4, the very address it was refusing.
 *
 * So the refusal only touches the trail (plain IMEM, always allowed) and a
 * counter, which the host reads back from the trail.
 */
extern unsigned mmio_denied_count;

static inline void mmio_denied(uintptr_t a, unsigned size)
{
	mmio_denied_count++;
	trail(0x2300, (uint32_t)a);
	trail(0x2301, size);
}

/* The guarded accessors. */
static inline uint32_t mmio_rd32(uintptr_t a)
{
	if (!mmio_read_ok(a, 4)) {
		mmio_denied(a, 4);
		return 0xffffffffu;
	}
	return *(volatile uint32_t *)a;
}

static inline uint64_t mmio_rd64(uintptr_t a)
{
	if (!mmio_read_ok(a, 8)) {
		mmio_denied(a, 8);
		return 0xffffffffffffffffull;
	}
	return *(volatile uint64_t *)a;
}

static inline uint16_t mmio_rd16(uintptr_t a)
{
	if (!mmio_read_ok(a, 2)) {
		mmio_denied(a, 2);
		return 0xffffu;
	}
	return *(volatile uint16_t *)a;
}

static inline uint8_t mmio_rd8(uintptr_t a)
{
	if (!mmio_read_ok(a, 1)) {
		mmio_denied(a, 1);
		return 0xffu;
	}
	return *(volatile uint8_t *)a;
}

static inline void mmio_wr32(uintptr_t a, uint32_t v)
{
	if (!mmio_write_ok(a, 4)) {
		mmio_denied(a, 4);
		return;
	}
	*(volatile uint32_t *)a = v;
}

static inline void mmio_wr16(uintptr_t a, uint16_t v)
{
	if (!mmio_write_ok(a, 2)) {
		mmio_denied(a, 2);
		return;
	}
	*(volatile uint16_t *)a = v;
}

static inline void mmio_wr8(uintptr_t a, uint8_t v)
{
	if (!mmio_write_ok(a, 1)) {
		mmio_denied(a, 1);
		return;
	}
	*(volatile uint8_t *)a = v;
}
