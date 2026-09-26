/* fn_network.c - NetworkDevice ($FD): Open, Read, Write, Close.

   Protocol: fujinet-nio docs/network_device_protocol.md, with the handlers
   in src/lib/network_device.cpp as the reference for every layout and
   status.  Sessions ride on net_service's C API (net_capi_*), so http://,
   https:// and tcp:// go through the same lwIP code as the N: device.

   How it waits.  nio answers NotReady and lets the host poll; fn-rom does
   that for Read (with a back-off of ~48 s in all) but treats anything other
   than Ok from Write as a failure.  So Read answers NotReady as nio does,
   while a Write the network cannot take yet keeps the request pending
   (fn_network_waiting) and is run again until it is taken - a wait the
   serial link never needed, since the ESP buffers the whole chunk.  A Write
   is safe to run again because offsets are sequential: the session's
   cursor says how much of the chunk already went, and only the rest is
   sent.  Thirty seconds without progress answers Timeout.

   JSON translation (TranslateConfigure, or the Open extension) buffers the
   whole response body, as nio does, and then serves the flattened result
   of the selector (fn_json.c) at any offset.  Configuring again with a new
   selector reuses the buffered body, so one request can answer several
   queries.  Buffering answers NotReady until the body has all arrived.

   Not here yet: XML/RSS translation (Unsupported, as in nio), Info/InfoRead, request headers other than Content-Type, and
   bodies of unknown length (our HTTP request carries a Content-Length). */

#include <stdlib.h>
#include <string.h>

#include "fn_json.h"
#include "fn_network.h"
#include "fn_tnfs.h"            /* fn_tnfs_io_now_ms: the device's clock */
#include "../net_service.h"

#define NET_OPEN      0x01u
#define NET_READ      0x02u
#define NET_WRITE     0x03u
#define NET_CLOSE     0x04u
#define NET_TRANSLATE 0x07u

#define TR_NONE       0u
#define TR_JSON       1u
#define TR_RSS        3u            /* the last type nio knows */

#define OPEN_FLAG_BODY_UNKNOWN  0x04u
#define OPEN_FLAG_ALLOW_EVICT   0x08u

#define OPEN_EXT_TRANSLATION    0x00000001u
#define OPEN_EXT_CONTENT_PROF   0x00000002u

#define OPEN_ACCEPTED           0x01u
#define OPEN_NEEDS_BODY         0x02u

#define PROTO_SEQ_READ          0x01u
#define PROTO_SEQ_WRITE         0x02u
#define PROTO_STREAMING         0x04u

#define READ_EOF                0x01u
#define READ_TRUNCATED          0x02u
#define READ_MORE               0x04u

#define TRANSLATE_READY         0x01u

#define SESSIONS      NET_CAPI_HANDLES
#define URL_MAX       256u
#define CTYPE_MAX     48u
#define WRITE_STALL_MS 30000u
#define BODY_MAX      (1024u * 1024u)   /* the most of a response we buffer */
#define BODY_STEP     4096u

typedef enum { S_FREE, S_OPENING, S_READY, S_FAILED } s_state;

typedef struct {
   s_state  state;
   uint8_t  gen;             /* handle = gen << 8 | index; never 0 */
   int      net;             /* net_capi handle */
   bool     http;
   uint8_t  method;
   uint32_t body_len;        /* POST/PUT body still to come through Write */
   uint32_t rd_pos;          /* next Read offset */
   uint32_t wr_pos;          /* next Write offset */
   uint32_t wr_since;        /* when the pending Write last made progress */
   bool     wr_waiting;      /* a Write is pending (wr_since is valid) */
   uint32_t used;            /* for eviction: larger = more recent */
   char     url[URL_MAX];
   char     ctype[CTYPE_MAX];
   uint8_t  fail;            /* the FujiBus status once S_FAILED */
   /* translation (heap, freed with the session) */
   uint8_t  tr_type;         /* TR_NONE or TR_JSON */
   char    *tr_sel;          /* the selector */
   char    *body;            /* the buffered response, NUL-terminated */
   uint32_t body_got, body_cap;
   bool     body_done;       /* all of it is in */
   char    *tr_out;          /* the translated text */
   uint32_t tr_len;
   bool     tr_ready;
} session_t;

static session_t s_s[SESSIONS];
static uint32_t  s_clock;
static bool      s_waiting;
static volatile bool s_reset;

bool fn_network_waiting(void) { return s_waiting; }
void fn_network_reset(void)   { s_reset = true; }

