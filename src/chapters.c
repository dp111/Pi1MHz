/*
 * chapters.c - parse and look up a side's chapter table (see chapters.h).
 */
#include "chapters.h"
#include "lib/textparse.h"

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* The next whitespace-separated word of line[*pos..end), or 0 length. */
static size_t word(const char *line, size_t end, size_t *pos, size_t *start)
{
   size_t p = *pos;
   while (p < end && is_space(line[p]))
      p++;
   *start = p;
   while (p < end && !is_space(line[p]) && line[p] != '#')
      p++;
   *pos = p;
   return p - *start;
}

bool chapters_parse(const char *text, size_t len, chapter_table_t *t, unsigned *bad_line)
{
   size_t pos = 0;
   unsigned line_no = 0;

   t->count = 0;
   while (pos < len) {
      size_t eol = pos;
      while (eol < len && text[eol] != '\n')
         eol++;
      line_no++;

      /* The line without its comment. */
      size_t end = pos;
      while (end < eol && text[end] != '#')
         end++;

      size_t p = pos, s1, s2, s3;
      size_t n1 = word(text, end, &p, &s1);
      if (n1 != 0u) {
         size_t n2 = word(text, end, &p, &s2);
         size_t n3 = word(text, end, &p, &s3);
         uint32_t number, first;
         (void)s3;
         if (n2 == 0u || n3 != 0u
             || !text_parse_u32(&text[s1], n1, CHAPTERS_MAX - 1u, &number)
             || !text_parse_u32(&text[s2], n2, CHAPTERS_PICTURE_MAX, &first)
             || first == 0u
             || t->count == CHAPTERS_MAX
             || (t->count != 0u && (number <= t->number[t->count - 1u]
                                    || first <= t->first[t->count - 1u])))
            goto bad;
         t->number[t->count] = (uint8_t)number;
         t->first[t->count] = first;
         t->count++;
      }
      pos = eol + 1u;
   }
   return true;

bad:
   t->count = 0;
   if (bad_line != NULL)
      *bad_line = line_no;
   return false;
}

int chapters_find(const chapter_table_t *t, uint32_t number)
{
   for (unsigned i = 0; i < t->count; i++)
      if (t->number[i] == number)
         return (int)i;
   return -1;
}

int chapters_at(const chapter_table_t *t, uint32_t picture)
{
   int at = -1;
   for (unsigned i = 0; i < t->count && t->first[i] <= picture; i++)
      at = (int)i;
   return at;
}
