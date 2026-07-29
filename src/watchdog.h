#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdint.h>

/* Registers a poll callback that re-arms the BCM2835 watchdog, so a stalled
   main loop reboots the Pi instead of needing a power cycle.  Enabled only
   when Pi1MHz.cfg sets "watchdog" to a timeout in seconds (1-15). */
void watchdog_stop(void);

/* Arm (or re-arm) the watchdog at the hardware maximum for the duration of
   boot, before Pi1MHz.cfg has been read.  Call periodically as boot
   progresses; watchdog_init() then takes over with the configured timeout, or
   stands it down if no watchdog was asked for. */
void watchdog_boot_kick(void);
void watchdog_init(uint8_t instance, uint8_t address);

#endif
