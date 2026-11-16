#pragma once
#include <stdint.h>

#include "g4.h"

/*
 * The LG G4 panel is 1440x2560 and the bootloader leaves a 12 MiB
 * continuous-splash window at 0x03400000 (cont_splash_mem). 1440*2560*3
 * (10.5 MiB, RGB888) fits that window; 1440*2560*4 does not.
 *
 * FB_BPP selects how the console encodes pixels, and shim/build.sh passes
 * 24 for the phone: the bootloader's RGB0/RGB1 pipes read the 12 MiB
 * cont_splash window as RGB888 (1440*2560*3 = 10.5 MiB fits; 32 bpp would
 * need 14.1 MiB and does not). The running kernel instead uses RGB2 with
 * ARGB8888 and stride 5760 (fb0 = "mdssfb_90000 1440,5120 stride=5760
 * bpp=32"): that is the config the FULL display test (DISPLAY_MODE=3)
 * sets up at 0x20000000. Two different pipelines, two different formats:
 * the display test passes its format at run time through
 * con_draw_text_bpp(). If the text is garbled, rebuild with FB_BPP=32
 * (console) or DISP_FB_BPP=32 (test).
 */
#define FB_BASE		G4_FB_BASE
#define FB_W		G4_FB_W
#define FB_H		G4_FB_H

#ifndef FB_BPP
#define FB_BPP		32
#endif

#define FB_STRIDE	((FB_W * FB_BPP) / 8)

#if FB_BPP == 32
#define FB_BYTES_PER_PIXEL 4
#elif FB_BPP == 24
#define FB_BYTES_PER_PIXEL 3
#elif FB_BPP == 16
#define FB_BYTES_PER_PIXEL 2
#else
#error "FB_BPP must be 16, 24 or 32"
#endif

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U
#define BLACK	0xff000000U

extern volatile uint8_t *con_fb;
extern uint32_t con_fb_w, con_fb_h, con_fb_stride;

/*
 * An ASCII copy of everything the console printed. The panel never shows
 * it (the G4's DSI is in command mode and the bootloader re-programmed the
 * controller), so this is the only way to get the log off the phone: the
 * shim copies it into scratch RAM, where it survives the reset and can be
 * read back from a recovery with /dev/mem.
 */
#define CON_LOG_SIZE	32768
extern char con_log[];
extern int con_log_len;

/*
 * Append a line to the ASCII log ONLY: nothing is painted on the panel.
 *
 * This is the way to write the log before the framebuffer is known to be
 * safe: on the phone the splash framebuffer sits in the XPU protected
 * region, and a write there is a data abort. The ASCII log lives in plain
 * RAM, so this cannot fault.
 */
void con_log_only(const char *label, const char *s);

/* Point the console at a framebuffer the device tree described. */
void con_set_fb(uintptr_t base, uint32_t w, uint32_t h, uint32_t stride);

/*
 * Stop (and resume) writing to the framebuffer. The ASCII log in con_log
 * keeps working: it lives in RAM.
 *
 * This matters on the phone, where the splash framebuffer sits inside the
 * 150 MiB "secure_region" the XPU protects from the non-secure world. A
 * write there takes a data abort, so the console has to be switchable off
 * before anything is painted.
 */
void con_disable(void);
void con_enable(void);
/* Redraw the whole ASCII log into the framebuffer. */
void con_repaint(void);

void con_clear(uint32_t bg);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
/* One name and one value: "PARANGE pa=0000000A". */
void con_hex1(const char *label, const char *a, uint32_t va);
/* Raw dump of memory, to identify what the bootloader left in a buffer. */
void con_dump(const char *label, uintptr_t addr, int bytes);

/*
 * Draw a string into an arbitrary framebuffer, in ARGB8888, with the
 * console's own font. The display test uses it to paint into the buffer
 * the MDP scans out (shim/display.c), which is not the console one.
 */
void con_draw_text(uintptr_t fb, uint32_t stride, int x, int y, const char *s,
		   uint32_t fg, uint32_t bg, int scale);

/*
 * The same, with the pixel format chosen at run time. The bootloader's
 * RGB0/RGB1 pipes read RGB888 (3 bytes per pixel), the kernel's RGB2
 * pipe reads ARGB8888 (4 bytes): drawing with the wrong stride garbles
 * the picture even when the scan-out is working.
 */
void con_draw_text_bpp(uintptr_t fb, uint32_t stride, int x, int y,
		       const char *s, uint32_t fg, uint32_t bg,
		       int scale, int bpp);
