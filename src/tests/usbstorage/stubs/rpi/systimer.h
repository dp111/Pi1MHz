/* Host stub: a clock the test moves (test_usb_storage.c). */
#ifndef STUB_SYSTIMER_H
#define STUB_SYSTIMER_H
#include <stdint.h>
uint32_t RPI_GetSystemTime(void);
uint64_t RPI_GetSystemTime64(void);
#endif
