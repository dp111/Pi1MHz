/* info.h */

#ifndef INFO_H
#define INFO_H

#include "mailbox.h"
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

#endif
