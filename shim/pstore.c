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
#define PSTORE_NO_CONSOLE 1
#include <stdint.h>
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

/*
 * THE PMSG ZONE: THE COPY THAT ACTUALLY SURVIVES (added 2026-11-15).
 *
 * WHY THE CONSOLE ZONE IS NOT ENOUGH
 * ------------------------------------
 * The console zone is a ONE SHOT channel, and the proof came on this phone.
 * The shim wrote its record, the phone was rebooted, TWRP was booted, and
 * /sys/fs/pstore/console-ramoops-0 came back with the RECOVERY's own
 * console in it: the shim's record was gone, 116488 bytes of kernel log
 * instead. The kernel writes its console to the console zone from the
 * moment it boots, and the zone is a ring: what was there first is
 * overwritten.
 *
 * That is why the record of a run that HUNG used to be unreadable.
 *
 * WHY THE PMSG ZONE WORKS
 * -----------------------
 * The ramoops region is split in zones, and the kernel only ever writes to
 * the one it is told to:
 *
 *   - the CONSOLE zone gets the kernel console (ramoops_pstore_write_buf,
 *     type PSTORE_TYPE_CONSOLE) -- that is what overwrites our record;
 *   - the PMSG zone gets the userspace log (/dev/pmsg, type
 *     PSTORE_TYPE_PMSG). No recovery writes to /dev/pmsg, so nothing
 *     touches this zone.
 *
 * The layout was read off the phone, not guessed: /proc/iomem of the
 * recovery's kernel lists four persistent_ram regions,
 *
 *   0ff00000-0ff7ffff   the record zone  (512 KiB)
 *   0ff80000-0ff9ffff   the CONSOLE zone  (128 KiB)
 *   0ffa0000-0ffbffff   the PMSG zone    (128 KiB)
 *   0ffc0000-0ffdffff   one more         (128 KiB)
 *
 * and the console file it produced is 116488 bytes, which is a 0x20000 zone
 * with its 12 byte header and its Reed-Solomon parity taken out. So the
 * console zone the recovery reads is at 0x0ff80000, and the pmsg zone is at
 * 0x0ffa0000.
 *
 * (The first attempt used 0x0ff20000, which was the layout computed from
 * msm8992-lge.dtsi instead of the device tree the phone actually boots.
 * The phone's /proc/iomem is the truth.)
 *
 * So 0x0ffa0000 is the pmsg zone, and it is the address the shim writes as
 * the third copy.
 *
 * And it is not a private channel: the pstore core reads every zone at
 * mount and makes a file out of it (fs/pstore/platform.c,
 * pstore_get_records -> psi->read -> pstore_mkfile), one file per zone and
 * per type:
 *
 *   console-ramoops-0   the kernel console (it eats our record)
 *   pmsg-ramoops-0      THE PMSG ZONE: our record, intact
 *
 * The signature is the same as the console zone's: the driver asks for
 * sig = 0 for both, and persistent_ram_post_init compares it with
 * sig ^ PERSISTENT_RAM_SIG = 0x43474244, which is what this file writes.
 *
 * The zone is 128 KiB, with the same 12 byte header and the same 16 byte
 * Reed-Solomon parity, so the same writer and the same reader are used.
 */
#ifndef RAMOOPS_PMSG_BASE
#define RAMOOPS_PMSG_BASE	0x0ffa0000UL
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
 * How many times the write may be attempted.
 *
 * It is more than two now (2026-11-15): the log is flushed several times
 * during a run (the earliest possible one, the one before the display is
 * touched, the one after it), and each flush rewrites the zones with the
 * log as it is at that moment. The counter still exists for one reason:
 * a write that FAULTS must not be retried for ever, because the exception
 * handler calls reset(), and reset() writes the log again. A zone that
 * faults once will fault every time.
 *
 * So the limit is a budget for the whole run, not for one call: four
 * flushes are enough for every path, and the fifth is refused.
 */
#define PSTORE_MAX_ATTEMPTS	64

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
	if (pstore_attempts >= PSTORE_MAX_ATTEMPTS)
		return;
	pstore_attempts++;

	/* The recovery's console zone first: it is the one that is read
	 * first, and the recovery is what comes up after the shim. */
	pstore_write_zone(RAMOOPS_BASE, text, len);
#if RAMOOPS_BASE2
	if (RAMOOPS_BASE2 != RAMOOPS_BASE)
		pstore_write_zone(RAMOOPS_BASE2, text, len);
#endif
	/*
	 * And the pmsg zone, which is the copy that survives: no kernel
	 * writes its console there (the console goes to the console zone),
	 * so the record of this run is still there when the recovery is up.
	 * It comes back as /sys/fs/pstore/pmsg-ramoops-0.
	 */
	pstore_write_zone(RAMOOPS_PMSG_BASE, text, len);
}

/*
 * The FIRST write of the run, before the watchdog is armed and before
 * anything that can hang.
 *
 * It writes ONLY the pmsg zone, and only that one, for two reasons:
 *
 *   - the pmsg zone is the copy that survives the kernel's console, and
 *     it is the one the recovery reads back as pmsg-ramoops-0;
 *   - the pmsg zone is 128 KiB in both layouts, exactly like the console
 *     zone, so the parity this writer computes is the parity the kernel's
 *     decoder expects. The other two candidates do not have that
 *     property: in the recovery's layout 0x0ff00000 is a 512 KiB RECORD
 *     zone, and writing it with the console zone's geometry puts the
 *     parity in the wrong place.
 *
 * If this one hangs, the phone is stuck before the watchdog is armed and
 * nothing can be read: that is why it is a separate function, and why the
 * watchdog is armed before the rest of the run.
 */
