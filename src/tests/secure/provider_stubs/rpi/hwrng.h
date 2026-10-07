#pragma once
/* Host-test stub of rpi/hwrng.h. */
#include <stdint.h>
void hwrng_start(void);
int  hwrng_word(uint32_t *out, uint32_t wait_us);
