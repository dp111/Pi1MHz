/* hwrng.h - the BCM2835 hardware RNG, mbedTLS's entropy source (hwrng.c). */
#ifndef HWRNG_H
#define HWRNG_H

/* Start the generator warming up; idempotent, does not wait. */
void hwrng_start(void);

#endif
