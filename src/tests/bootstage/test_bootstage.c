/* Host tests for the chain-boot marker in rpi/bootstage.c, and for the
 * boot-detail stamp reaching RAM.
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
/* Every clean is logged: a stamp survives a watchdog reset only if its line
   was cleaned after the store (the reset drops dirty L1). */
static uintptr_t clean_start, clean_end;     /* the last clean, line-rounded */
static unsigned int cleans;
#define TEST_LINE 32u                         /* ARM1176; the A53's 64 is coarser */
void _clean_cache_area(const void *start, unsigned int length)
{
   clean_start = (uintptr_t)start & ~(uintptr_t)(TEST_LINE - 1u);
   clean_end = (uintptr_t)start + length;
   cleans++;
}
/* Has the word at p been cleaned since the counter read `since`? */
static int cleaned_since(unsigned int since, const volatile void *p)
{
   uintptr_t a = (uintptr_t)p;
   return cleans != since && a >= clean_start && a + 4u <= clean_end;
}

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
   test_pm_rsts = 0x1000u;            /* HADPOR, as left by a power-on */

   CHECK(jump(120000u) == 1u, "a fresh mark (120 ms) not accepted");
   CHECK(RPI_ChainBootJumpUs() == 120000u, "jump time %u us, want 120000",
         (unsigned)RPI_ChainBootJumpUs());
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

   /* Bit 12 (HADPOR) is for display only: a kernel before 9f46af5 stored
      the register without it, and it is sticky from power-on, so a push
      across that change must still count.  A power-on is caught by the
      timer restarting (above). */
   test_pm_rsts = 0x1020u;
   RPI_ChainBootMark();
   test_lowmem[4] &= 0xfffu;          /* as an older kernel stored it */
   now += 100000u;
   RPI_ChainBootConsume();
   CHECK(RPI_ChainBooted() == 1u, "refused across the bit-12 format change");
   CHECK(RPI_ResetReason() == 0x1020u, "Reset reason row lost bit 12");

   /* A refused mark reports no jump time. */
   RPI_ChainBootMark();
   now += 5000000u;
   RPI_ChainBootConsume();
   CHECK(RPI_ChainBootJumpUs() == 0u, "a jump time reported for a refused mark");

   /* A stray byte in the complement, or RAM after a power-on. */
   RPI_ChainBootMark();
   ((volatile uint8_t *)test_lowmem)[5] ^= 0x0Du;
   now += 1000u;
   RPI_ChainBootConsume();
   CHECK(RPI_ChainBooted() == 0u, "accepted with the complement corrupted");
   memset(test_lowmem, 0x55, sizeof test_lowmem);
   RPI_ChainBootConsume();
   CHECK(RPI_ChainBooted() == 0u, "accepted on power-on RAM contents");

   /* The emulator-init stamp: Pi1MHz.c stamps i+1 before each init, and a
      hang there ends in a watchdog reset, which drops a dirty line - so the
      store must reach RAM, as RPI_BootStage's already does. */
   RPI_BootStage(BOOT_STAGE_ENTRY);
   {
      unsigned int before = cleans;
      RPI_BootDetail(7u);
      CHECK(RPI_BootStageBlock()[3] == 7u, "detail word not written");
      CHECK(cleaned_since(before, &RPI_BootStageBlock()[3]),
            "RPI_BootDetail left the stamp dirty in the cache");
   }
   /* ...and the next boot reports it. */
   RPI_BootStage(BOOT_STAGE_ENTRY);
   CHECK(RPI_BootDetailPrevious() == 7u, "previous detail %u, want 7",
         RPI_BootDetailPrevious());

   printf("%d checks, %d failed\n", checks, failures);
   if (failures == 0)
      printf("BOOTSTAGE TESTS PASSED\n");
   return failures ? 1 : 0;
}
