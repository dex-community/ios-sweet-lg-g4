/*
 * crc32.h -- the CRC32 of the payload stored on the microSD.
 *
 * The algorithm is the kernel's crc32_le (lib/crc32.c of the CAF tree the
 * phone's kernel is built from): the "bitwise little-endian AUTODIN II"
 * variant, polynomial 0xEDB88320, seed ~0. It is the same CRC as the
 * zlib one (zlib.crc32), which is what the host tool uses, so the two
 * sides agree.
 */
#pragma once
#include <stdint.h>

/* crc = crc32_le(0, data, len) */
uint32_t crc32_le(uint32_t crc, const uint8_t *p, uint32_t len);
