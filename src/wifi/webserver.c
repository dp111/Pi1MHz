/* Minimal HTTP file-browser webserver for Pi1MHz.
 *
 * Built directly on the lwIP raw/callback TCP API (NO_SYS=1).  Serves a
 * home page, a status page and a file browser that can list folders on
 * the SD card, download files and upload files (multipart/form-data).
 *
 * HTTP/1.1 persistent connections are supported: each kept-alive socket
 * carries up to WS_KEEP_ALIVE_MAX_REQUESTS requests before being torn
 * down; an idle keep-alive connection is closed after ~30 s by the
 * existing ws_poll deadline.  Clients that send "Connection: close"
 * (or HTTP/1.0) still get one-request-per-connection behaviour as
 * before.
 */

#include "webserver.h"

#include "md5.h"
#include "wifi.h"
#include "wifi_lwip.h"
#include "sdio.h"
#include "framebuffer_export.h"
#include "../BeebSCSI/fatfs/ff.h"
#include "../usb/mtp_fs.h"
#include "../rpi/screen.h"
#include "../rpi/exceptions.h"
#include "../rpi/info.h"
#include "../rpi/systimer.h"
#include "../Pi1MHz.h"
#include "../aun/aun_emulator.h"

#include "lwip/err.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Tunables                                                            */
/* ------------------------------------------------------------------ */

#define WS_HEADER_MAX        2048u   /* max HTTP request-header bytes   */
#define WS_FILE_CHUNK        8192u   /* SD read size while downloading  */
#define WS_BOUNDARY_MAX      128u    /* multipart boundary text limit   */
#define WS_UPLOAD_HEAD_MAX   1024u   /* multipart part-header limit     */
#define WS_UPLOAD_WORK       2048u   /* upload scan working buffer      */
#define WS_PATH_MAX          512u    /* SD path / URL path limit        */
#define WS_LISTING_HARD_CAP  8000u   /* max directory entries listed    */
#define WS_POLL_INTERVAL     1u      /* tcp_poll ticks (1 = ~500ms, the floor) */
#define WS_POLL_LIMIT        60u     /* idle polls (~0.5s each) -> ~30s abort */
#define WS_ERROR_TEXT_MAX    128u
#define WS_FB_BMP_MAX        (6u * 1024u * 1024u)  /* exported BMP size cap */
#define WS_REBOOT_DELAY_US   (1500u * 1000u)       /* defer reboot ~1.5s */
#define WS_FREE_REFRESH_US   (5u * 1000u * 1000u)  /* SD-free-space cache TTL */
/* Digest auth: nonces are accepted for this long after they are issued.
   Five minutes is enough for any UI workflow without leaving an
   indefinite replay window. */
#define WS_NONCE_MAX_AGE_US  (5u * 60u * 1000000u)
/* Maximum body length we are willing to discard while answering an
   un-authenticated PUT that arrived with Expect: 100-continue.  Real
   uploads of well-behaved clients are smaller than the SD card; an
   attacker on the LAN flooding huge unauthenticated PUTs hits this
   cap and gets the connection torn down. */
#define WS_DRAIN_MAX_BYTES   (64u * 1024u * 1024u)
/* Maximum number of requests we will service on a single keep-alive
   TCP connection before forcing it closed.  Bounded so a misbehaving
   peer cannot pin a slot indefinitely; Windows Explorer's typical
   "list folder + write one file" dance is under 20 requests, so 100
   leaves headroom without making the cap meaningful in normal use. */
#define WS_KEEP_ALIVE_MAX_REQUESTS  100u
/* PROPFIND child-cache size and TTL.  Windows Explorer's
   MiniRedirector follows the parent Depth:1 PROPFIND with a separate
   Depth:0 PROPFIND for EVERY child it just saw - dozens of
   round-trips that each used to need their own f_stat against the
   cold FATFS window cache (~5-15 ms apiece on SD).  After a parent
   walk we cache the FILINFO of each child by its full SD path; the
   per-child PROPFINDs answer from this cache without touching SD.
   Sized for a typical directory; oversize directories simply miss
   the cache and fall back to f_stat as before.  TTL is short so a
   stale cache cannot mislead a client that legitimately races a
   PROPFIND with an external (non-DAV) writer. */
#define WS_PROPFIND_CACHE_MAX_CHILDREN  128u
#define WS_PROPFIND_CACHE_TTL_US        (5u * 1000000u)
/* Headers a WebDAV response may need to carry.  Sized for the worst case
   (WWW-Authenticate digest line + DAV / Allow). */
#define WS_AUTH_HEADER_MAX   256u

/* ------------------------------------------------------------------ */
/* Connection model                                                    */
/* ------------------------------------------------------------------ */

typedef enum {
   CONN_RECV_HEADER = 0,   /* accumulating the HTTP request headers     */
   CONN_RECV_UPLOAD,       /* streaming a multipart body to the SD card */
   CONN_RECV_DAV_PUT,      /* streaming a WebDAV PUT body to the SD card*/
   CONN_DAV_COPY,          /* copying SD->SD one chunk per tick         */
   CONN_SEND_MEM,          /* sending an in-memory response             */
   CONN_SEND_FILE,         /* sending a header then streaming a file    */
   CONN_SEND_FB            /* sending a header then streaming the framebuffer */
} conn_state_t;

typedef enum {
   UP_PART_HEADER = 0,     /* reading the multipart part header         */
   UP_DATA,                /* streaming file bytes to the SD card       */
   UP_EPILOGUE,            /* upload finished, discarding trailing bytes*/
   UP_FAILED               /* upload failed, response already queued    */
} upload_state_t;

/* Transfer-Encoding: chunked (RFC 9112 §7.1) framing for a WebDAV PUT
   body - see dav_put_consume_chunked. */
typedef enum {
   DAV_CHUNK_SIZE = 0,     /* reading "<hex>[;ext]" CRLF (chunk-size line) */
   DAV_CHUNK_DATA,         /* streaming this chunk's data bytes            */
   DAV_CHUNK_DATA_CRLF,    /* skipping the CRLF that follows chunk data    */
   DAV_CHUNK_TRAILER       /* reading trailer field-lines / the final CRLF */
} dav_chunk_state_t;

typedef struct {
   struct tcp_pcb *pcb;
   conn_state_t    state;
   uint8_t         poll_count;
   /* HEAD response: queue exactly the same headers as GET would but
      drop the body bytes (file / framebuffer / multistatus etc.) in
      conn_pump.  RFC 9110 §9.3.2 - "the server MUST NOT send a
      content".  Browsers and Windows Explorer's WebDAV redirector
      both issue HEAD before some downloads and expect zero body. */
   bool            is_head;

   /* HTTP/1.1 keep-alive.  Set by process_request from the request's
      HTTP version + Connection header.  When true, conn_pump resets
      the connection back to CONN_RECV_HEADER after the response has
      been fully ACKed instead of calling conn_close.  This lets a
      single TCP socket carry multiple requests, which lets Windows
      Explorer's MiniRedirector keep its already-established Digest
      credentials and skip the 401-challenge / drain-then-retry dance
      on body-bearing PUTs - the slow "long pause" the user reported.
      Capped at WS_KEEP_ALIVE_MAX_REQUESTS per connection so a
      misbehaving client can't pin a slot forever. */
   bool            keep_alive;
   uint16_t        request_count;

   /* request header buffer */
   char    reqhdr[WS_HEADER_MAX + 1u];
   size_t  reqhdr_len;

   /* in-memory output (HTML body, or the HTTP header of a download) */
   char   *out;
   size_t  out_len;
   size_t  out_sent;

   /* file download */
   bool     dl_open;
   FIL      dl_file;
   uint32_t dl_remaining;
   bool     dl_eof;
   uint8_t  dl_buf[WS_FILE_CHUNK];
   size_t   dl_buf_len;
   size_t   dl_buf_sent;

   /* framebuffer streaming (CONN_SEND_FB); reuses dl_buf to hold rows */
   framebuffer_export_info_t fb_info;
   uint32_t fb_row;
   bool     fb_stale;      /* geometry changed mid-stream: blank remaining rows */

   /* output accounting (for the close decision) */
   uint32_t bytes_queued;
   uint32_t bytes_acked;
   bool     producing_done;
   /* request bytes arrived while a response was in flight and were
      discarded (we do not implement pipelining): close instead of
      keep-alive so the client retries at once rather than stalling */
   bool     pipelined_bytes_dropped;
   /* Bytes of the current request's body still to be discarded before the
      next request may be parsed.  Set for a body-bearing verb whose handler
      does not consume the body (PROPFIND/PROPPATCH/LOCK/MKCOL) when the body
      trails the headers into later segments.  Draining these keeps the
      connection alive (avoiding a reconnect per request) without letting the
      stray body desync the next request.  Persists across
      conn_reset_for_next_request until it reaches zero. */
   uint32_t drain_remaining;

   /* upload */
   upload_state_t up_state;
   bool     up_complete;
   bool     up_file_open;
   /* Multipart upload and WebDAV PUT are mutually exclusive (a
      connection is in either CONN_RECV_UPLOAD or CONN_RECV_DAV_PUT,
      never both), so share one FIL between them and gate access via
      the up_file_open / dav_put_open booleans.  Saves ~560 bytes
      per ws_conn_t with no behavioural change. */
   union {
      FIL up;     /* CONN_RECV_UPLOAD: the multipart-decoded file */
      FIL dav;    /* CONN_RECV_DAV_PUT: the WebDAV PUT body */
   } write_file;
   char     up_delim[WS_BOUNDARY_MAX + 8u];   /* "\r\n--" + boundary    */
   size_t   up_delim_len;
   char     up_head[WS_UPLOAD_HEAD_MAX + 1u];
   size_t   up_head_len;
   uint8_t  up_tail[WS_BOUNDARY_MAX + 8u];     /* held-back bytes        */
   size_t   up_tail_len;
   char     up_dir[WS_PATH_MAX];
   char     up_name[FF_LFN_BUF + 1];
   uint32_t up_bytes_written;

   /* WebDAV PUT streaming (CONN_RECV_DAV_PUT).  remaining starts at
      Content-Length and counts down as bytes are written to
      write_file.dav.  put_response_status / put_response_text are
      stashed so the 201/204 reply can be queued the moment the
      last body byte lands. */
   bool     dav_put_open;
   /* dav_put_draining: set when a PUT request arrived without valid
      digest credentials but Windows Explorer's MiniRedirector is
      waiting for the Expect: 100-continue handshake to complete.
      The standard "send 401 immediately" reply makes MiniRedirector
      cancel the request and refuse to retry, so the file just never
      uploads.  Instead we send 100 Continue, accept the body bytes
      into /dev/null (no temp file is opened), and once the full
      Content-Length is drained we send the 401 challenge - which
      MiniRedirector then retries cleanly on a fresh connection.
      The dav_put_open flag stays false so dav_put_consume's f_write
      path is skipped; the dav_remaining counter still drives the
      drain.  Capped at WS_DRAIN_MAX_BYTES to avoid eating arbitrary
      uploads from an attacker on the LAN. */
   bool     dav_put_draining;
   uint32_t dav_remaining;
   /* Transfer-Encoding: chunked instead of a fixed Content-Length.
      Windows Explorer's MiniRedirector uses chunked PUTs in some
      client/registry configurations; rejecting them outright (as this
      server used to) is exactly the generic "cannot perform the
      requested operation" (Win32 0x8007003A) copy failure Explorer
      shows the user - reads/PROPFIND never carry a body so are never
      affected. dav_put_consume_chunked() decodes the chunk framing and
      feeds the data through dav_put_write_bytes(); dav_remaining is
      unused while chunked - the terminating chunk decides when the
      body ends, not a byte counter. */
   bool               dav_put_chunked;
   dav_chunk_state_t  dav_chunk_state;
   uint32_t           dav_chunk_remaining;  /* bytes left in this chunk, or CRLF-skip count */
   char               dav_chunk_linebuf[24];/* chunk-size / trailer line accumulator */
   size_t             dav_chunk_linelen;
   uint32_t           dav_chunk_drained;    /* cumulative bytes; only enforced while draining */
   int      dav_put_status;
   const char *dav_put_status_text;
   /* dav_put_target is the FINAL path the client asked us to PUT.
      dav_put_tmppath is where the body is actually written ("<target>.part")
      so a client disconnect halfway through an upload does not destroy a
      pre-existing file at dav_put_target.  On a clean upload we close the
      temp file, unlink the old target (if any) and f_rename the temp to
      the final name; on any error path we unlink the temp file. */
   char     dav_put_target[WS_PATH_MAX];
   char     dav_put_tmppath[WS_PATH_MAX + 8u];

   /* PROPPATCH date capture (CONN stays in send/header state; this rides
      the keep-alive drain).  Windows Explorer sends the Win32LastModified-
      Time XML body in a SEPARATE TCP segment from the PROPPATCH headers, so
      the in-segment parse misses it and the copied file keeps its default
      date.  When that happens we stash the target path, seed dl_buf (idle
      here) with the body bytes already present, and let conn_consume's drain
      loop append the trailing bytes; once the whole body is in we parse it
      and stamp the FAT mtime.  pp_capture gates the whole dance and, like
      drain_remaining, survives conn_reset_for_next_request. */
   bool     pp_capture;
   size_t   pp_buf_len;                  /* bytes of the body held in dl_buf */
   char     pp_path[WS_PATH_MAX];        /* target whose mtime we will stamp */

   /* PUT bodies arrive one ~1460-byte TCP segment at a time.  Writing
      each segment straight to f_write forced FatFs down its single-block
      (CMD24) path, one SD command per segment, which throttled large
      uploads to a crawl (the ~97% "pause" the user reported).  Instead we
      accumulate body bytes in the connection's dl_buf - idle during a PUT
      (it only holds outbound download/framebuffer data) - and flush it in
      whole-buffer, sector-aligned WS_FILE_CHUNK chunks so FatFs issues
      multiblock (CMD25) writes.  dav_put_buf_len is the fill level; the
      final partial buffer is flushed by dav_put_finish before f_close. */
   size_t   dav_put_buf_len;

   /* WebDAV COPY streaming (CONN_DAV_COPY).  Reads from copy_src,
      writes to copy_dst, one chunk per webserver_poll tick (reusing
      dl_buf to keep the per-connection footprint flat).  At source
      EOF the 201/204 status is queued via dav_put_send_response so
      the existing reply path covers it; copy_dst_existed picks
      which status code that uses. */
   bool     copy_src_open;
   bool     copy_dst_open;
   bool     copy_dst_existed;
   FIL      copy_src;
   FIL      copy_dst;
} ws_conn_t;

typedef struct {
   char     name[FF_LFN_BUF + 1];
   uint32_t size;
   bool     is_dir;
} ws_dir_entry_t;

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */

static struct tcp_pcb *g_ws_listener;
static bool            g_ws_ready;
static char            g_ws_error[WS_ERROR_TEXT_MAX + 1u];
static bool            g_ws_reboot_pending;
static uint32_t        g_ws_reboot_at;
/* Cached SD card byte counts.  f_getfree walks the FAT and can stall a
   slow / fragmented card for hundreds of ms - too long for a TCP
   callback to hold the cooperative poll loop.  webserver_poll
   refreshes these in the background; route_status reads
   g_ws_sd_free_mb, and WebDAV PROPFIND reads
   g_ws_sd_total_bytes / g_ws_sd_free_bytes to populate the
   quota-available-bytes / quota-used-bytes properties that Windows
   Explorer uses to display the share's free / total size.  Without
   the quota report Explorer falls back to a fabricated default
   (typically around 1 TB total) - the cause of the wildly wrong
   "Total size 929 GB" line in the WebDAV mount root. */
static bool            g_ws_sd_free_valid;
static uint32_t        g_ws_sd_free_mb;
static uint64_t        g_ws_sd_total_bytes;
static uint64_t        g_ws_sd_free_bytes;
static uint32_t        g_ws_sd_free_age_us;

/* PROPFIND child cache (see WS_PROPFIND_CACHE_* above).  Populated
   when route_dav_propfind walks the children of a directory; queried
   by subsequent Depth:0 PROPFINDs on those children.  Invalidated by
   any DAV write method (route_dav_put / route_dav_delete /
   route_dav_mkcol / route_dav_move_or_copy).  The cache deliberately
   uses static storage to avoid heap fragmentation. */
typedef struct {
   char    path[WS_PATH_MAX];   /* full SD path of the child */
   FILINFO fno;
} ws_propfind_cache_entry_t;
static ws_propfind_cache_entry_t g_ws_propfind_cache_entries[WS_PROPFIND_CACHE_MAX_CHILDREN];
static size_t   g_ws_propfind_cache_count;
static uint32_t g_ws_propfind_cache_at_us;
static bool     g_ws_propfind_cache_valid;

static void ws_propfind_cache_invalidate(void)
{
   g_ws_propfind_cache_count = 0u;
   g_ws_propfind_cache_valid = false;
}

/* A DAV method just changed the SD filesystem (created, deleted, or renamed
   a file/dir).  Drop our own PROPFIND child cache AND poke MTP so its
   object-handle cache re-enumerates - otherwise a file deleted over WebDAV
   lingers in the Windows MTP view until the next OpenSession.  Call this only
   from actual mutation routes, never from the read/TTL paths (which would
   force a needless MTP rescan on every listing). */
static void ws_fs_mutated(void)
{
   ws_propfind_cache_invalidate();
   mtp_fs_notify_fs_changed();
}

static void ws_propfind_cache_begin(void)
{
   ws_propfind_cache_invalidate();
}

static void ws_propfind_cache_add(const char *path, const FILINFO *fno)
{
   if (g_ws_propfind_cache_count >= WS_PROPFIND_CACHE_MAX_CHILDREN)
      return;
   ws_propfind_cache_entry_t *e =
      &g_ws_propfind_cache_entries[g_ws_propfind_cache_count];
   strlcpy(e->path, path, sizeof e->path);
   e->fno = *fno;
   ++g_ws_propfind_cache_count;
}

static void ws_propfind_cache_commit(void)
{
   g_ws_propfind_cache_at_us = RPI_GetSystemTime();
   g_ws_propfind_cache_valid = true;
}

/* Returns true and fills *out if `path` is in the cache and the
   cache has not expired. */
static bool ws_propfind_cache_lookup(const char *path, FILINFO *out)
{
   if (!g_ws_propfind_cache_valid)
      return false;
   if ((RPI_GetSystemTime() - g_ws_propfind_cache_at_us)
       >= WS_PROPFIND_CACHE_TTL_US) {
      ws_propfind_cache_invalidate();
      return false;
   }
   for (size_t i = 0u; i < g_ws_propfind_cache_count; ++i) {
      if (strcmp(g_ws_propfind_cache_entries[i].path, path) == 0) {
         *out = g_ws_propfind_cache_entries[i].fno;
         return true;
      }
   }
   return false;
}

/* WebDAV COPY is per-tick: only one COPY may be in flight at a time
   so the bookkeeping is trivially a single pointer.  A second COPY
   arriving while one is active gets 503 Service Unavailable.  Cleared
   from conn_close / ws_err so a torn-down connection mid-copy doesn't
   leave a dangling reference. */
static ws_conn_t      *g_ws_active_copy;

/* Rolling digest-auth nonce.  Regenerated lazily once it crosses
   WS_NONCE_MAX_AGE_US; clients are challenged with stale=true and
   re-authenticate transparently after that.  The server-side secret is
   initialised once from the system timer so nonces are unpredictable
   across reboots. */
static char            g_ws_nonce[MD5_HEX_LEN + 1u];
static uint32_t        g_ws_nonce_issued_us;
static uint32_t        g_ws_nonce_counter;
static uint32_t        g_ws_nonce_secret;
static bool            g_ws_nonce_secret_initialised;

static void ws_set_error(const char *message)
{
   if (message == NULL) {
      g_ws_error[0] = '\0';
      return;
   }
   strlcpy(g_ws_error, message, sizeof g_ws_error);
}

/* ------------------------------------------------------------------ */
/* Dynamic string buffer                                               */
/* ------------------------------------------------------------------ */

typedef struct {
   char  *data;
   size_t len;
   size_t cap;
   bool   failed;
} ws_strbuf_t;

static void sb_init(ws_strbuf_t *sb)
{
   sb->data = NULL;
   sb->len = 0u;
   sb->cap = 0u;
   sb->failed = false;
}

static void sb_free(ws_strbuf_t *sb)
{
   free(sb->data);
   sb->data = NULL;
   sb->len = 0u;
   sb->cap = 0u;
}

static bool sb_reserve(ws_strbuf_t *sb, size_t extra)
{
   size_t need;
   size_t ncap;
   char  *nd;

   if (sb->failed)
      return false;

   need = sb->len + extra + 1u;
   if (need <= sb->cap)
      return true;

   ncap = (sb->cap == 0u) ? 1024u : sb->cap;
   while (ncap < need)
      ncap *= 2u;

   nd = realloc(sb->data, ncap);
   if (nd == NULL) {
      sb->failed = true;
      return false;
   }
   sb->data = nd;
   sb->cap = ncap;
   return true;
}

static void sb_putc(ws_strbuf_t *sb, char c)
{
   if (!sb_reserve(sb, 1u))
      return;
   sb->data[sb->len++] = c;
   sb->data[sb->len] = '\0';
}

static void sb_write(ws_strbuf_t *sb, const char *p, size_t n)
{
   if (n == 0u || !sb_reserve(sb, n))
      return;
   memcpy(sb->data + sb->len, p, n);
   sb->len += n;
   sb->data[sb->len] = '\0';
}

static void sb_puts(ws_strbuf_t *sb, const char *s)
{
   sb_write(sb, s, strlen(s));
}

static void sb_printf(ws_strbuf_t *sb, const char *fmt, ...)
   __attribute__((format(printf, 2, 3)));

static void sb_printf(ws_strbuf_t *sb, const char *fmt, ...)
{
   va_list ap;
   int     n;

   va_start(ap, fmt);
   n = vsnprintf(NULL, 0u, fmt, ap);
   va_end(ap);
   if (n < 0) {
      sb->failed = true;
      return;
   }
   if (!sb_reserve(sb, (size_t)n))
      return;
   va_start(ap, fmt);
   vsnprintf(sb->data + sb->len, (size_t)n + 1u, fmt, ap);
   va_end(ap);
   sb->len += (size_t)n;
}

/* ------------------------------------------------------------------ */
/* Small text helpers                                                  */
/* ------------------------------------------------------------------ */

