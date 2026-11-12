#pragma once
/* Host-test stub, for helpers.c and ram_emulator.c. */
#include <stdbool.h>
#include <stdint.h>
uint32_t mem_info(int size);
char *get_info_string(void);
uint32_t get_temp_millidegrees(void);
bool info_refresh_cached(void);
