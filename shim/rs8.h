#pragma once
#include <stdint.h>

/* Reed-Solomon encoder, 8 bit symbols, 16 parity symbols (see rs8.c). */
void rs8_init(void);
int rs8_encode(const uint8_t *data, int len, uint8_t *ecc);
