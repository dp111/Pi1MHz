/* Fuzz webserver.c's untrusted-input parsers with hostile bytes under
 * ASan/UBSan.  Every input and output buffer is heap-allocated at its
 * EXACT size, so any read or write one byte past what the parser was
 * given becomes an ASan fault instead of a silent pass.  On top of the
 * memory-safety net, the security-relevant postconditions are asserted:
 *   - ws_url_decode output carries no control bytes (header smuggling)
 *   - ws_normalize_path output is always absolute, canonical, and free
 *     of backslashes / double slashes / trailing slashes
 *   - a true return from dav_url_to_sdpath never yields a ".." segment
 *   - every parser NUL-terminates within the buffer it was given
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "md5.h"

#include "ws_defines.inc"
#include "ws_parsers.inc"

/* Deterministic PRNG (xorshift32), matching the other suites. */
static uint32_t seed = 0x9e3779bu;
static uint32_t rnd(void)
{
   seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
   return seed;
}

/* Bytes biased towards parser separators; NUL excluded (string inputs). */
static char rnd_char(void)
{
   static const char tasty[] =
      "GET POST PUT /%2e%5c./\\\"'=;,:?&x-AaFf09\r\n\t Host"
      "boundary=filename=nonce~#[]()<>@";
   uint32_t r = rnd();
   if ((r & 7u) == 0u) {
      uint8_t b = (uint8_t)(r >> 8);
      return (b == 0u) ? 'A' : (char)b;
   }
   return tasty[(r >> 8) % (sizeof tasty - 1u)];
}

/* Heap string of exactly len chars + NUL, hostile content. */
static char *rnd_str(size_t len)
{
   char *s = malloc(len + 1u);
   size_t i;
   assert(s != NULL);
   for (i = 0u; i < len; ++i)
      s[i] = rnd_char();
   s[len] = '\0';
   return s;
}

/* Exact-size heap buffer for outputs (no slack for overruns to hide in),
   pre-filled with a non-NUL byte.  A parser that reports success without
   NUL-terminating its output then makes the strlen() check below run off
   the end of the allocation - an ASan fault - instead of quietly passing
   on a zero byte that happened to be there. */
static char *outbuf(size_t sz)
{
   char *b = malloc(sz);
   assert(b != NULL);
   memset(b, 0xAB, sz);
   return b;
}

static void no_ctrl(const char *s)
{
   for (; *s != '\0'; ++s)
      assert((unsigned char)*s >= 0x20u && (unsigned char)*s != 0x7Fu);
}

static void assert_canonical_path(const char *p, size_t osz)
{
   size_t n = strlen(p);
   assert(n < osz);
   if (osz >= 2u) {
      assert(p[0] == '/');
      assert(strstr(p, "//") == NULL);
      assert(strchr(p, '\\') == NULL);
      if (n > 1u)
         assert(p[n - 1u] != '/');
   }
}

static const char *const header_names[] = {
   "Host", "Content-Length", "Content-Type", "Authorization",
   "Transfer-Encoding", "Destination", "Depth", "Expect", "Connection", "X"
};
static const char *const digest_keys[] = {
   "username", "realm", "nonce", "uri", "response", "nc", "cnonce",
   "qop", "algorithm", "opaque"
};

