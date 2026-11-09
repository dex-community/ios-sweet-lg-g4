/*
 * sd_payload.c -- read the XNU payload from the microSD into RAM.
 *
 * The card must already be initialized (shim/sd.c does it, and it runs
 * before the loader). The payload is read block by block, its CRC is
 * checked, and the loader is told to use that buffer instead of the
 * initrd of the boot image (shim/loader.c).
 */
#include <stdint.h>
#include "g4.h"
#include "console.h"
#include "trail.h"
#include "crc32.h"
#include "sd.h"
#include "sd_payload.h"
#include "loader.h"

/*
 * Where the payload is put in RAM while it is checked.
 *
 * The RAM bank 1 starts at 0x10000000 (the phone's /proc/iomem, see
 * STATUS.md) and the XNU kernel goes to phys_base, which is in the bank
 * above 0x80000000: the two do not overlap, and the loader refuses a
 * payload that would overlap the kernel anyway.
 */
#if SHIM_EMMC_MODEL
#define SD_PAYLOAD_RAM		0x42000000UL
#else
#define SD_PAYLOAD_RAM		0x10000000UL
#endif
/* The biggest payload we accept (64 MiB): more than that would not fit
 * with any margin. */
#define SD_PAYLOAD_MAX		(64u << 20)

int sd_payload_error;
uint64_t sd_payload_addr;
uint32_t sd_payload_len;
uint32_t sd_payload_crc;
uint32_t sd_payload_crc_expected;

int sd_payload_load(void)
{
	struct sd_payload_hdr hdr;
	uint8_t *dst;
	uint32_t left, crc;
	unsigned blocks;

	sd_payload_error = 0;
	sd_payload_addr = 0;
	sd_payload_len = 0;

	/* The header block. */
	if (sd_read_block(SD_PAYLOAD_HDR_LBA, (uint8_t *)&hdr)) {
		sd_payload_error = -1;
		return sd_payload_error;
	}

	for (unsigned i = 0; i < 8; i++) {
		if (hdr.magic[i] != SD_PAYLOAD_MAGIC[i]) {
			sd_payload_error = -2;	/* no payload on the card */
			return sd_payload_error;
		}
	}
	if (hdr.version != SD_PAYLOAD_VERSION) {
		sd_payload_error = -3;
		return sd_payload_error;
	}
	if (hdr.len == 0 || hdr.len > SD_PAYLOAD_MAX) {
		sd_payload_error = -4;
		return sd_payload_error;
	}

	sd_payload_len = hdr.len;
	sd_payload_crc_expected = hdr.crc32;
	trail(0x2700, hdr.len);
	trail(0x2701, hdr.crc32);

	/* The payload itself, block by block. The last block is padded with
	 * zeros: the CRC is over the real length only. */
	dst = (uint8_t *)SD_PAYLOAD_RAM;
	left = hdr.len;
	blocks = 0;
	while (left) {
		uint8_t block[512];
		uint32_t n = left > 512 ? 512 : left;

		if (sd_read_block(SD_PAYLOAD_HDR_LBA + 1 + blocks, block)) {
			sd_payload_error = -5;
			return sd_payload_error;
		}
		for (uint32_t i = 0; i < n; i++)
			dst[blocks * 512 + i] = block[i];
		left -= n;
		blocks++;
	}

	/* The CRC of what we read, over the real length. */
	crc = crc32_le(0xffffffffu, dst, hdr.len) ^ 0xffffffffu;
	sd_payload_crc = crc;
	trail(0x2702, crc);

	if (crc != hdr.crc32) {
		sd_payload_error = -6;	/* the payload is corrupted */
		return sd_payload_error;
	}

	sd_payload_addr = SD_PAYLOAD_RAM;
	con_hex2("SDPAY", "len=", sd_payload_len, "crc=", crc);
	con_hex2("SDPAY", "at=", sd_payload_addr, "exp=", hdr.crc32);

	/* The loader uses this buffer instead of the initrd. */
	loader_set_payload(sd_payload_addr, sd_payload_len);
	return 0;
}
