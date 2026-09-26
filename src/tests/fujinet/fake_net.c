/* fake_net.c - net_service's C API (net_capi_*) for the host tests of the
   network device.

   Hosts: "ok.test" opens after fake_net_open_delay() "not yet"s and serves
   the body set by fake_net_body(); "dns.fail" fails to resolve.  A URL with
   no host is a parameter error at once, as net_url_parse makes it.  Reads
   deliver at most fake_net_chunk() bytes, none while fake_net_stall() lasts,
   and NET_EOF once the body is gone.  Writes take at most fake_net_accept()
   bytes a call (0: all of it) into a sink. */

#include <string.h>

#include "../../net_service.h"
#include "fake_net.h"

#define H0 NET_BEEB_HANDLES

static struct {
   bool taken, open;
   int opening;           /* "not yet"s left */
   bool fail;
   uint32_t rd;
} H[NET_MAX_HANDLES];

static bool s_enabled = true;
static int s_delay;
static uint8_t s_body[8192];
static uint32_t s_body_len, s_chunk = 0xFFFFFFFFu, s_accept;
static int s_stall;
static bool s_wblock;
static fake_net_open_t s_last;
static uint8_t s_sink[8192];
static uint32_t s_sunk;
static int s_opens;

void fake_net_reset(void)
{
   memset(H, 0, sizeof H);
   s_enabled = true;
   s_delay = 0;
   s_body_len = 0;
   s_chunk = 0xFFFFFFFFu;
   s_accept = 0;
   s_stall = 0;
   s_wblock = false;
   s_sunk = 0;
   s_opens = 0;
   memset(&s_last, 0, sizeof s_last);
}

void fake_net_enable(bool on) { s_enabled = on; }
void fake_net_open_delay(int calls) { s_delay = calls; }
void fake_net_body(const void *d, uint32_t n) { memcpy(s_body, d, n); s_body_len = n; }
void fake_net_chunk(uint32_t n) { s_chunk = n; }
void fake_net_stall(int reads) { s_stall = reads; }
void fake_net_accept(uint32_t n) { s_accept = n; }
void fake_net_write_block(bool on) { s_wblock = on; }
const fake_net_open_t *fake_net_last_open(void) { return &s_last; }
const uint8_t *fake_net_sink(uint32_t *n) { *n = s_sunk; return s_sink; }
int fake_net_opens(void) { return s_opens; }

int fake_net_handles_taken(void)
{
   int n = 0;
   for (unsigned int i = H0; i < NET_MAX_HANDLES; i++) n += H[i].taken;
   return n;
}

static bool valid(int h) { return h >= (int)H0 && h < (int)NET_MAX_HANDLES && H[h].taken; }

bool net_capi_enabled(void) { return s_enabled; }

int net_capi_alloc(void)
{
   for (unsigned int i = H0; i < NET_MAX_HANDLES; i++)
      if (!H[i].taken) {
         memset(&H[i], 0, sizeof H[i]);
         H[i].taken = true;
         return (int)i;
      }
   return -1;
}

uint8_t net_capi_open(int h, const char *url, uint8_t mode, const net_http_opts_t *o)
{
   if (!s_enabled) return NET_ERR_DISABLED;
   if (!valid(h) || !url) return NET_ERR_PARAM;
   if (H[h].open) return NET_OK;
   if (H[h].fail) return NET_ERR_DNS;
   const char *host = strstr(url, "://");
   if (!host || !host[3] || host[3] == '/') return NET_ERR_PARAM;
   host += 3;
   if (H[h].opening == 0 && !H[h].rd) {           /* first call for this open */
      H[h].opening = s_delay + 1;
      H[h].rd = 1;                                 /* marks "started" */
      s_opens++;
      snprintf(s_last.url, sizeof s_last.url, "%s", url);
      s_last.mode = mode;
      s_last.method = o ? o->method : 0;
      s_last.body_len = o ? o->body_len : 0;
      snprintf(s_last.ctype, sizeof s_last.ctype, "%s",
               o && o->content_type ? o->content_type : "");
   }
   if (--H[h].opening > 0) return NET_PENDING;
   if (strncmp(host, "dns.fail", 8) == 0) { H[h].fail = true; return NET_ERR_DNS; }
   H[h].open = true;
   H[h].rd = 0;
   return NET_OK;
}

uint8_t net_capi_read(int h, uint8_t *dst, uint32_t max, uint32_t *got)
{
   *got = 0;
   if (!valid(h) || !dst) return NET_ERR_PARAM;
   if (!H[h].open) return NET_OK;
   if (H[h].rd >= s_body_len) return NET_EOF;
   if (max && s_stall > 0) { s_stall--; return NET_OK; }
   uint32_t n = s_body_len - H[h].rd;
   if (n > max) n = max;
   if (n > s_chunk) n = s_chunk;
   memcpy(dst, s_body + H[h].rd, n);
   H[h].rd += n;
   *got = n;
   return NET_OK;
}

uint8_t net_capi_write(int h, const uint8_t *src, uint32_t len, uint32_t *done)
{
   *done = 0;
   if (!valid(h) || !src) return NET_ERR_PARAM;
   if (!H[h].open) return NET_ERR_NOTOPEN;
   if (s_wblock) return NET_OK;
   uint32_t n = s_accept && len > s_accept ? s_accept : len;
   if (s_sunk + n > sizeof s_sink) return NET_ERR_CONN;
   memcpy(s_sink + s_sunk, src, n);
   s_sunk += n;
   *done = n;
   return NET_OK;
}

void net_capi_close(int h)
{
   if (valid(h)) H[h].taken = false;
}

uint16_t net_capi_http_code(int h) { return valid(h) && H[h].open ? 200u : 0u; }
