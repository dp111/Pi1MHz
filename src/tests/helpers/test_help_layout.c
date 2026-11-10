/* The help screen's lines must fit the Beeb's 40 columns with worst-case
   values substituted (it is printed paged, so its length is not capped).
   This renders the template exactly as helpers_screen_setup does and
   measures it. */
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
      /* a line that fills the last column has already wrapped the Beeb's
         cursor when its CR LF comes: 40 characters are two rows */
      rows += (unsigned)(w / HELPERS_HELP_COLUMNS) + 1u;
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
            0x88u, 136, HELPERS_USER_ROM_NONE, 65, 4, 2);
   rows = rows_of(buf, &widest, &over);
   printf("typical: %u rows, widest %u\n", rows, widest);
   ok(over == 0, "typical: every line fits 40 columns");
   /* 26 lines; this dirty-tree describe makes the version line exactly 40
      columns, which costs a row.  A release's shorter one gives 26. */
   ok(rows == 27, "typical: 27 rows with a 40-column version line (printed paged)");

   /* Worst case: every substituted value at its widest.  The revision code
      is shown without its flag bits, so it is at most 6 hex digits, and the
      SoC throttles at 85C, so the temperature stays under 100.0. */
   static const char wdate[] = "2026-12-31 23:59:59";
   snprintf(buf, sizeof buf, HELPERS_HELP_FMT("D"),
            "V99.99-9999-gffffffff-dirty.ffffffff", "ffffff 9999MHz", 99L, 9L,
            wdate + 2, wdate + 5, wdate + 8, wdate + 11,
            0xFFu, 255, HELPERS_USER_ROM_NONE, 255, 255, 255);
   rows = rows_of(buf, &widest, &over);
   printf("worst:   %u rows, widest %u\n", rows, widest);
   ok(over <= 1, "worst case: only the version line may wrap");
   ok(rows <= 27u, "worst case, no user ROMs: 27 rows at most (the version line may wrap)");
   unsigned none_rows = rows;

   /* All six user ROMs, each name longer than fits: one row each, cut a
      column short of 40, replacing the one "Your ROM" row. */
   {
      static const char longname[] = "ROM10-a-very-long-name-for-a-sideways-rom-image.rom";
      char roms[6u * 42u + 1u];
      size_t len = 0;
      for (unsigned i = 0; i < 6u; i++)
         len += (size_t)snprintf(roms + len, sizeof roms - len, HELPERS_USER_ROM_LINE, 10u + i, longname);
      snprintf(buf, sizeof buf, HELPERS_HELP_FMT("D"),
               "V99.99-9999-gffffffff-dirty.ffffffff", "ffffff 9999MHz", 99L, 9L,
               wdate + 2, wdate + 5, wdate + 8, wdate + 11,
               0xFFu, 255, roms, 255, 255, 255);
      rows = rows_of(buf, &widest, &over);
      printf("six ROMs: %u rows, widest %u\n", rows, widest);
      ok(over <= 1, "six user ROMs: none of their lines wraps");
      ok(strstr(roms, "\r\n") - roms == (int)HELPERS_HELP_COLUMNS - 1, "a cut name stops a column short of the edge");
      ok(rows == none_rows + 5u, "six user ROMs: one row each, in place of the one");
      ok(strlen(buf) < 1536u, "fits the 1.5 KB copy at &FFE000");
   }

   printf("\n%d checks, %d failures\n", checks, fails);
   return fails ? 1 : 0;
}