static int ws_lc(int c)
{
   return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int ws_stricmp(const char *a, const char *b)
{
   while (*a != '\0' && *b != '\0') {
      int d = ws_lc((unsigned char)*a) - ws_lc((unsigned char)*b);
      if (d != 0)
         return d;
      ++a;
      ++b;
   }
   return ws_lc((unsigned char)*a) - ws_lc((unsigned char)*b);
}

static bool ws_prefix(const char *prefix, const char *s)
{
   while (*prefix != '\0') {
      if (*prefix != *s)
         return false;
      ++prefix;
      ++s;
   }
   return true;
}

static bool ws_prefix_ci(const char *s, const char *prefix, size_t n)
{
   size_t k;
   for (k = 0u; k < n; ++k) {
      if (ws_lc((unsigned char)s[k]) != ws_lc((unsigned char)prefix[k]))
         return false;
   }
   return true;
}

static bool ws_prefix_ci_str(const char *s, const char *prefix)
{
   return ws_prefix_ci(s, prefix, strlen(prefix));
}

/* Case-insensitive substring search.  Returns a pointer into `hay` at
   the first match for `needle`, or NULL.  Used to scan HTTP header
   field values for tokens that may appear with arbitrary case (e.g.,
   "100-continue" in the Expect: field). */
static const char *ws_strcasestr(const char *hay, const char *needle)
{
   size_t nlen = strlen(needle);
   if (nlen == 0u)
      return hay;
   for (; *hay != '\0'; ++hay) {
      if (ws_prefix_ci(hay, needle, nlen))
         return hay;
   }
   return NULL;
}

static int ws_hexval(char c)
{
   if (c >= '0' && c <= '9')
      return c - '0';
   if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
   if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
   return -1;
}

/* Decode percent-escapes (%XX) into raw bytes.  Control characters
   (incl. NUL) decoded from %XX would let a client inject a string
   terminator past a length check or smuggle CR/LF into a header
   builder; reject them at decode time and replace with '_' so the
   resulting path always survives strlen / strcmp.  ws_path_is_safe
   later does the same control-byte check, so the two together
   defend in depth - but every consumer of ws_url_decode benefits
   from the early reject.

   Returns true if the whole input fit in dst (NUL included); false
   if it had to be truncated to fit.  Callers that want to surface
   the truncation as a 400 rather than a silent 404 use the return
   value; callers that only feed in known-bounded paths can ignore
   it (the output is always NUL-terminated regardless). */
static bool ws_url_decode(const char *src, char *dst, size_t dstsz)
{
   size_t o = 0u;
   while (*src != '\0' && o + 1u < dstsz) {
      unsigned char c;
      if (*src == '%' && src[1] != '\0' && src[2] != '\0') {
         int hi = ws_hexval(src[1]);
         int lo = ws_hexval(src[2]);
         if (hi >= 0 && lo >= 0) {
            c = (unsigned char)((hi << 4) | lo);
            dst[o++] = (c < 0x20u || c == 0x7Fu) ? '_' : (char)c;
            src += 3;
            continue;
         }
      }
      c = (unsigned char)*src++;
      dst[o++] = (c < 0x20u || c == 0x7Fu) ? '_' : (char)c;
   }
   dst[o] = '\0';
   return *src == '\0';
}

static bool ws_is_unreserved(char c)
{
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
       || (c >= '0' && c <= '9')
       || c == '-' || c == '_' || c == '.' || c == '~';
}

/* Append `s` to `sb`, percent-encoding everything that is not safe in a
   URL path.  '/' is left intact so whole paths can be encoded at once. */
static void sb_urlpath(ws_strbuf_t *sb, const char *s)
{
   static const char hex[] = "0123456789ABCDEF";
   for (; *s != '\0'; ++s) {
      unsigned char c = (unsigned char)*s;
      if (ws_is_unreserved((char)c) || c == '/') {
         sb_putc(sb, (char)c);
      } else {
         sb_putc(sb, '%');
         sb_putc(sb, hex[c >> 4]);
         sb_putc(sb, hex[c & 0x0Fu]);
      }
   }
}

/* Append `s` to `sb`, escaping the HTML metacharacters. */
static void sb_html(ws_strbuf_t *sb, const char *s)
{
   for (; *s != '\0'; ++s) {
      switch (*s) {
         case '&':  sb_puts(sb, "&amp;");  break;
         case '<':  sb_puts(sb, "&lt;");   break;
         case '>':  sb_puts(sb, "&gt;");   break;
         case '"':  sb_puts(sb, "&quot;"); break;
         case '\'': sb_puts(sb, "&#39;");  break;
         default:   sb_putc(sb, *s);       break;
      }
   }
}

static int ws_memfind(const uint8_t *hay, size_t hlen,
                      const uint8_t *need, size_t nlen)
{
   size_t i;
   if (nlen == 0u || hlen < nlen)
      return -1;
   for (i = 0u; i + nlen <= hlen; ++i) {
      if (hay[i] == need[0] && memcmp(hay + i, need, nlen) == 0)
         return (int)i;
   }
   return -1;
}

/* Return the offset just past the blank line that ends a header block,
   or -1 if it has not been seen yet. */
static int ws_find_header_end(const char *buf, size_t len)
{
   size_t i;
   for (i = 0u; i + 1u < len; ++i) {
      if (buf[i] == '\n' && buf[i + 1u] == '\n')
         return (int)(i + 2u);
      if (i + 3u < len
          && buf[i] == '\r' && buf[i + 1u] == '\n'
          && buf[i + 2u] == '\r' && buf[i + 3u] == '\n')
         return (int)(i + 4u);
   }
   return -1;
}

static const char *ws_basename(const char *p)
{
   const char *b = p;
   const char *s;
   for (s = p; *s != '\0'; ++s) {
      if (*s == '/' || *s == '\\')
         b = s + 1;
   }
   return b;
}

static void ws_human_size(uint32_t bytes, char *buf, size_t n)
{
   if (bytes < 1024u) {
      snprintf(buf, n, "%lu B", (unsigned long)bytes);
   } else if (bytes < 1024u * 1024u) {
      snprintf(buf, n, "%lu.%lu KB",
               (unsigned long)(bytes / 1024u),
               (unsigned long)((bytes % 1024u) * 10u / 1024u));
   } else {
      snprintf(buf, n, "%lu.%lu MB",
               (unsigned long)(bytes / (1024u * 1024u)),
               (unsigned long)(((bytes / 1024u) % 1024u) * 10u / 1024u));
   }
}

static void ws_ip_str(const ip4_addr_t *a, char *buf, size_t n)
{
   char *s = ip4addr_ntoa(a);
   strlcpy(buf, (s != NULL) ? s : "0.0.0.0", n);
}

/* Store a 32-bit value little-endian (BMP headers are little-endian). */
static void ws_store_u32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)(v & 0xFFu);
   p[1] = (uint8_t)((v >> 8) & 0xFFu);
   p[2] = (uint8_t)((v >> 16) & 0xFFu);
   p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* Format a uint64_t as a decimal string into out[0..sz-1] (NUL
   terminated).  The bare-metal printf here has no %llu support, so
   format the digits by hand and hand the caller a printable %s.
   A uint64_t is at most 20 decimal digits, so a 21+ byte buffer is
   always enough; sz of 24 is the recommended call size. */
static void ws_format_u64(char *out, size_t sz, uint64_t v)
{
   char tmp[24];
   size_t n = 0u;

   if (sz == 0u)
      return;

   /* Extract digits in reverse, then copy them out in order.  The
      uint64_t division compiles to a libgcc __udivdi3 call which is
      already linked in via FatFs / md5, so this adds no new toolchain
      dependency. */
   do {
      tmp[n++] = (char)('0' + (unsigned int)(v % 10u));
      v /= 10u;
   } while (v != 0u && n < sizeof tmp);

   if (n >= sz)
      n = sz - 1u;
   {
      size_t i;
      for (i = 0u; i < n; ++i)
         out[i] = tmp[n - 1u - i];
      out[n] = '\0';
   }
}

/* ------------------------------------------------------------------ */
/* HTTP request parsing                                                */
/* ------------------------------------------------------------------ */

static bool ws_parse_request_line(const char *hdr, char *method, size_t msz,
                                  char *path, size_t psz)
{
   size_t i = 0u;
   size_t o = 0u;

   while (hdr[i] != '\0' && hdr[i] != ' '
          && hdr[i] != '\r' && hdr[i] != '\n') {
      if (o + 1u < msz)
         method[o++] = hdr[i];
      ++i;
   }
   method[o] = '\0';
   if (hdr[i] != ' ')
      return false;

   while (hdr[i] == ' ')
      ++i;

   o = 0u;
   while (hdr[i] != '\0' && hdr[i] != ' '
          && hdr[i] != '\r' && hdr[i] != '\n') {
      if (o + 1u < psz)
         path[o++] = hdr[i];
      ++i;
   }
   path[o] = '\0';
   return o > 0u;
}

/* Find the value of header `name` within hdr[0..limit). */
static bool ws_find_header(const char *hdr, size_t limit, const char *name,
                           char *out, size_t osz)
{
   size_t namelen = strlen(name);
   size_t i = 0u;

   /* skip the request line */
   while (i < limit && hdr[i] != '\n')
      ++i;
   if (i < limit)
      ++i;

   while (i < limit) {
      if (i + namelen < limit
          && ws_prefix_ci(hdr + i, name, namelen)
          && hdr[i + namelen] == ':') {
         size_t v = i + namelen + 1u;
         size_t o = 0u;
         while (v < limit && (hdr[v] == ' ' || hdr[v] == '\t'))
            ++v;
         while (v < limit && hdr[v] != '\r' && hdr[v] != '\n') {
            if (o + 1u < osz)
               out[o++] = hdr[v];
            ++v;
         }
         out[o] = '\0';
         return true;
      }
      while (i < limit && hdr[i] != '\n')
         ++i;
      if (i < limit)
         ++i;
   }
   return false;
}

static bool ws_extract_boundary(const char *ctype, char *out, size_t osz)
{
   const char *b = ctype;
   size_t      o = 0u;

   while (*b != '\0' && !ws_prefix_ci(b, "boundary=", 9u))
      ++b;
   if (*b == '\0')
      return false;
   b += 9u;

   if (*b == '"') {
      ++b;
      while (*b != '\0' && *b != '"') {
         if (o + 1u < osz)
            out[o++] = *b;
         ++b;
      }
   } else {
      while (*b != '\0' && *b != ';' && *b != ' '
             && *b != '\r' && *b != '\n') {
         if (o + 1u < osz)
            out[o++] = *b;
         ++b;
      }
   }
   out[o] = '\0';
   return o > 0u;
}

static bool ws_extract_filename(const char *parthdr, char *out, size_t osz)
{
   const char *f = parthdr;
   size_t      o = 0u;

   while (*f != '\0' && !ws_prefix_ci(f, "filename=", 9u))
      ++f;
   if (*f == '\0')
      return false;
   f += 9u;
   if (*f != '"')
      return false;
   ++f;
   while (*f != '\0' && *f != '"') {
      if (o + 1u < osz)
         out[o++] = *f;
      ++f;
   }
   out[o] = '\0';
   return true;
}

/* ------------------------------------------------------------------ */
/* SD path helpers                                                     */
/* ------------------------------------------------------------------ */

static bool ws_path_is_safe(const char *p)
{
   const char *s = p;
   while (*s != '\0') {
      if ((unsigned char)*s < 0x20u)
         return false;
      ++s;
   }
   s = p;
   while (*s != '\0') {
      const char *start = s;
      size_t      len;
      while (*s != '\0' && *s != '/')
         ++s;
      len = (size_t)(s - start);
      if (len == 2u && start[0] == '.' && start[1] == '.')
         return false;
      if (*s == '/')
         ++s;
   }
   return true;
}

/* Normalise a decoded path: force a leading '/', collapse repeated
   slashes, drop a trailing slash (except for the root). */
static void ws_normalize_path(const char *raw, char *out, size_t osz)
{
   size_t o = 0u;
   const char *s;

   if (raw == NULL || raw[0] == '\0') {
      strlcpy(out, "/", osz);
      return;
   }
   if (raw[0] != '/' && o + 1u < osz)
      out[o++] = '/';
   for (s = raw; *s != '\0'; ++s) {
      if (*s == '/' && o > 0u && out[o - 1u] == '/')
         continue;
      if (o + 1u < osz)
         out[o++] = *s;
   }
   out[o] = '\0';
   while (o > 1u && out[o - 1u] == '/')
      out[--o] = '\0';
   if (o == 0u)
      strlcpy(out, "/", osz);
}

static void ws_parent_path(const char *sdpath, char *out, size_t osz)
{
   char *slash;
   strlcpy(out, sdpath, osz);
   slash = strrchr(out, '/');
   if (slash == out)
      out[1] = '\0';            /* "/x" -> "/" */
   else if (slash != NULL)
      *slash = '\0';            /* "/a/b" -> "/a" */
   else
      strlcpy(out, "/", osz);
}

static bool ws_is_root(const char *p)
{
   return p[0] == '/' && p[1] == '\0';
}

/* ------------------------------------------------------------------ */
/* Digest authentication (RFC 2617, qop=auth, MD5)                     */
/* ------------------------------------------------------------------ */

/* True if Pi1MHz.cfg set both webdav_user and webdav_password.
   Auth is enforced on every route only in that case; otherwise the
   webserver behaves as the legacy unauthenticated file browser. */
static bool ws_digest_enabled(void)
{
   const wifi_config_t *cfg = wifi_get_config();

   return cfg != NULL
       && cfg->webdav_user[0]     != '\0'
       && cfg->webdav_password[0] != '\0';
}

/* Mix the chip's WiFi MAC plus the system timer into the secret on
   first use.  RPI_GetSystemTime at boot is highly predictable
   (a few hundred ms) on its own, so an attacker who could time the
   first 401 challenge could guess the secret to within a few k of
   values.  The chip MAC (read once from cur_etheraddr by
   sdio_runtime_get_chip_mac) is 24 bits of board-OTP entropy on top
   of that, which is enough to push the search space out of trivial
   range.  Still NOT cryptographically strong - digest auth over
   plain HTTP isn't secret anyway - but materially harder. */
static uint32_t ws_seed_secret(void)
{
   uint8_t  mac[6] = { 0u, 0u, 0u, 0u, 0u, 0u };
   uint32_t s = RPI_GetSystemTime();
   (void)sdio_runtime_get_chip_mac(mac);
   s ^= ((uint32_t)mac[0] <<  0)
      | ((uint32_t)mac[1] <<  8)
      | ((uint32_t)mac[2] << 16)
      | ((uint32_t)mac[3] << 24);
   s ^= ((uint32_t)mac[4] <<  0) | ((uint32_t)mac[5] << 16);
   s ^= 0xA5A5A5A5u;
   return s;
}

static void ws_digest_refresh_nonce(void)
{
   uint32_t now;
   uint8_t  seed[12];
   md5_ctx_t ctx;
   uint8_t  digest[MD5_DIGEST_LEN];
   static const char hex[] = "0123456789abcdef";
   unsigned int i;

   now = RPI_GetSystemTime();

   if (!g_ws_nonce_secret_initialised) {
      g_ws_nonce_secret = ws_seed_secret();
      g_ws_nonce_secret_initialised = true;
   }

   if (g_ws_nonce[0] != '\0'
       && (now - g_ws_nonce_issued_us) < WS_NONCE_MAX_AGE_US)
      return;

   ++g_ws_nonce_counter;
   seed[ 0] = (uint8_t)(now                  );
   seed[ 1] = (uint8_t)(now >>  8            );
   seed[ 2] = (uint8_t)(now >> 16            );
   seed[ 3] = (uint8_t)(now >> 24            );
   seed[ 4] = (uint8_t)(g_ws_nonce_secret    );
   seed[ 5] = (uint8_t)(g_ws_nonce_secret >>  8);
   seed[ 6] = (uint8_t)(g_ws_nonce_secret >> 16);
   seed[ 7] = (uint8_t)(g_ws_nonce_secret >> 24);
   seed[ 8] = (uint8_t)(g_ws_nonce_counter   );
   seed[ 9] = (uint8_t)(g_ws_nonce_counter >>  8);
   seed[10] = (uint8_t)(g_ws_nonce_counter >> 16);
   seed[11] = (uint8_t)(g_ws_nonce_counter >> 24);

   md5_init(&ctx);
   md5_update(&ctx, seed, sizeof seed);
   md5_final(&ctx, digest);

   for (i = 0u; i < MD5_DIGEST_LEN; ++i) {
      g_ws_nonce[(i * 2u)     ] = hex[(digest[i] >> 4) & 0x0Fu];
      g_ws_nonce[(i * 2u) + 1u] = hex[ digest[i]       & 0x0Fu];
   }
   g_ws_nonce[MD5_HEX_LEN] = '\0';
   g_ws_nonce_issued_us = now;
}

/* True if the supplied nonce is the current one AND was issued less than
   WS_NONCE_MAX_AGE_US ago.  A non-current-but-recent nonce is treated as
   stale (caller signals stale=true in the challenge so the client
   transparently re-authenticates). */
static bool ws_digest_nonce_current(const char *nonce)
{
   uint32_t now;

   if (nonce == NULL || g_ws_nonce[0] == '\0')
      return false;
   if (strcmp(nonce, g_ws_nonce) != 0)
      return false;

   now = RPI_GetSystemTime();
   return (now - g_ws_nonce_issued_us) < WS_NONCE_MAX_AGE_US;
}

/* Extract a quoted or token value for `key` from a digest Authorization
   header.  Returns true and writes into out[] (NUL-terminated) when
   found.  The parser is intentionally tolerant: it accepts
   key=value, key="value", and the various whitespace + comma layouts
   browsers actually emit. */
static bool ws_digest_field(const char *hdr, const char *key,
                            char *out, size_t out_sz)
{
   size_t keylen = strlen(key);
   const char *p = hdr;

   while (*p != '\0') {
      while (*p == ' ' || *p == '\t' || *p == ',')
         ++p;
      if (*p == '\0')
         break;
      if (ws_prefix_ci(p, key, keylen) && p[keylen] == '=') {
         p += keylen + 1u;
         size_t o = 0u;
         if (*p == '"') {
            ++p;
            while (*p != '\0' && *p != '"') {
               if (o + 1u < out_sz)
                  out[o++] = *p;
               ++p;
            }
            if (*p == '"')
               // cppcheck-suppress unreadVariable
               ++p;
         } else {
            while (*p != '\0' && *p != ',' && *p != ' ' && *p != '\t') {
               if (o + 1u < out_sz)
                  out[o++] = *p;
               ++p;
            }
         }
         out[o] = '\0';
         return true;
      }
      /* skip this key=... pair */
      while (*p != '\0' && *p != ',') {
         if (*p == '"') {
            ++p;
            while (*p != '\0' && *p != '"')
               ++p;
            if (*p == '"')
               ++p;
         } else {
            ++p;
         }
      }
   }
   return false;
}

/* Build the HA1 = MD5(user:realm:password) hex string. */
static void ws_digest_compute_ha1(md5_hex_t ha1)
{
   const wifi_config_t *cfg = wifi_get_config();
   const void *parts[5];
   size_t      lens[5];

   parts[0] = cfg->webdav_user;     lens[0] = strlen(cfg->webdav_user);
   parts[1] = ":";                  lens[1] = 1u;
   parts[2] = cfg->webdav_realm;    lens[2] = strlen(cfg->webdav_realm);
   parts[3] = ":";                  lens[3] = 1u;
   parts[4] = cfg->webdav_password; lens[4] = strlen(cfg->webdav_password);

   md5_hex_cat(ha1, parts, lens, 5);
}

/* Build HA2 = MD5(method:uri). */
static void ws_digest_compute_ha2(md5_hex_t ha2,
                                  const char *method, const char *uri)
{
   const void *parts[3];
   size_t      lens[3];

   parts[0] = method; lens[0] = strlen(method);
   parts[1] = ":";    lens[1] = 1u;
   parts[2] = uri;    lens[2] = strlen(uri);

   md5_hex_cat(ha2, parts, lens, 3);
}

/* Build the expected response hash (qop=auth flavour):
   MD5(HA1:nonce:nc:cnonce:qop:HA2). */
static void ws_digest_compute_response(md5_hex_t response,
                                       const char *ha1, const char *nonce,
                                       const char *nc, const char *cnonce,
                                       const char *qop, const char *ha2)
{
   const void *parts[11];
   size_t      lens[11];

   parts[ 0] = ha1;    lens[ 0] = MD5_HEX_LEN;
   parts[ 1] = ":";    lens[ 1] = 1u;
   parts[ 2] = nonce;  lens[ 2] = strlen(nonce);
   parts[ 3] = ":";    lens[ 3] = 1u;
   parts[ 4] = nc;     lens[ 4] = strlen(nc);
   parts[ 5] = ":";    lens[ 5] = 1u;
   parts[ 6] = cnonce; lens[ 6] = strlen(cnonce);
   parts[ 7] = ":";    lens[ 7] = 1u;
   parts[ 8] = qop;    lens[ 8] = strlen(qop);
   parts[ 9] = ":";    lens[ 9] = 1u;
   parts[10] = ha2;    lens[10] = MD5_HEX_LEN;

   md5_hex_cat(response, parts, lens, 11);
}

/* Build the RFC-2069 (qop-absent) response hash: MD5(HA1:nonce:HA2).
   Some HTTP clients (older curl, some library defaults) still send no
   qop; we accept both forms. */
static void ws_digest_compute_response_legacy(md5_hex_t response,
                                              const char *ha1,
                                              const char *nonce,
                                              const char *ha2)
{
   const void *parts[5];
   size_t      lens[5];

   parts[0] = ha1;   lens[0] = MD5_HEX_LEN;
   parts[1] = ":";   lens[1] = 1u;
   parts[2] = nonce; lens[2] = strlen(nonce);
   parts[3] = ":";   lens[3] = 1u;
   parts[4] = ha2;   lens[4] = MD5_HEX_LEN;

   md5_hex_cat(response, parts, lens, 5);
}

/* Compare two MD5 hex strings case-insensitively.  Reject up front
   if received_hex is not exactly MD5_HEX_LEN characters - this makes
   the byte-compare loop a fixed MD5_HEX_LEN iterations whose runtime
   does not depend on which bytes differ, which is the meaningful
   property for digest auth (timing leaks of the matched-prefix length
   would let an attacker incrementally guess a valid response).  We
   do NOT claim true constant-time at the strlen prefix - on this
   platform with no shared cache and a single-threaded poll loop the
   timing budget for an attacker is generous regardless - but the OR
   fold over all MD5_HEX_LEN bytes is honest about its intent.

   `expected_hex` is the locally-computed digest from md5_hex_cat
   (exactly MD5_HEX_LEN chars, NOT NUL-terminated - reading the index
   MD5_HEX_LEN slot would be a buffer overread).  `received_hex` is
   the value pulled out of the client's Authorization header (always
   NUL-terminated by ws_digest_field); we require it to be exactly
   MD5_HEX_LEN chars long. */
static bool ws_hex_eq_ci(const md5_hex_t expected_hex,
                         const char *received_hex)
{
   unsigned int diff = 0u;
   size_t i;

   /* Length check first - explicit, not folded into the loop - so the
      compare body is always exactly MD5_HEX_LEN iterations and never
      reaches past either buffer. */
   if (strlen(received_hex) != MD5_HEX_LEN)
      return false;

   for (i = 0u; i < MD5_HEX_LEN; ++i) {
      diff |= (unsigned int)((unsigned char)ws_lc((unsigned char)expected_hex[i])
                           ^ (unsigned char)ws_lc((unsigned char)received_hex[i]));
   }
   return diff == 0u;
}

typedef enum {
   WS_AUTH_OK = 0,         /* credentials matched                       */
   WS_AUTH_REQUIRED,       /* no/invalid Authorization, send challenge  */
   WS_AUTH_STALE           /* hash OK but nonce expired, send stale=true*/
} ws_auth_status_t;

static ws_auth_status_t ws_digest_verify(const char *method,
                                         const char *header_block,
                                         size_t header_limit)
{
   char authz[640];
   char field_nonce[MD5_HEX_LEN + 1];
   char field_response[MD5_HEX_LEN + 1];
   const wifi_config_t *cfg = wifi_get_config();
   md5_hex_t ha2;
   md5_hex_t expected;

   if (!ws_find_header(header_block, header_limit, "Authorization",
                       authz, sizeof authz))
      return WS_AUTH_REQUIRED;

   if (!ws_prefix_ci_str(authz, "Digest "))
      return WS_AUTH_REQUIRED;

   /* Step past the "Digest " scheme prefix BEFORE handing the buffer to
      ws_digest_field.  The parser walks key=value pairs separated by
      commas; without this skip, its first iteration treats the bare
      "Digest" token + the first pair (e.g. Digest username="Pi1MHz") as
      a single value because there's no comma between them, and the
      username key is then never matched.  The HTTP-header parsing
      guarantees ws_find_header trims leading whitespace, so authz
      begins exactly with "Digest " (7 bytes) and an additional
      whitespace skip handles servers that might send "Digest  " etc. */
   {
      char         field_username[WIFI_WEBDAV_USER_MAX_LEN + 1];
      char         field_realm[WIFI_WEBDAV_REALM_MAX_LEN + 1];
      char         field_uri[WS_PATH_MAX];
      char         field_qop[16];
      md5_hex_t    ha1;
      bool         have_qop;
      const char *fields = authz + 7;            /* past "Digest " */
      while (*fields == ' ' || *fields == '\t')
         ++fields;

      /* Required fields */
      if (!ws_digest_field(fields, "username", field_username, sizeof field_username)
          || !ws_digest_field(fields, "nonce",    field_nonce,    sizeof field_nonce)
          || !ws_digest_field(fields, "uri",      field_uri,      sizeof field_uri)
          || !ws_digest_field(fields, "response", field_response, sizeof field_response))
         return WS_AUTH_REQUIRED;

      /* Optional realm: if present must match ours, but we don't require it. */
      if (ws_digest_field(fields, "realm", field_realm, sizeof field_realm)) {
         if (strcmp(field_realm, cfg->webdav_realm) != 0)
            return WS_AUTH_REQUIRED;
      }

      if (strcmp(field_username, cfg->webdav_user) != 0)
         return WS_AUTH_REQUIRED;

      ws_digest_compute_ha1(ha1);
      ws_digest_compute_ha2(ha2, method, field_uri);

      have_qop = ws_digest_field(fields, "qop", field_qop, sizeof field_qop)
              && field_qop[0] != '\0';

      if (have_qop) {
         char field_nc[16];
         char field_cnonce[64];
         if (!ws_digest_field(fields, "nc",     field_nc,     sizeof field_nc)
             || !ws_digest_field(fields, "cnonce", field_cnonce, sizeof field_cnonce))
            return WS_AUTH_REQUIRED;
         ws_digest_compute_response(expected, ha1, field_nonce,
                                    field_nc, field_cnonce, field_qop, ha2);
      } else {
         ws_digest_compute_response_legacy(expected, ha1, field_nonce, ha2);
      }
   }

   if (!ws_hex_eq_ci(expected, field_response))
      return WS_AUTH_REQUIRED;

   /* The hash matched, but the nonce may be too old.  Tell the client
      to repeat with the freshly issued one. */
   if (!ws_digest_nonce_current(field_nonce))
      return WS_AUTH_STALE;

   return WS_AUTH_OK;
}

/* Build a WWW-Authenticate digest challenge into `out`.  Caller will
   wedge this into a 401 response.  Returns the number of bytes
   written (always < out_sz). */
static size_t ws_digest_build_challenge(char *out, size_t out_sz, bool stale)
{
   const wifi_config_t *cfg = wifi_get_config();
   int n;

   ws_digest_refresh_nonce();

   n = snprintf(out, out_sz,
                "WWW-Authenticate: Digest realm=\"%s\", qop=\"auth\", "
                "algorithm=MD5, nonce=\"%s\"%s\r\n",
                cfg->webdav_realm, g_ws_nonce, stale ? ", stale=true" : "");
   if (n < 0 || (size_t)n >= out_sz)
      return 0u;
   return (size_t)n;
}

/* ------------------------------------------------------------------ */
/* Forward declarations                                                */
/* ------------------------------------------------------------------ */

/* conn_close / conn_pump return true if the connection was torn down
   via tcp_abort() rather than tcp_close().  lwIP's raw-API contract
   requires any callback that calls tcp_abort to return ERR_ABRT, so
   ws_sent / ws_poll propagate this up. */
static bool conn_close(ws_conn_t *c, bool abort_conn);
static bool conn_pump(ws_conn_t *c);
static bool conn_consume(ws_conn_t *c, const uint8_t *data, size_t len);
static void conn_reset_for_next_request(ws_conn_t *c, size_t pipelined_keep);
static bool process_request(ws_conn_t *c, int body_at);
static bool upload_consume(ws_conn_t *c, const uint8_t *data, size_t len);
/* WebDAV body-consumer + the SD-file fallback used by the GET path. */
static bool dav_put_consume(ws_conn_t *c, const uint8_t *data, size_t len);
static bool start_download(ws_conn_t *c, const char *sdpath);

/* ------------------------------------------------------------------ */
/* HTML page scaffolding                                               */
/* ------------------------------------------------------------------ */

