/*
 * TinyUSB board_millis() implementation using the existing system timer.
 */

#include <stdint.h>

#include "../rpi/systimer.h"

uint32_t board_millis(void)
{
    /* Use 64-bit system timer (microseconds) to avoid 32-bit rollover. */
    rpi_sys_timer_t *timer = RPI_GetSystemTimer();
    uint32_t hi1;
    uint32_t lo;
    uint32_t hi2;

    /* Read hi/lo/hi to avoid rollover between reads. */
    do {
        hi1 = timer->counter_hi;
        lo = timer->counter_lo;
        hi2 = timer->counter_hi;
    } while (hi1 != hi2);

    uint64_t us = ((uint64_t)hi1 << 32) | lo;
    return (uint32_t)(us / 1000u);
}
