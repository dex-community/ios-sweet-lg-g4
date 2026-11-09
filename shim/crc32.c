/*
 * crc32.c -- the CRC32 of the payload stored on the microSD.
 *
 * It is the kernel's crc32_le, copied from lib/crc32.c of the CAF tree
 * (the bitwise variant, CRC_LE_BITS == 1, polynomial CRCPOLY_LE =
 * 0xEDB88320, seed ~0). It is the same CRC as zlib's, so the host tool
 * (tools/sd-write.sh, python zlib.crc32) and the shim agree; the test
 * tools/crc32-test.sh checks it against zlib byte for byte.
 */
#include <stdint.h>
#include "crc32.h"

/* CRCPOLY_LE, from lib/crc32.c of the CAF tree. */
#define CRCPOLY_LE		0xedb88320u

uint32_t crc32_le(uint32_t crc, const uint8_t *p, uint32_t len)
{
	while (len--) {
		int i;

		crc ^= *p++;
		for (i = 0; i < 8; i++)
			crc = (crc >> 1) ^ ((crc & 1) ? CRCPOLY_LE : 0);
	}
	return crc;
}