static void page_open(ws_strbuf_t *b, const char *title)
{
   sb_puts(b,
      "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>");
   sb_html(b, title);
   sb_puts(b,
      "</title><style>"
      "body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;margin:0;"
      "background:#f4f4f6;color:#1d1d1f}"
      "header{background:#1d1d2e;color:#fff;padding:14px 20px;font-size:18px;"
      "font-weight:600}"
      "header a{color:#9db8ff;text-decoration:none;font-size:14px;"
      "font-weight:400;margin-left:14px}"
      "main{max-width:880px;margin:0 auto;padding:20px}"
      "h1{font-size:20px;margin:0 0 14px;word-break:break-all}"
      "a{color:#2c5fd6}"
      "table{width:100%;border-collapse:collapse;background:#fff;"
      "border-radius:8px;overflow:hidden}"
      "th,td{text-align:left;padding:9px 12px;border-bottom:1px solid #e6e6ea;"
      "font-size:14px}"
      "th{background:#ececf0}"
      "tr:last-child td{border-bottom:none}"
      "td.r,th.r{text-align:right;color:#666;white-space:nowrap}"
      ".card{background:#fff;border-radius:8px;padding:16px;margin-bottom:16px}"
      ".err{color:#c0271a}"
      ".muted{color:#777;font-size:13px}"
      "input[type=submit]{background:#2c5fd6;color:#fff;border:0;"
      "border-radius:6px;padding:8px 16px;font-size:14px;cursor:pointer}"
      "</style></head><body><header>Pi1MHz"
      "<a href=\"/\">Home</a><a href=\"/files/\">Files</a>"
      "<a href=\"/framebuffer\">Screen</a><a href=\"/status\">Status</a>"
      "</header><main>");
}

static void page_close(ws_strbuf_t *b)
{
   sb_puts(b, "</main></body></html>");
}

/* Append the "/files..." URL for an SD directory path. */
static void append_files_url(ws_strbuf_t *b, const char *sdpath)
{
   sb_puts(b, "/files");
   if (ws_is_root(sdpath))
      sb_putc(b, '/');
   else
      sb_urlpath(b, sdpath);
}

static void table_row(ws_strbuf_t *b, const char *label, const char *value)
{
   sb_puts(b, "<tr><th>");
   sb_html(b, label);
   sb_puts(b, "</th><td>");
   sb_html(b, value);
   sb_puts(b, "</td></tr>");
}

/* ------------------------------------------------------------------ */
/* Connection lifecycle                                                */
/* ------------------------------------------------------------------ */

static bool conn_close(ws_conn_t *c, bool abort_conn)
{
   bool aborted = false;

   if (c == NULL)
      return false;

   if (c->dl_open) {
      f_close(&c->dl_file);
      c->dl_open = false;
   }
   if (c->up_file_open) {
      f_close(&c->write_file.up);
      c->up_file_open = false;
   }
   if (c->dav_put_open) {
      if (wifi_debug_enabled())
         wifi_debug_printf("PUT: conn_close mid-PUT (dav_remaining=%lu target='%s')\n",
                           (unsigned long)c->dav_remaining, c->dav_put_target);
      f_close(&c->write_file.dav);
      c->dav_put_open = false;
      /* Connection torn down mid-PUT: discard the partial temp file so
         the SD card does not accumulate "<name>.part" droppings.  The
         final target was deliberately not touched until the body
         completed (dav_put_consume's f_rename) so the user's previous
         file - if any - is intact. */
      if (c->dav_put_tmppath[0] != '\0')
         (void)f_unlink(c->dav_put_tmppath);
   }
   if (c->copy_src_open) {
      f_close(&c->copy_src);
      c->copy_src_open = false;
   }
   if (c->copy_dst_open) {
      f_close(&c->copy_dst);
      c->copy_dst_open = false;
   }
   /* Clear the active-COPY slot if we were the one holding it: a
      timeout / client-disconnect mid-COPY must release the slot so
      the next COPY request isn't rejected with 503 forever. */
   if (g_ws_active_copy == c)
      g_ws_active_copy = NULL;

   if (c->pcb != NULL) {
      struct tcp_pcb *pcb = c->pcb;
      c->pcb = NULL;
      tcp_arg(pcb, NULL);
      tcp_recv(pcb, NULL);
      tcp_sent(pcb, NULL);
      tcp_poll(pcb, NULL, 0);
      tcp_err(pcb, NULL);
      if (abort_conn) {
         tcp_abort(pcb);
         aborted = true;
      } else if (tcp_close(pcb) != ERR_OK) {
         tcp_abort(pcb);
         aborted = true;
      }
   }

   free(c->out);
   free(c);
   return aborted;
}

/* Convert one framebuffer scanline into a 24-bit bottom-up BMP row.
   bmp_row 0 is the bottom of the image; framebuffer memory is top-down,
   so it maps to framebuffer row (height - 1 - bmp_row).  Exactly
   row_padded bytes are written (BGR pixels followed by zero padding). */
static void fb_render_row(const framebuffer_export_info_t *info,
                          uint32_t bmp_row, uint8_t *dst, uint32_t row_padded)
{
   const uint8_t *fb  = (const uint8_t *)(uintptr_t)info->address;
   const uint8_t *src = fb + (size_t)(info->height - 1u - bmp_row)
                             * info->pitch;
   uint32_t x;

   for (x = 0u; x < info->width; ++x) {
      uint8_t cr;
      uint8_t cg;
      uint8_t cb;

      if (info->bits_per_pixel == 8u) {
         uint32_t e = screen_get_palette_entry(src[x]);   /* 0x00RRGGBB */
         cr = (uint8_t)((e >> 16) & 0xFFu);
         cg = (uint8_t)((e >> 8) & 0xFFu);
         cb = (uint8_t)(e & 0xFFu);
      } else if (info->bits_per_pixel == 16u) {
         uint32_t v  = (uint32_t)src[x * 2u]
                     | ((uint32_t)src[x * 2u + 1u] << 8);
         uint32_t r5 = (v >> 11) & 0x1Fu;
         uint32_t g6 = (v >> 5) & 0x3Fu;
         uint32_t b5 = v & 0x1Fu;
         cr = (uint8_t)((r5 << 3) | (r5 >> 2));
         cg = (uint8_t)((g6 << 2) | (g6 >> 4));
         cb = (uint8_t)((b5 << 3) | (b5 >> 2));
      } else {
         cr = src[x * 4u];
         cg = src[x * 4u + 1u];
         cb = src[x * 4u + 2u];
      }

      dst[x * 3u]      = cb;
      dst[x * 3u + 1u] = cg;
      dst[x * 3u + 2u] = cr;
   }

   for (x = info->width * 3u; x < row_padded; ++x)
      dst[x] = 0u;
}

static bool conn_pump(ws_conn_t *c)
{
   err_t err = ERR_OK;

   if (c == NULL || c->pcb == NULL)
      return false;

   /* in-memory portion (HTML body, or the HTTP header of a download) */
   while (c->out != NULL && c->out_sent < c->out_len) {
      u16_t  avail = tcp_sndbuf(c->pcb);
      size_t want  = c->out_len - c->out_sent;
      if (avail == 0u)
         break;
      if (want > avail)
         want = avail;
      err = tcp_write(c->pcb, c->out + c->out_sent, (u16_t)want,
                      TCP_WRITE_FLAG_COPY);
      if (err != ERR_OK)
         break;
      c->out_sent += want;
      c->bytes_queued += (uint32_t)want;
   }

   /* file body - skipped entirely for HEAD requests; the matching
      file_done condition below treats CONN_SEND_FILE + is_head as
      "nothing more to produce" so the connection closes after the
      headers have been ACKed. */
   if (err == ERR_OK && !c->is_head && c->state == CONN_SEND_FILE
       && (c->out == NULL || c->out_sent >= c->out_len)) {
      while (1) {
         u16_t  avail;
         size_t want;

         if (c->dl_buf_sent >= c->dl_buf_len) {
            UINT br = 0u;
            UINT req;
            if (c->dl_remaining == 0u || c->dl_eof)
               break;
            req = (c->dl_remaining < WS_FILE_CHUNK)
                  ? (UINT)c->dl_remaining : (UINT)WS_FILE_CHUNK;
            if (f_read(&c->dl_file, c->dl_buf, req, &br) != FR_OK
                || br == 0u) {
               c->dl_eof = true;
               break;
            }
            c->dl_buf_len = br;
            c->dl_buf_sent = 0u;
            c->dl_remaining -= br;
         }

         avail = tcp_sndbuf(c->pcb);
         if (avail == 0u)
            break;
         want = c->dl_buf_len - c->dl_buf_sent;
         if (want > avail)
            want = avail;
         err = tcp_write(c->pcb, c->dl_buf + c->dl_buf_sent, (u16_t)want,
                         TCP_WRITE_FLAG_COPY);
         if (err != ERR_OK)
            break;
         c->dl_buf_sent += want;
         c->bytes_queued += (uint32_t)want;
      }
   }

   /* framebuffer body: generate BMP rows on demand, a bounded few per
      call, so the conversion is spread across ticks rather than done in
      one long burst inside a single callback.  Skipped for HEAD - the
      caller already got the Content-Length / Content-Type headers it
      needed. */
   if (err == ERR_OK && !c->is_head && c->state == CONN_SEND_FB
       && (c->out == NULL || c->out_sent >= c->out_len)) {
      uint32_t row_padded = (c->fb_info.width * 3u + 3u) & ~3u;

      while (1) {
         u16_t  avail;
         size_t want;

         if (c->dl_buf_sent >= c->dl_buf_len) {
            size_t filled = 0u;

            if (c->fb_row >= c->fb_info.height)
               break;                      /* every row generated */
            /* The framebuffer is live: a mode change mid-download can
               shrink or move the allocation.  Re-check the geometry per
               refill and blank the remaining rows rather than reading
               through the stale snapshot (Content-Length has already
               been promised, so the row count cannot change). */
            if (!c->fb_stale) {
               framebuffer_export_info_t cur;
               if (!framebuffer_export_get_info(&cur)
                   || cur.address != c->fb_info.address
                   || cur.pitch != c->fb_info.pitch
                   || cur.width != c->fb_info.width
                   || cur.height != c->fb_info.height
                   || cur.bits_per_pixel != c->fb_info.bits_per_pixel
                   || cur.size < c->fb_info.size)
                  c->fb_stale = true;
            }
            while (c->fb_row < c->fb_info.height
                   && filled + row_padded <= sizeof c->dl_buf) {
               if (c->fb_stale)
                  memset(&c->dl_buf[filled], 0, row_padded);
               else
                  fb_render_row(&c->fb_info, c->fb_row,
                                &c->dl_buf[filled], row_padded);
               filled += row_padded;
               c->fb_row++;
            }
            if (filled == 0u)
               break;                      /* row wider than dl_buf */
            c->dl_buf_len = filled;
            c->dl_buf_sent = 0u;
         }

         avail = tcp_sndbuf(c->pcb);
         if (avail == 0u)
            break;
         want = c->dl_buf_len - c->dl_buf_sent;
         if (want > avail)
            want = avail;
         err = tcp_write(c->pcb, c->dl_buf + c->dl_buf_sent, (u16_t)want,
                         TCP_WRITE_FLAG_COPY);
         if (err != ERR_OK)
            break;
         c->dl_buf_sent += want;
         c->bytes_queued += (uint32_t)want;
      }
   }

   if (c->pcb != NULL)
      tcp_output(c->pcb);

   {
      bool mem_done = (c->out == NULL) || (c->out_sent >= c->out_len);
      /* For HEAD, file_done and fb_done are immediately true: the
         body branches above are skipped, so the only thing left to
         drain is the headers in c->out (covered by mem_done).
         conn_close will f_close the open dl_file on the way out. */
      bool file_done = (c->state != CONN_SEND_FILE) || c->is_head
                    || ((c->dl_buf_sent >= c->dl_buf_len)
                        && (c->dl_remaining == 0u || c->dl_eof));
      bool fb_done = (c->state != CONN_SEND_FB) || c->is_head
                    || ((c->dl_buf_sent >= c->dl_buf_len)
                        && c->fb_row >= c->fb_info.height);
      c->producing_done = mem_done && file_done && fb_done;
   }

   /* Only a connection that is actually SENDING can be completed by an
      ACK.  The interim "HTTP/1.1 100 Continue" is written straight to
      the pcb while the state is still CONN_RECV_DAV_PUT, so it is not
      accounted in c->out or c->bytes_queued.  Without this guard the
      ACK of those 25 bytes satisfies the test below - c->out is NULL
      so producing_done is true, and bytes_acked >= bytes_queued - and
      conn_reset_for_next_request would tear the upload down mid-body:
      temp file closed, .part unlinked, dav_remaining cleared, state
      back to CONN_RECV_HEADER.  The body bytes that followed were then
      parsed as a request line, which is what Windows Explorer (which
      sends Expect: 100-continue on every PUT) saw as 0x8007003A.
      Accounting the interim bytes into bytes_queued is not sufficient
      on its own, because 0 >= 0 still passes at the top of a
      body-receive state. */
   bool is_sending = (c->state == CONN_SEND_MEM)
                  || (c->state == CONN_SEND_FILE)
                  || (c->state == CONN_SEND_FB);

   if (is_sending && c->producing_done && c->bytes_acked >= c->bytes_queued) {
      if (c->keep_alive && !c->pipelined_bytes_dropped) {
         /* Response delivered and ACKed.  Reuse the TCP socket for
            the next request - this is what removes the "long pause"
            on Windows Explorer's body-bearing PUT (no fresh TCP
            handshake, no re-issue of the digest 401 challenge, and
            the unauth-PUT drain workaround does not run because the
            credentials cached on the previous request are still
            valid). */
         conn_reset_for_next_request(c, 0u);
         return false;
      }
      return conn_close(c, false);
   }

   return false;
}

/* ------------------------------------------------------------------ */
/* Response helpers                                                    */
/* ------------------------------------------------------------------ */

static bool ws_oom(ws_conn_t *c)
{
   (void)conn_close(c, true);
   return false;
}

/* Returns the Connection: header line every response should include.
   When this connection has been kept alive for the next request the
   client gets "Connection: keep-alive\r\n"; otherwise "close\r\n" so
   the client tears the socket down after the response.  Including
   the header explicitly on every response is required by RFC 9112
   §9.1 / §9.6 for the "close" case and recommended for "keep-alive"
   so the intent is unambiguous to HTTP/1.0 clients. */
static const char *ws_connection_hdr(const ws_conn_t *c)
{
   return c->keep_alive ? "Connection: keep-alive\r\n"
                        : "Connection: close\r\n";
}

/* Reset all per-request state on a kept-alive connection so the
   next request starts from a known empty position.  Anything that
   would be cleaned up by conn_close (open files, COPY slot, in-
   flight response buffer) is cleaned up here too, but the TCP pcb
   is preserved.  pipelined_keep, if non-zero, is the count of
   already-received body bytes for the NEXT request that we leave
   at the start of reqhdr; conn_consume will re-enter the header
   parser and continue from there. */
static void conn_reset_for_next_request(ws_conn_t *c, size_t pipelined_keep)
{
   if (c == NULL)
      return;
   if (c->dl_open) {
      f_close(&c->dl_file);
      c->dl_open = false;
   }
   if (c->up_file_open) {
      f_close(&c->write_file.up);
      c->up_file_open = false;
   }
   if (c->dav_put_open) {
      f_close(&c->write_file.dav);
      c->dav_put_open = false;
      if (c->dav_put_tmppath[0] != '\0')
         (void)f_unlink(c->dav_put_tmppath);
   }
   if (c->copy_src_open) {
      f_close(&c->copy_src);
      c->copy_src_open = false;
   }
   if (c->copy_dst_open) {
      f_close(&c->copy_dst);
      c->copy_dst_open = false;
   }
   c->copy_dst_existed = false;
   if (g_ws_active_copy == c)
      g_ws_active_copy = NULL;
   free(c->out);
   c->out = NULL;
   c->out_len = 0u;
   c->out_sent = 0u;
   c->bytes_queued = 0u;
   c->bytes_acked = 0u;
   c->producing_done = false;
   c->poll_count = 0u;
   c->is_head = false;
   c->dav_put_draining = false;
   c->dav_put_chunked = false;
   c->dav_put_buf_len = 0u;
   c->dav_remaining = 0u;
   c->dav_put_target[0] = '\0';
   c->dav_put_tmppath[0] = '\0';
   c->dl_remaining = 0u;
   c->dl_eof = false;
   c->dl_buf_len = 0u;
   c->dl_buf_sent = 0u;
   c->up_state = UP_PART_HEADER;
   c->up_complete = false;
   c->up_head_len = 0u;
   c->up_tail_len = 0u;
   c->up_delim_len = 0u;
   c->up_bytes_written = 0u;
   c->fb_row = 0u;
   c->fb_stale = false;
   c->pipelined_bytes_dropped = false;
   if (pipelined_keep > 0u && pipelined_keep <= c->reqhdr_len) {
      memmove(c->reqhdr, c->reqhdr + (c->reqhdr_len - pipelined_keep),
              pipelined_keep);
      c->reqhdr_len = pipelined_keep;
      /* pipelined_keep <= reqhdr_len, and reqhdr_len is invariantly
         <= WS_HEADER_MAX (capped in ws_recv), so this NUL lands at the
         last valid index at most. cppcheck cannot see the cross-function
         cap, hence the false positive. */
      // cppcheck-suppress arrayIndexOutOfBoundsCond
      c->reqhdr[pipelined_keep] = '\0';
   } else {
      c->reqhdr_len = 0u;
      c->reqhdr[0] = '\0';
   }
   c->state = CONN_RECV_HEADER;
}

/* Wrap an HTML body strbuf in a complete HTTP response and start it.
   Always consumes `body`.  Returns false only if the connection had to
   be aborted (out of memory). */
static bool ws_finish_html(ws_conn_t *c, int status, const char *stext,
                           ws_strbuf_t *body)
{
   ws_strbuf_t r;

   if (body->failed) {
      sb_free(body);
      return ws_oom(c);
   }

   sb_init(&r);
   sb_printf(&r,
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: text/html; charset=utf-8\r\n"
             "Content-Length: %lu\r\n"
             "%s"
             "\r\n",
             status, stext, (unsigned long)body->len,
             ws_connection_hdr(c));
   if (body->len > 0u && body->data != NULL)
      sb_write(&r, body->data, body->len);
   sb_free(body);

   if (r.failed) {
      sb_free(&r);
      return ws_oom(c);
   }

   free(c->out);
   c->out = r.data;
   c->out_len = r.len;
   c->out_sent = 0u;
   c->bytes_queued = 0u;
   c->bytes_acked = 0u;
   c->state = CONN_SEND_MEM;
   conn_pump(c);
   return true;
}

/* Build a 401 Unauthorized response carrying the digest challenge.
   Used both on the very first request (no Authorization header) and on
   stale=true. */
static bool ws_send_auth_challenge(ws_conn_t *c, bool stale)
{
   char         challenge[WS_AUTH_HEADER_MAX];
   size_t       challenge_len = ws_digest_build_challenge(challenge,
                                                          sizeof challenge,
                                                          stale);
   ws_strbuf_t  r;
   static const char body[] =
      "<!DOCTYPE html><html><body><h1>401 Unauthorized</h1>"
      "<p>This resource requires authentication.</p></body></html>";

   if (challenge_len == 0u)
      return ws_oom(c);

   sb_init(&r);
   sb_printf(&r,
             "HTTP/1.1 401 Unauthorized\r\n"
             "%s"
             "Content-Type: text/html; charset=utf-8\r\n"
             "Content-Length: %u\r\n"
             "%s"
             "\r\n",
             challenge, (unsigned int)(sizeof(body) - 1u),
             ws_connection_hdr(c));
   sb_write(&r, body, sizeof(body) - 1u);

   if (r.failed) {
      sb_free(&r);
      return ws_oom(c);
   }

   free(c->out);
   c->out = r.data;
   c->out_len = r.len;
   c->out_sent = 0u;
   c->bytes_queued = 0u;
   c->bytes_acked = 0u;
   c->state = CONN_SEND_MEM;
   conn_pump(c);
   return true;
}

static bool ws_error(ws_conn_t *c, int status, const char *stext,
                     const char *msg)
{
   ws_strbuf_t b;
   /* Log error responses to the debug console, but skip the 404
      misses that Windows Explorer generates on every directory open
      (desktop.ini, Thumbs.db, "AlbumArtSmall.jpg", "Folder.jpg" and
      similar probes - they're entirely routine and would otherwise
      drown the rest of the trace). */
   if (wifi_debug_enabled() && status != 404)
      wifi_debug_printf("RESP %d %s : %s\n", status, stext, msg);
   sb_init(&b);
   page_open(&b, stext);
   sb_printf(&b, "<h1>%d ", status);
   sb_html(&b, stext);
   sb_puts(&b, "</h1><div class=\"card\"><p class=\"err\">");
   sb_html(&b, msg);
   sb_puts(&b, "</p><p><a href=\"/\">Return home</a></p></div>");
   page_close(&b);
   return ws_finish_html(c, status, stext, &b);
}

/* 405 Method Not Allowed with the RFC 9110 §10.2.1-required Allow
   header.  Without Allow, generic HTTP clients (curl, browsers,
   strict WebDAV stacks) cannot tell which methods would succeed
   instead, and some hold the connection trying to puzzle out
   alternatives.  The default Allow list ("OPTIONS, GET, HEAD,
   PROPFIND") covers the routes accessible at every URL; callers
   that have a tighter set (e.g. "OPTIONS, PROPFIND" on the root
   for PUT-rejected paths) pass it through `allow_methods`. */
static bool ws_method_not_allowed(ws_conn_t *c,
                                  const char *allow_methods,
                                  const char *msg)
{
   ws_strbuf_t b;
   ws_strbuf_t r;
   if (wifi_debug_enabled())
      wifi_debug_printf("RESP 405 Method Not Allowed : %s\n", msg);
   sb_init(&b);
   page_open(&b, "Method Not Allowed");
   sb_puts(&b, "<h1>405 Method Not Allowed</h1>"
               "<div class=\"card\"><p class=\"err\">");
   sb_html(&b, msg);
   sb_puts(&b, "</p><p><a href=\"/\">Return home</a></p></div>");
   page_close(&b);
   if (b.failed) { sb_free(&b); return ws_oom(c); }
   sb_init(&r);
   sb_printf(&r,
             "HTTP/1.1 405 Method Not Allowed\r\n"
             "Allow: %s\r\n"
             "Content-Type: text/html; charset=utf-8\r\n"
             "Content-Length: %lu\r\n"
             "%s"
             "\r\n",
             allow_methods, (unsigned long)b.len,
             ws_connection_hdr(c));
   sb_write(&r, b.data, b.len);
   sb_free(&b);
   if (r.failed) { sb_free(&r); return ws_oom(c); }
   free(c->out); c->out = r.data; c->out_len = r.len;
   c->out_sent = 0u; c->bytes_queued = 0u; c->bytes_acked = 0u;
   c->state = CONN_SEND_MEM;
   conn_pump(c);
   return true;
}

/* ------------------------------------------------------------------ */
/* Page routes                                                         */
/* ------------------------------------------------------------------ */

static bool route_home(ws_conn_t *c)
{
   ws_strbuf_t b;
   sb_init(&b);
   page_open(&b, "Pi1MHz");
   sb_puts(&b,
      "<h1>Pi1MHz Web Interface</h1>"
      "<div class=\"card\">"
      "<p>Transfer files to and from the SD card, view the Pi VDU "
      "screen, or check the network status.</p>"
      "<p><a href=\"/files/\">Browse SD card files &rarr;</a></p>"
      "<p><a href=\"/framebuffer\">View the framebuffer &rarr;</a></p>"
      "<p><a href=\"/status\">Network status &rarr;</a></p>"
      "<p><a href=\"/aun\">AUN status &rarr;</a></p>"
      "<p><a href=\"/reboot\">Reboot the Pi &rarr;</a></p>"
      "</div>");
   page_close(&b);
   return ws_finish_html(c, 200, "OK", &b);
}

static bool route_aun(ws_conn_t *c)
{
   /* aun_status_text() formats the AUN engine state (station, map,
      queue depth, counters) as plain text; present it preformatted. */
   static char eco[1536];
   ws_strbuf_t b;

   aun_status_text(eco, sizeof eco);
   sb_init(&b);
   page_open(&b, "AUN");
   sb_puts(&b, "<h1>AUN</h1><div class=\"card\"><pre>");
   sb_html(&b, eco);
   sb_puts(&b, "</pre></div>");
   page_close(&b);
   return ws_finish_html(c, 200, "OK", &b);
}

