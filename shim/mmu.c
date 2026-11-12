/*
 * Milestone 3: EL1 stage-1 MMU with the 16K granule XNU uses.
 *
 * T0SZ=28 gives a 36-bit VA whose walk starts at level 2: one 16K table of
 * 2048 x 32MB blocks covers 64GB, identity mapped (VA == PA).
 *
 * Qualcomm XPUs fault on speculative reads of protected carve-outs (TZ,
 * hyp, modem...), so nothing is mapped cacheable except our own image:
 *   - default: Device-nGnRnE, execute-never (same as MMU off)
 *   - the 32MB block(s) holding the image get an L3 table: image pages are
 *     Normal write-back, the rest of the block stays Device
 *   - the splash framebuffer block is Normal non-cacheable (write-combining)
 */
#include <stdint.h>
#include "mmu.h"

#define PG_SHIFT	14
#define PG_SIZE		(1UL << PG_SHIFT)		/* 16K */
#define BLK_SHIFT	25
#define BLK_SIZE	(1UL << BLK_SHIFT)		/* 32MB level-2 block */
#define NENT		2048

#define ATTR_DEV	0	/* MAIR attr0 = 0x00 Device-nGnRnE */
#define ATTR_WB		1	/* MAIR attr1 = 0xff Normal WB RW-alloc */
#define ATTR_NC		2	/* MAIR attr2 = 0x44 Normal non-cacheable */
#define MAIR_VAL	0x44ff00UL

#define D_BLOCK		1UL
#define D_TABLE		3UL
#define D_PAGE		3UL
#define D_ATTR(i)	((uint64_t)(i) << 2)
#define D_SH_OUTER	(2UL << 8)
#define D_SH_INNER	(3UL << 8)
#define D_AF		(1UL << 10)
#define D_PXN		(1UL << 53)
#define D_UXN		(1UL << 54)

#define DEV_FLAGS	(D_ATTR(ATTR_DEV) | D_AF | D_PXN | D_UXN)
#define WB_FLAGS	(D_ATTR(ATTR_WB) | D_SH_INNER | D_AF | D_UXN)
#define NC_FLAGS	(D_ATTR(ATTR_NC) | D_SH_OUTER | D_AF | D_PXN | D_UXN)

/*
 * TCR: T0SZ=28, IRGN0/ORGN0=WBWA, SH0=inner, TG0=16K, TG1=16K, IPS=36 bit.
 *
 * TG0 is bits [15:14] and the encoding is NOT the one this file used to
 * claim. Per the ARM ARM (DDI0487, TCR_EL1) and Apple's own
 * osfmk/arm64/proc_reg.h:
 *
 *   TG0: 0b00 = 4KB, 0b01 = 64KB, 0b10 = 16KB
 *   TG1: 0b00 = 4KB (IMPLEMENTATION DEFINED), 0b01 = 16KB, 0b10 = 4KB,
 *        0b11 = 64KB
 *
 * The value here is TG0 = 0b10 (16 KiB), which is what the tables below
 * are built for: PG_SHIFT 14 (16 KiB pages), BLK_SHIFT 25 (32 MiB L2
 * blocks) and 2048 entries per table. TG1 = 0b01 is 16 KiB as well.
 *
 * A previous revision of this file believed the encoding was
 * "0 = 4K, 1 = 16K, 2 = 64K" and "fixed" TG0 from 2 to 1. That belief was
 * inverted: the original 2 was already the correct 16 KiB, and the change
 * to 1 programmed a 64 KiB granule. With TG0 = 64 KiB the walk starts at
 * the wrong level: the level 2 block covers 128 MiB with its base address
 * at bit 29, while the tables store 32 MiB blocks starting at bit 25, so
 * every translation is wrong and the first store after mmu_enable()
 * faults. That is the bug this file had introduced in itself.
 */
#define TCR_VAL		(28UL | (1UL << 8) | (1UL << 10) | (3UL << 12) | (2UL << 14) | \
			 (28UL << 16) | (1UL << 23) | (1UL << 30) | (1UL << 32))

#define SCTLR_M		(1UL << 0)
#define SCTLR_A		(1UL << 1)
#define SCTLR_C		(1UL << 2)
#define SCTLR_I		(1UL << 12)
#define SCTLR_WXN	(1UL << 19)

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))

extern char _head[], _end[];

/* Filled by start.S with the runtime address of _start. The linker places
 * the image at 0, but the bootloader loads it wherever it likes. */
extern uint64_t shim_runtime_base;
uint64_t shim_runtime_base;

/* 1 L2 + up to 2 L3 tables. The pool has to start on a 16K boundary: it is
 * the base of the level 2 table, and the low bits of TTBR0 are RES0. */
static uint64_t pt_pool[4 * NENT] __attribute__((aligned(16384)));

/*
 * Clean and invalidate a range. The clean matters: the bootloader may have
 * loaded the image (or the .bss the page tables live in) with the cache on
 * and never written it back to RAM, so an invalidation without a clean
 * would throw the data away instead of moving it to RAM.
 */
