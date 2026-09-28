/* hwrng.c - the BCM2835 hardware random number generator: mbedTLS's entropy
   source (MBEDTLS_ENTROPY_HARDWARE_ALT, wifi/mbedtls_config_pi1mhz.h), and
   wolfCrypt's through secure_service_wolfssh.c when PI1MHZ_SSH is built.

   hwrng_start() is called when the network service starts: three register
   writes, no waiting, so the warm-up (the first 0x40000 oscillator bits are
   discarded) is long over by the first
   https:// connection.  Started on first use instead, the warm-up outlasted
   the poll's wait and the first https:// after boot failed (MEASURED
   2026-09-26).  A word identical to the one
   before means the generator has stopped, and the poll fails rather than
   hand mbedTLS constant "entropy". */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "base.h"
#include "systimer.h"
#include "hwrng.h"

#define RNG_BASE      (PERIPHERAL_BASE + 0x104000u)
#define RNG_CTRL      (*(volatile uint32_t *)(RNG_BASE + 0x00u))
#define RNG_STATUS    (*(volatile uint32_t *)(RNG_BASE + 0x04u))
#define RNG_DATA      (*(volatile uint32_t *)(RNG_BASE + 0x08u))
#define RNG_INT_MASK  (*(volatile uint32_t *)(RNG_BASE + 0x10u))

#define RNG_WARMUP_BITS   0x40000u
#define RNG_WORD_WAIT_US  500000u      /* covers the warm-up; a stopped RNG fails */

/* MBEDTLS_ERR_ENTROPY_SOURCE_FAILED, without pulling in mbedTLS headers */
#define ENTROPY_SOURCE_FAILED  (-0x003C)

static bool rng_started;
static bool rng_have_last;
static uint32_t rng_last;

void hwrng_start(void)
{
   if (rng_started)
      return;
   RNG_INT_MASK |= 1u;
   RNG_STATUS = RNG_WARMUP_BITS;
   RNG_CTRL |= 1u;
   rng_started = true;
}

int hwrng_word(uint32_t *out, uint32_t wait_us)
{
   uint32_t t0 = RPI_GetSystemTime();
   while ((RNG_STATUS >> 24) == 0u)
      if (RPI_GetSystemTime() - t0 >= wait_us)
         return -1;
   uint32_t w = RNG_DATA;
   if (rng_have_last && w == rng_last)
      return -1;
   rng_last = w;
   rng_have_last = true;
   *out = w;
   return 0;
}

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
   (void)data;
   *olen = 0;
   hwrng_start();
   while (*olen < len) {
      uint32_t w;
      if (hwrng_word(&w, RNG_WORD_WAIT_US) != 0)
         return ENTROPY_SOURCE_FAILED;
      for (unsigned int i = 0; i < 4u && *olen < len; i++) {
         output[(*olen)++] = (unsigned char)w;
         w >>= 8;
      }
   }
   return 0;
}
