#ifndef CONFIG_H
#define CONFIG_H
#include <stdbool.h>
const char *config_get(const char *prop);   /* defined in test_net.c */
bool        config_get_bool(const char *key);
#endif
