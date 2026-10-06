/* Host stub for what rpi/info.c needs beyond the real info.h, mailbox.h and
   rpi.h: one header, which run_tests.sh puts at rpi/base.h and Pi1MHz.h. */
#ifndef BOARDINFO_STUBS_H
#define BOARDINFO_STUBS_H

#include <stdint.h>
#include <stdbool.h>

/* base.h */
#define PERIPHERAL_BASE 0x20000000UL
typedef volatile uint32_t rpi_reg_rw_t;
typedef volatile const uint32_t rpi_reg_ro_t;

/* Pi1MHz.h */
extern uint32_t Pi1MHz_now_us;

#endif
