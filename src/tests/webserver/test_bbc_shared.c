/* Host tests for POST /bbc/shared in webserver.c: the request's framing checks, the 100 Continue,
 * the body gathered straight into shared memory across TCP segments, and the {"written":n}
 * answer.
 *
 * Extracted VERBATIM from a copy of webserver.c by extract.awk (see run_tests.sh):
 * bbc_shared_begin, bbc_body_take, bbc_body_done, bbc_shared_done,
 * ws_send_100_continue_if_expected and the header parser they use.  ramprobe_api_shared, lwIP
 * and the response builders are stubbed below to record what happened; the API's own checks
 * are tested in src/tests/ramprobe/test_api.c.
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ws_defines.inc"
#include "ws_conn_stubs.h"
#include "ws_conn.inc"

/* ---- lwIP: tcp_write appends to a captured "wire" ---- */
typedef uint16_t u16_t;
#define TCP_WRITE_FLAG_COPY 0x01
static char   wire[256];
static size_t wire_len;
static int tcp_write(struct tcp_pcb *p, const void *d, u16_t n, int f)
{
   (void)p; (void)f;
   assert(wire_len + n <= sizeof wire);
   memcpy(wire + wire_len, d, n);
   wire_len += n;
   return 0;
}
static int tcp_output(struct tcp_pcb *p) { (void)p; return 0; }

/* ---- the probe's API: shared memory of SHARED_SIZE bytes ---- */
#define RAMPROBE_API_OUT 64u
#define SHARED_SIZE 4096u
static uint8_t shared[SHARED_SIZE];
static int api_calls;
static int ramprobe_api_shared(const char *query, size_t len, uint8_t **dest, char *out, size_t n)
{
   api_calls++;
   unsigned long off = query != NULL && strncmp(query, "offset=", 7) == 0 ? strtoul(query + 7, NULL, 10) : SHARED_SIZE + 1u;
   if (off > SHARED_SIZE || len == 0u || len > SHARED_SIZE - off) {
      snprintf(out, n, "{\"error\":\"bad\"}\n");
      return 400;
   }
   *dest = shared + off;
   return 200;
}
static int ramprobe_api_body(const char *p, const char *q, const uint8_t *b, size_t l, char *o, size_t n)
{
   (void)p; (void)q; (void)b; (void)l; (void)o; (void)n;
   assert(!"a /bbc/shared body went to the poke route");
   return 0;
}

/* ---- the response builders ---- */
typedef struct { char data[128]; size_t len; } ws_strbuf_t;
static void sb_init(ws_strbuf_t *b) { memset(b, 0, sizeof *b); }
static void sb_puts(ws_strbuf_t *b, const char *s) { strncat(b->data, s, sizeof b->data - strlen(b->data) - 1u); }
static int status_sent;
static char body_sent[128];
static bool ws_finish_typed(ws_conn_t *c, int status, const char *t, const char *type, ws_strbuf_t *b)
{
   (void)c; (void)t; (void)type;
   status_sent = status;
   snprintf(body_sent, sizeof body_sent, "%s", b->data);
   return true;
}
static bool ws_error(ws_conn_t *c, int status, const char *t, const char *m)
{
   (void)c; (void)t; (void)m;
   status_sent = status;
   body_sent[0] = '\0';
   return true;
}
static bool ws_oom(ws_conn_t *c) { (void)c; return false; }
static bool bbc_answer(ws_conn_t *c, bool post, const char *q, int status, char *text)
{
   (void)c; (void)post; (void)q; (void)status; free(text);
   return true;
}

#include "ws_bbc.inc"

static int failures;
static void ok(int cond, const char *what)
{
   printf("  %s: %s\n", cond ? "ok" : "FAIL", what);
   if (!cond) failures++;
}

