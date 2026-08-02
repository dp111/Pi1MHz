/* Host tests for webserver.c's Transfer-Encoding: chunked body parser
 * (dav_put_consume_chunked) - the state machine that decodes RFC 9112
 * chunk framing straight off hostile TCP segments during a WebDAV PUT.
 *
 * The parser, the real ws_conn_t it operates on and the tunables are
 * extracted VERBATIM from a copy of webserver.c by extract.awk (see
 * run_tests.sh).  Its three callees that touch the SD card / TCP
 * (dav_put_write_bytes, dav_put_finish, ws_error) are stubbed here to
 * record what the parser decided, so every decision is observable:
 * decoded bytes, completion, and the HTTP error it chose.
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ws_defines.inc"   /* WS_PATH_MAX / WS_FILE_CHUNK / drain cap */
#include "ws_conn_stubs.h"  /* FIL / framebuffer types the struct embeds */
#include "ws_conn.inc"      /* conn_state_t .. ws_conn_t, extracted */

/* ---- stub sinks: record instead of writing to SD / TCP ---- */
static uint8_t sink[1u << 16];
static size_t  sink_len;
static int     finish_calls;
static int     err_calls;
static int     err_status;
static bool    write_fail;

static bool dav_put_write_bytes(ws_conn_t *c, const uint8_t *data, size_t len)
{
   (void)c;
   if (write_fail)
      return false;
   assert(sink_len + len <= sizeof sink);
   memcpy(sink + sink_len, data, len);
   sink_len += len;
   return true;
}

static bool dav_put_finish(ws_conn_t *c)
{
   (void)c;
   finish_calls++;
   return true;
}

static bool ws_error(ws_conn_t *c, int status, const char *stext,
                     const char *msg)
{
   (void)c; (void)stext; (void)msg;
   err_calls++;
   err_status = status;
   return true;
}

#include "ws_chunked.inc"   /* dav_put_consume_chunked, extracted verbatim */

static ws_conn_t C;

static void reset(void)
{
   memset(&C, 0, sizeof C);       /* DAV_CHUNK_SIZE == 0 */
   sink_len = 0u;
   finish_calls = 0;
   err_calls = 0;
   err_status = 0;
   write_fail = false;
}

static bool feed(const char *s, size_t *consumed)
{
   return dav_put_consume_chunked(&C, (const uint8_t *)s, strlen(s), consumed);
}

static int checks, fails;
static void ok(int cond, const char *what)
{
   checks++;
   if (!cond) { fails++; printf("  FAIL: %s\n", what); }
   else         printf("  ok: %s\n", what);
}

static int sink_is(const char *want)
{
   return sink_len == strlen(want) && memcmp(sink, want, sink_len) == 0;
}

/* Deterministic PRNG (xorshift32), matching the other suites. */
static uint32_t seed = 0x2f6e2b1u;
static uint32_t rnd(void)
{
   seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
   return seed;
}

/* Build a valid chunked stream carrying `pay` into `out`. */
static size_t build_stream(uint8_t *out, size_t cap,
                           const uint8_t *pay, size_t plen)
{
   size_t o = 0u, off = 0u;
   while (off < plen) {
      size_t n = 1u + rnd() % 700u;
      int    w;
      if (n > plen - off)
         n = plen - off;
      if ((rnd() & 1u) != 0u)
         w = snprintf((char *)out + o, cap - o, "%zx\r\n", n);
      else
         w = snprintf((char *)out + o, cap - o, "%zX;ext=%u\r\n",
                      n, (unsigned)(rnd() & 0xFu));
      assert(w > 0);
      o += (size_t)w;
      assert(o + n + 2u < cap);
      memcpy(out + o, pay + off, n);
      o += n; off += n;
      out[o++] = '\r'; out[o++] = '\n';
   }
   {
      int w = snprintf((char *)out + o, cap - o, "0\r\nX-Trail: %u\r\n\r\n",
                       (unsigned)(rnd() & 0xFFu));
      assert(w > 0);
      o += (size_t)w;
   }
   return o;
}

/* Feed `stream` in random-sized slices until the parser reports
   completion or an error (as conn_consume would stop then too). */
