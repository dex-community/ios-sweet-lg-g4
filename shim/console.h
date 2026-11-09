#pragma once
#include <stdint.h>

#include "g4.h"

/*
 * The LG G4 panel is 1440x2560 and the bootloader leaves a 12 MiB
 * continuous-splash window at 0x03400000 (cont_splash_mem). 1440*2560*3
 * (10.5 MiB, RGB888) fits that window; 1440*2560*4 does not.
 *
 * FB_BPP selects how the console encodes pixels. The DSI node says
 * qcom,mdss-dsi-bpp = <0x18> (24 bpp on the wire), but the MDP's own
 * framebuffer is ARGB8888: the running phone reports
 * /sys/class/graphics/fb0 = "mdssfb_90000 1440,5120 stride=5760 bpp=32",
 * i.e. 4 bytes per pixel at a 1440 pixel stride. So 32 is the default. If
 * the first device test shows garbage, change this one value and rebuild:
 * 24 or 16 (RGB565).
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
/* Raw dump of memory, to identify what the bootloader left in a buffer. */
void con_dump(const char *label, uintptr_t addr, int bytes);
