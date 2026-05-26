/* info.h */

#ifndef INFO_H
#define INFO_H

#include "mailbox.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
   int rate;
   int min_rate;
   int max_rate;
} clock_info_t;

#define    COMPONENT_CORE 1
#define COMPONENT_SDRAM_C 2
#define COMPONENT_SDRAM_P 3
#define COMPONENT_SDRAM_I 4

extern uint32_t mem_info(int size);

extern uint32_t get_clock_rate(uint32_t clk_id);
/* Cached on boot, so this is safe to call at any time */
extern uint32_t get_speed(void);
/* Cached on boot, so this is safe to call at any time */
extern char *get_info_string(void);
extern void dump_useful_info(void);
/* Cached on boot, so this is safe to call at any time */
extern char *get_cmdline_prop(const char *prop);

float get_temp(void);

/* Foundation-style WiFi/Ethernet MAC computed by the VC4 firmware
   from the SoC's board-serial OTP fuses.  Returns true and writes
   six bytes if the mailbox query succeeds, false otherwise (in
   which case the caller should fall back to its own scheme).  On
   the Pi Zero W / Zero 2 W this is the WiFi MAC; on Pi 3B+ / 4 it
   is the wired Ethernet MAC.  Either way the address is stable
   across reboots and matches what Pi-OS would use for the same
   board. */
extern bool rpi_get_board_mac(uint8_t mac[6]);

#endif
