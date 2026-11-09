#pragma once
#include <stdint.h>

/* Boot the SWEETXNU payload found in the ABL ramdisk; returns only on error. */
const char *loader_boot(uintptr_t dtb);

/*
 * Use this buffer as the payload instead of the initrd of the boot image.
 * It is what the microSD path does (shim/sd_payload.c): the payload is read
 * from the card into RAM and checked, and the loader then reads the
 * segments from there.
 */
void loader_set_payload(uint64_t addr, uint64_t len);
