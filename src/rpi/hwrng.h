/* hwrng.h - the BCM2835 hardware RNG (hwrng.c): mbedTLS's entropy source,
   and wolfCrypt's in the optional SSH service. */
#ifndef HWRNG_H
#define HWRNG_H

#include <stdint.h>

/* Start the generator warming up; idempotent, does not wait. */
void hwrng_start(void);

/* Next word into *out: 0, or -1 if none arrives within wait_us (0 = don't
   wait) or it repeats the last word - a stopped generator. */
int hwrng_word(uint32_t *out, uint32_t wait_us);

#endif
