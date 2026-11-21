/* Host tests for src/chapters.c (see chapters.h for the file format). */
#include <stdio.h>
#include <string.h>

#include "chapters.h"

static int failures, checks;

#define CHECK(cond, ...) do { \
      checks++; \
      if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                     printf(__VA_ARGS__); printf("\n"); } \
   } while (0)

static bool parse(const char *text, chapter_table_t *t, unsigned *bad)
{
   *bad = 0;
   return chapters_parse(text, strlen(text), t, bad);
}

static bool parse_file(const char *name, chapter_table_t *t)
{
   static char text[4096];
   char path[512];
   unsigned bad = 0;
   snprintf(path, sizeof path, "%s/%s", HERE, name);
   FILE *f = fopen(path, "rb");
   if (f == NULL)
      return false;
   size_t n = fread(text, 1, sizeof text, f);
   fclose(f);
   bool ok = chapters_parse(text, n, t, &bad);
   CHECK(ok, "%s rejected at line %u", name, bad);
   return ok;
}

static void test_real_tables(void)
{
   chapter_table_t t;

   if (parse_file("bgb_s1.txt", &t)) {
      CHECK(t.count == 5u, "BGB S1: %u chapters", t.count);
      CHECK(chapters_at(&t, 1u) == 0 && chapters_at(&t, 3092u) == 0
            && chapters_at(&t, 3093u) == 1 && chapters_at(&t, 53450u) == 4,
            "BGB S1 boundaries");
      int i = chapters_find(&t, 3u);
      CHECK(i >= 0 && t.first[i] == 34798u, "BGB S1 chapter 03 starts at 34798");
   }
   if (parse_file("bgb_s2.txt", &t)) {
      CHECK(t.count == 4u && t.number[0] == 5u, "BGB S2 runs on from chapter 05");
      CHECK(chapters_find(&t, 0u) < 0, "BGB S2 has no chapter 00");
      CHECK(t.number[chapters_at(&t, 28478u)] == 6u && t.number[chapters_at(&t, 28479u)] == 7u,
            "BGB S2 06/07 boundary");
   }
   if (parse_file("national_b.txt", &t)) {
      CHECK(t.count == 8u, "National B: %u chapters", t.count);
      int i = chapters_find(&t, 7u);
      CHECK(i >= 0 && t.first[i] == 77999u, "National B chapter 07 starts at 77999");
      CHECK(t.number[chapters_at(&t, 89926u)] == 7u, "National B's last picture is in 07");
      CHECK(chapters_last(&t, chapters_find(&t, 0u)) == 151u
            && chapters_last(&t, chapters_find(&t, 6u)) == 77998u
            && chapters_last(&t, chapters_find(&t, 7u)) == 0u, "National B chapter ends");
   }
}

static void test_format(void)
{
   chapter_table_t t;
   unsigned bad;

   CHECK(parse("", &t, &bad) && t.count == 0u, "empty file");
   CHECK(parse("# only\n\n   \r\n", &t, &bad) && t.count == 0u, "comments and blanks");
   CHECK(parse("1 10\r\n2\t20 # x\n3 30", &t, &bad) && t.count == 3u
         && t.first[2] == 30u, "CRLF, tab, comment, no final newline");
   CHECK(parse("  05   00100   \n", &t, &bad) && t.number[0] == 5u && t.first[0] == 100u,
         "leading zeros and spaces");
   CHECK(parse("1 10#x\n", &t, &bad) && t.count == 1u, "comment touching the number");
   CHECK(parse("79 5\n", &t, &bad), "chapter 79");

   static const struct { const char *text; unsigned line; } bad_cases[] = {
      { "80 5\n", 1 },                    /* past 79 */
      { "1 0\n", 1 },                     /* picture 0 */
      { "1\n", 1 },                       /* no picture */
      { "1 10 20\n", 1 },                 /* extra word */
      { "1 10\n1 20\n", 2 },              /* chapter repeated */
      { "2 10\n1 20\n", 2 },              /* chapters out of order */
      { "1 20\n2 20\n", 2 },              /* pictures not ascending */
      { "# ok\n1 x\n", 2 },
      { "-1 10\n", 1 },
      { "1 1000000\n", 1 },
   };
   for (unsigned i = 0; i < sizeof bad_cases / sizeof bad_cases[0]; i++) {
      bool ok = parse(bad_cases[i].text, &t, &bad);
      CHECK(!ok && t.count == 0u && bad == bad_cases[i].line,
            "case %u: ok=%d count=%u line %u, want rejected at %u",
            i, ok, t.count, bad, bad_cases[i].line);
   }
}

static void test_lookup(void)
{
   chapter_table_t t;
   unsigned bad;
   (void)parse("3 100\n4 200\n", &t, &bad);
   CHECK(chapters_at(&t, 99u) == -1, "before the first chapter");
   CHECK(chapters_at(&t, 100u) == 0 && chapters_at(&t, 199u) == 0
         && chapters_at(&t, 200u) == 1 && chapters_at(&t, 999999u) == 1, "ranges");
   CHECK(chapters_find(&t, 4u) == 1 && chapters_find(&t, 5u) == -1, "find");
   CHECK(chapters_last(&t, 0) == 199u && chapters_last(&t, 1) == 0u
         && chapters_last(&t, -1) == 0u, "last picture");
}

int main(void)
{
   test_real_tables();
   test_format();
   test_lookup();
   printf("%d checks, %d failed\n", checks, failures);
   if (failures == 0)
      printf("CHAPTERS TESTS PASSED\n");
   return failures ? 1 : 0;
}
