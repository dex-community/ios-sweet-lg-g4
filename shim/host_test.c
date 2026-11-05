/* Render the console layout on the host: ./out/host_test && view out/host.ppm */
#include <stdio.h>
#include "console.h"

static uint8_t host_fb[FB_H * FB_STRIDE];

int main(void)
{
	con_fb = host_fb;
	con_clear(RED);
	con_str("SHIM", "LG G4 (H815) bring-up shim");
	con_hex("MMFR0", 0x0000000000101122ULL);
	con_hex2("PSCI", "ver=", 0x10001, "smccc=", 0x10002);
	con_hex2("CPU7", "ret=", 0, "mpidr=", 0x81000700);
	con_clear(BLUE);
	con_str("PANIC", "sync exception");
	con_hex("ESR", 0x96000010);

	FILE *f = fopen("out/host.ppm", "wb");
	fprintf(f, "P6 %d %d 255\n", FB_W, FB_H);
	for (int y = 0; y < FB_H; y++) {
		for (int x = 0; x < FB_W; x++) {
			const uint8_t *p = host_fb + (size_t)y * FB_STRIDE + (size_t)x * FB_BYTES_PER_PIXEL;
			unsigned char px[3];
#if FB_BPP == 32
			px[0] = p[2]; px[1] = p[1]; px[2] = p[0];	/* BGR -> RGB */
#elif FB_BPP == 24
			px[0] = p[0]; px[1] = p[1]; px[2] = p[2];
#else
			unsigned short v = (unsigned short)(p[0] | (p[1] << 8));
			px[0] = (unsigned char)(((v >> 11) & 0x1f) * 255 / 31);
			px[1] = (unsigned char)(((v >> 5) & 0x3f) * 255 / 63);
			px[2] = (unsigned char)((v & 0x1f) * 255 / 31);
#endif
			fwrite(px, 1, 3, f);
		}
	}
	fclose(f);
	return 0;
}