static uint16_t handle_of(const session_t *s)
{
   return (uint16_t)(s->gen << 8 | (uint16_t)(s - s_s));
}

static session_t *session_for(uint16_t handle)
{
   unsigned int i = handle & 0xFFu;
   if (i >= SESSIONS || s_s[i].state == S_FREE || s_s[i].gen != handle >> 8)
      return NULL;
   s_s[i].used = ++s_clock;
   return &s_s[i];
}

/* Drop the translation; keep the buffered body unless `body` too. */
static void translation_clear(session_t *s, bool body)
{
   free(s->tr_sel);
   free(s->tr_out);
   s->tr_sel = s->tr_out = NULL;
   s->tr_type = TR_NONE;
   s->tr_len = 0;
   s->tr_ready = false;
   if (body) {
      free(s->body);
      s->body = NULL;
      s->body_got = s->body_cap = 0;
      s->body_done = false;
   }
}

static void session_free(session_t *s)
{
   if (s->state != S_FREE)
      net_capi_close(s->net);
   translation_clear(s, true);
   s->state = S_FREE;
}

static uint8_t status_of(uint8_t net_err)
{
   return net_err == NET_ERR_PARAM ? FB_INVALID_REQUEST : FB_IO_ERROR;
}

/* Advance an opening session: true once it can carry data. */
static bool advance(session_t *s)
{
   if (s->state != S_OPENING)
      return s->state == S_READY;
   net_http_opts_t o = { .method = s->method, .body_len = s->body_len,
                         .content_type = s->ctype[0] ? s->ctype : NULL };
   uint8_t r = net_capi_open(s->net, s->url,
                             s->body_len ? NET_OPEN_RW : NET_OPEN_READ, &o);
   if (r == NET_OK)
      s->state = S_READY;
   else if (r != NET_PENDING) {
      s->state = S_FAILED;
      s->fail = status_of(r);
   }
   return s->state == S_READY;
}

void fn_network_poll(void)
{
   if (s_reset) {
      s_reset = false;
      for (unsigned int i = 0; i < SESSIONS; i++)
         session_free(&s_s[i]);
   }
   for (unsigned int i = 0; i < SESSIONS; i++)
      (void)advance(&s_s[i]);
}

static void put_prefix(fb_out *out, uint8_t flags)
{
   fb_put_u8(out, FB_VERSION);
   fb_put_u8(out, flags);
   fb_put_u16(out, 0);                  /* reserved */
}

static bool prefix_is(const char *url, const char *scheme)
{
   size_t n = strlen(scheme);
   for (size_t i = 0; i < n; i++) {
      char c = url[i];
      if (c >= 'A' && c <= 'Z')
         c = (char)(c - 'A' + 'a');
      if (c != scheme[i])
         return false;
   }
   return true;
}

/* A translation block: type, flags, u16-length selector.  The selector is
   returned as a pointer into the request. */
typedef struct { uint8_t type; const uint8_t *sel; uint16_t sel_len; } tr_cfg;

static void get_tr_cfg(fb_in *in, tr_cfg *c)
{
   c->type = fb_get_u8(in);
   (void)fb_get_u8(in);                 /* flags: none defined */
   c->sel_len = fb_get_u16(in);
   c->sel = fb_get_bytes(in, c->sel_len);
}

/* nio's configure_translation: an unknown type is refused, XML and RSS
   have no translator here, None switches translation off. */
static uint8_t configure(session_t *s, const tr_cfg *c)
{
   translation_clear(s, false);
   if (c->type == TR_NONE)
      return FB_OK;
   if (c->type != TR_JSON)
      return FB_UNSUPPORTED;
   s->tr_sel = malloc((size_t)c->sel_len + 1u);
   if (!s->tr_sel)
      return FB_INTERNAL_ERROR;
   if (c->sel_len)
      memcpy(s->tr_sel, c->sel, c->sel_len);
   s->tr_sel[c->sel_len] = '\0';
   s->tr_type = c->type;
   return FB_OK;
}

/* nio's ensure_translation_ready: take in the whole body, then translate.
   NotReady while the body is still arriving. */
