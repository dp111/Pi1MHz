/* Minimal HTTP file-browser webserver for Pi1MHz.
 *
 * Built directly on the lwIP raw/callback TCP API (NO_SYS=1).  Serves a
 * home page, a status page and a file browser that can list folders on
 * the SD card, download files and upload files (multipart/form-data).
 *
 * One request per connection (HTTP/1.1 with "Connection: close").
 */

#include "webserver.h"

#include "wifi.h"
#include "wifi_lwip.h"
#include "sdio.h"
#include "framebuffer_export.h"
#include "../BeebSCSI/fatfs/ff.h"
#include "../rpi/screen.h"
#include "../rpi/exceptions.h"
#include "../rpi/info.h"
#include "../rpi/systimer.h"
#include "../Pi1MHz.h"

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

/* ------------------------------------------------------------------ */
/* Connection model                                                    */
/* ------------------------------------------------------------------ */

typedef enum {
   CONN_RECV_HEADER = 0,   /* accumulating the HTTP request headers     */
   CONN_RECV_UPLOAD,       /* streaming a multipart body to the SD card */
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

typedef struct {
   struct tcp_pcb *pcb;
   conn_state_t    state;
   uint8_t         poll_count;

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

   /* output accounting (for the close decision) */
   uint32_t bytes_queued;
   uint32_t bytes_acked;
   bool     producing_done;

   /* upload */
   upload_state_t up_state;
   bool     up_complete;
   bool     up_file_open;
   FIL      up_file;
   char     up_delim[WS_BOUNDARY_MAX + 8u];   /* "\r\n--" + boundary    */
   size_t   up_delim_len;
   char     up_head[WS_UPLOAD_HEAD_MAX + 1u];
   size_t   up_head_len;
   uint8_t  up_tail[WS_BOUNDARY_MAX + 8u];     /* held-back bytes        */
   size_t   up_tail_len;
   char     up_dir[WS_PATH_MAX];
   char     up_name[FF_LFN_BUF + 1];
   uint32_t up_bytes_written;
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
/* Cached SD free-space.  f_getfree walks the FAT and can stall a
   slow / fragmented card for hundreds of ms - too long for a TCP
   callback to hold the cooperative poll loop.  webserver_poll
   refreshes this in the background and route_status reads it. */
static bool            g_ws_sd_free_valid;
static uint32_t        g_ws_sd_free_mb;
static uint32_t        g_ws_sd_free_age_us;

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

static void ws_url_decode(const char *src, char *dst, size_t dstsz)
{
   size_t o = 0u;
   while (*src != '\0' && o + 1u < dstsz) {
      if (*src == '%' && src[1] != '\0' && src[2] != '\0') {
         int hi = ws_hexval(src[1]);
         int lo = ws_hexval(src[2]);
         if (hi >= 0 && lo >= 0) {
            dst[o++] = (char)((hi << 4) | lo);
            src += 3;
            continue;
         }
      }
      dst[o++] = *src++;
   }
   dst[o] = '\0';
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
/* Forward declarations                                                */
/* ------------------------------------------------------------------ */

/* conn_close / conn_pump return true if the connection was torn down
   via tcp_abort() rather than tcp_close().  lwIP's raw-API contract
   requires any callback that calls tcp_abort to return ERR_ABRT, so
   ws_sent / ws_poll propagate this up. */
static bool conn_close(ws_conn_t *c, bool abort_conn);
static bool conn_pump(ws_conn_t *c);
static bool conn_consume(ws_conn_t *c, const uint8_t *data, size_t len);
static bool process_request(ws_conn_t *c, int body_at);
static bool upload_consume(ws_conn_t *c, const uint8_t *data, size_t len);

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
      f_close(&c->up_file);
      c->up_file_open = false;
   }

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

   /* file body */
   if (err == ERR_OK && c->state == CONN_SEND_FILE
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
      one long burst inside a single callback. */
   if (err == ERR_OK && c->state == CONN_SEND_FB
       && (c->out == NULL || c->out_sent >= c->out_len)) {
      uint32_t row_padded = (c->fb_info.width * 3u + 3u) & ~3u;

      while (1) {
         u16_t  avail;
         size_t want;

         if (c->dl_buf_sent >= c->dl_buf_len) {
            size_t filled = 0u;

            if (c->fb_row >= c->fb_info.height)
               break;                      /* every row generated */
            while (c->fb_row < c->fb_info.height
                   && filled + row_padded <= sizeof c->dl_buf) {
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
      bool file_done = (c->state != CONN_SEND_FILE)
                    || ((c->dl_buf_sent >= c->dl_buf_len)
                        && (c->dl_remaining == 0u || c->dl_eof));
      bool fb_done = (c->state != CONN_SEND_FB)
                    || ((c->dl_buf_sent >= c->dl_buf_len)
                        && c->fb_row >= c->fb_info.height);
      c->producing_done = mem_done && file_done && fb_done;
   }

   if (c->producing_done && c->bytes_acked >= c->bytes_queued)
      return conn_close(c, false);

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
             "Connection: close\r\n"
             "\r\n",
             status, stext, (unsigned long)body->len);
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

static bool ws_error(ws_conn_t *c, int status, const char *stext,
                     const char *msg)
{
   ws_strbuf_t b;
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
      "<p><a href=\"/reboot\">Reboot the Pi &rarr;</a></p>"
      "</div>");
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

   if (rpi_get_board_mac(mac)) {
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
                 "Connection: close\r\n"
                 "\r\n",
                 (unsigned long)total);
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
   sb_printf(&h,
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: application/octet-stream\r\n"
             "Content-Length: %lu\r\n"
             "Content-Disposition: attachment; filename=\"%s\"\r\n"
             "Connection: close\r\n"
             "\r\n",
             (unsigned long)size, dname);
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

   ws_url_decode(rawpath + 6, decoded, sizeof decoded);   /* skip "/files" */
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
      f_close(&c->up_file);
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
   if (f_write(&c->up_file, data, (UINT)len, &bw) != FR_OK || bw != len)
      return upload_fail(c, "Writing to the SD card failed "
                            "(the card may be full or write-protected).");
   c->up_bytes_written += (uint32_t)len;
   return true;
}

static bool upload_finish(ws_conn_t *c)
{
   ws_strbuf_t b;

   if (c->up_file_open) {
      FRESULT fr = f_close(&c->up_file);
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

   if (f_open(&c->up_file, full, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
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
      size_t  maxslice = WS_UPLOAD_WORK - hold;
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

   ws_url_decode(rawpath + 6, decoded, sizeof decoded);   /* skip "/files" */
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
/* Request dispatch                                                    */
/* ------------------------------------------------------------------ */

static bool process_request(ws_conn_t *c, int body_at)
{
   char  method[8];
   char  rawpath[WS_PATH_MAX];
   char *query;

   if (!ws_parse_request_line(c->reqhdr, method, sizeof method,
                              rawpath, sizeof rawpath))
      return ws_error(c, 400, "Bad Request", "The request was malformed.");

   query = strchr(rawpath, '?');
   if (query != NULL)
      *query = '\0';

   if (strcmp(method, "GET") == 0) {
      if (strcmp(rawpath, "/") == 0)
         return route_home(c);
      if (strcmp(rawpath, "/status") == 0)
         return route_status(c);
      if (strcmp(rawpath, "/framebuffer") == 0)
         return route_framebuffer(c);
      if (strcmp(rawpath, "/framebuffer.bmp") == 0)
         return route_framebuffer_bmp(c);
      if (strcmp(rawpath, "/reboot") == 0)
         return route_reboot_confirm(c);
      if (strcmp(rawpath, "/files") == 0 || ws_prefix("/files/", rawpath))
         return route_files_get(c, rawpath);
      return ws_error(c, 404, "Not Found", "No such page.");
   }

   if (strcmp(method, "POST") == 0) {
      if (strcmp(rawpath, "/reboot") == 0)
         return route_reboot_do(c);
      if (strcmp(rawpath, "/files") == 0 || ws_prefix("/files/", rawpath))
         return route_upload(c, rawpath, body_at);
      return ws_error(c, 404, "Not Found", "No such page.");
   }

   return ws_error(c, 405, "Method Not Allowed",
                   "Only GET and POST are supported.");
}

static bool conn_consume(ws_conn_t *c, const uint8_t *data, size_t len)
{
   size_t pos = 0u;

   while (pos < len) {
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
      } else {
         /* CONN_SEND_*: ignore any further request bytes */
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
   struct pbuf *q;
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
      f_close(&c->up_file);
      c->up_file_open = false;
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
   responsive without taking the listener offline. */
static void webserver_refresh_sd_free(void)
{
   uint32_t now = RPI_GetSystemTime();
   DWORD    nclst = 0u;
   FATFS   *fs = NULL;

   if (g_ws_sd_free_valid
       && (now - g_ws_sd_free_age_us) < WS_FREE_REFRESH_US)
      return;

   g_ws_sd_free_age_us = now;
   if (f_getfree("/", &nclst, &fs) == FR_OK && fs != NULL) {
      uint64_t free_sect = (uint64_t)nclst * (uint64_t)fs->csize;
      g_ws_sd_free_mb = (uint32_t)(free_sect / 2048u);
      g_ws_sd_free_valid = true;
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