static bool route_status(ws_conn_t *c)
{
   wifi_status_t                st  = wifi_get_status();
   const wifi_config_t         *cfg = wifi_get_config();
   const wifi_lwip_context_t   *lw  = wifi_lwip_get_context();
   sdio_runtime_status_t        rs  = sdio_runtime_get_status();
   ws_strbuf_t                  b;
   char                         tmp[64];
   uint8_t                      mac[6];

   sb_init(&b);
   page_open(&b, "Status");
   sb_puts(&b, "<h1>Status</h1><div class=\"card\"><table>");

   table_row(&b, "WiFi state", wifi_state_name(st.state));
   table_row(&b, "SSID",
             (cfg != NULL && cfg->ssid[0] != '\0') ? cfg->ssid : "(not set)");
   table_row(&b, "Hostname",
             (cfg != NULL && cfg->hostname[0] != '\0')
                ? cfg->hostname : "(default)");
   table_row(&b, "Link", sdio_runtime_link_is_up() ? "up" : "down");

   if (lw != NULL) {
      char ip[20];
      ws_ip_str(netif_ip4_addr(&lw->netif), ip, sizeof ip);
      table_row(&b, "IP address", ip);
      ws_ip_str(netif_ip4_netmask(&lw->netif), ip, sizeof ip);
      table_row(&b, "Netmask", ip);
      ws_ip_str(netif_ip4_gw(&lw->netif), ip, sizeof ip);
      table_row(&b, "Gateway", ip);
   }

   /* Show the chip's actual transmit MAC (captured at boot via the
      cur_etheraddr GET-VAR), not the VC4 mailbox value - they are
      DIFFERENT addresses when NVRAM patching is off.  The chip MAC
      is what the AP sees, and it is what lwIP advertises, so it is
      the only one worth showing on /status. */
   if (sdio_runtime_get_chip_mac(mac)) {
      snprintf(tmp, sizeof tmp, "%02X:%02X:%02X:%02X:%02X:%02X",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      table_row(&b, "MAC address", tmp);
   }

   snprintf(tmp, sizeof tmp, "%lu", (unsigned long)rs.tx_frames);
   table_row(&b, "Frames sent", tmp);
   snprintf(tmp, sizeof tmp, "%lu", (unsigned long)rs.rx_frames);
   table_row(&b, "Frames received", tmp);
   snprintf(tmp, sizeof tmp, "%u",
            (unsigned int)((cfg != NULL) ? cfg->http_port : 80u));
   table_row(&b, "HTTP port", tmp);

   if (g_ws_sd_free_valid) {
      snprintf(tmp, sizeof tmp, "%lu MB free",
               (unsigned long)g_ws_sd_free_mb);
      table_row(&b, "SD card", tmp);
   } else {
      /* webserver_poll refreshes the cache in the background; in the
         narrow window before the first refresh has run, just say so
         rather than stalling the TCP callback on f_getfree. */
      table_row(&b, "SD card", "(querying)");
   }

   sb_puts(&b, "</table></div>");
   sb_puts(&b, "<p><a href=\"/files/\">Browse files &rarr;</a> &middot; "
               "<a href=\"/reboot\">Reboot the Pi</a></p>");
   page_close(&b);
   return ws_finish_html(c, 200, "OK", &b);
}

/* ------------------------------------------------------------------ */
/* Framebuffer view                                                    */
/* ------------------------------------------------------------------ */

static bool route_framebuffer(ws_conn_t *c)
{
   framebuffer_export_info_t info;
   ws_strbuf_t               b;

   sb_init(&b);
   page_open(&b, "Framebuffer");
   sb_puts(&b, "<h1>Framebuffer</h1>");

   if (!framebuffer_export_get_info(&info)
       || info.width == 0u || info.height == 0u) {
      sb_puts(&b, "<div class=\"card\"><p class=\"err\">"
                  "The framebuffer is not available - the Pi VDU may be "
                  "inactive.</p></div>");
      page_close(&b);
      return ws_finish_html(c, 200, "OK", &b);
   }

   sb_puts(&b, "<div class=\"card\">");
   sb_printf(&b, "<p class=\"muted\">%lu &times; %lu pixels, %lu bpp</p>",
             (unsigned long)info.width,
             (unsigned long)info.height,
             (unsigned long)info.bits_per_pixel);
   sb_puts(&b,
      "<p><img src=\"/framebuffer.bmp\" alt=\"Pi VDU framebuffer\" "
      "style=\"max-width:100%;image-rendering:pixelated;"
      "border:1px solid #ccc;background:#000\"></p>"
      "<p><a href=\"/framebuffer\">Refresh</a></p></div>");
   page_close(&b);
   return ws_finish_html(c, 200, "OK", &b);
}

/* Serve the current framebuffer as a 24-bit BMP.  Only the HTTP header
   and the 54-byte BMP header are built up front; the pixel rows are
   generated on demand by conn_pump() (state CONN_SEND_FB), so the
   conversion is spread across ticks instead of run as one long burst. */
static bool route_framebuffer_bmp(ws_conn_t *c)
{
   framebuffer_export_info_t info;
   char     header[160];
   uint8_t *out;
   uint8_t *bmp;
   uint32_t row_padded;
   uint32_t pixel_bytes;
   uint32_t total;
   uint32_t bytes_per_pixel;
   size_t   hdr_len;
   int      hn;

   if (!framebuffer_export_get_info(&info)
       || info.width == 0u || info.height == 0u || info.address == 0u)
      return ws_error(c, 503, "Service Unavailable",
                      "The framebuffer is not available.");

   if (info.bits_per_pixel != 8u && info.bits_per_pixel != 16u
       && info.bits_per_pixel != 32u)
      return ws_error(c, 501, "Not Implemented",
                      "This framebuffer colour depth cannot be exported.");

   if (info.width > 8192u || info.height > 8192u)
      return ws_error(c, 503, "Service Unavailable",
                      "The framebuffer is too large to export.");

   /* Each source row must lie wholly within the framebuffer region. */
   bytes_per_pixel = info.bits_per_pixel / 8u;
   if (info.pitch < info.width * bytes_per_pixel
       || info.size < info.pitch * info.height)
      return ws_error(c, 503, "Service Unavailable",
                      "The framebuffer geometry is inconsistent.");

   row_padded = (info.width * 3u + 3u) & ~3u;
   if (row_padded > sizeof c->dl_buf)        /* one row per stream chunk */
      return ws_error(c, 503, "Service Unavailable",
                      "The framebuffer is too wide to export.");
   pixel_bytes = row_padded * info.height;
   total       = 54u + pixel_bytes;
   if (total > WS_FB_BMP_MAX)
      return ws_error(c, 503, "Service Unavailable",
                      "The framebuffer is too large to export.");

   hn = snprintf(header, sizeof header,
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: image/bmp\r\n"
                 "Content-Length: %lu\r\n"
                 "Cache-Control: no-store\r\n"
                 "%s"
                 "\r\n",
                 (unsigned long)total, ws_connection_hdr(c));
   if (hn <= 0 || (size_t)hn >= sizeof header)
      return ws_error(c, 500, "Internal Server Error",
                      "Could not build the image header.");
   hdr_len = (size_t)hn;

   /* out holds only the HTTP header + the 54-byte BMP header; the pixel
      rows are streamed afterwards by conn_pump(). */
   out = malloc(hdr_len + 54u);
   if (out == NULL)
      return ws_error(c, 500, "Internal Server Error",
                      "Out of memory rendering the framebuffer.");

   memcpy(out, header, hdr_len);
   bmp = out + hdr_len;
   memset(bmp, 0, 54u);

   /* BMP file header (14 bytes) + BITMAPINFOHEADER (40 bytes). */
   bmp[0] = 'B';
   bmp[1] = 'M';
   ws_store_u32(&bmp[2], total);
   ws_store_u32(&bmp[10], 54u);          /* offset to pixel data */
   ws_store_u32(&bmp[14], 40u);          /* DIB header size */
   ws_store_u32(&bmp[18], info.width);
   ws_store_u32(&bmp[22], info.height);  /* positive height = bottom-up */
   bmp[26] = 1;                          /* colour planes */
   bmp[28] = 24;                         /* bits per pixel */
   ws_store_u32(&bmp[34], pixel_bytes);
   ws_store_u32(&bmp[38], 2835u);        /* ~72 DPI (x) */
   ws_store_u32(&bmp[42], 2835u);        /* ~72 DPI (y) */

   free(c->out);
   c->out = (char *)out;
   c->out_len = hdr_len + 54u;
   c->out_sent = 0u;
   c->bytes_queued = 0u;
   c->bytes_acked = 0u;
   c->fb_info = info;
   c->fb_row = 0u;
   c->fb_stale = false;
   c->dl_buf_len = 0u;
   c->dl_buf_sent = 0u;
   c->state = CONN_SEND_FB;
   conn_pump(c);
   return true;
}

/* ------------------------------------------------------------------ */
/* Reboot                                                              */
/* ------------------------------------------------------------------ */

static bool route_reboot_confirm(ws_conn_t *c)
{
   ws_strbuf_t b;

   sb_init(&b);
   page_open(&b, "Reboot");
   sb_puts(&b,
      "<h1>Reboot the Pi1MHz</h1>"
      "<div class=\"card\">"
      "<p>This restarts the device.  The BBC will briefly lose its "
      "emulated peripherals, and this web session will drop.</p>"
      "<form method=\"post\" action=\"/reboot\">"
      "<p><input type=\"submit\" value=\"Reboot now\"></p>"
      "</form>"
      "<p><a href=\"/\">Cancel</a></p>"
      "</div>");
   page_close(&b);
   return ws_finish_html(c, 200, "OK", &b);
}

static bool route_reboot_do(ws_conn_t *c)
{
   ws_strbuf_t b;

   /* Defer the reboot: webserver_poll() carries it out once the delay
      has elapsed, by which time this response page has been sent. */
   g_ws_reboot_pending = true;
   g_ws_reboot_at = RPI_GetSystemTime();

   sb_init(&b);
   page_open(&b, "Rebooting");
   sb_puts(&b,
      "<h1>Rebooting</h1>"
      "<div class=\"card\">"
      "<p>The Pi1MHz is restarting now.  This page will stop "
      "responding - reconnect in a few seconds.</p>"
      "</div>");
   page_close(&b);
   return ws_finish_html(c, 200, "OK", &b);
}

/* ------------------------------------------------------------------ */
/* Directory listing                                                   */
/* ------------------------------------------------------------------ */

static int ws_entry_cmp(const void *pa, const void *pb)
{
   const ws_dir_entry_t *a = (const ws_dir_entry_t *)pa;
   const ws_dir_entry_t *b = (const ws_dir_entry_t *)pb;
   if (a->is_dir != b->is_dir)
      return a->is_dir ? -1 : 1;
   return ws_stricmp(a->name, b->name);
}

static bool render_listing(ws_conn_t *c, const char *sdpath)
{
   DIR             dir;
   FILINFO         fno;
   FRESULT         fr;
   ws_dir_entry_t *entries = NULL;
   size_t          count = 0u;
   size_t          cap = 0u;
   bool            truncated = false;
   bool            oom = false;
   size_t          i;
   ws_strbuf_t     b;

   fr = f_opendir(&dir, sdpath);
   if (fr != FR_OK) {
      if (fr == FR_NO_PATH || fr == FR_NO_FILE || fr == FR_INVALID_NAME)
         return ws_error(c, 404, "Not Found",
                         "That folder does not exist on the SD card.");
      return ws_error(c, 503, "Service Unavailable",
                      "The SD card could not be read.");
   }

   for (;;) {
      fr = f_readdir(&dir, &fno);
      if (fr != FR_OK || fno.fname[0] == '\0')
         break;
      if (count >= WS_LISTING_HARD_CAP) {
         truncated = true;
         break;
      }
      if (count >= cap) {
         size_t          ncap = (cap == 0u) ? 64u : cap * 2u;
         ws_dir_entry_t *ne = realloc(entries, ncap * sizeof *ne);
         if (ne == NULL) {
            oom = true;
            break;
         }
         entries = ne;
         cap = ncap;
      }
      strlcpy(entries[count].name, fno.fname, sizeof entries[count].name);
      entries[count].size = (uint32_t)fno.fsize;
      entries[count].is_dir = (fno.fattrib & AM_DIR) != 0u;
      ++count;
   }
   f_closedir(&dir);

   if (oom) {
      free(entries);
      return ws_error(c, 500, "Internal Server Error",
                      "Ran out of memory while listing the folder.");
   }

   if (count > 1u)
      qsort(entries, count, sizeof *entries, ws_entry_cmp);

   sb_init(&b);
   page_open(&b, "Files");
   sb_puts(&b, "<h1>");
   sb_html(&b, sdpath);
   sb_puts(&b, "</h1>");

   sb_puts(&b,
      "<table><tr><th>Name</th><th class=\"r\">Size</th></tr>");

   if (!ws_is_root(sdpath)) {
      char pp[WS_PATH_MAX];
      ws_parent_path(sdpath, pp, sizeof pp);
      sb_puts(&b, "<tr><td><a href=\"");
      append_files_url(&b, pp);
      sb_puts(&b, "\">../</a></td><td class=\"r\"></td></tr>");
   }

   for (i = 0u; i < count; ++i) {
      sb_puts(&b, "<tr><td><a href=\"/files");
      if (!ws_is_root(sdpath))
         sb_urlpath(&b, sdpath);
      sb_putc(&b, '/');
      sb_urlpath(&b, entries[i].name);
      if (entries[i].is_dir)
         sb_putc(&b, '/');
      sb_puts(&b, "\">");
      sb_html(&b, entries[i].name);
      if (entries[i].is_dir)
         sb_putc(&b, '/');
      sb_puts(&b, "</a></td><td class=\"r\">");
      if (entries[i].is_dir) {
         sb_puts(&b, "&lt;DIR&gt;");
      } else {
         char hs[32];
         ws_human_size(entries[i].size, hs, sizeof hs);
         sb_html(&b, hs);
      }
      sb_puts(&b, "</td></tr>");
   }
   sb_puts(&b, "</table>");
   if (truncated)
      sb_puts(&b,
         "<p class=\"muted\">Listing truncated &mdash; folder too large.</p>");

   sb_puts(&b,
      "<div class=\"card\" style=\"margin-top:16px\">"
      "<form method=\"post\" enctype=\"multipart/form-data\" action=\"");
   sb_puts(&b, "/files");
   if (!ws_is_root(sdpath))
      sb_urlpath(&b, sdpath);
   sb_putc(&b, '/');
   sb_puts(&b,
      "\"><p>Upload a file into this folder:</p>"
      "<input type=\"file\" name=\"file\" required> "
      "<input type=\"submit\" value=\"Upload\"></form></div>");

   free(entries);
   page_close(&b);
   return ws_finish_html(c, 200, "OK", &b);
}

/* ------------------------------------------------------------------ */
/* File download                                                       */
/* ------------------------------------------------------------------ */

static bool start_download(ws_conn_t *c, const char *sdpath)
{
   FRESULT     fr;
   uint32_t    size;
   char        dname[FF_LFN_BUF + 1];
   const char *src;
   size_t      o = 0u;
   ws_strbuf_t h;

   fr = f_open(&c->dl_file, sdpath, FA_READ);
   if (fr != FR_OK)
      return ws_error(c, 404, "Not Found",
                      "The file could not be opened.");
   c->dl_open = true;
   size = (uint32_t)f_size(&c->dl_file);
   c->dl_remaining = size;
   c->dl_eof = false;
   c->dl_buf_len = 0u;
   c->dl_buf_sent = 0u;

   /* sanitise the name for the Content-Disposition header */
   for (src = ws_basename(sdpath); *src != '\0' && o + 1u < sizeof dname;
        ++src) {
      char ch = *src;
      if (ch == '"' || ch == '\\' || (unsigned char)ch < 0x20u)
         ch = '_';
      dname[o++] = ch;
   }
   dname[o] = '\0';
   if (o == 0u)
      strlcpy(dname, "download", sizeof dname);

   sb_init(&h);
   {
      /* Pick a Content-Type from the extension so a browser PROPFIND-
         mounted view will preview text/images directly instead of
         offering them as a forced download.  Anything unmapped stays
         as application/octet-stream and gets the Content-Disposition:
         attachment hint.  The map is intentionally small; add more
         entries as they prove useful. */
      const char *ext = strrchr(ws_basename(sdpath), '.');
      const char *ctype = "application/octet-stream";
      const char *cdisp = "attachment; ";
      if (ext != NULL) {
         ++ext;   /* skip the '.' */
         if      (ws_prefix_ci_str(ext, "txt"))  { ctype = "text/plain; charset=utf-8";  cdisp = "inline; "; }
         else if (ws_prefix_ci_str(ext, "log"))  { ctype = "text/plain; charset=utf-8";  cdisp = "inline; "; }
         else if (ws_prefix_ci_str(ext, "md"))   { ctype = "text/plain; charset=utf-8";  cdisp = "inline; "; }
         else if (ws_prefix_ci_str(ext, "htm"))  { ctype = "text/html; charset=utf-8";   cdisp = "inline; "; }
         else if (ws_prefix_ci_str(ext, "html")) { ctype = "text/html; charset=utf-8";   cdisp = "inline; "; }
         else if (ws_prefix_ci_str(ext, "css"))  { ctype = "text/css; charset=utf-8";    cdisp = "inline; "; }
         else if (ws_prefix_ci_str(ext, "js"))   { ctype = "application/javascript";     cdisp = "inline; "; }
         else if (ws_prefix_ci_str(ext, "json")) { ctype = "application/json";           cdisp = "inline; "; }
         else if (ws_prefix_ci_str(ext, "xml"))  { ctype = "application/xml";            cdisp = "inline; "; }
         else if (ws_prefix_ci_str(ext, "png"))  { ctype = "image/png";                  cdisp = "inline; "; }
         else if (ws_prefix_ci_str(ext, "jpg"))  { ctype = "image/jpeg";                 cdisp = "inline; "; }
         else if (ws_prefix_ci_str(ext, "jpeg")) { ctype = "image/jpeg";                 cdisp = "inline; "; }
         else if (ws_prefix_ci_str(ext, "gif"))  { ctype = "image/gif";                  cdisp = "inline; "; }
         else if (ws_prefix_ci_str(ext, "svg"))  { ctype = "image/svg+xml";              cdisp = "inline; "; }
         else if (ws_prefix_ci_str(ext, "bmp"))  { ctype = "image/bmp";                  cdisp = "inline; "; }
         else if (ws_prefix_ci_str(ext, "pdf"))  { ctype = "application/pdf";            cdisp = "inline; "; }
         else if (ws_prefix_ci_str(ext, "wav"))  { ctype = "audio/wav";                  cdisp = "inline; "; }
         else if (ws_prefix_ci_str(ext, "mp3"))  { ctype = "audio/mpeg";                 cdisp = "inline; "; }
      }
      sb_printf(&h,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: %s\r\n"
                "Content-Length: %lu\r\n"
                "Content-Disposition: %sfilename=\"%s\"\r\n"
                "%s"
                "\r\n",
                ctype, (unsigned long)size, cdisp, dname,
                ws_connection_hdr(c));
   }
   if (h.failed) {
      sb_free(&h);
      f_close(&c->dl_file);
      c->dl_open = false;
      return ws_oom(c);
   }

   free(c->out);
   c->out = h.data;            /* ownership transferred */
   c->out_len = h.len;
   c->out_sent = 0u;
   c->bytes_queued = 0u;
   c->bytes_acked = 0u;
   c->state = CONN_SEND_FILE;
   conn_pump(c);
   return true;
}

static bool route_files_get(ws_conn_t *c, const char *rawpath)
{
   char    decoded[WS_PATH_MAX];
   char    sdpath[WS_PATH_MAX];
   FILINFO fno;

   if (!ws_url_decode(rawpath + 6, decoded, sizeof decoded))   /* skip "/files" */
      return ws_error(c, 400, "Bad Request",
                      "That path is too long.");
   ws_normalize_path(decoded, sdpath, sizeof sdpath);
   if (!ws_path_is_safe(sdpath))
      return ws_error(c, 400, "Bad Request", "That path is not allowed.");

   if (ws_is_root(sdpath))
      return render_listing(c, sdpath);

   if (f_stat(sdpath, &fno) != FR_OK)
      return ws_error(c, 404, "Not Found",
                      "That file or folder does not exist.");
   if ((fno.fattrib & AM_DIR) != 0u)
      return render_listing(c, sdpath);
   return start_download(c, sdpath);
}

/* ------------------------------------------------------------------ */
/* File upload (multipart/form-data)                                   */
/* ------------------------------------------------------------------ */

static bool upload_fail(ws_conn_t *c, const char *msg)
{
   ws_strbuf_t b;

   if (c->up_file_open) {
      f_close(&c->write_file.up);
      c->up_file_open = false;
   }
   c->up_state = UP_FAILED;

   sb_init(&b);
   page_open(&b, "Upload failed");
   sb_puts(&b, "<h1>Upload failed</h1><div class=\"card\">"
               "<p class=\"err\">");
   sb_html(&b, msg);
   sb_puts(&b, "</p><p><a href=\"");
   append_files_url(&b, c->up_dir);
   sb_puts(&b, "\">Back to folder</a></p></div>");
   page_close(&b);
   return ws_finish_html(c, 400, "Bad Request", &b);
}

static bool upload_write(ws_conn_t *c, const uint8_t *data, size_t len)
{
   UINT bw = 0u;

   if (len == 0u || !c->up_file_open)
      return true;
   if (f_write(&c->write_file.up, data, (UINT)len, &bw) != FR_OK || bw != len)
      return upload_fail(c, "Writing to the SD card failed "
                            "(the card may be full or write-protected).");
   c->up_bytes_written += (uint32_t)len;
   return true;
}

static bool upload_finish(ws_conn_t *c)
{
   ws_strbuf_t b;

   if (c->up_file_open) {
      FRESULT fr = f_close(&c->write_file.up);
      c->up_file_open = false;
      if (fr != FR_OK)
         return upload_fail(c, "The file could not be saved correctly.");
   }
   c->up_complete = true;
   c->up_state = UP_EPILOGUE;

   sb_init(&b);
   page_open(&b, "Upload complete");
   sb_puts(&b, "<h1>Upload complete</h1><div class=\"card\"><p><code>");
   sb_html(&b, c->up_name);
   sb_printf(&b, "</code> (%lu bytes) was saved.</p><p><a href=\"",
             (unsigned long)c->up_bytes_written);
   append_files_url(&b, c->up_dir);
   sb_puts(&b, "\">Back to folder</a></p></div>");
   page_close(&b);
   return ws_finish_html(c, 200, "OK", &b);
}

/* Parse the multipart part header in up_head[] and open the target file. */
static bool upload_begin_part(ws_conn_t *c)
{
   char        fname[FF_LFN_BUF + 1];
   const char *base;
   const char *s;
   /* Big enough for up_dir (up to WS_PATH_MAX-1) + '/' + base (up to
      FF_LFN_BUF) + NUL, so the snprintf below can never truncate. */
   char        full[WS_PATH_MAX + FF_LFN_BUF + 2u];

   if (!ws_extract_filename(c->up_head, fname, sizeof fname)
       || fname[0] == '\0')
      return upload_fail(c, "No file was selected for upload.");

   base = ws_basename(fname);
   if (base[0] == '\0'
       || strcmp(base, ".") == 0 || strcmp(base, "..") == 0)
      return upload_fail(c, "The uploaded file has an invalid name.");
   for (s = base; *s != '\0'; ++s) {
      if ((unsigned char)*s < 0x20u)
         return upload_fail(c, "The uploaded file has an invalid name.");
   }

   if (ws_is_root(c->up_dir))
      snprintf(full, sizeof full, "/%s", base);
   else
      snprintf(full, sizeof full, "%s/%s", c->up_dir, base);

   if (f_open(&c->write_file.up, full, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
      return upload_fail(c, "The file could not be created on the SD card.");

   c->up_file_open = true;
   c->up_bytes_written = 0u;
   strlcpy(c->up_name, base, sizeof c->up_name);
   return true;
}

/* Stream multipart body bytes to the SD card, holding back the bytes
   that could still turn out to be the closing boundary. */
static bool upload_feed_data(ws_conn_t *c, const uint8_t *data, size_t len)
{
   size_t delim = c->up_delim_len;
   size_t hold  = (delim > 0u) ? delim - 1u : 0u;

   while (len > 0u && c->up_state == UP_DATA) {
      uint8_t work[WS_UPLOAD_WORK];
      size_t  maxslice = WS_UPLOAD_WORK - c->up_tail_len;
      size_t  slice = (len < maxslice) ? len : maxslice;
      size_t  work_len;
      int     found;

      memcpy(work, c->up_tail, c->up_tail_len);
      memcpy(work + c->up_tail_len, data, slice);
      work_len = c->up_tail_len + slice;
      data += slice;
      len  -= slice;

      found = ws_memfind(work, work_len,
                         (const uint8_t *)c->up_delim, delim);
      if (found >= 0) {
         /* Distinguish the closing boundary ("\r\n--<boundary>--\r\n",
            ends the form) from an intermediate boundary
            ("\r\n--<boundary>\r\n", introduces another part).  The
            two bytes that follow the matched delimiter tell us which.
            We may have to look in both the working slice and the
            still-unread caller buffer; if neither has 2 bytes yet,
            stash the remainder back in up_tail and ask for more.
            Without this check, an intermediate boundary causes the
            first part to be saved and every subsequent part to be
            silently dropped on the floor - a tedious failure mode for
            anyone curl'ing multiple -F file uploads. */
         size_t after_off = (size_t)found + delim;
         uint8_t peek[2] = {0u, 0u};
         size_t  peek_have = 0u;
         if (after_off < work_len) {
            size_t take = work_len - after_off;
            if (take > 2u) take = 2u;
            memcpy(peek, work + after_off, take);
            peek_have += take;
         }
         if (peek_have < 2u && len > 0u) {
            size_t take = 2u - peek_have;
            if (take > len) take = len;
            memcpy(peek + peek_have, data, take);
            peek_have += take;
         }
         if (peek_have < 2u) {
            /* Not enough bytes to decide yet - keep the matched
               delimiter in up_tail and wait for the next callback. */
            if (!upload_write(c, work, (size_t)found))
               return false;
            if (c->up_state != UP_DATA)
               return true;
            /* peek[] may include bytes taken from the caller buffer that
               were never copied into work[], so stash it explicitly. */
            c->up_tail_len = delim + peek_have;
            memmove(c->up_tail, work + found, delim);
            memcpy(c->up_tail + delim, peek, peek_have);
            return true;
         }
         if (peek[0] != '-' || peek[1] != '-') {
            /* Intermediate boundary: another part follows.  We only
               support single-file uploads from the built-in form, so
               reject explicitly rather than silently dropping the
               rest.  The first part has already been written - close
               it cleanly before failing so the saved file is intact. */
            if (c->up_file_open) {
               (void)f_close(&c->write_file.up);
               c->up_file_open = false;
            }
            return upload_fail(c,
                "Multi-file uploads are not supported; please submit "
                "one file per upload.");
         }
         /* Closing boundary - finalise the first (and only) part. */
         if (!upload_write(c, work, (size_t)found))
            return false;
         if (c->up_state != UP_DATA)
            return true;          /* a write error was reported */
         c->up_tail_len = 0u;
         return upload_finish(c);
      } else {
         size_t writeable = (work_len > hold) ? (work_len - hold) : 0u;
         if (writeable > 0u) {
            if (!upload_write(c, work, writeable))
               return false;
            if (c->up_state != UP_DATA)
               return true;
         }
         c->up_tail_len = work_len - writeable;
         memmove(c->up_tail, work + writeable, c->up_tail_len);
      }
   }
   return true;
}

static bool upload_consume(ws_conn_t *c, const uint8_t *data, size_t len)
{
   size_t pos = 0u;

   while (pos < len) {
      if (c->up_state == UP_PART_HEADER) {
         size_t space = WS_UPLOAD_HEAD_MAX - c->up_head_len;
         size_t take  = len - pos;
         int    body_at;

         if (take > space)
            take = space;
         memcpy(c->up_head + c->up_head_len, data + pos, take);
         c->up_head_len += take;
         pos += take;
         c->up_head[c->up_head_len] = '\0';

         body_at = ws_find_header_end(c->up_head, c->up_head_len);
         if (body_at >= 0) {
            if (!upload_begin_part(c))
               return false;
            if (c->up_state == UP_FAILED)
               return true;       /* failure page already queued */
            c->up_state = UP_DATA;
            c->up_tail_len = 0u;
            if (!upload_feed_data(c,
                                  (const uint8_t *)c->up_head + body_at,
                                  c->up_head_len - (size_t)body_at))
               return false;
         } else if (c->up_head_len >= WS_UPLOAD_HEAD_MAX) {
            return upload_fail(c, "The upload form data is malformed.");
         } else {
            return true;          /* need more data */
         }
      } else if (c->up_state == UP_DATA) {
         if (!upload_feed_data(c, data + pos, len - pos))
            return false;
         pos = len;
      } else {
         /* UP_EPILOGUE / UP_FAILED: discard the rest */
         pos = len;
      }
   }
   return true;
}

static bool route_upload(ws_conn_t *c, const char *rawpath, int body_at)
{
   char    decoded[WS_PATH_MAX];
   char    dir[WS_PATH_MAX];
   char    ctype[256];
   char    boundary[WS_BOUNDARY_MAX];
   FILINFO fno;

   if (!ws_url_decode(rawpath + 6, decoded, sizeof decoded))   /* skip "/files" */
      return ws_error(c, 400, "Bad Request",
                      "That path is too long.");
   ws_normalize_path(decoded, dir, sizeof dir);
   if (!ws_path_is_safe(dir))
      return ws_error(c, 400, "Bad Request", "That path is not allowed.");

   if (!ws_is_root(dir)) {
      if (f_stat(dir, &fno) != FR_OK || (fno.fattrib & AM_DIR) == 0u)
         return ws_error(c, 404, "Not Found",
                         "The upload folder does not exist.");
   }

   if (!ws_find_header(c->reqhdr, (size_t)body_at, "Content-Type",
                       ctype, sizeof ctype)
       || !ws_prefix_ci_str(ctype, "multipart/form-data"))
      return ws_error(c, 400, "Bad Request",
                      "Uploads must use multipart/form-data.");
   if (!ws_extract_boundary(ctype, boundary, sizeof boundary))
      return ws_error(c, 400, "Bad Request",
                      "The upload is missing its multipart boundary.");

   strlcpy(c->up_dir, dir, sizeof c->up_dir);
   c->up_delim[0] = '\r';
   c->up_delim[1] = '\n';
   c->up_delim[2] = '-';
   c->up_delim[3] = '-';
   strlcpy(c->up_delim + 4, boundary, sizeof c->up_delim - 4u);
   c->up_delim_len = 4u + strlen(boundary);

   c->state = CONN_RECV_UPLOAD;
   c->up_state = UP_PART_HEADER;
   c->up_head_len = 0u;
   c->up_tail_len = 0u;
   c->up_complete = false;
   c->up_bytes_written = 0u;

   return upload_consume(c, (const uint8_t *)c->reqhdr + body_at,
                         c->reqhdr_len - (size_t)body_at);
}

/* ------------------------------------------------------------------ */
/* WebDAV (RFC 4918, class-1 + LOCK stubs)                             */
/* ------------------------------------------------------------------ */

/* Convert the URL path coming in on a WebDAV request to an absolute
   SD-card path.  Empty / "/" / unspecified becomes "/".  Returns
   false if the URL didn't fit in the decode buffer (caller should
   surface a 400 - the path is too long to handle, NOT a missing
   resource), or if the path-safety check rejects it (control chars,
   ".."). */
static bool dav_url_to_sdpath(const char *rawpath, char *sdpath,
                              size_t sdpath_sz)
{
   char decoded[WS_PATH_MAX];

   if (!ws_url_decode(rawpath, decoded, sizeof decoded))
      return false;
   ws_normalize_path(decoded, sdpath, sdpath_sz);
   return ws_path_is_safe(sdpath);
}

/* Day-of-week from a Gregorian Y/M/D via Zeller's congruence.  Used
   to fill in the day-name field of the IMF-fixdate header below; some
   strict client toolchains (RFC 7231 parsers in Java HttpClient and a
   few macOS libraries) reject the header if the weekday does not
   match the date.  Returns 0..6 = Sun..Sat. */
static unsigned int ws_day_of_week(unsigned int year, unsigned int month,
                                   unsigned int day)
{
   unsigned int y = year;
   unsigned int m = month;
   unsigned int K, J;
   /* Zeller treats Jan/Feb as months 13/14 of the previous year. */
   if (m < 3u) { m += 12u; y -= 1u; }
   K = y % 100u;
   J = y / 100u;
   /* h = 0 Saturday, 1 Sunday, ..., 6 Friday */
   {
      unsigned int h = (day + (13u * (m + 1u)) / 5u
                        + K + K/4u + J/4u + 5u*J) % 7u;
      /* Re-map to 0..6 = Sun..Sat (what our days[] table expects). */
      return (h + 6u) % 7u;
   }
}

/* HTTP-date (RFC 7231 IMF-fixdate) for a FatFs FILINFO timestamp.
   FatFs stores date/time in DOS format:
     fdate: bits 15..9 = year - 1980, bits 8..5 = month (1-12),
            bits 4..0  = day (1-31)
     ftime: bits 15..11 = hour, bits 10..5 = minute,
            bits 4..0  = sec/2
   Decode that into "DDD, DD Mon YYYY HH:MM:SS GMT".  The weekday is
   computed via Zeller's congruence so strict clients that re-parse
   the header (RFC 7231 forbids mismatching weekday/date) accept it.

   If fdate is 0 (file created by code that didn't set a timestamp -
   FatFs returns this when there is no RTC and no explicit time set
   via f_utime / FATFS_TIMER hook), fall back to the firmware build
   date so the entry shows SOMETHING reasonable rather than the DOS
   epoch (Jan 1 1980). */
/* Decode the compiler's build date from __DATE__ ("Mmm DD YYYY", day
   space-padded for 1-9) at compile time, so the build-date fallbacks below
   do not parse a string constant at runtime.  Day and year are branchless
   arithmetic; month maps the 3-letter prefix (3rd letter is unique except
   'n' = Jan/Jun and 'r' = Mar/Apr, split by the 2nd / 1st letter). */
#define WS_BUILD_DAY   ((unsigned)((((__DATE__[4] >> 4) & 1) * (__DATE__[4] & 0x0F)) * 10 \
                                   + (__DATE__[5] - '0')))
#define WS_BUILD_YEAR  ((unsigned)((__DATE__[7]-'0')*1000 + (__DATE__[8]-'0')*100 \
                                   + (__DATE__[9]-'0')*10 + (__DATE__[10]-'0')))
