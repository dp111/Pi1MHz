/* Test stub: system microsecond clock, driven by the test via g_now_us. */
#ifndef STUB_SYSTIMER_H
#define STUB_SYSTIMER_H
#include <stdint.h>
extern uint64_t g_now_us;               /* defined in test_net.c */
static inline uint64_t RPI_GetSystemTime64(void) { return g_now_us; }
#endif
