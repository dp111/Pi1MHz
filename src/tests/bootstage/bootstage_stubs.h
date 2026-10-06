/* Host stub for the hardware bootstage.c touches: one header, which
   run_tests.sh puts at base.h, cache.h, lowmem.h and systimer.h.  The fixed
   low-RAM marker and the PM reset-reason register become test variables. */
#ifndef BOOTSTAGE_STUBS_H
#define BOOTSTAGE_STUBS_H

#include <stdint.h>

extern uint32_t test_lowmem[8];        /* stands in for LOWMEM_MARKERS */
extern uint32_t test_pm_rsts;          /* stands in for PM_RSTS */

/* lowmem.h */
#define LOWMEM_CHAIN_MARKER ((uintptr_t)test_lowmem)
/* base.h: bootstage.c reads PM_RSTS at PERIPHERAL_BASE + 0x00100020 */
#define PERIPHERAL_BASE ((uintptr_t)&test_pm_rsts - 0x00100020u)
/* systimer.h */
uint64_t RPI_GetSystemTime64(void);
/* cache.h */
void _clean_cache_area(const void *start, unsigned int length);

#endif
