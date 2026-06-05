#pragma once
#include <stdbool.h>
bool wifi_debug_enabled(void);
void wifi_debug_printf(const char *format, ...);
