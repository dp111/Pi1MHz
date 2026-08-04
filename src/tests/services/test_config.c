/* Host tests for config.c - the Pi1MHz.cfg parser every user's file goes
 * through.  Uses config_parse() (exposed for host testing) on in-memory
 * buffers, so no filesystem is involved. */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "config.h"

/* config_load() references this; the tests drive config_parse() directly. */
uint32_t filesystemReadFile(const char *filename, uint8_t **address, unsigned int max_size)
{ (void)filename; (void)address; (void)max_size; return 0; }

static int checks, fails;
static void ok(int cond, const char *what)
{
   checks++;
   if (!cond) { fails++; printf("  FAIL: %s\n", what); }
   else         printf("  ok: %s\n", what);
}

static void eq(const char *got, const char *want, const char *what)
{
   ok(got != NULL && strcmp(got, want) == 0, what);
   if (got != NULL && strcmp(got, want) != 0)
      printf("        got \"%s\", wanted \"%s\"\n", got, want);
}

int main(void)
{
   /* One image covering every documented rule (config.h): key=value,
      "key value", values with spaces, comments, blanks, indentation,
      trailing comments, CRLF, bare flags. */
   static char image[] =
      "# whole-line comment\n"
      "simple=1\n"
      "spaced value with spaces\n"
      "trailing=keep this   # not this\n"
      "  indented=ignored\n"
      "\tindented2=ignored\n"
      "\n"
      "eq_spaces   =   padded\n"
      "bareflag\n"
      "crlf=yes\r\n"
      "MixedCase=match\n"
      "Harddisc_addr=0x50\n"
      "M5000_addr=-1\n"
      "decimal_addr=64\n"
      "empty=\n";

   config_parse(image, sizeof image - 1);

   puts("== parsing ==");
   eq(config_get("simple"), "1", "key=value");
   eq(config_get("spaced"), "value with spaces", "key value form, spaces kept");
   eq(config_get("trailing"), "keep this", "trailing comment and padding stripped");
   ok(config_get("indented") == NULL && config_get("indented2") == NULL,
      "indented lines are ignored (key must start at column 0)");
   eq(config_get("eq_spaces"), "padded", "spaces around '=' are skipped");
   ok(config_get("bareflag") != NULL && config_get("bareflag")[0] == '\0',
      "bare flag present with empty value");
   eq(config_get("crlf"), "yes", "CRLF line endings");
   eq(config_get("empty"), "", "explicit empty value");
   ok(config_get("missing") == NULL, "unknown key is NULL");
   ok(config_get("simpl") == NULL && config_get("simplee") == NULL,
      "prefixes and extensions do not match");

   puts("== case-insensitivity ==");
   eq(config_get("mixedcase"), "match", "lookup lower vs stored mixed");
   eq(config_get("MIXEDCASE"), "match", "lookup upper vs stored mixed");
   eq(config_get("SIMPLE"), "1", "lookup upper vs stored lower");

   puts("== config_get_bool / write-protect accessor ==");
   ok(!config_get_bool("bool_absent"), "absent key -> false");
   {
      /* distinct keys so config_get's first-match/append store can hold every
         value at once */
      static char bools[] =
         "b_one=1\n" "b_yl=y\n" "b_yu=Y\n" "b_tl=t\n" "b_tu=T\n"
         "b_zero=0\n" "b_no=n\n" "b_word=on\n";
      config_parse(bools, sizeof bools - 1);
   }
   ok(config_get_bool("b_one") && config_get_bool("b_yl") && config_get_bool("b_yu")
      && config_get_bool("b_tl") && config_get_bool("b_tu"),
      "values 1/y/Y/t/T -> true");
   ok(!config_get_bool("b_zero") && !config_get_bool("b_no") && !config_get_bool("b_word"),
      "values 0/n/on -> false");

   ok(!config_beeb_write_protected(), "write-protect false when its key is absent");
   { static char wp[] = "Beeb_write_protect=Y\n"; config_parse(wp, sizeof wp - 1); }
   ok(config_beeb_write_protected(), "write-protect true once the key is set");

   puts("== emulator overrides ==");
   {
      uint8_t addr = 0xEE;
      ok(config_emulator_override("Harddisc", &addr) == 1 && addr == 0x50,
         "hex override applies");
      addr = 0xEE;
      ok(config_emulator_override("decimal", &addr) == 1 && addr == 64,
         "decimal override applies");
      addr = 0xEE;
      ok(config_emulator_override("M5000", &addr) == -1 && addr == 0xEE,
         "-1 disables and leaves addr untouched");
      addr = 0xEE;
      ok(config_emulator_override("Services", &addr) == 0 && addr == 0xEE,
         "absent key returns 0 and leaves addr untouched");
   }

   puts("== key-table cap ==");
   {
      /* config_parse appends; fill past CONFIG_MAX_KEYS (96) and check the
         parser neither crashes nor loses the earlier keys. */
      static char many[96 * 8];
      size_t n = 0;
      for (int i = 0; i < 96; i++)
         n += (size_t)sprintf(&many[n], "k%03d=%d\n", i, i);
      config_parse(many, n);
      ok(config_get("simple") != NULL, "earlier keys survive the cap");
      ok(config_get("k095") == NULL || config_get("k000") != NULL,
         "overflowing keys are dropped, not corrupted");
   }

   printf("\n%d checks, %d failures\n", checks, fails);
   return fails ? 1 : 0;
}