void pstore_early_write(const char *text, unsigned len)
{
	if (!text || !len)
		return;
	pstore_write_zone(RAMOOPS_PMSG_BASE, text, len);
}

/*
 * Read back the log of the PREVIOUS run (added 2026-11-10).
 *
 * WHY
 * ---
 * The pstore is a one shot channel: the console zone of the ramoops holds
 * the LAST kernel that wrote there, and the recovery (or Android) writes
 * its own console over it as soon as it boots. So the log of the run that
 * just finished is gone by the time anybody could read it -- and it was
 * exactly the log of the run that BOOTLOOPED that was needed.
 *
 * The shim itself can read the zone: it is plain DRAM, it is in the MMIO
 * allowlist (the MEMORY windows), and the shim runs BEFORE anything else
 * touches it. So the shim reads the record of the previous run at startup
 * and prints it, before it writes its own log over it.
 *
 * That gives a chain of runs with no external tool at all: every run prints
 * the one before it.
 *
 * THE FORMAT (the one pstore_write_zone writes)
 *   "[G4SHIMPS1] len=<n> crc=<crc32>\n" <n bytes of text> "[G4SHIMPS1-END]\n"
 *
 * The CRC is checked: a record whose parity the kernel's ECC decoder has
 * mangled does not have to be believed.
 */
#define PSTORE_MARKER		"[G4SHIMPS1] len="
#define PSTORE_MARKER_LEN	(sizeof(PSTORE_MARKER) - 1)

static inline uint8_t pstore_ring(const uint8_t *data, uint32_t start, unsigned k)
{
	return data[(start + k) % RAMOOPS_DATA];
}

static int pstore_hexval(uint8_t c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/*
 * Read the record of the previous run out of ONE zone and print it.
 *
 * Returns 1 if a record was found (and printed), 0 if there was none.
 */
static int pstore_read_zone(uintptr_t base)
{
	uint8_t *data = (uint8_t *)RAMOOPS_DATA_BASE(base);
	uint32_t sig, start, size;
	unsigned k, best = 0, pos, i, len = 0;
	uint32_t crc = 0;
	int found = 0;
	char line[160];

	sig = p_rd32(base);
	start = p_rd32((uintptr_t)base + 4);
	size = p_rd32((uintptr_t)base + 8);
	if (sig != RAMOOPS_SIG)
		return 0;
	if (start > RAMOOPS_DATA || size > RAMOOPS_DATA)
		return 0;
	if (size < PSTORE_MARKER_LEN + 32)
		return 0;

	/*
	 * The ring: the oldest byte is at 'start', the newest at
	 * start+size-1 (that is how the kernel reads it back,
	 * fs/pstore/ram_core.c, persistent_ram_read). More than one record
	 * can be in there, one per run: the LAST header is the most recent.
	 */
	for (k = 0; k + PSTORE_MARKER_LEN <= size; k++) {
		int ok = 1;

		for (i = 0; i < PSTORE_MARKER_LEN; i++) {
			if (pstore_ring(data, start, k + i) !=
			    (uint8_t)PSTORE_MARKER[i]) {
				ok = 0;
				break;
			}
		}
		if (ok) {
			best = k;
			found = 1;
		}
	}
	if (!found)
		return 0;

	/* The header of the record: "len=<n> crc=<8 hex digits>\n". */
	pos = best + PSTORE_MARKER_LEN;
	while (pos < size && pstore_ring(data, start, pos) >= '0' &&
	       pstore_ring(data, start, pos) <= '9') {
		len = len * 10 + (pstore_ring(data, start, pos) - '0');
		pos++;
	}
	pos += 5;			/* " crc=" */
	for (i = 0; i < 8; i++) {
		int v = pstore_hexval(pstore_ring(data, start, pos + i));

		if (v < 0)
			return 0;
		crc = (crc << 4) | (uint32_t)v;
	}
	pos += 8 + 1;			/* the eight digits and the newline */

	if (len == 0 || pos + len > size)
		return 0;

#ifndef PSTORE_NO_CONSOLE
	con_str("PREV", "--- the log of the previous run ---");
#endif
	if (crc32_buf(data + ((start + pos) % RAMOOPS_DATA), len) != crc) {
#ifndef PSTORE_NO_CONSOLE
		con_str("PREV", "the record is damaged (the CRC does not match)");
#endif
		return 1;
	}

	/* Print it line by line: the console takes one line at a time. */
	{
		const uint8_t *text = data + ((start + pos) % RAMOOPS_DATA);
		unsigned off = 0;

		while (off < len) {
			unsigned n = 0;

			while (off + n < len && text[off + n] != '\n' &&
			       n < sizeof(line) - 1)
				n++;
			for (i = 0; i < n; i++)
				line[i] = (char)text[off + i];
			line[n] = 0;
#ifndef PSTORE_NO_CONSOLE
			con_str("PREV", line);
#endif
			off += n + 1;
		}
	}
#ifndef PSTORE_NO_CONSOLE
	con_str("PREV", "--- end of the previous run ---");
#endif
	return 1;
}

void pstore_read_previous(void)
{
	/*
	 * The pmsg zone comes first: it is the one that is NOT eaten by the
	 * kernel's console, so it holds the log of the run that really
	 * happened. The console zones are read as well, for the case where
	 * the pmsg zone was never written (an older shim, or a run that
	 * died before the first flush).
	 */
	if (pstore_read_zone(RAMOOPS_PMSG_BASE))
		return;
	if (pstore_read_zone(RAMOOPS_BASE))
		return;
#if RAMOOPS_BASE2
	if (RAMOOPS_BASE2 != RAMOOPS_BASE)
		pstore_read_zone(RAMOOPS_BASE2);
#endif
}
