/*
 * Minimal text console on the bootloader's splash framebuffer. With the MMU
 * off every access is Device memory, so stay aligned (-mstrict-align).
 *
 * The pixel format is selectable at build time (FB_BPP, see console.h); the
 * G4 panel is RGB888.
 */
#include "console.h"
#include "font.h"

#ifndef CON_SCALE
#define CON_SCALE	3
#endif
#define SCALE		CON_SCALE
#define CH_W		(8 * SCALE)
#define CH_H		(16 * SCALE)
#define MARGIN		24
#define LABEL_COLS	8

volatile uint8_t *con_fb = (volatile uint8_t *)FB_BASE;
char con_log[CON_LOG_SIZE];
int con_log_len;
uint32_t con_fb_w = FB_W, con_fb_h = FB_H, con_fb_stride = FB_STRIDE;
static uint32_t con_bg;
static int row;
static int con_fb_on = 1;

void con_disable(void)
{
	con_fb_on = 0;
}

void con_enable(void)
{
	con_fb_on = 1;
}

void con_set_fb(uintptr_t base, uint32_t w, uint32_t h, uint32_t stride)
{
	con_fb = (volatile uint8_t *)base;
	con_fb_w = w;
	con_fb_h = h;
	con_fb_stride = stride;
}

/* Store one pixel in the configured format. Colors are 0xAARRGGBB. */
static void put_px(int x, int y, uint32_t c)
{
	volatile uint8_t *p;

	if (!con_fb_on)
		return;
	p = con_fb + (uintptr_t)y * con_fb_stride + (uintptr_t)x * FB_BYTES_PER_PIXEL;

#if FB_BPP == 32
	*(volatile uint32_t *)p = c;
#elif FB_BPP == 24
	p[0] = (uint8_t)(c >> 16);	/* R */
	p[1] = (uint8_t)(c >> 8);	/* G */
	p[2] = (uint8_t)c;		/* B */
#else
	/* RGB565 */
	*(volatile uint16_t *)p = (uint16_t)(((c >> 8) & 0xf800) |
					     ((c >> 5) & 0x07e0) |
					     ((c >> 3) & 0x001f));
#endif
}

void con_clear(uint32_t bg)
{
	con_bg = bg;
	row = 0;
	if (!con_fb_on)
		return;
	for (int y = 0; y < (int)con_fb_h; y++)
		for (int x = 0; x < (int)con_fb_w; x++)
			put_px(x, y, bg);
}

static void put_char(int col, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	int x0 = MARGIN + col * CH_W, y0 = MARGIN + row * CH_H;
	if (x0 + CH_W > (int)con_fb_w || y0 + CH_H > (int)con_fb_h)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? fg : con_bg;
			for (int sy = 0; sy < SCALE; sy++)
				for (int sx = 0; sx < SCALE; sx++)
					put_px(x0 + gx * SCALE + sx,
					       y0 + gy * SCALE + sy, c);
		}
}

static int put_str(int col, const char *s, uint32_t fg)
{
	while (*s)
		put_char(col++, *s++, fg);
	return col;
}

static void fmt_hex(char *buf, uint64_t v, int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	for (int i = 0; i < digits; i++)
		buf[i] = hex[(v >> (4 * (digits - 1 - i))) & 0xf];
	buf[digits] = 0;
}

void con_skip(void)
{
	row++;
}

/*
 * Draw the whole ASCII log again from the start.
 *
 * The console paints as it goes, but on the phone it starts switched off
 * (the splash window is inside the XPU protected secure region), so the
 * log has to be replayed once, at the end, into a framebuffer that turned
 * out to be writable after all.
 */
void con_repaint(void)
{
	int n = con_log_len;
	int line = 0;
	int start = 0;

	if (!con_fb_on)
		return;
	con_clear(BLACK);
	for (int i = 0; i <= n; i++) {
		char c = (i < n) ? con_log[i] : '\n';

		if (c != '\n' && c != 0)
			continue;
		row = line;
		for (int j = start; j < i; j++)
			put_char(0, con_log[j], WHITE);
		line++;
		start = i + 1;
	}
}

/* Append one line to the ASCII log. */
static void log_line(const char *label, const char *s)
{
	int n = con_log_len;

	if (n + 2 >= CON_LOG_SIZE)
		return;
	for (const char *p = label; *p && n < CON_LOG_SIZE - 2; p++)
		con_log[n++] = *p;
	for (const char *p = s; *p && n < CON_LOG_SIZE - 2; p++)
		con_log[n++] = *p;
	con_log[n++] = '\n';
	con_log[n++] = 0;
	con_log_len = n;
}

void con_str(const char *label, const char *s)
{
	int col = put_str(0, label, YELLOW);

	/* Do not overwrite a label that is longer than the label column. */
	if (col < LABEL_COLS)
		col = LABEL_COLS;
	put_str(col, s, WHITE);
	row++;
	log_line(label, s);
}

void con_hex(const char *label, uint64_t v)
{
	char buf[17];
	fmt_hex(buf, v, 16);
	con_str(label, buf);
}

void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	char buf[9];
	int col = put_str(0, label, YELLOW);
	col = col < LABEL_COLS ? LABEL_COLS : col + 1;
	col = put_str(col, a, YELLOW);
	fmt_hex(buf, va, 8);
	col = put_str(col, buf, WHITE);
	if (b) {
		col = put_str(col + 1, b, YELLOW);
		fmt_hex(buf, vb, 8);
		put_str(col, buf, WHITE);
	}
	row++;
}

/* 16 bytes per line, offset + hex + ascii. Used to look at what the
 * bootloader left behind (framebuffer contents, MMIO registers). */
void con_dump(const char *label, uintptr_t addr, int bytes)
{
	const volatile uint8_t *p = (const volatile uint8_t *)addr;

	for (int off = 0; off < bytes; off += 16) {
		char line[64];
		int n = 0;
		for (int i = 0; i < 16; i++) {
			fmt_hex(line + n, p[off + i], 2);
			n += 2;
			line[n++] = ' ';
		}
		for (int i = 0; i < 16; i++) {
			uint8_t b = p[off + i];
			line[n++] = (b >= 0x20 && b < 0x7f) ? (char)b : '.';
			if (i == 7)
				line[n++] = ' ';
		}
		line[n] = 0;
		con_str(label, line);
	}
}