static void feed_sliced(const uint8_t *stream, size_t slen, size_t max_slice)
{
   size_t pos = 0u;
   while (pos < slen && finish_calls == 0 && err_calls == 0) {
      size_t n = 1u + rnd() % max_slice;
      size_t consumed = 0u;
      bool   r;
      if (n > slen - pos)
         n = slen - pos;
      r = dav_put_consume_chunked(&C, stream + pos, n, &consumed);
      assert(r);
      assert(consumed <= n);
      pos += consumed;
      if (consumed < n)
         break;                    /* parser stopped inside this slice */
   }
}

int main(void)
{
   size_t consumed;

   puts("== happy path ==");
   reset();
   ok(feed("5\r\nhello\r\n0\r\n\r\n", &consumed), "single chunk returns true");
   ok(sink_is("hello"), "payload decoded");
   ok(finish_calls == 1 && err_calls == 0, "finished exactly once");
   ok(consumed == strlen("5\r\nhello\r\n0\r\n\r\n"), "all bytes consumed");

   reset();
   ok(feed("4;name=v\r\nabcd\r\nA\r\n0123456789\r\n0\r\n\r\n", &consumed),
      "chunk-ext ignored");
   ok(sink_is("abcd0123456789") && finish_calls == 1, "two chunks decoded");

   reset();
   (void)feed("a\r\n0123456789\r\n0\r\n\r\n", &consumed);
   ok(sink_is("0123456789") && finish_calls == 1, "lowercase hex size");

   reset();
   {
      static const char S[] = "5\r\nhello\r\n0\r\n\r\n";
      size_t i;
      bool   r = true;
      for (i = 0u; i < strlen(S) && finish_calls == 0; ++i) {
         r = dav_put_consume_chunked(&C, (const uint8_t *)&S[i], 1u,
                                     &consumed);
         assert(r);
      }
      ok(sink_is("hello") && finish_calls == 1 && err_calls == 0,
         "byte-at-a-time feed decodes identically");
   }

   puts("== line ending tolerance ==");
   reset();
   (void)feed("5\nhello\r\n0\n\r\n", &consumed);
   ok(sink_is("hello") && finish_calls == 1,
      "bare-LF size and trailer lines accepted");
   /* The CRLF after chunk DATA is skipped blind (2 bytes, unvalidated):
      RFC-sloppy but harmless - framing stays in sync via the size lines.
      Documenting actual behaviour. */
   reset();
   (void)feed("5\r\nhelloXY0\r\n\r\n", &consumed);
   ok(sink_is("hello") && finish_calls == 1,
      "post-data 2 bytes skipped blind (documented)");
   /* Consequence: an all-LF stream desyncs at the data boundary and is
      rejected rather than mis-framed. */
   reset();
   (void)feed("5\nhello\n0\n\n", &consumed);
   ok(err_calls == 1 && err_status == 400 && finish_calls == 0,
      "LF-only data boundary rejected, not misparsed");

   puts("== malformed size lines ==");
   reset();
   ok(feed("zz\r\nhello", &consumed), "bad hex returns (response queued)");
   ok(err_calls == 1 && err_status == 400, "bad hex -> 400");
   ok(consumed == 4u, "consumed stops after the bad line");

   reset();
   (void)feed("\r\n0\r\n\r\n", &consumed);
   ok(err_calls == 1 && err_status == 400 && consumed == 2u,
      "empty size line -> 400");

   reset();
   (void)feed("5;ext\x01=v\r\nhello", &consumed);
   ok(sink_is("hello") && err_calls == 0,
      "junk inside chunk-ext ignored (after the ; separator)");

   reset();
   (void)feed("0x5\r\nhello", &consumed);
   ok(err_calls == 1 && err_status == 400, "0x prefix is not hex here");

   puts("== size-line overflow / cap ==");
   reset();
   /* Longer than dav_chunk_linebuf: must be REJECTED, not truncated -
      a truncated "000...0005" would otherwise parse as its prefix. */
   (void)feed("000000000000000000000000000005\r\nhello", &consumed);
   ok(err_calls == 1 && err_status == 400 && sink_len == 0u,
      "overlong size line rejected, never truncated");

   reset();
   (void)feed("80000000\r\n", &consumed);
   ok(err_calls == 1 && err_status == 400, "size > 0x7FFFFFFF rejected");

   reset();
   (void)feed("FFFFFFFFFFFFFFFF\r\n", &consumed);
   ok(err_calls == 1 && err_status == 400, "64-bit size rejected");

   reset();
   (void)feed("7FFFFFFF\r\nAB", &consumed);
   ok(err_calls == 0 && sink_is("AB"),
      "size 0x7FFFFFFF accepted, data streams");

   puts("== trailers ==");
   reset();
   (void)feed("3\r\nxyz\r\n0\r\nX-Trailer: ignored\r\nAnother: one\r\n\r\nEXTRA",
              &consumed);
   ok(sink_is("xyz") && finish_calls == 1 && err_calls == 0,
      "trailer field-lines discarded, then finish");
   ok(consumed == strlen("3\r\nxyz\r\n0\r\nX-Trailer: ignored\r\nAnother: one\r\n\r\n"),
      "bytes after the final CRLF left unconsumed (pipelining)");

   reset();
   (void)feed("0\r\n"
              "X-Very-Long-Trailer-Line-Exceeding-The-Line-Buffer: xxxxxxxx\r\n"
              "\r\n", &consumed);
   ok(finish_calls == 1 && err_calls == 0,
      "overlong trailer line discarded harmlessly");

   puts("== drain cap (unauthenticated PUT) ==");
   reset();
   C.dav_put_draining = true;
   C.dav_chunk_drained = WS_DRAIN_MAX_BYTES - 10u;
   (void)feed("A\r\n0123456789", &consumed);
   ok(err_calls == 0 && C.dav_chunk_drained == WS_DRAIN_MAX_BYTES,
      "drain exactly to the cap allowed");
   (void)feed("\r\n1\r\nZ", &consumed);   /* finish the CRLF, then 1 more */
   ok(err_calls == 1 && err_status == 413,
      "one byte past the cap -> 413");

   reset();
   C.dav_put_draining = true;
   C.dav_chunk_drained = WS_DRAIN_MAX_BYTES - 4u;
   (void)feed("A\r\n0123456789", &consumed);
   ok(err_calls == 1 && err_status == 413,
      "cumulative cap enforced mid-chunk");

   puts("== write failure ==");
   reset();
   write_fail = true;
   ok(feed("5\r\nhel", &consumed),
      "SD write failure keeps the connection alive (returns true)");
   ok(finish_calls == 0 && err_calls == 0 && consumed == 3u,
      "no finish, no extra error, data bytes unconsumed");

   puts("== property: random payloads, random slicing ==");
   {
      static uint8_t pay[3000];
      static uint8_t stream[32768];
      int iter;
      int bad = 0;
      for (iter = 0; iter < 200; ++iter) {
         size_t plen = rnd() % sizeof pay;
         size_t slen, i;
         for (i = 0u; i < plen; ++i)
            pay[i] = (uint8_t)(rnd() & 0xFFu);
         slen = build_stream(stream, sizeof stream, pay, plen);

         reset();
         feed_sliced(stream, slen, 37u);
         if (!(finish_calls == 1 && err_calls == 0
               && sink_len == plen && memcmp(sink, pay, plen) == 0))
            bad++;
      }
      ok(bad == 0, "200 random streams x random slices decode exactly");
   }

   puts("== fuzz: garbage streams (ASan/UBSan) ==");
   {
      static uint8_t g[300];
      static const char alpha[] = "0123456789abcdefABCDEF;\r\n xX-:%\"";
      int iter;
      for (iter = 0; iter < 20000; ++iter) {
         size_t glen = rnd() % sizeof g;
         size_t i;
         for (i = 0u; i < glen; ++i) {
            uint32_t r = rnd();
            g[i] = ((r & 7u) == 0u)
                      ? (uint8_t)(r >> 8)
                      : (uint8_t)alpha[(r >> 8) % (sizeof alpha - 1u)];
         }
         reset();
         feed_sliced(g, glen, 23u);
         assert(err_calls + finish_calls <= 1);
         assert(sink_len <= glen);
      }
      ok(1, "20k hostile streams survived (no crash, single verdict)");
   }

   printf("\n%d checks, %d failures\n", checks, fails);
   return fails ? 1 : 0;
}
