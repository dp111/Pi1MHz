#pragma once
#include <stdbool.h>
const char *config_get(const char *key);
bool config_get_flag(const char *key, bool dflt);