#define WS_BUILD_MONTH ((unsigned)( \
     __DATE__[2]=='n' ? (__DATE__[1]=='a' ? 1 : 6)  \
   : __DATE__[2]=='b' ? 2                            \
   : __DATE__[2]=='r' ? (__DATE__[0]=='M' ? 3 : 4)  \
   : __DATE__[2]=='y' ? 5  : __DATE__[2]=='l' ? 7   \
   : __DATE__[2]=='g' ? 8  : __DATE__[2]=='p' ? 9   \
   : __DATE__[2]=='t' ? 10 : __DATE__[2]=='v' ? 11 : 12))

/* Shift a FAT-packed (fdate,ftime) pair by `minutes` (may be negative),
   rolling across day/month/year boundaries, and clamp into FAT's
   representable range (1980..2107).  Used to translate between the
   timezone-naive local time held in the SD directory entry and the GMT
   that Windows' WebDAV client sends/expects (the device has no RTC or
   timezone of its own - see webdav_utc_offset_minutes).  A zero offset,
   or fdate == 0 (no timestamp / build-date fallback), is left untouched.
   Uses Howard Hinnant's days<->civil algorithms for the date math. */
static void fat_shift_minutes(uint16_t *fdate, uint16_t *ftime, int minutes)
{
   int  year, month, day, hh, mm, ss2;
   long y, era, yoe, doy, doe, days, total, ndays, minofday;
   long z, era2, doe2, yoe2, y2, doy2, mp;

   if (minutes == 0 || *fdate == 0u)
      return;

   year  = 1980 + (int)(((unsigned)*fdate >> 9) & 0x7Fu);
   month = (int)(((unsigned)*fdate >> 5) & 0x0Fu);
   day   = (int)((unsigned)*fdate        & 0x1Fu);
   hh    = (int)(((unsigned)*ftime >> 11) & 0x1Fu);
   mm    = (int)(((unsigned)*ftime >> 5)  & 0x3Fu);
   ss2   = (int)((unsigned)*ftime        & 0x1Fu);   /* seconds / 2 */

   if (month < 1) month = 1;
   if (day   < 1) day   = 1;

   /* days since 1970-01-01 */
   y   = year - (month <= 2 ? 1 : 0);
   era = (y >= 0 ? y : y - 399) / 400;
   yoe = y - era * 400;
   doy = (153L * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
   doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
   days = era * 146097L + doe - 719468L;

   total    = days * 1440L + (long)hh * 60L + (long)mm + (long)minutes;
   ndays    = total / 1440L;
   minofday = total - ndays * 1440L;
   if (minofday < 0) { minofday += 1440L; ndays -= 1; }

   hh = (int)(minofday / 60L);
   mm = (int)(minofday % 60L);

   /* civil date from day count */
   z    = ndays + 719468L;
   era2 = (z >= 0 ? z : z - 146096) / 146097;
   doe2 = z - era2 * 146097L;
   yoe2 = (doe2 - doe2 / 1460L + doe2 / 36524L - doe2 / 146096L) / 365L;
   y2   = yoe2 + era2 * 400L;
   doy2 = doe2 - (365L * yoe2 + yoe2 / 4L - yoe2 / 100L);
   mp   = (5L * doy2 + 2L) / 153L;
   day   = (int)(doy2 - (153L * mp + 2L) / 5L + 1L);
   month = (int)(mp + (mp < 10 ? 3 : -9));
   year  = (int)(y2 + (month <= 2 ? 1 : 0));

   if (year < 1980) {
      year = 1980; month = 1; day = 1; hh = 0; mm = 0; ss2 = 0;
   } else if (year > 2107) {
      year = 2107; month = 12; day = 31; hh = 23; mm = 59; ss2 = 29;
   }

   *fdate = (uint16_t)(((unsigned)(year - 1980) << 9)
                       | ((unsigned)month << 5) | (unsigned)day);
   *ftime = (uint16_t)(((unsigned)hh << 11)
                       | ((unsigned)mm << 5) | (unsigned)ss2);
}

/* Minutes east of UTC configured for this device (0 when unset). */
static int ws_utc_offset_minutes(void)
{
   const wifi_config_t *cfg = wifi_get_config();
   return (cfg != NULL) ? (int)cfg->webdav_utc_offset_minutes : 0;
}

static void dav_format_date(char *out, size_t out_sz,
                            uint16_t fdate, uint16_t ftime)
{
   static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
   static const char *days   = "SunMonTueWedThuFriSat";

   if (fdate != 0u) {
      unsigned int year   = ((unsigned int)fdate >> 9)  + 1980u;
      unsigned int month  = ((unsigned int)fdate >> 5)  & 0x0Fu;
      unsigned int day    = (unsigned int)fdate         & 0x1Fu;
      unsigned int hour   = ((unsigned int)ftime >> 11) & 0x1Fu;
      unsigned int minute = ((unsigned int)ftime >> 5)  & 0x3Fu;
      unsigned int second = ((unsigned int)ftime & 0x1Fu) * 2u;

      if (month >= 1u && month <= 12u) {
         unsigned int dow = ws_day_of_week(year, month, day);
         snprintf(out, out_sz,
                  "%.3s, %02u %.3s %04u %02u:%02u:%02u GMT",
                  &days[dow * 3u], day,
                  &months[(month - 1u) * 3u], year,
                  hour, minute, second);
         return;
      }
      /* Malformed fdate - fall through to the build-date fallback. */
   }

   {
      /* Build-date fallback: the compiler's __DATE__ is decoded at compile
         time via the WS_BUILD_* macros above; __TIME__ is "HH:MM:SS". */
      const char  *bt = __TIME__;
      unsigned int day  = WS_BUILD_DAY;
      unsigned int mi   = WS_BUILD_MONTH - 1u;   /* 0-based index into months[] */
      unsigned int year = WS_BUILD_YEAR;
      unsigned int dow  = ws_day_of_week(year, mi + 1u, day);

      snprintf(out, out_sz,
               "%.3s, %02u %.3s %04u %c%c:%c%c:%c%c GMT",
               &days[dow * 3u], day, &months[mi * 3u], year,
               bt[0], bt[1], bt[3], bt[4], bt[6], bt[7]);
   }
}

/* ISO-8601 (RFC 3339) creation-date for the WebDAV <D:creationdate>
   element.  Same DOS-fields decode as dav_format_date, just emitted
   in YYYY-MM-DDTHH:MM:SSZ format.  Build-date fallback when fdate
   is 0. */
static void dav_format_creationdate(char *out, size_t out_sz,
                                    uint16_t fdate, uint16_t ftime)
{
   if (fdate != 0u) {
      unsigned int year   = ((unsigned int)fdate >> 9)  + 1980u;
      unsigned int month  = ((unsigned int)fdate >> 5)  & 0x0Fu;
      unsigned int day    = (unsigned int)fdate         & 0x1Fu;
      unsigned int hour   = ((unsigned int)ftime >> 11) & 0x1Fu;
      unsigned int minute = ((unsigned int)ftime >> 5)  & 0x3Fu;
      unsigned int second = ((unsigned int)ftime & 0x1Fu) * 2u;

      if (month >= 1u && month <= 12u) {
         snprintf(out, out_sz, "%04u-%02u-%02uT%02u:%02u:%02uZ",
                  year, month, day, hour, minute, second);
         return;
      }
   }
   /* Build-date fallback - emit __DATE__/__TIME__ in ISO format, decoded
      at compile time via the shared WS_BUILD_* macros (see above). */
   {
      /* Build-date fallback: __DATE__/__TIME__ decoded at compile time via
         the shared WS_BUILD_* macros above. */
      const char  *bt = __TIME__;
      unsigned int day   = WS_BUILD_DAY;
      unsigned int month = WS_BUILD_MONTH;
      unsigned int year  = WS_BUILD_YEAR;

      snprintf(out, out_sz, "%04u-%02u-%02uT%c%c:%c%c:%c%cZ",
               year, month, day,
               bt[0], bt[1], bt[3], bt[4], bt[6], bt[7]);
   }
}

/* Emit one PROPFIND <D:response> element for an SD entry.  url_path
   must already be URL-encoded; is_dir controls the resourcetype.
   size is ignored for directories.  fdate/ftime are the DOS-format
   timestamp fields from the FatFs FILINFO; zero values fall back to
   the firmware build date in the formatting helpers. */
static void dav_emit_response(ws_strbuf_t *b, const char *url_path,
                              bool is_dir, uint32_t size,
                              const char *display_name,
                              uint16_t fdate, uint16_t ftime)
{
   char modified[40];
   char created[32];

   /* The FAT entry holds local time; getlastmodified/creationdate are GMT.
      Convert local -> GMT by shifting back by the configured offset. */
   fat_shift_minutes(&fdate, &ftime, -ws_utc_offset_minutes());

   dav_format_date(modified, sizeof modified, fdate, ftime);
   dav_format_creationdate(created, sizeof created, fdate, ftime);

   sb_puts(b, "<D:response><D:href>");
   sb_html(b, url_path);    /* url_path is already %-encoded; sb_html escapes XML */
   sb_puts(b, "</D:href><D:propstat><D:prop>"
              "<D:displayname>");
   sb_html(b, display_name);
   sb_puts(b, "</D:displayname>"
              "<D:getlastmodified>");
   sb_puts(b, modified);
   sb_puts(b, "</D:getlastmodified>"
              "<D:creationdate>");
   sb_puts(b, created);
   sb_puts(b, "</D:creationdate>"
              "<D:resourcetype>");
   if (is_dir)
      sb_puts(b, "<D:collection/>");
   sb_puts(b, "</D:resourcetype>");
   if (!is_dir) {
      sb_printf(b, "<D:getcontentlength>%lu</D:getcontentlength>"
                   "<D:getcontenttype>application/octet-stream</D:getcontenttype>",
                (unsigned long)size);
   } else if (g_ws_sd_free_valid) {
      /* RFC 4331: quota-available-bytes is free space, quota-used-bytes
         is currently consumed.  Windows Explorer reads these from the
         response for the share's root (and sometimes any directory) to
         show "X free of Y" in the address bar - without them it shows
         a fabricated default (the user's 86.8 GB / 929 GB report).
         Emit them only when the cached numbers are valid; with no
         cache yet, omitting the properties returns the same client
         behaviour as before.

         The values are uint64_t (a 32 GB card needs > 32 bits) but the
         bare-metal printf has no %llu support.  Format the digits by
         hand into a 24-byte buffer and emit via %s. */
      char     dec_free[24];
      char     dec_used[24];
      uint64_t used = (g_ws_sd_total_bytes > g_ws_sd_free_bytes)
                    ? (g_ws_sd_total_bytes - g_ws_sd_free_bytes) : 0u;
      ws_format_u64(dec_free, sizeof dec_free, g_ws_sd_free_bytes);
      ws_format_u64(dec_used, sizeof dec_used, used);
      sb_printf(b,
                "<D:quota-available-bytes>%s</D:quota-available-bytes>"
                "<D:quota-used-bytes>%s</D:quota-used-bytes>",
                dec_free, dec_used);
   }
   sb_puts(b, "<D:supportedlock>"
              "<D:lockentry><D:lockscope><D:exclusive/></D:lockscope>"
              "<D:locktype><D:write/></D:locktype></D:lockentry>"
              "</D:supportedlock>"
              "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>"
              "</D:response>");
}

/* Build the "/dav-encoded" form of an SD path: every segment %-encoded
   except '/' itself.  Used for the <D:href> field.  trailing_slash
   appends '/' for non-root paths; the root path is already "/" and
   needs no further suffix. */
static void dav_append_url_path(ws_strbuf_t *b, const char *sdpath,
                                bool trailing_slash)
{
   sb_urlpath(b, sdpath);
   if (trailing_slash && (sdpath[0] != '/' || sdpath[1] != '\0'))
      sb_putc(b, '/');
}

static bool route_dav_options(ws_conn_t *c)
{
   ws_strbuf_t r;

   sb_init(&r);
   sb_printf(&r,
      "HTTP/1.1 200 OK\r\n"
      "DAV: 1, 2\r\n"
      "MS-Author-Via: DAV\r\n"
      "Allow: OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, COPY, MOVE, "
      "PROPFIND, PROPPATCH, LOCK, UNLOCK, POST\r\n"
      "Content-Length: 0\r\n"
      "%s"
      "\r\n", ws_connection_hdr(c));

   if (r.failed) {
      sb_free(&r);
      return ws_oom(c);
   }
   free(c->out);
   c->out = r.data;
   c->out_len = r.len;
   c->out_sent = 0u;
   c->bytes_queued = 0u;
   c->bytes_acked = 0u;
   c->state = CONN_SEND_MEM;
   conn_pump(c);
   return true;
}

static bool route_dav_propfind(ws_conn_t *c, const char *rawpath, int body_at)
{
   char        sdpath[WS_PATH_MAX];
   char        depth_hdr[16];
   FILINFO     fno;
   ws_strbuf_t b;
   int         depth = 1;
   (void)body_at;   /* PROPFIND body lists requested props; we return them all */

   if (!dav_url_to_sdpath(rawpath, sdpath, sizeof sdpath))
      return ws_error(c, 400, "Bad Request", "That path is not allowed.");

   if (ws_find_header(c->reqhdr, c->reqhdr_len, "Depth",
                      depth_hdr, sizeof depth_hdr)) {
      /* Depth: 0 -> self only; 1 -> self + immediate children;
         "infinity" -> reject explicitly per RFC 4918 §9.1 so the
         client knows the listing it gets back is bounded and is not
         a complete tree.  Silently capping at 1 (the original
         behaviour) made deep clients believe the truncated listing
         was the whole story.  Anything else -> treat as 1. */
      if (ws_prefix_ci_str(depth_hdr, "infinity"))
         return ws_error(c, 403, "Forbidden",
                         "PROPFIND with Depth: infinity is not "
                         "supported; use Depth: 0 or 1.");
      depth = (strcmp(depth_hdr, "0") == 0) ? 0 : 1;
   }

   bool is_root = ws_is_root(sdpath);
   bool is_dir;
   if (is_root) {
      is_dir = true;
   } else {
      /* Cache hit avoids f_stat against the cold FATFS window.  For
         Windows Explorer's per-child PROPFIND fanout (one PROPFIND
         per directory entry after the parent walk) this is the hot
         path - the parent walk populated the cache, every child
         lookup is then memory-only. */
      if (!ws_propfind_cache_lookup(sdpath, &fno)) {
         if (f_stat(sdpath, &fno) != FR_OK)
            return ws_error(c, 404, "Not Found", "No such resource.");
      }
      is_dir = (fno.fattrib & AM_DIR) != 0u;
   }

   sb_init(&b);
   sb_puts(&b,
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<D:multistatus xmlns:D=\"DAV:\">");

   /* Self */
   {
      ws_strbuf_t url;
      sb_init(&url);
      dav_append_url_path(&url, sdpath, is_dir);
      if (url.data == NULL) {
         sb_free(&b);
         return ws_oom(c);
      }
      dav_emit_response(&b, url.data, is_dir,
                        is_root ? 0u : (uint32_t)fno.fsize,
                        is_root ? "/" : ws_basename(sdpath),
                        /* root has no stat info - pass 0 fdate so
                           dav_format_date falls back to build date */
                        is_root ? 0u : fno.fdate,
                        is_root ? 0u : fno.ftime);
      sb_free(&url);
   }

   /* Children at depth 1 */
   if (depth == 1 && is_dir) {
      DIR     dir;
      FILINFO chld;
      /* Start a fresh PROPFIND cache generation.  We populate it as
         we walk the children below; the next per-child Depth:0
         PROPFIND will hit this cache and skip its own f_stat. */
      ws_propfind_cache_begin();
      if (f_opendir(&dir, sdpath) == FR_OK) {
         while (f_readdir(&dir, &chld) == FR_OK && chld.fname[0] != '\0') {
            ws_strbuf_t url;
            bool        child_is_dir = (chld.fattrib & AM_DIR) != 0u;

            sb_init(&url);
            if (!is_root) sb_urlpath(&url, sdpath);
            sb_putc(&url, '/');
            sb_urlpath(&url, chld.fname);
            if (child_is_dir) sb_putc(&url, '/');
            /* Propagate OOM: a partial multistatus would be reported
               to the client as a complete listing, and files would
               silently disappear from the WebDAV view.  Bail with
               503 so the client retries. */
            if (url.failed || url.data == NULL) {
               sb_free(&url);
               f_closedir(&dir);
               sb_free(&b);
               return ws_oom(c);
            }
            dav_emit_response(&b, url.data, child_is_dir,
                              (uint32_t)chld.fsize, chld.fname,
                              chld.fdate, chld.ftime);
            sb_free(&url);
            if (b.failed) {
               f_closedir(&dir);
               sb_free(&b);
               return ws_oom(c);
            }
            /* Record the child's FILINFO for the cache.  Build the
               decoded SD path the way ws_propfind_cache_lookup expects
               to see from a future PROPFIND request (the same shape
               that dav_url_to_sdpath produces). */
            {
               char child_sdpath[WS_PATH_MAX];
               int n = is_root
                     ? snprintf(child_sdpath, sizeof child_sdpath, "/%s", chld.fname)
                     : snprintf(child_sdpath, sizeof child_sdpath, "%s/%s", sdpath, chld.fname);
               if (n > 0 && (size_t)n < sizeof child_sdpath) {
                  ws_propfind_cache_add(child_sdpath, &chld);
               }
            }
         }
         f_closedir(&dir);
      }
      ws_propfind_cache_commit();
   }

   sb_puts(&b, "</D:multistatus>");

   if (b.failed) {
      sb_free(&b);
      return ws_oom(c);
   }

   {
      ws_strbuf_t r;
      sb_init(&r);
      sb_printf(&r,
                "HTTP/1.1 207 Multi-Status\r\n"
                "Content-Type: application/xml; charset=\"utf-8\"\r\n"
                "Content-Length: %lu\r\n"
                "DAV: 1, 2\r\n"
                "MS-Author-Via: DAV\r\n"
                "%s"
                "\r\n",
                (unsigned long)b.len, ws_connection_hdr(c));
      sb_write(&r, b.data, b.len);
      sb_free(&b);

      if (r.failed) {
         sb_free(&r);
         return ws_oom(c);
      }
      if (wifi_debug_enabled())
         wifi_debug_printf("RESP 207 Multi-Status : PROPFIND '%s' depth=%d\n",
                           sdpath, depth);
      free(c->out);
      c->out = r.data;
      c->out_len = r.len;
      c->out_sent = 0u;
      c->bytes_queued = 0u;
      c->bytes_acked = 0u;
      c->state = CONN_SEND_MEM;
      conn_pump(c);
   }
   return true;
}

/* Build and queue the response for the PUT that has just received its
   last body byte.  201 Created for a new file, 204 No Content if an
   existing file was overwritten. */
static bool dav_put_send_response(ws_conn_t *c)
{
   ws_strbuf_t r;

   sb_init(&r);
   sb_printf(&r,
             "HTTP/1.1 %d %s\r\n"
             "Content-Length: 0\r\n"
             "%s"
             "\r\n",
             c->dav_put_status, c->dav_put_status_text,
             ws_connection_hdr(c));
   if (r.failed) {
      sb_free(&r);
      return ws_oom(c);
   }
   free(c->out);
   c->out = r.data;
   c->out_len = r.len;
   c->out_sent = 0u;
   c->bytes_queued = 0u;
   c->bytes_acked = 0u;
   c->state = CONN_SEND_MEM;
   conn_pump(c);
   return true;
}

/* Drain n bytes from `data` into the open PUT temp file.  When the byte
   counter hits zero, close the temp, atomically rename it over the
   target, and queue the response.  Any failure path unlinks the temp
   so a partial upload never lingers on the SD card.

   The same routine also drives the "unauthenticated drain" used by the
   401-after-Expect: 100-continue workaround for Windows Explorer's
   MiniRedirector (see route_dav_put / process_request).  In that mode
   dav_put_open is false, the bytes get silently discarded, and once
   dav_remaining hits zero we send the digest challenge instead of
   201/204. */
/* Write `len` body bytes to the open PUT temp file, or discard them if
   dav_put_open is false (the unauthenticated-drain mode).  Shared by
   the Content-Length sink (dav_put_consume) and the chunked-body sink
   (dav_put_consume_chunked) below - neither decides on its own when
   the body ends, so the "close + rename + respond" tail lives
   separately in dav_put_finish(). */
/* Flush whatever has accumulated in the dl_buf PUT staging buffer to the
   temp file in a single f_write.  Called both when the buffer fills to a
   whole WS_FILE_CHUNK (sector-aligned -> FatFs multiblock CMD25) and, for
   the trailing partial buffer, from dav_put_finish before f_close.

   Returns true on success.  On a write error it tears the temp file down
   (closing it and unlinking the ".part" file, so the pre-existing target is
   left untouched), queues a 507 response, and returns **false** so the
   caller stops feeding the body and does NOT proceed to the target
   unlink/rename.  Note ws_error itself returns true (response queued, keep
   the connection to send it), so the 507 is queued and false is returned
   explicitly - the caller must keep the connection alive, not abort it. */
static bool dav_put_flush(ws_conn_t *c)
{
   UINT bw = 0u;

   if (c->dav_put_buf_len == 0u)
      return true;
   if (f_write(&c->write_file.dav, c->dl_buf, (UINT)c->dav_put_buf_len, &bw)
          != FR_OK || bw != c->dav_put_buf_len) {
      f_close(&c->write_file.dav);
      c->dav_put_open = false;
      c->dav_put_buf_len = 0u;
      (void)f_unlink(c->dav_put_tmppath);
      (void)ws_error(c, 507, "Insufficient Storage",
                     "Writing to the SD card failed.");
      return false;
   }
   c->dav_put_buf_len = 0u;
   return true;
}

static bool dav_put_write_bytes(ws_conn_t *c, const uint8_t *data, size_t len)
{
   if (!c->dav_put_open || len == 0u)
      return true;

   /* Copy into dl_buf, flushing each time it fills to a full chunk.  A
      single TCP segment (~1460 B) never spans more than one flush, but the
      loop handles arbitrary len defensively. */
   while (len > 0u) {
      size_t space = WS_FILE_CHUNK - c->dav_put_buf_len;
      size_t n     = (len < space) ? len : space;

      memcpy(c->dl_buf + c->dav_put_buf_len, data, n);
      c->dav_put_buf_len += n;
      data += n;
      len  -= n;
      if (c->dav_put_buf_len == WS_FILE_CHUNK && !dav_put_flush(c))
         return false;
   }
   return true;
}

/* The PUT body has been fully received (Content-Length hit zero, or
   the chunked terminator was seen): close the temp file, replace the
   target, and queue the response - or, in unauthenticated-drain mode,
   hand off to the digest challenge. */
static bool dav_put_finish(ws_conn_t *c)
{
   if (c->dav_put_open) {
      FRESULT fr;
      if (!dav_put_flush(c))
         return true;   /* trailing flush failed: 507 queued, temp gone,
                           target intact.  Keep the connection to send the
                           507; do NOT fall through to the unlink/rename. */
      fr = f_close(&c->write_file.dav);
      c->dav_put_open = false;
      if (fr != FR_OK) {
         (void)f_unlink(c->dav_put_tmppath);
         return ws_error(c, 500, "Internal Server Error",
                         "Could not close the uploaded file.");
      }
   }
   /* Atomic-ish replace: drop the existing target if any (f_rename
      on FatFs fails if the destination exists), then rename.  If
      the unlink succeeds but the rename then fails we've destroyed
      the original; mitigate by renaming the temp back to a
      recoverable name first.  In practice f_rename within the same
      FAT directory does not allocate, so the failure window is
      extremely small. */
   /* Unauthenticated drain finished: hand control to the digest
      challenge.  No file work was performed; nothing on the SD
      card has been touched. */
   if (c->dav_put_draining) {
      c->dav_put_draining = false;
      return ws_send_auth_challenge(c, false);
   }
   (void)f_unlink(c->dav_put_target);
   if (f_rename(c->dav_put_tmppath, c->dav_put_target) != FR_OK) {
      (void)f_unlink(c->dav_put_tmppath);
      return ws_error(c, 500, "Internal Server Error",
                      "Could not finalize the uploaded file.");
   }
   /* Nudge a connected MTP host to refresh: 201 = new object, 204 = an
      existing object whose contents changed. */
   if (c->dav_put_status == 201)
      mtp_fs_notify_object_added(c->dav_put_target);
   else
      mtp_fs_notify_object_changed(c->dav_put_target);
   return dav_put_send_response(c);
}

static bool dav_put_consume(ws_conn_t *c, const uint8_t *data, size_t len)
{
   size_t take = (len < c->dav_remaining) ? len : c->dav_remaining;

   if (!dav_put_write_bytes(c, data, take))
      return true;   /* write failed: 507 queued, temp gone, target intact.
                        Keep the connection (return true, NOT abort) so the
                        507 is sent; do not decrement/finish/rename. */
   c->dav_remaining -= (uint32_t)take;
   if (c->dav_remaining == 0u)
      return dav_put_finish(c);
   return true;
}

/* Incrementally decode an HTTP "Transfer-Encoding: chunked" PUT body
   (RFC 9112 §7.1) across however many TCP segments it arrives in, and
   feed the decoded data bytes to the same sink dav_put_consume uses
   for a fixed Content-Length.  Chunk extensions are skipped and
   trailer fields are read and discarded (RFC 9112 §7.1.2).

      chunk        = chunk-size [ chunk-ext ] CRLF chunk-data CRLF
      chunk-size   = 1*HEXDIG
      last-chunk   = 1*("0") [ chunk-ext ] CRLF
      trailer-part = *( field-line CRLF )
   and the body ends at the blank line after the last-chunk / trailer-part. */
static bool dav_put_consume_chunked(ws_conn_t *c, const uint8_t *data,
                                    size_t len, size_t *consumed)
{
   size_t pos = 0u;

   while (pos < len) {
      if (c->dav_chunk_state == DAV_CHUNK_SIZE ||
          c->dav_chunk_state == DAV_CHUNK_TRAILER) {
         /* Accumulate one CRLF-terminated line a byte at a time; both
            states share this loop and differ only in what a completed
            line means (see below). */
         while (pos < len) {
            uint8_t ch = data[pos++];
            if (ch != '\n') {
               if (ch != '\r' &&
                   c->dav_chunk_linelen < sizeof c->dav_chunk_linebuf)
                  c->dav_chunk_linebuf[c->dav_chunk_linelen++] = (char)ch;
               continue;
            }
            if (c->dav_chunk_state == DAV_CHUNK_TRAILER) {
               bool blank = (c->dav_chunk_linelen == 0u);
               c->dav_chunk_linelen = 0u;
               if (blank) {
                  if (consumed != NULL) *consumed = pos;
                  return dav_put_finish(c);    /* trailer-part terminator */
               }
               break;                          /* discard, read the next line */
            }
            {
               size_t   n = c->dav_chunk_linelen;
               uint32_t size = 0u;
               bool     bad = (n == 0u);
               size_t   i;
               for (i = 0u; !bad && i < n; i++) {
                  char     h = c->dav_chunk_linebuf[i];
                  uint32_t d;
                  if (h == ';')
                     break;                    /* chunk-ext: ignore the rest */
                  if (h >= '0' && h <= '9')      d = (uint32_t)(h - '0');
                  else if (h >= 'a' && h <= 'f') d = (uint32_t)(h - 'a' + 10);
                  else if (h >= 'A' && h <= 'F') d = (uint32_t)(h - 'A' + 10);
                  else { bad = true; break; }
                  if (size > (0x7FFFFFFFu - d) / 16u) { bad = true; break; }
                  size = size * 16u + d;
               }
               c->dav_chunk_linelen = 0u;
               if (bad) {
                  if (consumed != NULL) *consumed = pos;
                  return ws_error(c, 400, "Bad Request",
                                  "Malformed chunk size.");
               }
               c->dav_chunk_remaining = size;
               c->dav_chunk_state = (size == 0u) ? DAV_CHUNK_TRAILER
                                                  : DAV_CHUNK_DATA;
            }
            break;
         }
      } else if (c->dav_chunk_state == DAV_CHUNK_DATA) {
         size_t avail = len - pos;
         size_t take  = (avail < c->dav_chunk_remaining) ? avail
                                                          : c->dav_chunk_remaining;
         if (c->dav_put_draining) {
            /* No Content-Length to size the drain up-front (unlike the
               cl <= WS_DRAIN_MAX_BYTES check in process_request), so the
               cap is enforced cumulatively here instead. */
            if ((uint64_t)c->dav_chunk_drained + take > WS_DRAIN_MAX_BYTES) {
               if (consumed != NULL) *consumed = pos;
               return ws_error(c, 413, "Payload Too Large",
                               "Unauthenticated PUT body exceeds the drain limit.");
            }
            c->dav_chunk_drained += (uint32_t)take;
         }
         if (!dav_put_write_bytes(c, data + pos, take)) {
            /* write failed: 507 queued, temp gone, target intact.  Keep the
               connection alive to send the 507 (return true, not abort). */
            if (consumed != NULL) *consumed = pos;
            return true;
         }
         pos += take;
         c->dav_chunk_remaining -= (uint32_t)take;
         if (c->dav_chunk_remaining == 0u)
            c->dav_chunk_state = DAV_CHUNK_DATA_CRLF;
      } else {   /* DAV_CHUNK_DATA_CRLF: skip the 2 bytes after chunk-data */
         if (c->dav_chunk_remaining == 0u)
            c->dav_chunk_remaining = 2u;
         size_t avail = len - pos;
         size_t take  = (avail < c->dav_chunk_remaining) ? avail
                                                          : c->dav_chunk_remaining;
         pos += take;
         c->dav_chunk_remaining -= (uint32_t)take;
         if (c->dav_chunk_remaining == 0u)
            c->dav_chunk_state = DAV_CHUNK_SIZE;
      }
   }
   if (consumed != NULL) *consumed = pos;
   return true;
}

static bool route_dav_put(ws_conn_t *c, const char *rawpath, int body_at)
{
   char    sdpath[WS_PATH_MAX];
   char    len_hdr[24];
   char    te_hdr[32];
   FILINFO fno;
   uint32_t content_length = 0u;
   const char *p;
   bool    target_existed;

   /* Any successful PUT changes a child's FILINFO; drop the per-
      directory cache so the next PROPFIND fetches fresh data.
      Invalidating up-front (rather than only on the success branch
      that calls f_rename) keeps the bookkeeping local to one line. */
   ws_fs_mutated();

   if (!dav_url_to_sdpath(rawpath, sdpath, sizeof sdpath))
      return ws_error(c, 400, "Bad Request", "That path is not allowed.");
   if (ws_is_root(sdpath))
      return ws_method_not_allowed(c,
                                   "OPTIONS, GET, HEAD, PROPFIND, PROPPATCH",
                                   "PUT on the root is not allowed.");

   /* RFC 7230 §3.3.1 / RFC 9112 §6.1: if Transfer-Encoding is present it
      takes precedence over Content-Length.  Decoded below by
      dav_put_consume_chunked() instead of the fixed-length sink -
      needed because Windows Explorer's MiniRedirector uses chunked
      PUTs in some client/registry configurations; rejecting them
      outright (as this server used to, with a 501) surfaces to the
      user as a generic "cannot perform the requested operation" copy
      failure (Win32 0x8007003A) with no indication a PUT was even the
      cause, since reads/PROPFIND never carry a body and so are never
      affected. */
   bool te_chunked = ws_find_header(c->reqhdr, c->reqhdr_len, "Transfer-Encoding",
                                    te_hdr, sizeof te_hdr)
                    && ws_strcasestr(te_hdr, "chunked") != NULL;

   if (!te_chunked) {
      /* Content-Length is required for the fixed-length streaming model. */
      if (!ws_find_header(c->reqhdr, c->reqhdr_len, "Content-Length",
                          len_hdr, sizeof len_hdr))
         return ws_error(c, 411, "Length Required",
                         "PUT requires a Content-Length.");
      /* Parse the digit string into a uint32_t, rejecting both
         non-digits and overflow.  Reject sizes >= 2 GiB up front: the
         counter is a uint32_t and the streaming model doesn't track
         anything bigger, so a value that wraps would let the client
         confuse the server about where the upload should end.  An
         explicit 413 is the right reply per RFC 9110 §15.5.14. */
      for (p = len_hdr; *p != '\0'; ++p) {
         if (*p < '0' || *p > '9')
            return ws_error(c, 400, "Bad Request",
                            "Malformed Content-Length.");
         if (content_length > (UINT32_C(0x7FFFFFFF) - 9u) / 10u)
            return ws_error(c, 413, "Payload Too Large",
                            "Content-Length exceeds the PUT size limit.");
         content_length = (content_length * 10u) + (uint32_t)(*p - '0');
      }
   }

   target_existed = f_stat(sdpath, &fno) == FR_OK;
   if (target_existed && (fno.fattrib & AM_DIR) != 0u)
      return ws_error(c, 409, "Conflict",
                      "Cannot PUT over an existing directory.");

   if (wifi_debug_enabled()) {
      wifi_debug_printf("PUT target='%s' te_chunked=%u cl=%lu existed=%u\n",
                        sdpath,
                        te_chunked ? 1u : 0u,
                        (unsigned long)content_length,
                        target_existed ? 1u : 0u);
   }

   /* Windows Explorer's MiniRedirector write-permission probe.
      Before sending the real PUT body, Windows issues a PUT with
      Content-Length: 0 to test whether the server will accept the
      write at all (auth OK, target writable, etc.).  A strictly
      RFC-compliant "PUT with empty body = create empty resource"
      reaction unlinks the existing file and replaces it with an
      empty one - which destroys the user's data before the real
      upload even begins.  Symptom in the trace:
          PUT /file CL='0'
          PUT-body: +0 bytes
          PUT: rename '/file.part' -> '/file'    <- file is now 0 B
          ...real PUT then DELETE rolls back...

      For an EXISTING target we treat CL=0 as a no-op probe and
      reply 204 No Content without touching the file - the user's
      data is preserved, and the subsequent LOCK/HEAD/PUT real-
      upload sequence proceeds normally.

      For a NEW target we still fall through to the regular temp+
      rename path so an empty file is created at the target.  Some
      Windows MiniRedirector versions follow the CL=0 probe with a
      HEAD on the same path before issuing the real PUT; if the
      file does not yet exist Windows treats the 404 as evidence
      the upload "failed" and rolls back with DELETE.  Honouring
      RFC 4918 / 9110 ("PUT with empty body creates an empty
      resource") on first creation keeps the HEAD succeeding.  A
      chunked PUT never hits this branch: its length isn't known until
      the terminating chunk, so te_chunked always falls through to the
      regular temp+rename path below. */
   if (!te_chunked && content_length == 0u && target_existed) {
      ws_strbuf_t r;
      if (wifi_debug_enabled())
         wifi_debug_printf("PUT: CL=0 probe on existing target, noop reply\n");
      sb_init(&r);
      sb_printf(&r,
                "HTTP/1.1 204 No Content\r\n"
                "Content-Length: 0\r\n"
                "%s"
                "\r\n",
                ws_connection_hdr(c));
      if (r.failed) { sb_free(&r); return ws_oom(c); }
      free(c->out); c->out = r.data; c->out_len = r.len;
      c->out_sent = 0u; c->bytes_queued = 0u; c->bytes_acked = 0u;
      c->state = CONN_SEND_MEM;
      conn_pump(c);
      return true;
   }

   /* A CL=0 PUT on a NEW target deliberately falls through to the
      temp+rename path below, so an empty file really is created at
      sdpath.  Answering 204 without creating it (and virtualizing only
      HEAD) is not enough: Explorer verifies the probe with PROPFIND,
      which stats the filesystem and would 404, and it reads that 404 as
      a failed upload and rolls back with DELETE. */

   /* Write to a temp file in the same directory so the eventual rename
      is atomic and a mid-upload disconnect cannot destroy the existing
      file at sdpath.  The temp name is "<target>.part"; if a previous
      aborted upload left a stale .part we overwrite it (FA_CREATE_ALWAYS). */
   strlcpy(c->dav_put_target, sdpath, sizeof c->dav_put_target);
   {
      int tn = snprintf(c->dav_put_tmppath, sizeof c->dav_put_tmppath,
                        "%s.part", sdpath);
      if (tn <= 0 || (size_t)tn >= sizeof c->dav_put_tmppath)
         return ws_error(c, 414, "URI Too Long",
                         "Target path is too long for a temp upload.");
   }

   if (f_open(&c->write_file.dav, c->dav_put_tmppath,
              FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
      return ws_error(c, 409, "Conflict",
                      "Cannot create the target (missing parent folder?).");
   c->dav_put_open       = true;
   c->dav_put_buf_len    = 0u;
   c->dav_put_chunked    = te_chunked;
   c->dav_remaining      = content_length;      /* unused while te_chunked */
   c->dav_chunk_state    = DAV_CHUNK_SIZE;
   c->dav_chunk_remaining= 0u;
   c->dav_chunk_linelen  = 0u;
   c->dav_put_status     = target_existed ? 204 : 201;
   c->dav_put_status_text= target_existed ? "No Content" : "Created";

   c->state = CONN_RECV_DAV_PUT;

   /* Expect: 100-continue handshake (RFC 9110 §10.1.1).  Windows
      Explorer's MiniRedirector sends "Expect: 100-continue" on every
      PUT and refuses to transmit the body until it sees a
      "HTTP/1.1 100 Continue" interim response.  Without this reply
      Windows hangs on the empty pipe; one TCP keep-alive byte slips
      through after a few seconds and then the connection times out.
      Combined with Windows' DELETE-before-PUT overwrite pattern, the
      target ends up removed and never replaced - exactly the
      "transfer stops after one byte, file gone" symptom.

      Cyberduck and other DAV clients don't send Expect, so they hit
      none of this and work today.

      The 100 response is written directly to the pcb; it is NOT
      queued via c->out because the FINAL response (201/204) also
      uses c->out and we'd lose it.  tcp_output flushes the small
      25-byte write immediately so Windows starts the body before the
      next ws_recv callback. */
   {
      char expect_hdr[32];
      if (ws_find_header(c->reqhdr, c->reqhdr_len, "Expect",
                         expect_hdr, sizeof expect_hdr)
          && ws_strcasestr(expect_hdr, "100-continue") != NULL) {
         static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
         if (c->pcb != NULL) {
            (void)tcp_write(c->pcb, cont, (u16_t)(sizeof cont - 1u),
                            TCP_WRITE_FLAG_COPY);
            (void)tcp_output(c->pcb);
         }
      }
   }

   /* The HTTP-header parse may have buffered some of the body already. */
   if ((size_t)body_at < c->reqhdr_len) {
      size_t already = c->reqhdr_len - (size_t)body_at;
      return te_chunked
          ? dav_put_consume_chunked(c, (const uint8_t *)c->reqhdr + body_at,
                          already, NULL)
           : dav_put_consume(c, (const uint8_t *)c->reqhdr + body_at, already);
   }
   /* Zero-length PUT (Content-Length model only): close + reply immediately. */
   if (!te_chunked && content_length == 0u)
      return dav_put_consume(c, NULL, 0u);

   return true;
}

/* Recursive DELETE for a collection: f_unlink only succeeds on files
   and empty directories, so walk the tree depth-first and unlink
   each child before unlinking its parent.  RFC 4918 §9.6.1 says
   DELETE on a collection MUST behave as DELETE with Depth: infinity.
   Bounded recursion (DAV_DELETE_MAX_DEPTH) keeps the stack budget
   predictable - each level adds one DIR (~600 bytes) plus the local
   FILINFO and the path buffer below.  Returns the first FRESULT
   that wasn't FR_OK, or FR_OK on full success. */
#define DAV_DELETE_MAX_DEPTH 16u
static FRESULT dav_delete_tree(char *path, size_t path_max, unsigned depth)
{
   DIR     dir;
   FILINFO fno;
   FRESULT fr;
   size_t  base_len;

   if (depth >= DAV_DELETE_MAX_DEPTH)
      return FR_DENIED;       /* too deep - refuse rather than overflow */

   fr = f_opendir(&dir, path);
   if (fr != FR_OK)
      return fr;

   base_len = strlen(path);
   while ((fr = f_readdir(&dir, &fno)) == FR_OK && fno.fname[0] != '\0') {
      size_t name_len = strlen(fno.fname);

      /* path = "<base>/<child>" - bail with FR_DENIED if it doesn't fit.
         path_max is the caller's buffer size; we need '/' + name + NUL. */
      if (base_len + 1u + name_len + 1u > path_max) {
         fr = FR_DENIED;
         break;
      }
      path[base_len] = '/';
      memcpy(&path[base_len + 1u], fno.fname, name_len + 1u);

      if ((fno.fattrib & AM_DIR) != 0u) {
         fr = dav_delete_tree(path, path_max, depth + 1u);
         if (fr != FR_OK)
            break;
      }
      fr = f_unlink(path);
      if (fr != FR_OK)
         break;

      path[base_len] = '\0';
   }

   path[base_len] = '\0';
   f_closedir(&dir);
   return fr;
}

static bool route_dav_delete(ws_conn_t *c, const char *rawpath)
{
   char    sdpath[WS_PATH_MAX];
   FILINFO fno;
   FRESULT fr;

   ws_fs_mutated();

   if (!dav_url_to_sdpath(rawpath, sdpath, sizeof sdpath))
      return ws_error(c, 400, "Bad Request", "That path is not allowed.");
   if (ws_is_root(sdpath))
      return ws_error(c, 403, "Forbidden", "Refusing to delete the root.");
   if (f_stat(sdpath, &fno) != FR_OK)
      return ws_error(c, 404, "Not Found", "No such resource.");

   /* RFC 4918 §9.6.1: DELETE on a collection acts as DELETE with
      Depth: infinity.  Walk depth-first via dav_delete_tree, then
      unlink the (now-empty) collection itself.  For a regular file
      f_unlink does the job in one call. */
   if ((fno.fattrib & AM_DIR) != 0u) {
      fr = dav_delete_tree(sdpath, sizeof sdpath, 0u);
      if (fr == FR_OK)
         fr = f_unlink(sdpath);
   } else {
      fr = f_unlink(sdpath);
   }

   if (fr != FR_OK) {
      if (fr == FR_DENIED)
         return ws_error(c, 409, "Conflict",
                         "Could not delete - too deep, locked, or read-only.");
      return ws_error(c, 500, "Internal Server Error",
                      "Delete failed.");
   }

   mtp_fs_notify_object_removed(sdpath);

   {
      ws_strbuf_t r;
      sb_init(&r);
      sb_printf(&r,
                "HTTP/1.1 204 No Content\r\n"
                "Content-Length: 0\r\n"
                "%s"
                "\r\n",
                ws_connection_hdr(c));
      if (r.failed) { sb_free(&r); return ws_oom(c); }
      free(c->out); c->out = r.data; c->out_len = r.len;
      c->out_sent = 0u; c->bytes_queued = 0u; c->bytes_acked = 0u;
      c->state = CONN_SEND_MEM;
      conn_pump(c);
   }
   return true;
}

static bool route_dav_mkcol(ws_conn_t *c, const char *rawpath)
{
   char    sdpath[WS_PATH_MAX];
   char    cl_hdr[24];
   FRESULT fr;

   ws_fs_mutated();

   if (!dav_url_to_sdpath(rawpath, sdpath, sizeof sdpath))
      return ws_error(c, 400, "Bad Request", "That path is not allowed.");
   if (ws_is_root(sdpath))
      return ws_method_not_allowed(c,
                                   "OPTIONS, GET, HEAD, PROPFIND, PROPPATCH",
                                   "Root already exists.");

   /* RFC 4918 §9.3.1: a server that does not support the body of an
      MKCOL request MUST respond with 415 Unsupported Media Type.  We
      do not parse extended MKCOL bodies, so reject anything with a
      non-zero Content-Length rather than silently dropping it. */
   if (ws_find_header(c->reqhdr, c->reqhdr_len, "Content-Length",
                      cl_hdr, sizeof cl_hdr)
       && cl_hdr[0] != '\0' && cl_hdr[0] != '0') {
      return ws_error(c, 415, "Unsupported Media Type",
                      "MKCOL request bodies are not supported.");
   }

   fr = f_mkdir(sdpath);
   if (fr == FR_EXIST)
      return ws_method_not_allowed(c,
                                   "OPTIONS, GET, HEAD, PUT, DELETE, COPY, "
                                   "MOVE, PROPFIND, PROPPATCH, LOCK, UNLOCK",
                                   "Resource already exists.");
   if (fr == FR_NO_PATH)
      return ws_error(c, 409, "Conflict",
                      "Parent collection does not exist.");
   if (fr != FR_OK)
      return ws_error(c, 500, "Internal Server Error", "mkdir failed.");

   mtp_fs_notify_object_added(sdpath);

   {
      ws_strbuf_t r;
      sb_init(&r);
      sb_printf(&r,
                "HTTP/1.1 201 Created\r\n"
                "Content-Length: 0\r\n"
                "%s"
                "\r\n",
                ws_connection_hdr(c));
      if (r.failed) { sb_free(&r); return ws_oom(c); }
      free(c->out); c->out = r.data; c->out_len = r.len;
      c->out_sent = 0u; c->bytes_queued = 0u; c->bytes_acked = 0u;
      c->state = CONN_SEND_MEM;
      conn_pump(c);
   }
   return true;
}

/* Extract the SD-path portion from a Destination: header.  The header is
   in the form "http://host[:port]/url-path"; we strip the scheme/host
   and URL-decode the path. */
static bool dav_destination_sdpath(const char *dest, char *out, size_t out_sz)
{
   const char *p = dest;
   const char *path;

   if (ws_prefix_ci_str(p, "http://"))  p += 7;
   else if (ws_prefix_ci_str(p, "https://")) p += 8;

   /* Skip host[:port] */
   path = strchr(p, '/');
   if (path == NULL)
      return false;

   return dav_url_to_sdpath(path, out, out_sz);
}

/* Queue the 201 Created / 204 No Content reply after a MOVE / COPY
   completes.  Shared between the MOVE fast path (which finishes inside
   route_dav_move_or_copy) and the per-tick COPY pump (which finishes
   in ws_copy_step). */
static bool dav_move_copy_send_response(ws_conn_t *c, bool dst_existed)
{
   ws_strbuf_t r;

   sb_init(&r);
   sb_printf(&r,
             "HTTP/1.1 %d %s\r\n"
             "Content-Length: 0\r\n"
             "%s"
             "\r\n",
             dst_existed ? 204 : 201,
             dst_existed ? "No Content" : "Created",
             ws_connection_hdr(c));
   if (r.failed) {
      sb_free(&r);
      return ws_oom(c);
   }
   free(c->out);
   c->out = r.data;
   c->out_len = r.len;
   c->out_sent = 0u;
   c->bytes_queued = 0u;
   c->bytes_acked = 0u;
   c->state = CONN_SEND_MEM;
   conn_pump(c);
   return true;
}

/* One per-tick step of an active CONN_DAV_COPY: read one chunk from
   copy_src, write it to copy_dst, return.  At source EOF (or on any
   error) tear down the COPY state and queue the response.  Called
   from webserver_poll once per main-loop tick - exactly the same
   cadence the per-tick SDIO stages use, so the cooperative 1 MHz
   loop never sees more than one f_read + f_write per tick. */
static void ws_copy_step(ws_conn_t *c)
{
   UINT     br = 0u;
   UINT     bw = 0u;
   FRESULT  fr;

   if (c == NULL || c->state != CONN_DAV_COPY)
      return;

   fr = f_read(&c->copy_src, c->dl_buf, (UINT)sizeof c->dl_buf, &br);
   if (fr != FR_OK) {
      f_close(&c->copy_src); c->copy_src_open = false;
      f_close(&c->copy_dst); c->copy_dst_open = false;
      g_ws_active_copy = NULL;
      (void)ws_error(c, 500, "Internal Server Error",
                     "Read failed during COPY.");
      return;
   }

   if (br > 0u) {
      if (f_write(&c->copy_dst, c->dl_buf, br, &bw) != FR_OK || bw != br) {
         f_close(&c->copy_src); c->copy_src_open = false;
         f_close(&c->copy_dst); c->copy_dst_open = false;
         g_ws_active_copy = NULL;
         (void)ws_error(c, 507, "Insufficient Storage",
                        "Write failed during COPY.");
         return;
      }
      /* The TCP connection sees no traffic during a COPY, so ws_poll
         would otherwise eventually time it out at WS_POLL_LIMIT (~30s).
         Reset the counter on each chunk so the timeout only fires if
         the COPY makes no progress at all (e.g. a wedged SD card),
         not just because the file is large. */
      c->poll_count = 0u;
   }

   /* EOF when f_read produces less than a full chunk (or zero).  This
      matches the original do/while exit condition. */
   if (br < (UINT)sizeof c->dl_buf) {
      bool dst_existed = c->copy_dst_existed;
      f_close(&c->copy_src); c->copy_src_open = false;
      f_close(&c->copy_dst); c->copy_dst_open = false;
      g_ws_active_copy = NULL;
      if (dst_existed)
         mtp_fs_notify_object_changed(c->dav_put_target);
      else
         mtp_fs_notify_object_added(c->dav_put_target);
      (void)dav_move_copy_send_response(c, dst_existed);
   }
}

static bool route_dav_move_or_copy(ws_conn_t *c, const char *rawpath, bool is_move)
{
   char    src[WS_PATH_MAX];
   char    dst[WS_PATH_MAX];
   char    dest_hdr[WS_PATH_MAX + 32];
   char    over_hdr[8];

   ws_fs_mutated();

   bool    overwrite = true;
   FILINFO fno_src;
   bool    dst_existed;

   if (!dav_url_to_sdpath(rawpath, src, sizeof src))
      return ws_error(c, 400, "Bad Request", "Bad source path.");
   if (!ws_find_header(c->reqhdr, c->reqhdr_len, "Destination",
                       dest_hdr, sizeof dest_hdr))
      return ws_error(c, 400, "Bad Request", "Missing Destination header.");
   if (!dav_destination_sdpath(dest_hdr, dst, sizeof dst))
      return ws_error(c, 400, "Bad Request", "Bad Destination path.");
   if (ws_find_header(c->reqhdr, c->reqhdr_len, "Overwrite",
                      over_hdr, sizeof over_hdr)) {
      overwrite = (over_hdr[0] == 'T' || over_hdr[0] == 't');
   }
   if (ws_is_root(src) || ws_is_root(dst))
      return ws_error(c, 403, "Forbidden", "Refusing to touch the root.");
   if (f_stat(src, &fno_src) != FR_OK)
      return ws_error(c, 404, "Not Found", "Source does not exist.");

   {
      FILINFO fno_dst;
      dst_existed = f_stat(dst, &fno_dst) == FR_OK;
      if (dst_existed && !overwrite)
         return ws_error(c, 412, "Precondition Failed",
                         "Destination exists and Overwrite is F.");
      /* If the destination is itself a directory we cannot replace it
         via f_unlink + create-file; the unlink would refuse a non-
         empty dir and we would then emit a generic "Rename failed".
         Detect this up front and tell the client clearly so it can
         issue DELETE on the directory first or pick a different
         destination.  RFC 4918 §9.9.5 / §9.8.5 list this as a "MUST"
         consideration for both MOVE and COPY. */
      if (dst_existed && (fno_dst.fattrib & AM_DIR) != 0u)
         return ws_error(c, 409, "Conflict",
                         "Destination is a directory; remove it or "
                         "choose a different name first.");
   }
   if (dst_existed) {
      /* File destination - remove it so f_rename / f_open succeed.
         Failure here is uncommon (would be e.g. a read-only flag in
         FatFs); leave it to the rename / open call below to surface
         the precise reason via its own error path. */
      (void)f_unlink(dst);
   }

   if (is_move) {
      FRESULT fr = f_rename(src, dst);
      if (fr != FR_OK)
         return ws_error(c, 500, "Internal Server Error", "Rename failed.");
      mtp_fs_notify_object_removed(src);
      if (dst_existed)
         mtp_fs_notify_object_changed(dst);
      else
         mtp_fs_notify_object_added(dst);
      return dav_move_copy_send_response(c, dst_existed);
   }

   /* COPY (per-tick).  Only one COPY may be active at a time so the
      per-tick bookkeeping stays a single global pointer.  A second
      COPY arriving while one is in flight gets 503 Service
      Unavailable - the client can retry once the first one is
      done.  The original implementation streamed src->dst
      synchronously inside this callback; on a 10 MB file that froze
      the cooperative poll loop (and the 1 MHz bus) for seconds.
      The per-tick version does one ~8 KB chunk per webserver_poll
      tick from ws_copy_step, so the worst case bus glitch is one
      f_read+f_write per chunk (~2 ms on a typical card). */
   if ((fno_src.fattrib & AM_DIR) != 0u)
      return ws_error(c, 501, "Not Implemented",
                      "Recursive collection COPY is not supported.");
   if (g_ws_active_copy != NULL)
      return ws_error(c, 503, "Service Unavailable",
                      "Another COPY is in progress; try again shortly.");
   if (f_open(&c->copy_src, src, FA_READ) != FR_OK)
      return ws_error(c, 500, "Internal Server Error",
                      "Could not open source.");
   c->copy_src_open = true;
   if (f_open(&c->copy_dst, dst, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
      f_close(&c->copy_src); c->copy_src_open = false;
      return ws_error(c, 500, "Internal Server Error",
                      "Could not create destination.");
   }
   c->copy_dst_open = true;
   c->copy_dst_existed = dst_existed;
   /* Stash the destination path for the completion-time MTP event.  PUT and
      COPY are mutually exclusive on a connection, so dav_put_target is idle
      here and reused rather than adding a second WS_PATH_MAX buffer. */
   strlcpy(c->dav_put_target, dst, sizeof c->dav_put_target);
   c->state = CONN_DAV_COPY;
   g_ws_active_copy = c;
   return true;
}

/* LOCK / UNLOCK stubs.  We are technically a class-2 server: we return a
   well-formed lock response with a synthetic opaque-lock token so that
   Windows Explorer's WebDAV redirector keeps writing, but no state is
   actually retained.  Single-user device - faking it is safe. */
static bool route_dav_lock(ws_conn_t *c, const char *rawpath)
{
   char        sdpath[WS_PATH_MAX];
   char        depth_hdr[16];
   const char *depth_text = "infinity";
   FILINFO     fno;
   FIL         lf;
   bool        target_exists = false;
   bool        created_placeholder = false;
   int         status = 200;
   const char *stext = "OK";
   ws_strbuf_t body;
   char        token[48];
   uint32_t    a, b;

   if (!dav_url_to_sdpath(rawpath, sdpath, sizeof sdpath))
      return ws_error(c, 400, "Bad Request", "That path is not allowed.");
   target_exists = (f_stat(sdpath, &fno) == FR_OK);
   if (!target_exists) {
      /* Windows Explorer follows LOCK with PROPFIND on the same URL and
         expects that resource lookup to succeed before it proceeds to PUT.
         Emulate lock-null creation by materializing an empty placeholder
         file now; the subsequent PUT atomically overwrites it. */
      if (f_open(&lf, sdpath, FA_CREATE_NEW | FA_WRITE) != FR_OK)
         return ws_error(c, 409, "Conflict",
                         "Cannot lock target (missing parent folder?).");
      if (f_close(&lf) != FR_OK)
         return ws_error(c, 500, "Internal Server Error",
                         "Could not finalize lock target.");
      ws_propfind_cache_invalidate();        /* our own child cache */
      mtp_fs_notify_object_added(sdpath);     /* MTP cache + host event */
      created_placeholder = true;
      target_exists = true;
      /* RFC allows 201 for lock-null creation, but some Windows
         MiniRedirector builds are stricter and only proceed cleanly
         when LOCK replies 200.  Keep 200 for compatibility. */
      status = 200;
      stext  = "OK";
   }

   if (ws_find_header(c->reqhdr, c->reqhdr_len, "Depth",
                      depth_hdr, sizeof depth_hdr)) {
      if (strcmp(depth_hdr, "0") == 0)
         depth_text = "0";
      else
         depth_text = "infinity";
   }

   /* Real lock state is not persisted (the chip is single-user), but
      the token still wants to be unguessable so a client that uses
      it in If: headers doesn't trivially collide with a different
      session.  Mix in the chip MAC and a bumped counter on top of
      the system timer, the same way ws_digest_refresh_nonce does. */
   ws_digest_refresh_nonce();   /* ensures g_ws_nonce_secret is set */
   ++g_ws_nonce_counter;
   a = RPI_GetSystemTime() ^ g_ws_nonce_secret;
   b = g_ws_nonce_counter ^ (g_ws_nonce_secret >> 16) ^ (g_ws_nonce_secret << 16);
   snprintf(token, sizeof token,
            "opaquelocktoken:%08lx-%08lx",
            (unsigned long)a, (unsigned long)b);

   sb_init(&body);
   sb_puts(&body,
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<D:prop xmlns:D=\"DAV:\"><D:lockdiscovery><D:activelock>"
      "<D:locktype><D:write/></D:locktype>"
      "<D:lockscope><D:exclusive/></D:lockscope>"
      "<D:depth>");
   sb_puts(&body, depth_text);
   sb_puts(&body,
      "</D:depth>"
      "<D:owner><D:href>Pi1MHz</D:href></D:owner>"
      "<D:timeout>Second-3600</D:timeout>"
      "<D:locktoken><D:href>");
   sb_html(&body, token);
   {
      ws_strbuf_t lockurl;
      sb_init(&lockurl);
      dav_append_url_path(&lockurl, sdpath, false);
      sb_puts(&body,
         "</D:href></D:locktoken>"
         "<D:lockroot><D:href>");
      if (lockurl.data != NULL)
         sb_html(&body, lockurl.data);
      sb_free(&lockurl);
   }
   sb_puts(&body,
      "</D:href></D:lockroot>"
      "</D:activelock></D:lockdiscovery></D:prop>");
   if (body.failed) { sb_free(&body); return ws_oom(c); }

   {
      ws_strbuf_t r;
      sb_init(&r);
      sb_printf(&r,
                "HTTP/1.1 %d %s\r\n"
                "Lock-Token: <%s>\r\n"
                "Timeout: Second-3600\r\n"
                "DAV: 1, 2\r\n"
                "Content-Type: application/xml; charset=\"utf-8\"\r\n"
                "Content-Length: %lu\r\n"
                "%s"
                "\r\n",
                status, stext, token, (unsigned long)body.len,
                ws_connection_hdr(c));
      sb_write(&r, body.data, body.len);
      sb_free(&body);
      if (r.failed) { sb_free(&r); return ws_oom(c); }
      if (wifi_debug_enabled())
         wifi_debug_printf("LOCK target='%s' existed=%u created=%u -> %d %s\n",
                           sdpath,
                           created_placeholder ? 0u : 1u,
                           created_placeholder ? 1u : 0u,
                           status, stext);
      free(c->out); c->out = r.data; c->out_len = r.len;
      c->out_sent = 0u; c->bytes_queued = 0u; c->bytes_acked = 0u;
      c->state = CONN_SEND_MEM;
      conn_pump(c);
   }
   return true;
}

static bool route_dav_unlock(ws_conn_t *c)
{
   ws_strbuf_t r;
   sb_init(&r);
   sb_printf(&r,
             "HTTP/1.1 204 No Content\r\n"
             "Content-Length: 0\r\n"
             "%s"
             "\r\n",
             ws_connection_hdr(c));
   if (r.failed) { sb_free(&r); return ws_oom(c); }
   free(c->out); c->out = r.data; c->out_len = r.len;
   c->out_sent = 0u; c->bytes_queued = 0u; c->bytes_acked = 0u;
   c->state = CONN_SEND_MEM;
   conn_pump(c);
   return true;
}

/* PROPPATCH stub.  Windows Explorer's MiniRedirector issues PROPPATCH
   immediately before the PUT body to set Win32CreationTime /
   Win32LastModifiedTime / Win32LastAccessTime / Win32FileAttributes -
   it uses these to preserve the source file's timestamps on the
   uploaded copy.  If the server returns 405 Method Not Allowed (as we
   used to before this stub existed), MiniRedirector treats the entire
   upload as failed and rolls back by issuing DELETE on the target,
   even though the PUT itself would have succeeded.  Symptom: a one-
   segment PUT goes out, the body never follows, the target file is
   then DELETEd - exactly the "transfer stops after one byte, file
   gone" report from the trace.

   FatFs has no API for setting modification timestamps from a host
   string and on a single-user device the metadata round-trip serves
   no real purpose, so this stub accepts every property update and
   returns 207 Multi-Status with a single 200 OK propstat covering
   them all.  RFC 4918 §9.2 explicitly permits returning success
   without persisting the property (the client treats the operation
   as "accepted").

   We do not parse the PROPPATCH XML body - any bytes that arrive
   after the headers stay in the receive window and are quietly
   dropped once c->state advances to CONN_SEND_MEM (see conn_consume).
   The response body therefore does not enumerate which properties
   were "accepted"; a generic "all OK" propstat is enough to satisfy
   Windows. */
/* Length-bounded substring search: the request body in c->reqhdr is not
   NUL-terminated, so strstr() can't be used on it. */
static const char *dav_memfind(const char *hay, size_t hay_len,
                               const char *needle)
{
   size_t nlen = strlen(needle);
   size_t i;
   if (nlen == 0u || hay_len < nlen)
      return NULL;
   for (i = 0u; i + nlen <= hay_len; ++i)
      if (memcmp(hay + i, needle, nlen) == 0)
         return hay + i;
   return NULL;
}

/* Parse an RFC1123 HTTP-date ("Wed, 22 Jul 2026 09:00:00 GMT") into the
   FAT-packed date/time words FatFs uses.  Windows Explorer sends the file's
   modification time in this format via PROPPATCH.  The weekday prefix and
   the GMT suffix are ignored (FAT timestamps are timezone-naive and Windows
   always sends GMT).  Returns false on any malformed field. */
static bool dav_parse_http_date(const char *s, size_t len,
                                uint16_t *fdate, uint16_t *ftime)
{
   static const char months[12][4] = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
   char         buf[40];
   size_t       n = (len < sizeof buf - 1u) ? len : sizeof buf - 1u;
   const char  *p;
   const char  *comma;
   unsigned     day = 0u, year = 0u, hh = 0u, mm = 0u, ss = 0u, month = 0u;
   unsigned     i;

   memcpy(buf, s, n);
   buf[n] = '\0';

   p = buf;
   comma = strchr(buf, ',');            /* skip the "Wed, " weekday prefix */
   if (comma != NULL)
      p = comma + 1;
   while (*p == ' ') ++p;

   if (*p < '0' || *p > '9') return false;
   while (*p >= '0' && *p <= '9') day = day * 10u + (unsigned)(*p++ - '0');
   while (*p == ' ') ++p;

   for (i = 0u; i < 12u; ++i)
      if (strncmp(p, months[i], 3) == 0) { month = i + 1u; break; }
   if (month == 0u) return false;
   p += 3;
   while (*p == ' ') ++p;

   if (*p < '0' || *p > '9') return false;
   while (*p >= '0' && *p <= '9') year = year * 10u + (unsigned)(*p++ - '0');
   while (*p == ' ') ++p;

   if (*p < '0' || *p > '9') return false;
   while (*p >= '0' && *p <= '9') hh = hh * 10u + (unsigned)(*p++ - '0');
   if (*p++ != ':') return false;
   while (*p >= '0' && *p <= '9') mm = mm * 10u + (unsigned)(*p++ - '0');
   if (*p++ != ':') return false;
   while (*p >= '0' && *p <= '9') ss = ss * 10u + (unsigned)(*p++ - '0');

   if (year < 1980u || year > 2107u || day < 1u || day > 31u
       || hh > 23u || mm > 59u || ss > 59u)
      return false;

   *fdate = (uint16_t)(((year - 1980u) << 9) | (month << 5) | day);
   *ftime = (uint16_t)((hh << 11) | (mm << 5) | (ss / 2u));
   return true;
}

/* Best-effort: locate <*:Win32LastModifiedTime>RFC1123-date</...> in a
   PROPPATCH body and stamp it onto the FAT directory entry (converting the
   GMT the client sends to the local time the entry holds).  Shared by the
   in-segment fast path and the split-segment capture path.  A missing or
   unparseable value just leaves the file's existing date untouched. */
static void dav_apply_win32_mtime(const char *sdpath,
                                  const char *body, size_t blen)
{
   const char *tag = dav_memfind(body, blen, "Win32LastModifiedTime>");
   const char *val;
   size_t      rem, j;

   if (tag == NULL)
      return;
   val = tag + (sizeof "Win32LastModifiedTime>" - 1u);
   rem = blen - (size_t)(val - body);
   for (j = 0u; j < rem; ++j) {
      if (val[j] == '<') {
         FILINFO  fno;
         uint16_t fdate, ftime;
         if (dav_parse_http_date(val, j, &fdate, &ftime)) {
            /* Client sends GMT; the FAT entry holds local time. */
            fat_shift_minutes(&fdate, &ftime, ws_utc_offset_minutes());
            fno.fdate = fdate;
            fno.ftime = ftime;
            (void)f_utime(sdpath, &fno);   /* best-effort */
         }
         return;
      }
   }
}

static bool route_dav_proppatch(ws_conn_t *c, const char *rawpath, int body_at)
{
   char        sdpath[WS_PATH_MAX];
   ws_strbuf_t b;
   ws_strbuf_t url;
   size_t      already;

   if (!dav_url_to_sdpath(rawpath, sdpath, sizeof sdpath))
      return ws_error(c, 400, "Bad Request", "That path is not allowed.");

   /* Windows Explorer supplies the file's real modification time here, as a
      <*:Win32LastModifiedTime> RFC1123 date sent after the content PUT.  This
      device has no RTC (FatFs runs FF_FS_NORTC, so every file is created with
      a fixed date), so persisting Explorer's value into the FAT directory
      entry with f_utime is the only way a copied file ends up with the right
      timestamp - and PROPFIND already reads fdate/ftime back, so directory
      listings then show it.

      The body often arrives in a LATER TCP segment than the headers (which
      the keep-alive drain block flagged by setting drain_remaining > 0).
      When it does, stash the target and the bytes present so far in dl_buf
      (idle during PROPPATCH) and let conn_consume's drain loop append the
      rest, stamping the date once the full body is in.  When the whole body
      is already here, stamp it immediately. */
   already = ((size_t)body_at < c->reqhdr_len)
           ? (c->reqhdr_len - (size_t)body_at) : 0u;
   if (c->drain_remaining == 0u) {
      if (already > 0u)
         dav_apply_win32_mtime(sdpath, c->reqhdr + body_at, already);
   } else {
      size_t n = (already < WS_FILE_CHUNK) ? already : WS_FILE_CHUNK;
      memcpy(c->dl_buf, c->reqhdr + body_at, n);
      c->pp_buf_len = n;
      c->pp_capture = true;
      strlcpy(c->pp_path, sdpath, sizeof c->pp_path);
   }

   sb_init(&b);
   sb_puts(&b,
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<D:multistatus xmlns:D=\"DAV:\">"
      "<D:response><D:href>");
   sb_init(&url);
   dav_append_url_path(&url, sdpath, false);
   if (url.data != NULL)
      sb_html(&b, url.data);
   sb_free(&url);
   sb_puts(&b,
      "</D:href><D:propstat>"
      "<D:prop/>"
      "<D:status>HTTP/1.1 200 OK</D:status>"
      "</D:propstat></D:response></D:multistatus>");

   if (b.failed) {
      sb_free(&b);
      return ws_oom(c);
   }

   {
      ws_strbuf_t r;
      sb_init(&r);
      sb_printf(&r,
                "HTTP/1.1 207 Multi-Status\r\n"
                "Content-Type: application/xml; charset=\"utf-8\"\r\n"
                "Content-Length: %lu\r\n"
                "%s"
                "\r\n",
                (unsigned long)b.len, ws_connection_hdr(c));
      sb_write(&r, b.data, b.len);
      sb_free(&b);

      if (r.failed) {
         sb_free(&r);
         return ws_oom(c);
      }
      free(c->out);
      c->out = r.data;
      c->out_len = r.len;
      c->out_sent = 0u;
      c->bytes_queued = 0u;
      c->bytes_acked = 0u;
      c->state = CONN_SEND_MEM;
      conn_pump(c);
   }
   return true;
}

/* GET fallback for paths that don't match a management route: try to
   serve the URL path as an SD file (the same view a WebDAV-mounted
   client gets).  Returns false if the path isn't a regular file - the
   caller then sends 404. */
/* Returns true after queuing a response (success OR a 405 we
   emitted ourselves); false if the path is unparseable, root, or
   doesn't exist - in which case the caller emits the catch-all 404.
   Distinguishing "exists but is a collection" lets us return the
   RFC-compliant 405 ("GET on a collection is not allowed; try
   PROPFIND") rather than misleadingly claiming the URL doesn't
   exist. */
static bool route_dav_get_file(ws_conn_t *c, const char *rawpath)
{
   char    sdpath[WS_PATH_MAX];
   FILINFO fno;

   if (!dav_url_to_sdpath(rawpath, sdpath, sizeof sdpath))
      return false;
   if (ws_is_root(sdpath))
      return false;
   if (f_stat(sdpath, &fno) != FR_OK)
      return false;
   if ((fno.fattrib & AM_DIR) != 0u)
      return ws_method_not_allowed(c,
                                   "OPTIONS, PROPFIND, PROPPATCH",
                                   "GET on a collection is not supported; use PROPFIND.");

   return start_download(c, sdpath);
}

/* ------------------------------------------------------------------ */
/* Request dispatch                                                    */
/* ------------------------------------------------------------------ */

/* Method-string → method type bucket.  Used by the digest check (HEAD
   is treated as GET for hash purposes per RFC) and the dispatcher. */
static bool ws_method_is(const char *method, const char *want)
{
   return strcmp(method, want) == 0;
}

static bool process_request(ws_conn_t *c, int body_at)
{
   char  method[12];
   char  rawpath[WS_PATH_MAX];
   char *query;

   if (!ws_parse_request_line(c->reqhdr, method, sizeof method,
                              rawpath, sizeof rawpath))
      return ws_error(c, 400, "Bad Request", "The request was malformed.");

   /* HEAD is dispatched to the same GET handlers; conn_pump consults
      is_head to suppress the body while still sending the headers
      (and their Content-Length / Content-Type / etc.). */
   c->is_head = (strcmp(method, "HEAD") == 0);

   /* HTTP/1.1 keep-alive negotiation.  Default for HTTP/1.1 is keep-
      alive unless the request says "Connection: close"; default for
      HTTP/1.0 is close unless the request says "Connection: Keep-
      Alive".  The request line is the first line of c->reqhdr and
      always ends in " HTTP/<version>\r\n", so a strstr over the
      first line tells us which.  ws_strcasestr handles the case-
      insensitive Connection value (Windows uses "Keep-Alive"). */
   {
      bool is_http_11;
      char conn_hdr[32] = "";
      bool client_wants_close;
      bool client_wants_keep;
      const char *first_line_end = strchr(c->reqhdr, '\n');
      size_t first_line_len = (first_line_end != NULL)
                            ? (size_t)(first_line_end - c->reqhdr)
                            : c->reqhdr_len;
      /* strchr stopped at the '\n', so first_line_len still counts the
         CR of the CRLF terminator.  Drop it: otherwise the last eight
         bytes are "TTP/1.1\r", the test below never matches, and
         keep-alive is silently disabled for every compliant client -
         HTTP/1.1 peers rely on it being the default and do not send an
         explicit "Connection: keep-alive". */
      if (first_line_len > 0u && c->reqhdr[first_line_len - 1u] == '\r')
         --first_line_len;
      is_http_11 = (first_line_len >= 8u
                    && memcmp(c->reqhdr + first_line_len - 8u,
                              "HTTP/1.1", 8u) == 0);
      (void)ws_find_header(c->reqhdr, c->reqhdr_len, "Connection",
                           conn_hdr, sizeof conn_hdr);
      client_wants_close = (ws_strcasestr(conn_hdr, "close") != NULL);
      client_wants_keep  = (ws_strcasestr(conn_hdr, "keep-alive") != NULL);
      c->keep_alive = is_http_11 ? !client_wants_close
                                 : client_wants_keep;
      /* Enforce the per-connection request cap so a misbehaving peer
         cannot pin a TCP slot.  The +1 lands on the cap when the
         response we are about to queue is the last allowed one. */
      ++c->request_count;
      if (c->request_count >= WS_KEEP_ALIVE_MAX_REQUESTS)
         c->keep_alive = false;
   }

   /* Keep-alive + an unconsumed request body = connection desync.  Only PUT
      and POST consume their bodies; every other verb (PROPFIND, PROPPATCH,
      LOCK, MKCOL, ...) queues a response and never reads the XML body.  When
      that body is fully present in this segment it is harmlessly discarded at
      reset, but when it arrives in a LATER TCP segment - which Windows
      Explorer's redirector does routinely, sending headers and body as
      separate segments - the leftover bytes would be parsed as the next
      request line and desync the reused connection.  curl packs headers+body
      into one segment so it never trips this, which is why the full DAV write
      sequence completes over curl yet Explorer stalled after LOCK.

      Fix: record how many body bytes still trail so conn_consume can DRAIN
      (discard) them before parsing the next request.  This keeps the
      connection alive - a reconnect per body-bearing verb is what made the
      copy pause at ~97% (each reconnect risks the multi-second SYN-loss
      latency).  A body too large to drain sanely, or a chunked body whose
      length we can't size up-front, falls back to closing the connection. */
   if (c->keep_alive
       && c->drain_remaining == 0u
       && !ws_method_is(method, "PUT")
       && !ws_method_is(method, "POST")) {
      char te_hdr[32] = "";
      char cl_hdr[24];
      if (ws_find_header(c->reqhdr, c->reqhdr_len, "Transfer-Encoding",
                         te_hdr, sizeof te_hdr)
          && ws_strcasestr(te_hdr, "chunked") != NULL) {
         c->keep_alive = false;                 /* can't size a chunked drain */
      } else if (ws_find_header(c->reqhdr, c->reqhdr_len, "Content-Length",
                                cl_hdr, sizeof cl_hdr)) {
         uint32_t cl = 0u;
         bool     cl_ok = true;
         const char *p;
         for (p = cl_hdr; *p != '\0'; ++p) {
            if (*p < '0' || *p > '9') { cl_ok = false; break; }
            cl = cl * 10u + (uint32_t)(*p - '0');
         }
         size_t already = ((size_t)body_at < c->reqhdr_len)
                        ? (c->reqhdr_len - (size_t)body_at) : 0u;
         if (!cl_ok || cl > WS_DRAIN_MAX_BYTES)
            c->keep_alive = false;              /* unparseable / absurd length */
         else if (cl > already)
            c->drain_remaining = cl - (uint32_t)already;
      }
   }

   query = strchr(rawpath, '?');
   if (query != NULL)
      *query = '\0';

   if (wifi_debug_enabled())
      wifi_debug_printf("REQ %s %s\n", method, rawpath);

   /* Digest auth.  When configured, every route requires a valid
      Authorization header.  OPTIONS is intentionally NOT exempt - the
      WebDAV client sends it after authenticating, and exempting it
      makes the realm appear to disagree on later requests. */
   if (ws_digest_enabled()) {
      ws_auth_status_t a = ws_digest_verify(method, c->reqhdr,
                                            c->reqhdr_len);
      if (a == WS_AUTH_REQUIRED || a == WS_AUTH_STALE) {
         /* Windows Explorer's MiniRedirector quirk: a PUT that
            carries Expect: 100-continue + a non-zero body and gets
            a 401 reply BEFORE the 100 Continue handshake is treated
            as a fatal failure - the client cancels the request and
            never retries with credentials, even though every other
            verb (HEAD, PROPFIND, PROPPATCH, LOCK) cleanly retries.
            Symptom: only one REQ PUT line in the trace, immediately
            followed by REQ DELETE rollback.

            Workaround: when the unauthenticated request is a body-
            bearing PUT with Expect: 100-continue, send the 100
            response so MiniRedirector ships the body, drain those
            bytes into nowhere (no temp file, nothing is written to
            the SD card), and only then send the 401 challenge.  The
            connection is closed after the challenge, the client
            opens a fresh one and resends the PUT with the digest
            credentials.  The body has to be sent twice on the wire
            but the upload completes - which is the entire point.

            Capped at WS_DRAIN_MAX_BYTES so an attacker on the LAN
            can't make us spin reading an arbitrary-size unauthed
            body. */
         if (ws_method_is(method, "PUT")) {
            char cl_hdr[24];
            char ex_hdr[32] = "";
            char te_hdr2[32] = "";
            bool chunked = ws_find_header(c->reqhdr, c->reqhdr_len,
                                          "Transfer-Encoding", te_hdr2,
                                          sizeof te_hdr2)
                         && ws_strcasestr(te_hdr2, "chunked") != NULL;
            bool have_cl = ws_find_header(c->reqhdr, c->reqhdr_len,
                                          "Content-Length", cl_hdr,
                                          sizeof cl_hdr);
            uint32_t cl = 0u;
            bool cl_ok = have_cl;
            const char *p;
            for (p = cl_hdr; have_cl && *p != '\0'; ++p) {
               if (*p < '0' || *p > '9') { cl_ok = false; break; }
               if (cl > (UINT32_C(0x7FFFFFFF) - 9u) / 10u) { cl_ok = false; break; }
               cl = cl * 10u + (uint32_t)(*p - '0');
            }
            /* A chunked body has no Content-Length to size the drain
               up-front (cl_ok/cl are meaningless then); the cumulative
               dav_chunk_drained cap in dav_put_consume_chunked takes
               over that job instead. */
            if (chunked || (cl_ok && cl > 0u && cl <= WS_DRAIN_MAX_BYTES)) {
               /* Whether or not the client sent Expect: 100-continue,
                  MiniRedirector treats an immediate 401 on a body-
                  bearing PUT as fatal.  Send the 100 Continue only
                  when the client actually asked for it - sending an
                  unsolicited 100 to a HTTP/1.0-style client may
                  confuse its parser.  Then accept and discard the
                  full body (Content-Length- or chunk-delimited), and
                  only then return the 401 so the client's next
                  connection retries cleanly with credentials. */
               bool has_expect;
               size_t already;
               (void)ws_find_header(c->reqhdr, c->reqhdr_len, "Expect",
                                    ex_hdr, sizeof ex_hdr);
               has_expect = (ex_hdr[0] != '\0'
                             && ws_strcasestr(ex_hdr, "100-continue") != NULL);
               already = ((size_t)body_at < c->reqhdr_len)
                       ? (c->reqhdr_len - (size_t)body_at) : 0u;
               if (has_expect && c->pcb != NULL) {
                  static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
                  (void)tcp_write(c->pcb, cont,
                                  (u16_t)(sizeof cont - 1u),
                                  TCP_WRITE_FLAG_COPY);
                  (void)tcp_output(c->pcb);
               }
               c->dav_put_open      = false;
               c->dav_put_draining  = true;
               c->dav_put_chunked   = chunked;
               c->state             = CONN_RECV_DAV_PUT;
               if (chunked) {
                  c->dav_chunk_state     = DAV_CHUNK_SIZE;
                  c->dav_chunk_remaining = 0u;
                  c->dav_chunk_linelen   = 0u;
                  c->dav_chunk_drained   = 0u;
               } else {
                  c->dav_remaining = cl;
               }
               if (already > 0u)
                  return chunked
                       ? dav_put_consume_chunked(c,
                                                 (const uint8_t *)c->reqhdr + body_at,
                                        already, NULL)
                       : dav_put_consume(c,
                                         (const uint8_t *)c->reqhdr + body_at,
                                         already);
               return true;
            }
         }
         return ws_send_auth_challenge(c, a == WS_AUTH_STALE);
      }
      /* WS_AUTH_OK - fall through */
   }

   /* WebDAV verbs (RFC 4918 + LOCK stubs).  These take precedence over
      the legacy GET/POST handlers so the WebDAV mount covers the
      whole URL space; the management routes below only fire for the
      handful of GET / POST paths they own. */
   if (ws_method_is(method, "OPTIONS"))
      return route_dav_options(c);
   if (ws_method_is(method, "PROPFIND"))
      return route_dav_propfind(c, rawpath, body_at);
   if (ws_method_is(method, "PROPPATCH"))
      return route_dav_proppatch(c, rawpath, body_at);
   if (ws_method_is(method, "PUT"))
      return route_dav_put(c, rawpath, body_at);
   if (ws_method_is(method, "DELETE"))
      return route_dav_delete(c, rawpath);
   if (ws_method_is(method, "MKCOL"))
      return route_dav_mkcol(c, rawpath);
   if (ws_method_is(method, "MOVE"))
      return route_dav_move_or_copy(c, rawpath, true);
   if (ws_method_is(method, "COPY"))
      return route_dav_move_or_copy(c, rawpath, false);
   if (ws_method_is(method, "LOCK"))
      return route_dav_lock(c, rawpath);
   if (ws_method_is(method, "UNLOCK"))
      return route_dav_unlock(c);

   if (ws_method_is(method, "GET") || ws_method_is(method, "HEAD")) {
      if (strcmp(rawpath, "/") == 0)
         return route_home(c);
      if (strcmp(rawpath, "/status") == 0)
         return route_status(c);
      if (strcmp(rawpath, "/aun") == 0)
         return route_aun(c);
      if (strcmp(rawpath, "/framebuffer") == 0)
         return route_framebuffer(c);
      if (strcmp(rawpath, "/framebuffer.bmp") == 0)
         return route_framebuffer_bmp(c);
      if (strcmp(rawpath, "/reboot") == 0)
         return route_reboot_confirm(c);
      if (strcmp(rawpath, "/files") == 0 || ws_prefix("/files/", rawpath))
         return route_files_get(c, rawpath);
      /* Fallback: serve the URL path as an SD file (the WebDAV "/"
         mount view).  If the path doesn't resolve to a regular file,
         this returns false and we 404. */
      if (route_dav_get_file(c, rawpath))
         return true;
      return ws_error(c, 404, "Not Found", "No such page.");
   }

   if (ws_method_is(method, "POST")) {
      if (strcmp(rawpath, "/reboot") == 0)
         return route_reboot_do(c);
      if (strcmp(rawpath, "/files") == 0 || ws_prefix("/files/", rawpath))
         return route_upload(c, rawpath, body_at);
      return ws_error(c, 404, "Not Found", "No such page.");
   }

   return ws_method_not_allowed(c,
                                "OPTIONS, GET, HEAD, PUT, DELETE, COPY, "
                                "MOVE, MKCOL, PROPFIND, PROPPATCH, LOCK, "
                                "UNLOCK, POST",
                                "Unsupported method.");
}

static bool conn_consume(ws_conn_t *c, const uint8_t *data, size_t len)
{
   size_t pos = 0u;

   while (pos < len) {
      /* Discard any still-trailing body of a body-bearing verb whose handler
         did not consume it (see the drain_remaining note in process_request).
         Done before the state dispatch because these bytes may arrive while
         the response is still in flight (CONN_SEND_MEM) or after the
         connection has reset for the next request (CONN_RECV_HEADER); in both
         cases they are body, not a new request. */
      if (c->drain_remaining > 0u) {
         size_t avail = len - pos;
         size_t d     = (avail < c->drain_remaining) ? avail
                                                     : c->drain_remaining;
         /* A split-segment PROPPATCH body is being drained: capture it into
            dl_buf (up to its size) so the trailing Win32LastModifiedTime can
            still be parsed once the whole body has arrived (see
            route_dav_proppatch).  Overflow past dl_buf is dropped - the date
            tag sits near the start of the body, well inside 8 KB. */
         if (c->pp_capture && c->pp_buf_len < WS_FILE_CHUNK) {
            size_t space = WS_FILE_CHUNK - c->pp_buf_len;
            size_t cap   = (d < space) ? d : space;
            memcpy(c->dl_buf + c->pp_buf_len, data + pos, cap);
            c->pp_buf_len += cap;
         }
         pos += d;
         c->drain_remaining -= (uint32_t)d;
         if (c->drain_remaining == 0u && c->pp_capture) {
            dav_apply_win32_mtime(c->pp_path, (const char *)c->dl_buf,
                                  c->pp_buf_len);
            c->pp_capture = false;
            c->pp_buf_len = 0u;
         }
         continue;
      }
      if (c->state == CONN_RECV_HEADER) {
         size_t space = WS_HEADER_MAX - c->reqhdr_len;
         size_t take  = len - pos;
         int    body_at;

         if (take > space)
            take = space;
         memcpy(c->reqhdr + c->reqhdr_len, data + pos, take);
         c->reqhdr_len += take;
         pos += take;
         c->reqhdr[c->reqhdr_len] = '\0';

         body_at = ws_find_header_end(c->reqhdr, c->reqhdr_len);
         if (body_at >= 0) {
            if (!process_request(c, body_at))
               return false;
            /* loop continues; any remaining bytes go to the new state */
         } else if (c->reqhdr_len >= WS_HEADER_MAX) {
            return ws_error(c, 431, "Request Header Fields Too Large",
                            "The request header is too large.");
         } else {
            return true;          /* wait for more header data */
         }
      } else if (c->state == CONN_RECV_UPLOAD) {
         if (!upload_consume(c, data + pos, len - pos))
            return false;
         pos = len;
      } else if (c->state == CONN_RECV_DAV_PUT) {
         if (c->dav_put_chunked) {
            size_t consumed = 0u;
            if (!dav_put_consume_chunked(c, data + pos, len - pos, &consumed))
               return false;
            pos += consumed;
         } else {
            size_t rem = len - pos;
            size_t take = (rem < c->dav_remaining) ? rem : c->dav_remaining;
            /* dav_remaining == 0 here means the body is fully received but
               more bytes are still in this segment: they belong to the next
               (pipelined) request, which we do not buffer.  Without this
               guard take would be 0, pos would never advance, and the
               while (pos < len) loop would spin forever inside the lwIP
               TCP callback - a hard lockup of the cooperative main loop.
               Baseline's unconditional pos = len avoided this; restore that
               safety by dropping the pipelined tail. */
            if (take == 0u) {
               c->pipelined_bytes_dropped = true;
               return true;
            }
            if (!dav_put_consume(c, data + pos, take))
               return false;
            pos += take;
         }
      } else {
         /* CONN_SEND_*: a pipelined request we do not buffer.  Remember
            the drop so the response completion closes the connection -
            the client then retries immediately instead of waiting on a
            kept-alive socket that will never answer. */
         if (pos < len)
            c->pipelined_bytes_dropped = true;
         return true;
      }
   }
   return true;
}

/* ------------------------------------------------------------------ */
/* lwIP callbacks                                                      */
/* ------------------------------------------------------------------ */

static err_t ws_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p,
                     err_t err)
{
   ws_conn_t   *c = (ws_conn_t *)arg;
   const struct pbuf *q;
   bool         alive = true;

   if (c == NULL) {
      if (p != NULL)
         pbuf_free(p);
      return ERR_OK;
   }

   if (err != ERR_OK) {
      if (p != NULL)
         pbuf_free(p);
      (void)conn_close(c, true);
      return ERR_ABRT;
   }

   if (p == NULL) {
      /* the remote end closed the connection.  If tcp_close fails it
         falls back to tcp_abort; lwIP then requires ERR_ABRT from this
         callback or it will keep touching the freed pcb. */
      return conn_close(c, false) ? ERR_ABRT : ERR_OK;
   }

   tcp_recved(tpcb, p->tot_len);
   c->poll_count = 0u;

   for (q = p; q != NULL && alive; q = q->next)
      alive = conn_consume(c, (const uint8_t *)q->payload, q->len);

   pbuf_free(p);
   return alive ? ERR_OK : ERR_ABRT;
}

static err_t ws_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
   ws_conn_t *c = (ws_conn_t *)arg;
   (void)tpcb;

   if (c == NULL)
      return ERR_OK;

   c->bytes_acked += len;
   c->poll_count = 0u;
   /* conn_pump may close the connection and, if tcp_close fails, fall
      back to tcp_abort.  lwIP requires the callback to return ERR_ABRT
      whenever tcp_abort was called from within it, otherwise the stack
      keeps touching the now-freed pcb. */
   return conn_pump(c) ? ERR_ABRT : ERR_OK;
}

static err_t ws_poll(void *arg, struct tcp_pcb *tpcb)
{
   ws_conn_t *c = (ws_conn_t *)arg;
   (void)tpcb;

   if (c == NULL)
      return ERR_OK;

   if (++c->poll_count > WS_POLL_LIMIT) {
      (void)conn_close(c, true);
      return ERR_ABRT;
   }

   if (c->state == CONN_SEND_MEM || c->state == CONN_SEND_FILE
       || c->state == CONN_SEND_FB) {
      /* conn_pump may close the connection; if tcp_close fails it falls
         back to tcp_abort, in which case lwIP requires ERR_ABRT here so
         the stack does not touch the freed pcb. */
      return conn_pump(c) ? ERR_ABRT : ERR_OK;
   }
   return ERR_OK;
}

static void ws_err(void *arg, err_t err)
{
   ws_conn_t *c = (ws_conn_t *)arg;
   (void)err;

   if (c == NULL)
      return;

   /* lwIP has already freed the pcb */
   c->pcb = NULL;
   if (c->dl_open) {
      f_close(&c->dl_file);
      c->dl_open = false;
   }
   if (c->up_file_open) {
      f_close(&c->write_file.up);
      c->up_file_open = false;
   }
   if (c->copy_src_open) {
      f_close(&c->copy_src);
      c->copy_src_open = false;
   }
   if (c->copy_dst_open) {
      f_close(&c->copy_dst);
      c->copy_dst_open = false;
   }
   if (g_ws_active_copy == c)
      g_ws_active_copy = NULL;
   if (c->dav_put_open) {
      f_close(&c->write_file.dav);
      c->dav_put_open = false;
   }
   free(c->out);
   free(c);
}

static err_t ws_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
   ws_conn_t *c;
   (void)arg;

   if (err != ERR_OK || newpcb == NULL)
      return ERR_VAL;

   c = calloc(1u, sizeof *c);
   if (c == NULL) {
      tcp_abort(newpcb);
      return ERR_ABRT;
   }

   c->pcb = newpcb;
   c->state = CONN_RECV_HEADER;

   tcp_arg(newpcb, c);
   tcp_recv(newpcb, ws_recv);
   tcp_sent(newpcb, ws_sent);
   tcp_poll(newpcb, ws_poll, WS_POLL_INTERVAL);
   tcp_err(newpcb, ws_err);
   return ERR_OK;
}

/* ------------------------------------------------------------------ */
/* Public interface                                                    */
/* ------------------------------------------------------------------ */

/* Refresh the SD-card free-space cache.  f_getfree can take hundreds
   of ms on a slow / fragmented card so it must not run from a TCP
   callback; doing it here from the cooperative poll keeps /status
   responsive without taking the listener offline.

   We compute both the free byte count and the total byte count.
   FatFs's fs->n_fatent is the total number of FAT entries; the
   usable cluster count is n_fatent - 2 (the first two entries are
   reserved).  Multiplying by fs->csize (sectors-per-cluster) and
   the sector size (FF_MAX_SS, normally 512 on SD) gives the byte
   counts that WebDAV PROPFIND reports as quota-available-bytes /
   quota-used-bytes. */
static void webserver_refresh_sd_free(void)
{
   uint32_t now = RPI_GetSystemTime();
   DWORD    nclst = 0u;
   FATFS   *fs = NULL;

   if (g_ws_sd_free_valid
       && (now - g_ws_sd_free_age_us) < WS_FREE_REFRESH_US)
      return;

   g_ws_sd_free_age_us = now;
   /* FatFs treats the path argument as a logical-drive prefix, not a
      filesystem path - "" picks the default drive and is what the
      MTP backend uses too.  Passing "/" works on this build because
      FF_FS_RPATH is enabled, but "" is the canonical form. */
   if (f_getfree("", &nclst, &fs) == FR_OK && fs != NULL) {
      /* FF_MAX_SS is the FatFs sector-size limit; on SD it is 512.
         Use a literal 512u here to avoid pulling in FF_MAX_SS from
         the ffconf - SD cards do not use anything else in practice
         and Windows shows the size in 1 KB / 1 MB units anyway. */
      uint64_t sector_bytes  = 512u;
      uint64_t cluster_bytes = (uint64_t)fs->csize * sector_bytes;
      uint64_t total_clst    = (uint64_t)(fs->n_fatent - 2u);
      g_ws_sd_total_bytes = total_clst * cluster_bytes;
      g_ws_sd_free_bytes  = (uint64_t)nclst * cluster_bytes;
      g_ws_sd_free_mb     = (uint32_t)(g_ws_sd_free_bytes / (1024u * 1024u));
      g_ws_sd_free_valid  = true;
   } else {
      g_ws_sd_free_valid = false;
   }
}


/* Poll hook: once a reboot has been requested via POST /reboot and the
   short grace period has elapsed - long enough for the response page to
   have been delivered - restart the Pi.  reboot_now() does not return. */
void webserver_poll(void)
{
   if (g_ws_reboot_pending
       && (RPI_GetSystemTime() - g_ws_reboot_at) >= WS_REBOOT_DELAY_US) {
      reboot_now();
   }

   /* Advance the active WebDAV COPY by one chunk.  Only one COPY is
      ever in flight at a time (route_dav_move_or_copy enforces this),
      so the bookkeeping is a single pointer.  ws_copy_step itself
      clears g_ws_active_copy when the transfer completes or fails. */
   if (g_ws_active_copy != NULL)
      ws_copy_step(g_ws_active_copy);

   if (g_ws_ready)
      webserver_refresh_sd_free();
}

void webserver_init(void)
{
   const wifi_config_t *config = wifi_get_config();
   struct tcp_pcb      *listener;
   uint16_t             port;

   g_ws_ready = false;
   ws_set_error(NULL);

   if (g_ws_listener != NULL) {
      tcp_arg(g_ws_listener, NULL);
      tcp_accept(g_ws_listener, NULL);
      tcp_close(g_ws_listener);
      g_ws_listener = NULL;
   }

   port = (config != NULL && config->http_port != 0u)
          ? config->http_port : 80u;

   listener = tcp_new_ip_type(IPADDR_TYPE_V4);
   if (listener == NULL) {
      ws_set_error("could not allocate the HTTP listener");
      return;
   }

   if (tcp_bind(listener, IP_ADDR_ANY, port) != ERR_OK) {
      tcp_close(listener);
      ws_set_error("could not bind the HTTP port");
      return;
   }

   g_ws_listener = tcp_listen_with_backlog(listener, 4u);
   if (g_ws_listener == NULL) {
      tcp_close(listener);
      ws_set_error("could not listen on the HTTP port");
      return;
   }

   tcp_arg(g_ws_listener, NULL);
   tcp_accept(g_ws_listener, ws_accept);

   /* No poll registration: webserver_poll is called from
      wifi_dispatch_poll in wifi.c so the whole WiFi stack costs a
      single slot in the main Pi1MHz poll table. */

   g_ws_ready = true;
   /* Pre-warm the SD-card free-space cache so the very first PROPFIND
      a client makes already carries quota-available-bytes /
      quota-used-bytes properties.  Without this, Windows Explorer
      receives a no-quota response on its first request and falls
      back to a fabricated default (the "86.8 GB free of 929 GB"
      report) which then sticks in the Explorer UI even after later
      PROPFINDs do include the real numbers.  f_getfree may take
      hundreds of ms on a slow / fragmented card but this only runs
      once at server bring-up; subsequent refreshes are gated by the
      WS_FREE_REFRESH_US TTL in webserver_poll. */
   webserver_refresh_sd_free();
   wifi_note_http_ready();
}

bool webserver_is_ready(void)
{
   return g_ws_ready;
}

const char *webserver_last_error(void)
{
   return g_ws_error;
}