static uint8_t translation_ready(session_t *s)
{
   if (s->tr_type == TR_NONE || s->tr_ready)
      return FB_OK;
   if (s->state == S_FAILED)
      return s->fail;
   if (!advance(s) || s->wr_pos < s->body_len)
      return FB_NOT_READY;
   while (!s->body_done) {
      if (s->body_cap - s->body_got < BODY_STEP + 1u) {
         if (s->body_cap >= BODY_MAX)
            return FB_IO_ERROR;             /* larger than we will hold */
         char *b = realloc(s->body, s->body_cap + BODY_STEP * 4u);
         if (!b)
            return FB_INTERNAL_ERROR;
         s->body = b;
         s->body_cap += BODY_STEP * 4u;
      }
      uint32_t got = 0;
      uint8_t r = net_capi_read(s->net, (uint8_t *)s->body + s->body_got, BODY_STEP, &got);
      s->body_got += got;
      if (r == NET_EOF)
         s->body_done = true;
      else if (r != NET_OK) {
         s->state = S_FAILED;
         s->fail = status_of(r);
         return s->fail;
      } else if (got == 0)
         return FB_NOT_READY;
   }
   if (!s->body) {                          /* an empty body */
      s->body = malloc(1);
      if (!s->body)
         return FB_INTERNAL_ERROR;
   }
   s->body[s->body_got] = '\0';
   if (!fn_json_translate(s->body, s->tr_sel, &s->tr_out, &s->tr_len))
      return FB_INTERNAL_ERROR;
   s->tr_ready = true;
   return FB_OK;
}

static uint8_t do_open(fb_in *in, fb_out *out)
{
   uint8_t method = fb_get_u8(in);
   uint8_t flags = fb_get_u8(in);
   uint16_t url_len = fb_get_u16(in);
   const uint8_t *url = fb_get_bytes(in, url_len);
   char ctype[CTYPE_MAX] = "";
   bool other_header = false;

   uint16_t headers = fb_get_u16(in);
   for (uint16_t i = 0; i < headers && !in->bad; i++) {
      uint16_t kl = fb_get_u16(in);
      const uint8_t *k = fb_get_bytes(in, kl);
      uint16_t vl = fb_get_u16(in);
      const uint8_t *v = fb_get_bytes(in, vl);
      if (in->bad)
         break;
      static const char ct[] = "content-type";
      bool is_ct = kl == sizeof ct - 1u && vl < CTYPE_MAX;
      for (uint16_t j = 0; is_ct && j < kl; j++)
         is_ct = (k[j] | 0x20u) == (uint8_t)ct[j];
      if (is_ct) {
         memcpy(ctype, v, vl);
         ctype[vl] = '\0';
      } else
         other_header = true;
   }
   uint32_t body_len = fb_get_u32(in);
   uint16_t resp_headers = fb_get_u16(in);          /* only Info returns them */
   for (uint16_t i = 0; i < resp_headers && !in->bad; i++)
      (void)fb_get_bytes(in, fb_get_u16(in));
   uint32_t ext = 0;
   uint8_t profile = 0;
   tr_cfg tr = { .type = TR_NONE };
   if (fb_left(in)) {
      ext = fb_get_u32(in);
      if (ext & OPEN_EXT_TRANSLATION)
         get_tr_cfg(in, &tr);
      if (ext & OPEN_EXT_CONTENT_PROF)
         profile = fb_get_u8(in);
   }
   if (in->bad || fb_left(in) || url_len == 0 || url_len >= URL_MAX ||
       (ext & ~(OPEN_EXT_TRANSLATION | OPEN_EXT_CONTENT_PROF)) || profile > 3u ||
       tr.type > TR_RSS || method < NET_HTTP_GET || method > NET_HTTP_HEAD)
      return FB_INVALID_REQUEST;

   char u[URL_MAX];
   memcpy(u, url, url_len);
   u[url_len] = '\0';
   bool http = prefix_is(u, "http://") || prefix_is(u, "https://");
   if (!http && !prefix_is(u, "tcp://"))
      return FB_UNSUPPORTED;
   bool has_body = method == NET_HTTP_POST || method == NET_HTTP_PUT;
   if (!has_body && (body_len || (flags & OPEN_FLAG_BODY_UNKNOWN)))
      return FB_INVALID_REQUEST;
   if ((flags & OPEN_FLAG_BODY_UNKNOWN) || other_header)
      return FB_UNSUPPORTED;
   if (!ctype[0] && profile) {
      static const char *const types[] = { "", "application/json",
         "application/x-www-form-urlencoded", "text/plain" };
      strcpy(ctype, types[profile]);
   }
   if (!net_capi_enabled())
      return FB_DEVICE_NOT_FOUND;

   /* A free session, else (if allowed) the least recently used. */
   session_t *s = NULL;
   for (unsigned int i = 0; i < SESSIONS && !s; i++)
      if (s_s[i].state == S_FREE)
         s = &s_s[i];
   if (!s && (flags & OPEN_FLAG_ALLOW_EVICT)) {
      s = &s_s[0];
      for (unsigned int i = 1; i < SESSIONS; i++)
         if (s_s[i].used < s->used)
            s = &s_s[i];
      session_free(s);
   }
   if (!s)
      return FB_DEVICE_BUSY;
   int net = net_capi_alloc();
   if (net < 0)
      return FB_DEVICE_BUSY;

   uint8_t gen = (uint8_t)(s->gen + 1u);
   memset(s, 0, sizeof *s);
   s->gen = gen ? gen : 1u;
   s->state = S_OPENING;
   s->net = net;
   s->http = http;
   s->method = method;
   s->body_len = has_body ? body_len : 0u;
   s->used = ++s_clock;
   strcpy(s->url, u);
   strcpy(s->ctype, ctype);
   uint8_t st = configure(s, &tr);
   if (st != FB_OK) {
      session_free(s);
      return st;
   }
   (void)advance(s);                    /* start resolving now */
   if (s->state == S_FAILED && s->fail == FB_INVALID_REQUEST) {
      session_free(s);                  /* a URL the stack cannot parse */
      return FB_INVALID_REQUEST;
   }

   put_prefix(out, (uint8_t)(OPEN_ACCEPTED | (s->body_len ? OPEN_NEEDS_BODY : 0u)));
   fb_put_u16(out, handle_of(s));
   fb_put_u8(out, http ? 0u : (uint8_t)(PROTO_SEQ_READ | PROTO_SEQ_WRITE | PROTO_STREAMING));
   return FB_OK;
}

