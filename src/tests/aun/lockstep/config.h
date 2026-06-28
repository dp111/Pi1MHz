/* Host-test stub for the firmware config.h.
 *
 * aun_emulator.c reads all of its keys through config_get(); the lockstep
 * and fuzz_cmd harness mains implement it over a fake cmdline string (see
 * config_get() in lockstep/main.c and fuzz_cmd.c). Only the one entry point
 * the AUN emulator actually uses is declared here. */
#ifndef AUN_TEST_CONFIG_H
#define AUN_TEST_CONFIG_H

const char *config_get(const char *key);

#endif
