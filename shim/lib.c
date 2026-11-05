/*
 * The few libc bits a freestanding build needs. GCC turns struct
 * initialisation and array copies into calls to these.
 */
#include <stdint.h>

void *memset(void *dst, int c, unsigned long n);
void *memcpy(void *dst, const void *src, unsigned long n);

void *memset(void *dst, int c, unsigned long n)
{
	uint8_t *d = dst;

	while (n--)
		*d++ = (uint8_t)c;
	return dst;
}

void *memcpy(void *dst, const void *src, unsigned long n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	while (n--)
		*d++ = *s++;
	return dst;
}