/* A Read of the translated text: any offset, as nio's translator allows. */
static uint8_t read_translated(session_t *s, uint16_t handle, uint32_t offset,
                               uint16_t max, fb_out *out)
{
   uint8_t st = translation_ready(s);
   if (st != FB_OK)
      return st;
   if (offset > s->tr_len)
      return FB_INVALID_REQUEST;
   if (fb_room(out) < 12u)
      return FB_INTERNAL_ERROR;
   if (max > fb_room(out) - 12u)
      max = (uint16_t)(fb_room(out) - 12u);
   uint32_t n = s->tr_len - offset;
   if (n > max)
      n = max;
   bool eof = offset + n >= s->tr_len;
   uint8_t flags = (uint8_t)((eof ? READ_EOF : 0u) |
                             (n == max && !eof ? READ_TRUNCATED : 0u) |
                             (!eof ? READ_MORE : 0u));
   put_prefix(out, flags);
   fb_put_u16(out, handle);
   fb_put_u32(out, offset);
   fb_put_u16(out, (uint16_t)n);
   fb_put_bytes(out, s->tr_out ? s->tr_out + offset : "", (uint16_t)n);
   return FB_OK;
}

static uint8_t do_read(fb_in *in, fb_out *out)
{
   uint16_t handle = fb_get_u16(in);
   uint32_t offset = fb_get_u32(in);
   uint16_t max = fb_get_u16(in);
   if (in->bad || fb_left(in))
      return FB_INVALID_REQUEST;
   session_t *s = session_for(handle);
   if (!s)
      return FB_INVALID_REQUEST;
   if (s->tr_type != TR_NONE)
      return read_translated(s, handle, offset, max, out);
   bool ready = advance(s);
   if (s->state == S_FAILED)
      return s->fail;
   if (!ready || s->wr_pos < s->body_len)
      return FB_NOT_READY;              /* connecting, or the body is not all in */
   if (offset != s->rd_pos)
      return FB_INVALID_REQUEST;        /* sequential, as nio's HTTP and TCP */

   uint8_t *flags = fb_put_space(out, 2);   /* version, flags */
   fb_put_u16(out, 0);
   fb_put_u16(out, handle);
   fb_put_u32(out, offset);
   uint8_t *len = fb_put_space(out, 2);
   if (!flags || !len)
      return FB_INTERNAL_ERROR;
   if (max > fb_room(out))
      max = fb_room(out);
   uint8_t *data = out->p + out->len;

   uint32_t got = 0, none;
   uint8_t r = net_capi_read(s->net, data, max, &got);
   bool eof = r == NET_EOF;
   if (r != NET_OK && !eof) {
      s->state = S_FAILED;
      s->fail = status_of(r);
      return s->fail;
   }
   if (got == 0 && !eof)
      return FB_NOT_READY;              /* Ok with no bytes means end of data */
   if (!eof && net_capi_read(s->net, data + got, 0, &none) == NET_EOF)
      eof = true;                       /* that was the last of it */
   out->len = (uint16_t)(out->len + got);
   s->rd_pos += got;
   flags[0] = FB_VERSION;
   flags[1] = (uint8_t)((eof ? READ_EOF : 0u) | (got == max && !eof ? READ_TRUNCATED : 0u));
   len[0] = (uint8_t)got;
   len[1] = (uint8_t)(got >> 8);
   return FB_OK;
}

