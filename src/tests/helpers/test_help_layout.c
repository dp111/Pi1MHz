/* The help screen must fit the Beeb's 40 x 25 text screen with worst-case
   values substituted, and leave room for future helpers.  This renders the
   template exactly as helpers_screen_setup does and measures it. */
#include <stdio.h>
#include <string.h>
#include "help_fmt.inc"

static int checks, fails;
static void ok(int c, const char *what) { checks++; if (!c) { fails++; printf("  FAIL: %s\n", what); } else printf("  ok: %s\n", what); }

static unsigned rows_of(const char *text, unsigned *widest, unsigned *over)
{
   unsigned rows = 0; *widest = 0; *over = 0;
   const char *p = text;
   while (*p) {
      const char *e = strstr(p, "\r\n");
      size_t w = e ? (size_t)(e - p) : strlen(p);
      if (w > *widest) *widest = (unsigned)w;
      if (w > HELPERS_HELP_COLUMNS) (*over)++;
      rows += (w == 0) ? 1u : (unsigned)((w + HELPERS_HELP_COLUMNS - 1u) / HELPERS_HELP_COLUMNS);
      if (!e) break;
      p = e + 2;
   }
   return rows;
}

int main(void)
{
   char buf[2048];
   unsigned widest, over, rows;

   /* Typical: a 33-character git describe, real addresses. */
   static const char date[] = "2026-09-07 14:07:42";
   snprintf(buf, sizeof buf, HELPERS_HELP_FMT("R"),
            "V1.30-214-gd5dcf57-dirty.93616fe3", "902120 1000MHz", 46L, 2L,
            date + 2, date + 5, date + 8, date + 11,
            0x88u, 136, 65, 4, 2);
   rows = rows_of(buf, &widest, &over);
   printf("typical: %u rows, widest %u\n", rows, widest);
   ok(over == 0, "typical: every line fits 40 columns");
   ok(rows == 24, "typical: 24 rows (1 free for future helpers)");

   /* Worst case: every substituted value at its widest.  A revision code is
      at most 7 hex digits (24 bits of new-style code, plus the warranty bit
      at 25), and the SoC throttles at 85C, so the temperature stays under
      100.0. */
   static const char wdate[] = "2026-12-31 23:59:59";
   snprintf(buf, sizeof buf, HELPERS_HELP_FMT("D"),
            "V99.99-9999-gffffffff-dirty.ffffffff", "2ffffff 9999MHz", 99L, 9L,
            wdate + 2, wdate + 5, wdate + 8, wdate + 11,
            0xFFu, 255, 255, 255, 255);
   rows = rows_of(buf, &widest, &over);
   printf("worst:   %u rows, widest %u\n", rows, widest);
   ok(over <= 1, "worst case: only the version line may wrap");
   ok(rows <= HELPERS_HELP_ROWS, "worst case: still within 25 rows");
   ok(strlen(buf) < 1024u, "fits the 1 KB copy at &FFE000");

   printf("\n%d checks, %d failures\n", checks, fails);
   return fails ? 1 : 0;
}
