/* Host test for the Pi1MHz.cfg parser (config.c). Built against stub
 * rpi/BeebSCSI headers by run.sh. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include "config.h"

/* config_load() references filesystemReadFile; stub it (unused here). */
uint32_t filesystemReadFile(const char *f, uint8_t **a, unsigned int m)
{ (void)f; (void)a; (void)m; return 0; }

int main(void)
{
   static char cfg[] =
      "# Pi1MHz configuration\n"
      "teletext_server1=80.229.142.86:19761\n"
      "teletext_debug = 1            # inline comment\n"
      "aun_station = 0.42\n"
      "aun_map=1.254=10.0.0.1,1.200=10.0.0.2   # trailing comment\n"
      "   indented_key=ignored\n"      /* keys must start at column 0 */
      "flag_only\n"
      "empty_value=\n"
      "greeting=hello world\n"          /* values may contain spaces */
      "MixedCase=Hello\n"
      "Teletext_addr=-1\n"              /* negative address = disable */
      "Harddisc_addr=0x50\n";           /* address override */

   config_parse(cfg, strlen(cfg));

   assert(!strcmp(config_get("teletext_server1"), "80.229.142.86:19761"));
   assert(!strcmp(config_get("teletext_debug"), "1"));        /* trims ws + inline # */
   assert(!strcmp(config_get("aun_station"), "0.42"));
   assert(!strcmp(config_get("aun_map"), "1.254=10.0.0.1,1.200=10.0.0.2")); /* '=' kept */
   assert(config_get("indented_key") == NULL);                /* col-0 rule */
   assert(config_get("flag_only") != NULL && config_get("flag_only")[0] == '\0');
   assert(config_get("empty_value") != NULL && config_get("empty_value")[0] == '\0');
   assert(!strcmp(config_get("greeting"), "hello world"));     /* spaces in value */
   assert(!strcmp(config_get("MIXEDCASE"), "Hello"));          /* case-insensitive key */
   assert(config_get("nope") == NULL);

   /* emulator address override / disable resolver */
   uint8_t addr = 0xAA;
   assert(config_emulator_override("Teletext", &addr) < 0);          /* disabled */
   addr = 0xAA;
   assert(config_emulator_override("Harddisc", &addr) == 1 && addr == 0x50);
   addr = 0xAA;
   assert(config_emulator_override("M5000", &addr) == 0 && addr == 0xAA); /* no key, untouched */

   printf("all config tests passed\n");
   return 0;
}
