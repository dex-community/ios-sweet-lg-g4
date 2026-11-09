/*
 * pstore_console_write(): leave the shim's log in the ramoops console zone,
 * where the next Android boot reads it back.
 *
 * Why this exists
 * ---------------
 * The shim runs in place of the Android kernel, so it has no filesystem and
 * no way to hand a log over to the host. The two channels it had were the
 * eMMC card (mute: that is the whole problem) and a scratch address in RAM
 * (0x50000000), which survives the reset but can only be read with
 * /dev/mem, and on this phone /dev/mem answers ENXIO.
 *
 * The ramoops zone is the one piece of DRAM that survives the reset AND
 * that the host can read: the Android kernel's pstore driver turns it into
 * /sys/fs/pstore/console-ramoops-0.
 *
 * The layout, from the phone's own device tree
 * (msm8992-lg-h815-boot.dts:13066)
 *
 *   android,ramoops-buffer-start = <0xff00000>
 *   android,ramoops-buffer-size   = <0x100000>
 *   android,ramoops-console-size  = <0x20000>
 *   android,ramoops-pmsg-size     = <0x20000>
 *   android,ramoops-record-size   = <0xffffa>
 *   android,ramoops-ecc-size      = <0x10>
 *
 * and from the driver (fs/pstore/ram.c: ramoops_init):
 *
 *   - the record (crash dump) zones come first: max_dump_cnt =
 *     (size - console - pmsg) / record_size = 0xc0000 / 0xffffa = 12 of
 *     them, one after the other;
 *   - then the console zone, 0x20000 bytes;
 *   - then the pmsg zone, 0x20000 bytes.
 *
 * so the console zone starts at 0xff00000 + 12 * 0xffffa = 0x10affb8.
 *
 * Inside a zone (fs/pstore/ram_core.c):
 *
 *   struct persistent_ram_buffer { u32 sig; u32 start; u32 size; u8 data[]; }
 *
 * with sig = PERSISTENT_RAM_SIG = 0x43474244 ("DBGC"). start and size
 * describe a ring buffer: the data is written at start and start moves on,
 * size is how much of the ring holds data. The kernel reads the ring back
 * from start, wrapping at the end (persistent_ram_save_old).
 *
 * The zone is protected by a Reed-Solomon code: every 128 byte block of
 * the data has 16 parity bytes (persistent_ram_update_ecc) and the 12 byte
 * header has 16 more (persistent_ram_update_header_ecc). If the parity is
 * not written, the decoder sees a wrong syndrome, believes the block is
 * damaged and "corrects" it, which puts the OLD bytes back. That is why
 * the parity is computed here as well (shim/rs8.c).
 *
 * The block size is 128 (the default: the device tree does not set
 * android,ramoops-ecc-block-size) and the parity area takes
 * ((131060 - 16) + 143) / 144 = 911 blocks, so the data area is
 * 131060 - 912 * 16 = 116468 bytes. That number is confirmed by the file
 * the phone itself produced: /sys/fs/pstore/console-ramoops-0 is 116488
 * bytes = 116468 of data plus the 20 byte ECC notice.
 *
 * What the host gets
 * ------------------
 * The record is plain text, the same format the kernel's console uses:
 *
 *   [G4SHIMPS1] len=<n> crc=<crc32>\n
 *   <the log>
 *   [G4SHIMPS1-END]\n
 *
 * tools/read-pstore.py finds the last record, checks the CRC and prints the
 * log. The CRC is what tells a corrupted record (a parity block that the
 * decoder mangled) from a good one.
 */
#include <stdint.h>

#ifndef PSTORE_NO_CONSOLE
#include "console.h"
#endif
#include "pstore.h"
#include "rs8.h"

#ifndef PSTORE_HAVE_MEMCPY
void *memcpy(void *dst, const void *src, unsigned long n);
#endif

/*
 * Where the console zone can be.
 *
 * The two kernels that boot after the shim do not agree on the layout of
 * the region, and the shim cannot know which one will boot:
 *
 *   - the phone's own device tree (the one Android boots with) asks for a
 *     record size of 0xffffa, which does not fit in the space left for the
 *     record zones: there are none, and the console zone starts at the
 *     beginning of the region, 0xff00000;
 *   - the recovery's kernel says otherwise: /proc/iomem lists three
 *     persistent_ram regions, 0xff00000-0xff7ffff (512 KiB of record
 *     zones), 0xff80000-0xff9ffff (the console zone) and
 *     0xffa0000-0xffbffff (the pmsg zone).
 *
 * The shim writes the record to both, the recovery's first: the recovery
 * is the first kernel that boots after the shim, and it reads the zone it
 * knows. The copy at 0xff00000 is the one Android would read, but the
 * recovery treats that address as a record zone and empties it, so it only
 * survives if the recovery is never booted.
 */