static uint8_t do_write(fb_in *in, fb_out *out)
{
   uint16_t handle = fb_get_u16(in);
   uint32_t offset = fb_get_u32(in);
   uint16_t n = fb_get_u16(in);
   const uint8_t *data = fb_get_bytes(in, n);
   if (in->bad || fb_left(in))
      return FB_INVALID_REQUEST;
   session_t *s = session_for(handle);
   if (!s)
      return FB_INVALID_REQUEST;
   if (s->state == S_FAILED)
      return s->fail;
   /* The chunk must start at or before the cursor and reach it: a chunk
      already partly (or wholly) sent is this request being run again, or
      the host repeating one it lost the answer to. */
   if (offset > s->wr_pos || offset + n < s->wr_pos)
      return FB_INVALID_REQUEST;
   if (s->http && offset + n > s->body_len)
      return FB_INVALID_REQUEST;        /* no body declared, or past its end */

   uint32_t now = fn_tnfs_io_now_ms();
   if (offset + n > s->wr_pos) {
      if (!s->wr_waiting) {
         s->wr_waiting = true;
         s->wr_since = now;
      }
      uint32_t done = 0;
      uint8_t r = advance(s) ? net_capi_write(s->net, data + (s->wr_pos - offset),
                                              offset + n - s->wr_pos, &done)
                             : NET_OK;
      if (s->state == S_FAILED)
         return s->fail;
      if (r != NET_OK && r != NET_PENDING) {
         s->state = S_FAILED;
         s->fail = status_of(r);
         return s->fail;
      }
      if (done) {
         s->wr_pos += done;
         s->wr_since = now;
      }
      if (offset + n > s->wr_pos) {
         if (now - s->wr_since >= WRITE_STALL_MS) {
            s->wr_waiting = false;
            return FB_TIMEOUT;
         }
         s_waiting = true;
         return FB_OK;
      }
      s->wr_waiting = false;
   }
   put_prefix(out, 0);
   fb_put_u16(out, handle);
   fb_put_u32(out, offset);
   fb_put_u16(out, n);
   return FB_OK;
}

static uint8_t do_translate(fb_in *in, fb_out *out)
{
   uint16_t handle = fb_get_u16(in);
   if (in->bad)
      return FB_INVALID_REQUEST;
   session_t *s = session_for(handle);
   if (!s)
      return FB_INVALID_REQUEST;
   if (s->wr_pos < s->body_len)
      return FB_NOT_READY;              /* the request body is not all in */
   tr_cfg c;
   get_tr_cfg(in, &c);
   if (in->bad || fb_left(in) || c.type > TR_RSS)
      return FB_INVALID_REQUEST;
   uint8_t st = configure(s, &c);
   if (st == FB_OK)
      st = translation_ready(s);
   if (st != FB_OK)
      return st;
   put_prefix(out, s->tr_ready ? TRANSLATE_READY : 0u);
   fb_put_u16(out, handle);
   fb_put_u32(out, s->tr_len);
   return FB_OK;
}

static uint8_t do_close(fb_in *in, fb_out *out)
{
   uint16_t handle = fb_get_u16(in);
   if (in->bad || fb_left(in))
      return FB_INVALID_REQUEST;
   session_t *s = session_for(handle);
   if (!s)
      return FB_INVALID_REQUEST;
   session_free(s);
   put_prefix(out, 0);
   return FB_OK;
}

uint8_t fn_network_command(uint8_t command, fb_in *in, fb_out *out)
{
   s_waiting = false;
   if ((command < NET_OPEN || command > NET_CLOSE) && command != NET_TRANSLATE)
      return FB_UNSUPPORTED;            /* Info, InfoRead */
   if (fb_get_u8(in) != FB_VERSION)
      return FB_INVALID_REQUEST;
   switch (command) {
   case NET_OPEN:  return do_open(in, out);
   case NET_READ:  return do_read(in, out);
   case NET_WRITE: return do_write(in, out);
   case NET_CLOSE: return do_close(in, out);
   case NET_TRANSLATE: return do_translate(in, out);
   default:        return FB_UNSUPPORTED;
   }
}