static void dcache_civac_range(uintptr_t start, uintptr_t end)
{
	uint64_t line = 4UL << ((rsr("ctr_el0") >> 16) & 0xf);

	for (uintptr_t a = start & ~(line - 1); a < end; a += line)
		__asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static void zero_table(uint64_t *t)
{
	for (int i = 0; i < NENT; i++)
		t[i] = 0;
}

/*
 * ID_AA64MMFR0_EL1.TGran1, bits [23:20]: the granule sizes the core
 * implements for TTBR1. The encoding is a bitmask, not a code:
 * 0b0001 = 4 KiB, 0b0010 = 16 KiB, 0b0100 = 64 KiB, and the bits can be
 * combined (0b0110 = 4 KiB and 16 KiB, 0b0111 = all three).
 *
 * The check is "is 16 KiB implemented", i.e. bit 1 set: (v & 0x2) != 0.
 * The old test was "any of the bits set", which passes on a core that
 * only implements 4 KiB.
 */
int mmu_granule_ok(void)
{
	return ((rsr("id_aa64mmfr0_el1") >> 20) & 0x2) != 0;
}

int mmu_enable(struct mmu_info *info)
{
	uintptr_t base = (uintptr_t)shim_runtime_base;
	uintptr_t img_lo = (base + (uintptr_t)_head) & ~(PG_SIZE - 1);
	uintptr_t img_hi = (base + (uintptr_t)_end + PG_SIZE - 1) & ~(PG_SIZE - 1);
	uintptr_t pool = (uintptr_t)pt_pool;
	uint64_t *l2 = (uint64_t *)pool;
	int n_l3 = 0;

	/*
	 * These page tables are 16K-granule (the sweet configuration, and
	 * what XNU wants). ID_AA64MMFR0_EL1.TGran1 says whether the core
	 * supports it: the field is a bitmask where bit 1 is 16 KiB
	 * (0b0010 = 16 KiB only, 0b0110 = 4 KiB and 16 KiB, 0b0111 = all).
	 * The G4 has Cortex-A53/A57, so 16K is expected; check rather than
	 * assume, because a wrong granule walks into a fault storm.
	 */
	uint32_t tgran1 = (uint32_t)((rsr("id_aa64mmfr0_el1") >> 20) & 0xf);

	info->img_lo = img_lo;
	info->img_hi = img_hi;
	info->ttbr = pool;
	info->sctlr_before = rsr("sctlr_el1");
	info->sctlr_after = 0;
	info->n_l3 = 0;

	if ((tgran1 & 0x2) == 0) {
		info->n_l3 = -1;
		return -2;	/* no 16K granule */
	}
	if (((img_hi - 1) >> BLK_SHIFT) - (img_lo >> BLK_SHIFT) > 1)
		return -1;	/* only room for two L3 tables */

	/*
	 * The image lines may be stale in the cache from the bootloader, and
	 * the bootloader may have loaded the image with the cache on and
	 * never written it back: clean it to RAM and drop the stale lines, so
	 * that the code about to run and the .bss about to be used are the
	 * ones the MMU will find in RAM. This has to happen BEFORE the page
	 * tables are written, because the tables live in that .bss.
	 */
	dcache_civac_range(img_lo, img_hi);
	__asm__ volatile("ic iallu; dsb sy; isb" ::: "memory");

	/* L2: Device everywhere RAM or MMIO can be (first 8GB), rest unmapped */
	zero_table(l2);
	for (uint64_t i = 0; i < (8UL << 30) >> BLK_SHIFT; i++)
		l2[i] = (i << BLK_SHIFT) | DEV_FLAGS | D_BLOCK;
	l2[FB_PHYS >> BLK_SHIFT] = (FB_PHYS & ~(BLK_SIZE - 1)) | NC_FLAGS | D_BLOCK;

	/* L3 for each block that holds part of the image */
	for (uint64_t blk = img_lo >> BLK_SHIFT; blk <= (img_hi - 1) >> BLK_SHIFT; blk++) {
		uint64_t *l3 = (uint64_t *)(pool + (1 + n_l3++) * PG_SIZE);
		uint64_t base = blk << BLK_SHIFT;
		for (int i = 0; i < NENT; i++) {
			uint64_t pa = base + ((uint64_t)i << PG_SHIFT);
			int ours = pa >= img_lo && pa < img_hi;
			l3[i] = pa | (ours ? WB_FLAGS : DEV_FLAGS) | D_PAGE;
		}
		l2[blk] = (uintptr_t)l3 | D_TABLE;
	}
	info->n_l3 = n_l3;

	/*
	 * The tables are in the .bss, so they are in the cache: put them in
	 * RAM before the MMU starts walking them. An invalidation without a
	 * clean would leave RAM with the old contents and the first
	 * translation would fault.
	 */
	dcache_civac_range(pool, pool + (1 + n_l3) * PG_SIZE);

	wsr("mair_el1", MAIR_VAL);
	wsr("tcr_el1", TCR_VAL);
	wsr("ttbr0_el1", pool);
	__asm__ volatile("isb; tlbi vmalle1; dsb nsh; isb" ::: "memory");

	uint64_t sctlr = rsr("sctlr_el1");
	info->sctlr_before = sctlr;
	sctlr &= ~(SCTLR_A | SCTLR_WXN);
	sctlr |= SCTLR_M | SCTLR_C | SCTLR_I;
	wsr("sctlr_el1", sctlr);
	__asm__ volatile("isb" ::: "memory");
	info->sctlr_after = rsr("sctlr_el1");
	return 0;
}

/* Translate a VA with AT S1E1R; returns PAR_EL1. */
uint64_t mmu_probe(uintptr_t va)
{
	__asm__ volatile("at s1e1r, %0; isb" : : "r"(va) : "memory");
	return rsr("par_el1");
}