#ifndef RAMOOPS_BASE
#define RAMOOPS_BASE		0x0ff80000UL
#endif
#ifndef RAMOOPS_BASE2
#define RAMOOPS_BASE2		0x0ff00000UL
#endif
#define RAMOOPS_SIZE		0x00100000UL
#define RAMOOPS_RECORD		0x000ffffaUL
#define RAMOOPS_CONSOLE		0x00020000UL
#define RAMOOPS_PMSG		0x00020000UL
#define RAMOOPS_ECC		16		/* bytes per block */
#define RAMOOPS_BLK		128		/* bytes per block */
#define RAMOOPS_SIG		0x43474244UL	/* PERSISTENT_RAM_SIG */

/* The record zones come first, then the console zone, then the pmsg zone.
 *
 * There are no record zones here: the device tree asks for a record size of
 * 0xffffa (1048570 bytes) and the space left for them is only
 * 0x100000 - 0x20000 - 0x20000 = 0xc0000 (786432), so
 * max_dump_cnt = 0xc0000 / 0xffffa = 0. The phone's kernel carries on
 * with that ("ramoops: attached 0x100000@0xff00000", "console [pstore0]
 * enabled" in its dmesg) and puts the console zone at the start of the
 * region. The console file it produced is 116488 bytes, which is the
 * 116468 bytes of usable data of a 0x20000 zone plus the 20 byte notice
 * about the ECC: the same layout as below.
 */
#define RAMOOPS_DUMP		(RAMOOPS_SIZE - RAMOOPS_CONSOLE - RAMOOPS_PMSG)
#define RAMOOPS_NREC		(RAMOOPS_DUMP / RAMOOPS_RECORD)

/* The header is 12 bytes; the parity area is at the end of the zone. */
#define RAMOOPS_HDRLEN		12
#define RAMOOPS_BUF0		(RAMOOPS_CONSOLE - RAMOOPS_HDRLEN)
#define RAMOOPS_ECC_BLOCKS	(((RAMOOPS_BUF0 - RAMOOPS_ECC) + \
				  (RAMOOPS_BLK + RAMOOPS_ECC) - 1) / \
				 (RAMOOPS_BLK + RAMOOPS_ECC))
#define RAMOOPS_ECC_TOTAL	((RAMOOPS_ECC_BLOCKS + 1) * RAMOOPS_ECC)
#define RAMOOPS_DATA		(RAMOOPS_BUF0 - RAMOOPS_ECC_TOTAL)

#define RAMOOPS_DATA_BASE(h)	((h) + RAMOOPS_HDRLEN)
#define RAMOOPS_PAR_BASE(h)	(RAMOOPS_DATA_BASE(h) + RAMOOPS_DATA)
#define RAMOOPS_PAR_HDR(h)	(RAMOOPS_PAR_BASE(h) + RAMOOPS_ECC_BLOCKS * RAMOOPS_ECC)

/*
 * How many times the write may be attempted. Two: once when the eMMC
 * bring-up is over (the MMU is still off there, so the write goes straight
 * to the physical address) and once in reset(), with the MMU on. A third
 * attempt is never made: if the second one faults, the exception handler
 * calls reset() again and the phone must still be able to reset.
 */
static unsigned pstore_attempts;

static inline uint32_t p_rd32(uintptr_t a)
{
	return *(volatile uint32_t *)a;
}

static inline void p_wr32(uintptr_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
}

