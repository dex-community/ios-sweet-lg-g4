/*
 * Reed-Solomon encoder, 8 bit symbols, 16 parity symbols.
 *
 * The ramoops driver protects every 128 byte block of the console zone with
 * a Reed-Solomon code (fs/pstore/ram_core.c: persistent_ram_encode_rs8) and
 * the block header with the same code. When the shim appends its log to
 * that zone it has to write the parity bytes too: without them the decoder
 * sees a wrong syndrome, decides that the block is damaged and "corrects"
 * it, which puts the old bytes back.
 *
 * The code below is the kernel's own encoder (lib/reed_solomon/reed_solomon.c
 * and lib/reed_solomon/encode_rs.c, GPL), reduced to what is needed here:
 * GF(2^8) with the polynomial 0x11d, 16 roots, first consecutive root 0,
 * primitive element 1. The parameters are the ones ramoops uses:
 * init_rs(8, 0x11d, 0, 1, 16).
 *
 * tools/rs8-test.sh checks this file against the kernel's own, byte for byte.
 */
#include <stdint.h>

#include "rs8.h"

#define RS_MM		8		/* bits per symbol */
#define RS_NN		255		/* symbols per block */
#define RS_NROOTS	16		/* parity symbols */
#define RS_FCR		0		/* first consecutive root */
#define RS_PRIM		1		/* primitive element */
#define RS_GFPOLY	0x11d		/* field generator polynomial */

static uint16_t rs_alpha_to[RS_NN + 1];
static uint16_t rs_index_of[RS_NN + 1];
static uint16_t rs_genpoly[RS_NROOTS + 1];
static int rs_ready;

static int rs_modnn(int x)
{
	while (x >= RS_NN) {
		x -= RS_NN;
		x = (x >> RS_MM) + (x & RS_NN);
	}
	return x;
}

/* The Galois field tables and the generator polynomial (rs_init()). */
void rs8_init(void)
{
	int i, j, sr, root;

	rs_index_of[0] = RS_NN;		/* log(zero) = -inf */
	rs_alpha_to[RS_NN] = 0;		/* alpha**-inf = 0 */
	sr = 1;
	for (i = 0; i < RS_NN; i++) {
		rs_index_of[sr] = (uint16_t)i;
		rs_alpha_to[i] = (uint16_t)sr;
		sr <<= 1;
		if (sr & (1 << RS_MM))
			sr ^= RS_GFPOLY;
		sr &= RS_NN;
	}

	/* Form the code generator polynomial from its roots. */
	rs_genpoly[0] = 1;
	for (i = 0, root = RS_FCR * RS_PRIM; i < RS_NROOTS; i++, root += RS_PRIM) {
		rs_genpoly[i + 1] = 1;
		for (j = i; j > 0; j--) {
			if (rs_genpoly[j] != 0) {
				rs_genpoly[j] = rs_genpoly[j - 1] ^
					rs_alpha_to[rs_modnn((int)rs_index_of[rs_genpoly[j]] + root)];
			} else {
				rs_genpoly[j] = rs_genpoly[j - 1];
			}
		}
		rs_genpoly[0] = rs_alpha_to[rs_modnn((int)rs_index_of[rs_genpoly[0]] + root)];
	}
	/* Index form, for a quicker encoding. */
	for (i = 0; i <= RS_NROOTS; i++)
		rs_genpoly[i] = rs_index_of[rs_genpoly[i]];

	rs_ready = 1;
}

/*
 * The parity of len bytes of data, in ecc[0..15] (the low byte of each
 * 16 bit parity symbol, like persistent_ram_encode_rs8 does).
 */
int rs8_encode(const uint8_t *data, int len, uint8_t *ecc)
{
	uint16_t par[RS_NROOTS];
	int i, j;

	if (!rs_ready)
		rs8_init();
	if (len < 0 || len > RS_NN - RS_NROOTS)
		return -1;

	for (i = 0; i < RS_NROOTS; i++)
		par[i] = 0;
	for (i = 0; i < len; i++) {
		uint16_t fb = rs_index_of[(((uint16_t)data[i]) & 0xffu) ^ par[0]];

		if (fb != RS_NN) {
			for (j = 1; j < RS_NROOTS; j++)
				par[j] ^= rs_alpha_to[rs_modnn((int)fb +
							rs_genpoly[RS_NROOTS - j])];
		}
		/* Shift the register. */
		for (j = 0; j < RS_NROOTS - 1; j++)
			par[j] = par[j + 1];
		if (fb != RS_NN)
			par[RS_NROOTS - 1] = rs_alpha_to[rs_modnn((int)fb + rs_genpoly[0])];
		else
			par[RS_NROOTS - 1] = 0;
	}
	for (i = 0; i < RS_NROOTS; i++)
		ecc[i] = (uint8_t)par[i];
	return 0;
}
