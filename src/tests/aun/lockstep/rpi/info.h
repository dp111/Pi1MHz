#pragma once
/* AUN_LOG routes through LOG_DEBUG. In the host harness it must stay silent:
 * the lockstep protocol uses stdout, so any stray debug output would corrupt
 * it. (config keys now come from config.h's config_get, not get_cmdline_prop.) */
#define LOG_DEBUG(...) ((void)0)
