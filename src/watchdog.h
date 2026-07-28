#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdint.h>

/* Registers a poll callback that re-arms the BCM2835 watchdog, so a stalled
   main loop reboots the Pi instead of needing a power cycle.  Enabled only
   when Pi1MHz.cfg sets "watchdog" to a timeout in seconds (1-15). */
void watchdog_init(uint8_t instance, uint8_t address);

#endif
