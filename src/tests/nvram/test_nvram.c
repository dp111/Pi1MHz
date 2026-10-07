/*
 * Host tests for sdio_cyw43_condense_nvram() and the size of the buffer
 * sdio_runtime_boot_firmware() condenses into (Review 2026-10-06 D3).
 *
 * Both functions are extracted VERBATIM from src/wifi/sdio.c by
 * extract.awk (see run_tests.sh).  Every case mallocs exactly
 * sdio_cyw43_condensed_nvram_capacity(len) bytes, as the firmware does,
 * and runs under ASan, so a write past the allocation aborts the run.
 *
 * The output is also checked against an independent model of the format:
 * each line with '\r' dropped and everything from '#' removed, kept only if
 * something is left, NUL-terminated; then one more NUL, then NUL padding to
 * a multiple of 4.  The model is not compared where a '#' follows content
 * on the same line: the condenser then drops that line's NUL, joining it to
 * the next (none of the shipped .txt files has such a line).  The size
 * bound is checked for every input regardless.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvram.inc"

static unsigned g_checks;
static unsigned g_failures;

#define CHECK(cond, ...) do { \
   g_checks++; \
   if (!(cond)) { \
      g_failures++; \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__); \
      printf("\n"); \
   } \
} while (0)

static uint32_t model_condense(const uint8_t *in, uint32_t length, uint8_t *out,
                               bool *trailing_comment)
{
   uint32_t n = 0u;
   uint32_t i = 0u;

   *trailing_comment = false;
   while (i < length) {
      uint32_t line_start = n;
      bool comment = false;

      for (; i < length && in[i] != '\n'; ++i) {
         if (in[i] == '#') {
            if (!comment && n != line_start)
               *trailing_comment = true;
            comment = true;
         }
         else if (in[i] != '\r' && !comment)
            out[n++] = in[i];
      }
      if (i < length)
         ++i;                                   /* the '\n' */
      if (n != line_start)
         out[n++] = '\0';
   }
   out[n++] = '\0';
   while ((n & 3u) != 0u)
      out[n++] = '\0';
   return n;
}

static void check_one(const uint8_t *in, uint32_t length, const char *what)
{
   uint32_t capacity = sdio_cyw43_condensed_nvram_capacity(length);
   uint8_t *buf = malloc(capacity);
   uint8_t *want = malloc((size_t)length + 16u);
   uint32_t got_len;
   uint32_t want_len;
   bool trailing_comment;

   if (buf == NULL || want == NULL) {
      printf("FAIL: out of memory\n");
      exit(1);
   }
   memcpy(buf, in, length);
   got_len = sdio_cyw43_condense_nvram(buf, length);   /* ASan aborts on overflow */
   want_len = model_condense(in, length, want, &trailing_comment);

   CHECK(got_len <= capacity, "%s len=%u: wrote %u into %u", what,
         (unsigned)length, (unsigned)got_len, (unsigned)capacity);
   CHECK((got_len & 3u) == 0u, "%s len=%u: length %u not a multiple of 4", what,
         (unsigned)length, (unsigned)got_len);
   CHECK(trailing_comment
         || (got_len == want_len && memcmp(buf, want, want_len) == 0),
         "%s len=%u: output differs from the model (got %u, want %u)", what,
         (unsigned)length, (unsigned)got_len, (unsigned)want_len);
   free(buf);
   free(want);
}

static void check_str(const char *text, const char *expected, uint32_t expected_len)
{
   uint32_t length = (uint32_t)strlen(text);
   uint8_t *buf = malloc(sdio_cyw43_condensed_nvram_capacity(length));
   uint32_t got_len;

   if (buf == NULL) {
      printf("FAIL: out of memory\n");
      exit(1);
   }
   memcpy(buf, text, length);
   got_len = sdio_cyw43_condense_nvram(buf, length);
   CHECK(got_len == expected_len && memcmp(buf, expected, expected_len) == 0,
         "vector \"%s\": got %u bytes, want %u", text, (unsigned)got_len,
         (unsigned)expected_len);
   free(buf);
}

int main(void)
{
   static const uint8_t alphabet[] = { 'a', '=', '1', '\n', '\r', '#', ' ' };
   uint8_t in[96];
   uint32_t seed = 12345u;

   /* Known vectors, including the shapes the shipped .txt files use. */
   check_str("a=1\nb=2\n", "a=1\0b=2\0\0\0\0\0", 12u);
   check_str("# header\r\na=1\r\n\r\n#c\nb=2\n", "a=1\0b=2\0\0\0\0\0", 12u);
   check_str("", "\0\0\0\0", 4u);

   /* The D3 shape: no trailing newline and len % 4 == 3.  "abc" condenses
      to "abc\0" + "\0" + 3 pad = 8 bytes, one more than len + 4. */
   check_str("abc", "abc\0\0\0\0\0", 8u);

   /* Every length up to 80, one line of plain bytes, with and without a
      trailing newline (the newline-less case is the worst case). */
   for (uint32_t len = 0u; len <= 80u; ++len) {
      memset(in, 'x', len);
      check_one(in, len, "no-newline");
      if (len > 0u) {
         in[len - 1u] = '\n';
         check_one(in, len, "newline");
      }
   }

   /* Random mixes of key/value bytes, newlines, CRs and comments. */
   for (unsigned iter = 0u; iter < 50000u; ++iter) {
      uint32_t len;

      seed = seed * 1103515245u + 12345u;
      len = (seed >> 16) % 81u;
      for (uint32_t i = 0u; i < len; ++i) {
         seed = seed * 1103515245u + 12345u;
         in[i] = alphabet[(seed >> 16) % sizeof alphabet];
      }
      check_one(in, len, "random");
   }

   printf("%u checks, %u failures\n", g_checks, g_failures);
   return g_failures == 0u ? 0 : 1;
}