/* CRC-32 (IEEE 802.3, reflected, 0xedb88320), the one zlib uses. */
static uint32_t crc32_buf(const uint8_t *p, unsigned n)
{
	uint32_t crc = 0xffffffffu;

	for (unsigned i = 0; i < n; i++) {
		crc ^= p[i];
		for (int b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (0xedb88320u & (-(int32_t)(crc & 1)));
	}
	return ~crc;
}

/* The length in decimal: it is a number of bytes, and the host reads it
 * as a decimal number. */
static unsigned dec_str(char *out, uint32_t v)
{
	char tmp[12];
	unsigned n = 0, i;

	do {
		tmp[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	for (i = 0; i < n; i++)
		out[i] = tmp[n - 1 - i];
	out[n] = 0;
	return n;
}

static void hex8(char *out, uint32_t v)
{
	static const char h[] = "0123456789abcdef";

	for (int i = 0; i < 8; i++)
		out[i] = h[(v >> ((7 - i) * 4)) & 0xf];
	out[8] = 0;
}

/* Copy a piece of text into the ring and refresh the parity of the blocks
 * it touched (persistent_ram_update_ecc). The ring wraps: a piece that does
 * not fit at the end continues at the beginning. */
static void pstore_chunk(uint8_t *data, uint8_t *par, unsigned off,
			  const char *src, unsigned n)
{
	if (off + n > RAMOOPS_DATA) {
		unsigned first = RAMOOPS_DATA - off;

		pstore_chunk(data, par, off, src, first);
		pstore_chunk(data, par, 0, src + first, n - first);
		return;
	}
	memcpy(data + off, src, n);
	for (unsigned b = off / RAMOOPS_BLK; b <= (off + n - 1) / RAMOOPS_BLK; b++) {
		unsigned blen = RAMOOPS_DATA - b * RAMOOPS_BLK;

		if (blen > RAMOOPS_BLK)
			blen = RAMOOPS_BLK;
		rs8_encode(data + b * RAMOOPS_BLK, (int)blen, par + b * RAMOOPS_ECC);
	}
}

static void pstore_write_zone(uintptr_t base, const char *text, unsigned len)
{
	uint8_t *hdr = (uint8_t *)base;
	uint8_t *data = (uint8_t *)RAMOOPS_DATA_BASE(base);
	uint8_t *par = (uint8_t *)RAMOOPS_PAR_BASE(base);
	uint8_t *par_hdr = (uint8_t *)RAMOOPS_PAR_HDR(base);
	char head[64];
	uint32_t sig, start, size;
	unsigned hlen;

	if (len > RAMOOPS_DATA - 128)
		len = RAMOOPS_DATA - 128;

	/* The record header: "[G4SHIMPS1] len=<n> crc=<crc32>\n" */
	hlen = 0;
	{
		const char *m = "[G4SHIMPS1] len=";
		char num[24];

		while (*m)
			head[hlen++] = *m++;
		dec_str(num, (uint32_t)len);
		for (unsigned i = 0; num[i]; i++)
			head[hlen++] = num[i];
		head[hlen++] = ' ';
		{
			const char *c = "crc=";

			while (*c)
				head[hlen++] = *c++;
		}
		hex8(num, crc32_buf((const uint8_t *)text, len));
		for (int i = 0; i < 8; i++)
			head[hlen++] = num[i];
		head[hlen++] = '\n';
	}

	sig = p_rd32((uintptr_t)hdr);
	start = p_rd32((uintptr_t)hdr + 4);
	size = p_rd32((uintptr_t)hdr + 8);
	if (sig != RAMOOPS_SIG) {
		/* The zone was never used: start from an empty ring. */
		start = 0;
		size = 0;
	}
	if (start > RAMOOPS_DATA)
		start = 0;
	if (size > RAMOOPS_DATA)
		size = RAMOOPS_DATA;
#ifndef PSTORE_NO_CONSOLE
	con_hex2("PSTORE", "hdr=", (uint32_t)(uintptr_t)hdr, "sig=", sig);
	con_hex2("PSTORE", "start=", start, "size=", size);
#endif

	/* The header of the record, then the text, then the end marker. */
	pstore_chunk(data, par, start, head, hlen);
	{
		unsigned off = (start + hlen) % RAMOOPS_DATA;

		pstore_chunk(data, par, off, text, len);
		off = (off + len) % RAMOOPS_DATA;
		pstore_chunk(data, par, off, "[G4SHIMPS1-END]\n", 16);
		start = (off + 16) % RAMOOPS_DATA;
	}

	/* The ring moved on by what was written. */
	size += hlen + len + 16;
	if (size > RAMOOPS_DATA)
		size = RAMOOPS_DATA;
	p_wr32((uintptr_t)hdr, RAMOOPS_SIG);
	p_wr32((uintptr_t)hdr + 4, start);
	p_wr32((uintptr_t)hdr + 8, size);

	/* And the parity of the header itself. */
	rs8_encode(hdr, RAMOOPS_HDRLEN, par_hdr);
#ifndef PSTORE_NO_CONSOLE
	con_hex2("PSTORE", "wrote=", hlen + len + 16, "start=", start);
	con_hex2("PSTORE", "check=", p_rd32((uintptr_t)hdr + 8), "sig=",
		 p_rd32((uintptr_t)hdr));
#endif

	__asm__ volatile("dsb sy" ::: "memory");
}

void pstore_console_write(const char *text, unsigned len)
{
	if (!text || !len)
		return;
	if (pstore_attempts >= 2)
		return;
	pstore_attempts++;

	/* The recovery's console zone first: it is the one that is read
	 * first, and the recovery is what comes up after the shim. */
	pstore_write_zone(RAMOOPS_BASE, text, len);
#if RAMOOPS_BASE2
	if (RAMOOPS_BASE2 != RAMOOPS_BASE)
		pstore_write_zone(RAMOOPS_BASE2, text, len);
#endif
}
