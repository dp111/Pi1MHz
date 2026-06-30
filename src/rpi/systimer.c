#include <stdint.h>
#include "systimer.h"

static rpi_sys_timer_t* rpiSystemTimer = (rpi_sys_timer_t*)RPI_SYSTIMER_BASE;

rpi_sys_timer_t* RPI_GetSystemTimer(void)
{
    return rpiSystemTimer;
}

uint32_t RPI_GetSystemTime(void)
{
    return rpiSystemTimer->counter_lo;
}

/* Full 64-bit microsecond counter. counter_lo alone wraps every ~71.6 min,
 * so a millisecond value derived from it is not a clean mod-2^32 counter and
 * defeats wrap-safe deadline comparisons. Read hi/lo/hi to guard against a
 * low-word rollover occurring between the two reads. */
uint64_t RPI_GetSystemTime64(void)
{
    uint32_t hi, lo;
    do {
        hi = rpiSystemTimer->counter_hi;
        lo = rpiSystemTimer->counter_lo;
    } while (hi != rpiSystemTimer->counter_hi);
    return ((uint64_t)hi << 32) | lo;
}

void RPI_WaitMicroSeconds( uint32_t us )
{
    uint32_t ts = rpiSystemTimer->counter_lo;
// cppcheck-suppress duplicateExpression
    while ( ( rpiSystemTimer->counter_lo - ts ) < us )
    {
        /* BLANK */
    }
}
