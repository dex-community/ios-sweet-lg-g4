/*
 * sd_payload.h -- the payload (the XNU kernel + the ADT) on the microSD.
 *
 * WHY
 * ---
 * The eMMC is mute (the card does not answer: CMD1 in timeout, see
 * NOTES.md). The microSD, instead, works under Android and is driven from
 * the shim by shim/sd.c. So the payload that the shim has to jump to can
 * live on the SD instead of inside the boot image.
 *
 * THE LAYOUT (raw, on blocks of 512 bytes)
 * ----------------------------------------
 *
 *   block 0        the boot area of the card (a FAT partition: do not
 *                  touch it, the host tools only write from block 2048 on)
 *   block 2048     the header:
 *                    0   char magic[8] = "G4PAYLD1"
 *                    8   uint32 version = 1
 *                   12   uint32 len      (the payload, in bytes)
 *                   16   uint32 crc32    (zlib's CRC32 of the payload)
 *                   20   uint32 flags    (0)
 *                   24   the rest is zero
 *   block 2049...  the payload, len bytes, the rest of the block zero
 *   block 8192...  the log area (the same format of the eMMC log):
 *                    0   char magic[8] = "G4SHIMLG"
 *                    8   uint32 len
 *                   12   the text
 *
 * The blocks 2048 and 8192 are fixed, so the host and the shim agree
 * without having to look anything up.
 *
 * THE CRC
 * -------
 * The shim verifies the payload before using it: a corrupted payload would
 * jump to a random address. The CRC is the kernel's crc32_le (the same as
 * zlib's): tools/crc32-test.sh checks that the two agree.
 */
#pragma once
#include <stdint.h>

/* The blocks of the layout (512 bytes each). */
#define SD_PAYLOAD_HDR_LBA	2048
#define SD_PAYLOAD_LOG_LBA	8192

#define SD_PAYLOAD_MAGIC	"G4PAYLD1"
#define SD_PAYLOAD_LOG_MAGIC	"G4SHIMLG"
#define SD_PAYLOAD_VERSION	1

/* The header, as it is written on the card. */
struct sd_payload_hdr {
	char magic[8];
	uint32_t version;
	uint32_t len;
	uint32_t crc32;
	uint32_t flags;
	uint8_t pad[488];
};

/*
 * Read the payload from the card into RAM and check it.
 * Returns 0 if the payload is there and its CRC is right; a negative
 * number otherwise (the reason is in sd_payload_error).
 */
int sd_payload_load(void);

/* Why the last load failed. */
extern int sd_payload_error;

/* Where the payload ended up, and how big it is. */
extern uint64_t sd_payload_addr;
extern uint32_t sd_payload_len;

/* The CRC that was computed, and the one the header said. */
extern uint32_t sd_payload_crc;
extern uint32_t sd_payload_crc_expected;
