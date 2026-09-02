/* Host tests for the shared key=value file parser.
 *
 * parse_readfile() is what reads and rewrites every scsi<n>.cfg, and now the
 * WiFi profile too, so its behaviour - which values it takes, which it
 * clamps, and above all what survives a rewrite - is worth pinning down
 * before anyone edits it.  These tests record what the parser DOES, so they
 * can be run before and after a change and compared.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "rpi/fileparser.h"

/* ---- the in-memory card ------------------------------------------------- */

static char     g_file[8192];      /* what parse_readfile() will read      */
static size_t   g_file_len;
static bool     g_file_present = true;
static char     g_written[8192];   /* what it wrote back                   */
static size_t   g_written_len;
static bool     g_write_called;
static bool     g_write_fails;

uint32_t filesystemReadFile(const char *filename, uint8_t **address, unsigned int max_size)
{
   (void)filename; (void)max_size;
   if (!g_file_present) return 0;
   *address = malloc(g_file_len + 1u);
   if (*address == NULL) return 0;
   memcpy(*address, g_file, g_file_len);
   (*address)[g_file_len] = 0;
   return (uint32_t)g_file_len;
}

uint32_t filesystemWriteFile(const char *filename, const uint8_t *address, uint32_t max_size)
{
   (void)filename;
   g_write_called = true;
   if (g_write_fails) return 0;
   if (max_size > sizeof g_written) return 0;
   memcpy(g_written, address, max_size);
   g_written_len = max_size;
   return max_size;
}

bool filesystemWriteFileSafe(const char *filename, const uint8_t *address, uint32_t length)
{
   return filesystemWriteFile(filename, address, length) == length;
}

static void set_file(const char *text)
{
   g_file_len = strlen(text);
   memcpy(g_file, text, g_file_len);
   g_file_present = true;
   g_written_len = 0;
   g_written[0] = '\0';
   g_write_called = false;
}

/* ---- framework ---------------------------------------------------------- */

static int checks, fails;
static void ok(int cond, const char *what)
{
   checks++;
   if (!cond) { fails++; printf("  FAIL: %s\n", what); }
   else         printf("  ok: %s\n", what);
}

static const char *written(void)
{
   g_written[g_written_len] = '\0';
   return g_written;
}

enum { K_TITLE = 0, K_SIZE, K_INQUIRY, K_COUNT };
static const parserkey keys[] = {
   { "Title"   , 0,  8, STRING },
   { "Size"    , 5, 50, INTEGER },
   { "Inquiry" , 0,  4, NUMSTRING },
   { NULL      , 0,  0, STRING }
};