static ws_conn_t conn;
/* A request as it arrives: the header and the first `with` bytes of the body in one segment. */
static bool request(const char *query, const char *headers, const uint8_t *body, size_t with)
{
   memset(&conn, 0, sizeof conn);
   conn.pcb = (struct tcp_pcb *)&conn;
   int n = snprintf(conn.reqhdr, sizeof conn.reqhdr, "POST /bbc/shared HTTP/1.1\r\n%s\r\n", headers);
   assert((size_t)n + with <= WS_HEADER_MAX);
   memcpy(conn.reqhdr + n, body, with);
   conn.reqhdr_len = (size_t)n + with;
   wire_len = 0;
   status_sent = 0;
   api_calls = 0;
   return bbc_shared_begin(&conn, query, n);
}

int main(void)
{
   static uint8_t body[3000];
   for (size_t i = 0; i < sizeof body; i++) body[i] = (uint8_t)(i * 7u + 1u);

   puts("== POST /bbc/shared in webserver.c");
   memset(shared, 0, sizeof shared);
   ok(request("offset=100", "Content-Length: 1000\r\n", body, 1000) && status_sent == 200
      && strcmp(body_sent, "{\"written\":1000}\n") == 0 && memcmp(shared + 100, body, 1000) == 0,
      "a body that came with the header: in shared memory at the offset, {\"written\":1000}");

   memset(shared, 0, sizeof shared);
   ok(request("offset=0", "Content-Length: 3000\r\n", body, 500) && status_sent == 0 && conn.state == CONN_RECV_BBC,
      "a body still to come: gathering");
   size_t at = 500;
   while (at < 3000) {                                  /* the rest, in TCP segments */
      size_t seg = 3000 - at < 1460 ? 3000 - at : 1460;
      bool last = at + seg == 3000;
      size_t took = bbc_body_take(&conn, body + at, seg + (last ? 10u : 0u));   /* the last with 10 bytes of a next request */
      at += took;
      if (took != seg) break;
      if (conn.bbc_have == conn.bbc_need) (void)bbc_body_done(&conn);
   }
   ok(at == 3000 && memcmp(shared, body, 3000) == 0 && status_sent == 200 && strcmp(body_sent, "{\"written\":3000}\n") == 0,
      "the rest, across segments, takes no byte past the body: all in place, {\"written\":3000}");

   ok(request("offset=0", "Content-Length: 4\r\nExpect: 100-continue\r\n", body, 0) && conn.state == CONN_RECV_BBC
      && wire_len == 25 && memcmp(wire, "HTTP/1.1 100 Continue\r\n\r\n", 25) == 0,
      "Expect: 100-continue: the 100 goes out before the body");
   ok(request("offset=4095", "Content-Length: 4\r\nExpect: 100-continue\r\n", body, 0) && status_sent == 400 && wire_len == 0,
      "a request refused gets its error, and no 100");

   ok(request("offset=0", "Transfer-Encoding: chunked\r\n", body, 0) && status_sent == 400 && api_calls == 0,
      "Transfer-Encoding: refused, 400, before the API is asked");
   ok(request("offset=0", "Content-Length: 4\r\nTransfer-Encoding: identity\r\n", body, 4) && status_sent == 400 && api_calls == 0,
      "Transfer-Encoding with a Content-Length: refused too");
   ok(request("offset=0", "", body, 0) && status_sent == 400, "no Content-Length: refused (a length of 0)");
   ok(request("offset=0", "Content-Length: -4\r\n", body, 0) && status_sent == 400, "a negative Content-Length: refused");
   ok(request("offset=0", "Content-Length: 4x\r\n", body, 4) && status_sent == 400, "trailing text in it: refused");
   ok(request("offset=0", "Content-Length: 00000000000000000000004\r\n", body, 4) && status_sent == 400,
      "one too long for the header buffer, which would be cut short: refused");
   ok(request("offset=4093", "Content-Length: 4\r\n", body, 4) && status_sent == 400 && shared[4093] == 0,
      "past the end: refused, nothing written");

   if (failures) { printf("bbc/shared: %d failure(s)\n", failures); return 1; }
   puts("bbc/shared: all passed");
   return 0;
}