int main(void)
{
   int iter;

   /* ---- string parsers ---- */
   for (iter = 0; iter < 60000; ++iter) {
      size_t len = rnd() % 600u;
      char  *s   = rnd_str(len);

      /* URL decoding: output free of control bytes, terminated in-buffer */
      {
         size_t dsz = 1u + rnd() % 520u;
         char  *d   = outbuf(dsz);
         (void)ws_url_decode(s, d, dsz);
         assert(strlen(d) < dsz);
         no_ctrl(d);
         free(d);
      }

      /* Path pipeline: normalise keeps its invariants at any osz;
         a true dav_url_to_sdpath never lets traversal through. */
      {
         size_t osz = 1u + rnd() % 520u;
         char  *o   = outbuf(osz);
         ws_normalize_path(s, o, osz);
         assert_canonical_path(o, osz);
         free(o);

         o = outbuf(WS_PATH_MAX);
         if (dav_url_to_sdpath(s, o, WS_PATH_MAX)) {
            assert_canonical_path(o, WS_PATH_MAX);
            assert(ws_path_is_safe(o));
            no_ctrl(o);
         }
         (void)dav_destination_sdpath(s, o, WS_PATH_MAX);
         free(o);
         (void)ws_path_is_safe(s);
      }

      /* Request line */
      {
         size_t msz = 1u + rnd() % 12u;
         size_t psz = 1u + rnd() % 80u;
         char  *m   = outbuf(msz);
         char  *p   = outbuf(psz);
         /* The method is terminated on every path; the path buffer is
            only written once a space has been seen, so it is only
            meaningful (and only read by process_request) on success. */
         bool got = ws_parse_request_line(s, m, msz, p, psz);
         assert(strlen(m) < msz);
         if (got)
            assert(strlen(p) < psz && p[0] != '\0');
         free(m); free(p);
      }

      /* Header lookup within an exact limit */
      {
         size_t osz = 1u + rnd() % 64u;
         char  *o   = outbuf(osz);
         const char *name = header_names[rnd() % 10u];
         if (ws_find_header(s, len, name, o, osz))
            assert(strlen(o) < osz);
         free(o);
      }

      /* Multipart fields */
      {
         size_t osz = 1u + rnd() % 40u;
         char  *o   = outbuf(osz);
         if (ws_extract_boundary(s, o, osz))
            assert(strlen(o) < osz);
         if (ws_extract_filename(s, o, osz))
            assert(strlen(o) < osz);
         free(o);
      }

      /* Range header: a WS_RANGE_OK window always sits inside the file */
      {
         uint32_t size = rnd();
         if ((rnd() & 3u) == 0u)
            size &= 0xFFu;              /* exercise tiny / zero sizes too */
         uint32_t start = 0xDEADBEEFu, rlen = 0xDEADBEEFu;
         if (ws_parse_range(s, size, &start, &rlen) == WS_RANGE_OK) {
            assert(size > 0u);
            assert(rlen >= 1u);
            assert(start < size);
            assert((uint64_t)start + rlen <= size);
         }
      }

      /* Digest fields + uri binding + hex compare */
      {
         size_t osz = 1u + rnd() % 48u;
         char  *o   = outbuf(osz);
         if (ws_digest_field(s, digest_keys[rnd() % 10u], o, osz))
            assert(strlen(o) < osz);
         free(o);
      }
      {
         char *q = rnd_str(rnd() % 40u);
         (void)ws_digest_uri_matches(s, q, ((rnd() & 1u) != 0u) ? q : NULL);
         free(q);

         md5_hex_t e;
         memcpy(e, "00112233445566778899aabbccddeeff", MD5_HEX_LEN);
         (void)ws_hex_eq_ci(e, s);
      }

      /* Dates + XML tag scan */
      {
         uint16_t fd, ft;
         if (dav_parse_http_date(s, len, &fd, &ft)) {
            assert(((fd >> 9) + 1980u) <= 2107u);
            assert((ft >> 11) <= 23u);
         }
         (void)dav_memfind(s, len, "Win32LastModifiedTime>");
      }

      /* Small helpers */
      {
         char *t = rnd_str(rnd() % 12u);
         const char *b = ws_basename(s);
         assert(b >= s && b <= s + len);
         (void)ws_is_root(s);
         (void)ws_stricmp(s, t);
         (void)ws_prefix(t, s);
         (void)ws_strcasestr(s, t);
         (void)ws_hexval((char)(rnd() & 0xFFu));
         free(t);
      }
      {
         size_t osz = 1u + rnd() % 40u;
         char  *o   = outbuf(osz);
         ws_parent_path(s, o, osz);
         assert(strlen(o) < osz);
         free(o);
      }

      free(s);
   }
   puts("string fuzz: 60k hostile inputs across all parsers");

   /* ---- structured near-valid inputs to reach the deep paths ---- */
   for (iter = 0; iter < 20000; ++iter) {
      char  hdr[512];
      char *j1 = rnd_str(rnd() % 24u);
      char *j2 = rnd_str(rnd() % 24u);
      char *j3 = rnd_str(rnd() % 24u);
      int   n  = snprintf(hdr, sizeof hdr,
                          "%s /%s HTTP/1.1\r\n"
                          "Host: %s\r\n"
                          "Content-Type: multipart/form-data; boundary=%s\r\n"
                          "Authorization: Digest username=\"%s\", nonce=%s, "
                          "uri=\"/%s\", response=\"%s\"\r\n"
                          "\r\n",
                          j1, j2, j3, j2, j1, j3, j2, j1);
      size_t hl = (n < 0) ? 0u : ((size_t)n < sizeof hdr ? (size_t)n
                                                         : sizeof hdr - 1u);
      char m[8], p[48], v[64];
      int  he = ws_find_header_end(hdr, hl);
      assert(he <= (int)hl);
      (void)ws_parse_request_line(hdr, m, sizeof m, p, sizeof p);
      if (ws_find_header(hdr, hl, "Content-Type", v, sizeof v)) {
         char b[24];
         (void)ws_extract_boundary(v, b, sizeof b);
      }
      if (ws_find_header(hdr, hl, "Authorization", v, sizeof v)) {
         char f[40];
         size_t k;
         for (k = 0u; k < 10u; ++k)
            (void)ws_digest_field(v, digest_keys[k], f, sizeof f);
      }
      free(j1); free(j2); free(j3);
   }
   puts("structured fuzz: 20k near-valid request heads");

   /* ---- binary buffers with no NUL terminator ---- */
   for (iter = 0; iter < 20000; ++iter) {
      size_t blen = rnd() % 512u;
      uint8_t *b  = malloc(blen ? blen : 1u);
      size_t i;
      assert(b != NULL);
      for (i = 0u; i < blen; ++i)
         b[i] = (uint8_t)rnd();
      {
         int e = ws_find_header_end((const char *)b, blen);
         assert(e == -1 || (size_t)e <= blen);
      }
      {
         uint8_t need[8];
         size_t  nlen = rnd() % (sizeof need + 1u);
         int     at;
         for (i = 0u; i < nlen; ++i)
            need[i] = (uint8_t)rnd();
         at = ws_memfind(b, blen, need, nlen);
         assert(at == -1 || (size_t)at + nlen <= blen);
      }
      {
         uint16_t fd, ft;
         (void)dav_parse_http_date((const char *)b, blen, &fd, &ft);
         (void)dav_memfind((const char *)b, blen, "DAV:");
      }
      free(b);
   }
   puts("binary fuzz: 20k unterminated buffers");

   puts("parser fuzz passed: no overruns, invariants held");
   return 0;
}