int main(void)
{
   puts("== reading ==");
   {
      parserkeyvalue v[K_COUNT] = {0};
      set_file("Title=Disc\nSize=20\nInquiry=A1B2\n");
      ok(parse_readfile("f", 0, keys, v) != 0, "a well-formed file parses");
      ok(v[K_TITLE].length == 4 && strcmp(v[K_TITLE].v.string, "Disc") == 0,
         "STRING value read");
      ok(v[K_SIZE].length == 1 && *v[K_SIZE].v.integer == 20, "INTEGER value read");
      ok(v[K_INQUIRY].length == 2
         && (uint8_t)v[K_INQUIRY].v.string[0] == 0xA1u
         && (uint8_t)v[K_INQUIRY].v.string[1] == 0xB2u, "NUMSTRING read as bytes");
      parse_releasekeyvalues(v, K_COUNT);
   }
   {
      parserkeyvalue v[K_COUNT] = {0};
      set_file("title=lower\n");
      ok(parse_readfile("f", 0, keys, v) != 0 && v[K_TITLE].length == 5,
         "key matching is case-insensitive");
      parse_releasekeyvalues(v, K_COUNT);
   }
   {
      parserkeyvalue v[K_COUNT] = {0};
      set_file("Title=abcdefghijkl\n");          /* max is 8 */
      (void)parse_readfile("f", 0, keys, v);
      ok(v[K_TITLE].length == 8, "over-long STRING is truncated to the key maximum");
      parse_releasekeyvalues(v, K_COUNT);
   }
   {
      parserkeyvalue v[K_COUNT] = {0};
      set_file("Size=999\n");                    /* range is 5..50 */
      (void)parse_readfile("f", 0, keys, v);
      ok(v[K_SIZE].length == 1 && *v[K_SIZE].v.integer == 50,
         "INTEGER above the maximum is clamped");
      parse_releasekeyvalues(v, K_COUNT);
   }
   {
      parserkeyvalue v[K_COUNT] = {0};
      set_file("Size=1\n");
      (void)parse_readfile("f", 0, keys, v);
      ok(v[K_SIZE].length == 1 && *v[K_SIZE].v.integer == 5,
         "INTEGER below the minimum is clamped");
      parse_releasekeyvalues(v, K_COUNT);
   }
   {
      parserkeyvalue v[K_COUNT] = {0};
      g_file_present = false;
      ok(parse_readfile("missing", 0, keys, v) == 0, "a missing file fails");
      parse_releasekeyvalues(v, K_COUNT);
   }
   {
      parserkeyvalue v[K_COUNT] = {0};
      set_file("Title = spaced\nSize\t=\t9\n");
      (void)parse_readfile("f", 0, keys, v);
      ok(v[K_TITLE].length == 6 && strcmp(v[K_TITLE].v.string, "spaced") == 0
         && *v[K_SIZE].v.integer == 9, "spaces and tabs around = are skipped");
      parse_releasekeyvalues(v, K_COUNT);
   }
   {
      parserkeyvalue v[K_COUNT] = {0};
      set_file("Title=Disc  # a trailing comment\n");
      (void)parse_readfile("f", 0, keys, v);
      ok(v[K_TITLE].length == 4 && strcmp(v[K_TITLE].v.string, "Disc") == 0,
         "a trailing comment is not part of the value");
      parse_releasekeyvalues(v, K_COUNT);
   }

   puts("== NUMSTRING corners ==");
   {
      parserkeyvalue v[K_COUNT] = {0};
      set_file("Inquiry=0xA1B2\n");
      (void)parse_readfile("f", 0, keys, v);
      ok(v[K_INQUIRY].length == 2 && (uint8_t)v[K_INQUIRY].v.string[0] == 0xA1u
         && (uint8_t)v[K_INQUIRY].v.string[1] == 0xB2u, "a 0x prefix is stripped");
      parse_releasekeyvalues(v, K_COUNT);
   }
   {
      parserkeyvalue v[K_COUNT] = {0};
      set_file("Inquiry=A1B\n");               /* odd digit count */
      (void)parse_readfile("f", 0, keys, v);
      ok(v[K_INQUIRY].length == 2 && (uint8_t)v[K_INQUIRY].v.string[0] == 0x0Au
         && (uint8_t)v[K_INQUIRY].v.string[1] == 0x1Bu,
         "an odd digit count is padded from the left");
      parse_releasekeyvalues(v, K_COUNT);
   }
   {
      parserkeyvalue v[K_COUNT] = {0};
      set_file("Inquiry=A1B2C3D4E5F6\n");      /* max is 4 bytes */
      (void)parse_readfile("f", 0, keys, v);
      ok(v[K_INQUIRY].length == 4 && (uint8_t)v[K_INQUIRY].v.string[3] == 0xD4u,
         "an over-long NUMSTRING is truncated to the key maximum");
      parse_releasekeyvalues(v, K_COUNT);
   }
   {
      parserkeyvalue v[K_COUNT] = {0};
      set_file("Inquiry=A1\n");
      (void)parse_readfile("f", 0, keys, v);
      /* The allocation is always max bytes, zero filled, so a consumer that
         reads a fixed offset cannot overrun a short hand-edited value -
         ASan checks that claim here. */
      ok(v[K_INQUIRY].length == 1 && v[K_INQUIRY].v.string[3] == 0,
         "a short NUMSTRING is zero padded to the key maximum");
      parse_releasekeyvalues(v, K_COUNT);
   }
   {
      parserkeyvalue v[K_COUNT] = {0};
      set_file("Inquiry=zzzz\n");
      (void)parse_readfile("f", 0, keys, v);
      ok(v[K_INQUIRY].length == 0, "a malformed NUMSTRING stores nothing");
      parse_releasekeyvalues(v, K_COUNT);
   }

   puts("== rewriting ==");
   {
      parserkeyvalue v[K_COUNT] = {0};
      char newtitle[] = "Fresh";
      set_file("# a comment\n\nTitle=Old      # what it was\nUnknown=keepme\nSize=20\n");
      v[K_TITLE].v.string = newtitle;
      v[K_TITLE].length = strlen(newtitle);
      ok(parse_readfile("f", "f", keys, v) != 0, "rewrite succeeds");
      ok(strstr(written(), "Title=Fresh") != NULL, "the supplied value is written");
      ok(strstr(written(), "# a comment") != NULL, "a whole-line comment survives");
      ok(strstr(written(), "# what it was") != NULL, "a trailing comment survives");
      ok(strstr(written(), "Unknown=keepme") != NULL, "an unknown key survives untouched");
      ok(strstr(written(), "Size=20") != NULL, "a key with no supplied value keeps its own");
      ok(strstr(written(), "Old") == NULL, "the replaced value is gone");
   }
   {
      parserkeyvalue v[K_COUNT] = {0};
      set_file("Title=Old\n");
      ok(parse_readfile("f", "f", keys, v) != 0 && g_write_called,
         "a rewrite with no values supplied still writes");
      ok(strstr(written(), "Title=Old") != NULL, "and reproduces the file it read");
      parse_releasekeyvalues(v, K_COUNT);
   }
   {
      parserkeyvalue v[K_COUNT] = {0};
      char t[] = "New";
      set_file("Title=Old\n");
      v[K_TITLE].v.string = t; v[K_TITLE].length = 3;
      g_write_fails = true;
      ok(parse_readfile("f", "f", keys, v) == 0, "a failed write is reported");
      g_write_fails = false;
   }
   {
      parserkeyvalue v[K_COUNT] = {0};
      set_file("Title=Old\r\nSize=7\r\n");
      (void)parse_readfile("f", "f", keys, v);
      ok(strstr(written(), "\r\n") != NULL, "CRLF line endings survive a rewrite");
      parse_releasekeyvalues(v, K_COUNT);
   }
   {
      parserkeyvalue v[K_COUNT] = {0};
      uint8_t raw[2] = { 0xDEu, 0xADu };
      set_file("Inquiry=0000\n");
      v[K_INQUIRY].v.string = (char *)raw;
      v[K_INQUIRY].length = 2;
      (void)parse_readfile("f", "f", keys, v);
      ok(strstr(written(), "DEAD") != NULL, "NUMSTRING is written back as hex");
   }

   puts("== edges ==");
   {
      parserkeyvalue v[K_COUNT] = {0};
      set_file("Title=NoNewlineAtEnd");
      (void)parse_readfile("f", 0, keys, v);
      ok(v[K_TITLE].length == 8, "a value at end-of-file with no newline is read");
      parse_releasekeyvalues(v, K_COUNT);
   }
   {
      parserkeyvalue v[K_COUNT] = {0};
      set_file("Title=\nSize=6\n");
      (void)parse_readfile("f", 0, keys, v);
      ok(v[K_TITLE].length == 0, "a key with an empty value yields nothing");
      ok(v[K_SIZE].length == 1 && *v[K_SIZE].v.integer == 6,
         "and parsing continues to the next key");
      parse_releasekeyvalues(v, K_COUNT);
   }
   {
      parserkeyvalue v[K_COUNT] = {0};
      set_file("");
      ok(parse_readfile("f", 0, keys, v) == 0, "an empty file fails");
      parse_releasekeyvalues(v, K_COUNT);
   }

   printf("\n%d checks, %d failures\n", checks, fails);
   printf(fails ? "FILEPARSER TESTS FAILED\n" : "FILEPARSER TESTS PASSED\n");
   return fails != 0;
}
