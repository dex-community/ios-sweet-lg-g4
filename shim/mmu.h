#pragma once
#include <stdint.h>

#include "g4.h"

#define FB_PHYS	G4_FB_BASE

struct mmu_info {
	uint64_t img_lo, img_hi, ttbr;
	uint64_t sctlr_before, sctlr_after;
	int n_l3;
};

int mmu_enable(struct mmu_info *info);
/* 1 when the core supports the 16 KiB granule the page tables use. */
int mmu_granule_ok(void);
uint64_t mmu_probe(uintptr_t va);
