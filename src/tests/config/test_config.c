/* Host test for the Pi1MHz.cfg parser (config.c). Built against stub
 * rpi/BeebSCSI headers by run.sh. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include "config.h"

/* config_load()'s file read: NULL = the file is missing. */
static const char *stub_file;
uint32_t filesystemReadFile(const char *f, uint8_t **a, unsigned int m)
{
   (void)f; (void)m;
   if (stub_file == NULL)
      return 0;
   size_t n = strlen(stub_file);
   *a = malloc(n);
   memcpy(*a, stub_file, n);
   return (uint32_t)n;
}

/* config_load() latches, so each load runs in its own child process. */
static void load_case(const char *file, bool want_found, int want_keys)
{
   pid_t pid = fork();
   assert(pid >= 0);
   if (pid == 0) {
      int keys = -1;
      assert(!config_file_found(&keys) && keys == 0);    /* before any load */
      stub_file = file;
      config_load("/Pi1MHz/Pi1MHz.cfg");
      assert(config_file_found(&keys) == want_found);
      assert(keys == want_keys);
      config_load("/Pi1MHz/Pi1MHz.cfg");                 /* a second call changes nothing */
      assert(config_file_found(&keys) == want_found && keys == want_keys);
      _exit(0);
   }
   int st;
   assert(waitpid(pid, &st, 0) == pid);
   assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

int main(void)
{
   load_case(NULL, false, 0);                                  /* missing      */
   load_case("", false, 0);                                    /* empty        */
   load_case("# only a comment\n", true, 0);
   load_case("a=1\n# c\nb\n  indented=x\nc = 3 # t\n", true, 3);

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
      "Harddisc_addr=0x50\n"            /* address override */
      "f_off=off\n" "f_OFF=OFF\n" "f_on=On\n" "f_no=no\n" "f_two=2\n" "f_junk=maybe\n" "f_true=true\n"
      "BeebSID_addr=off\n"            /* a word: disabled, not &FC00 */
      "Serial_addr=010\n"             /* decimal ten, not octal 8 */
      "Rambyte_addr=&FD\n"            /* the BBC's hex */
      "Rampage_addr=0x40junk\n"       /* trailing text: disabled */
      "n_dec=42\n" "n_hex=0x2A\n" "n_amp=&2a\n" "n_oct=010\n" "n_neg=-1\n" "n_word=off\n"
      "n_tail=12abc\n" "n_big=300\n";

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
   addr = 0xAA;
   assert(config_emulator_override("BeebSID", &addr) < 0 && addr == 0xAA);  /* strtol said 0: base &FC00 */
   addr = 0xAA;
   assert(config_emulator_override("Serial", &addr) == 1 && addr == 10);   /* not octal */
   addr = 0xAA;
   assert(config_emulator_override("Rambyte", &addr) == 1 && addr == 0xFD);
   addr = 0xAA;
   assert(config_emulator_override("Rampage", &addr) < 0 && addr == 0xAA);

   /* numbers: the whole value or the default */
   assert(config_get_uint("n_dec", 255u, 7u) == 42u);
   assert(config_get_uint("n_hex", 255u, 7u) == 42u && config_get_uint("n_amp", 255u, 7u) == 42u);
   assert(config_get_uint("n_oct", 255u, 7u) == 10u);
   assert(config_get_uint("n_neg", 255u, 7u) == 7u);           /* atoi said -1 */
   assert(config_get_uint("n_word", 255u, 7u) == 7u);          /* atoi said 0 */
   assert(config_get_uint("n_tail", 255u, 7u) == 7u);          /* atoi said 12 */
   assert(config_get_uint("n_big", 255u, 7u) == 7u && config_get_uint("n_big", 300u, 7u) == 300u);
   assert(config_get_uint("empty_value", 255u, 7u) == 7u && config_get_uint("nope", 255u, 7u) == 7u);

   /* the one boolean parser (telnetd's own said "off" was on, wifi's said "no" was) */
   assert(!config_get_flag("f_off", true) && !config_get_flag("f_OFF", true));
   assert(config_get_flag("f_on", false) && config_get_bool("f_on"));
   assert(!config_get_flag("f_no", true));
   assert(config_get_flag("f_two", false) && config_get_flag("f_true", false));
   assert(config_get_flag("f_junk", true) && !config_get_flag("f_junk", false));   /* unknown: the default */
   assert(config_get_flag("empty_value", true) && config_get_flag("nope", true));
   assert(!config_get_bool("nope"));

   printf("all config tests passed\n");
   return 0;
}
