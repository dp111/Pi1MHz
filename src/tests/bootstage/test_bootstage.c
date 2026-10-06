/* Host tests for the chain-boot marker in rpi/bootstage.c.
 *
 * The marker decides more than a /status row: Pi1MHz.c skips launching the
 * VPU when it believes it was chain-booted, so a marker believed after a
 * reset leaves the 1MHz bus dead.  It must count only for a jump that has
 * just happened with no reset in between - pinned here against a fake
 * system timer, a fake reset-reason register and fake low RAM.
 */
#include <stdio.h>
#include <string.h>

#include "bootstage_stubs.h"
#include "rpi/rpi.h"

uint32_t test_lowmem[8];
uint32_t test_pm_rsts;
static uint64_t now = 0x0000000123456789ull;

uint64_t RPI_GetSystemTime64(void) { return now; }
void _clean_cache_area(const void *start, unsigned int length) { (void)start; (void)length; }

static int failures, checks;

#define CHECK(cond, ...) do { \
      checks++; \
      if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                     printf(__VA_ARGS__); printf("\n"); } \
   } while (0)

/* Mark, let us microseconds pass, consume: was it taken as a chain-boot? */
static unsigned int jump(uint32_t us)
{
   RPI_ChainBootMark();
   now += us;
   RPI_ChainBootConsume();
   return RPI_ChainBooted();
}

int main(void)
{
   test_pm_rsts = 0x1000u;            /* power-on flags, as left by the firmware */

   CHECK(jump(120000u) == 1u, "a fresh mark (120 ms) not accepted");
   RPI_ChainBootConsume();
   CHECK(RPI_ChainBooted() == 0u, "a consumed mark accepted a second time");

   CHECK(jump(5000000u) == 0u, "a mark 5 s old accepted - a reset in between");
   CHECK(jump(600000u) == 0u, "a mark 600 ms old accepted");

   /* A full-chip reset restarts the timer: the stamp is in the future. */
   RPI_ChainBootMark();
   now = 2500000u;                    /* 2.5 s after the reset */
   RPI_ChainBootConsume();
   CHECK(RPI_ChainBooted() == 0u, "accepted after the timer restarted");

   /* The watchdog flag appearing means a reset, however quick. */
   RPI_ChainBootMark();
   test_pm_rsts |= 0x20u;
   now += 100000u;
   RPI_ChainBootConsume();
   CHECK(RPI_ChainBooted() == 0u, "accepted with the watchdog flag newly set");

   /* ...while a flag already set at the jump is no reason to refuse. */
   CHECK(jump(100000u) == 1u, "refused with a sticky flag unchanged since the mark");

   /* A stray byte in the complement, or RAM after a power-on. */
   RPI_ChainBootMark();
   ((volatile uint8_t *)test_lowmem)[5] ^= 0x0Du;
   now += 1000u;
   RPI_ChainBootConsume();
   CHECK(RPI_ChainBooted() == 0u, "accepted with the complement corrupted");
   memset(test_lowmem, 0x55, sizeof test_lowmem);
   RPI_ChainBootConsume();
   CHECK(RPI_ChainBooted() == 0u, "accepted on power-on RAM contents");

   printf("%d checks, %d failed\n", checks, failures);
   if (failures == 0)
      printf("BOOTSTAGE TESTS PASSED\n");
   return failures ? 1 : 0;
}
