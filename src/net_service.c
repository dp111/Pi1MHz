/*
  The IP/net service - raw TCP client sockets + DNS on the &FCA6 services
  port.  See net_service.h for the command-block ABI.

  Structure mirrors the AUN service: the FRED write handler
  (net_service_command) runs in FIQ and only latches the request in a
  one-slot mailbox; net_service_poll() executes it on the main loop, where
  every lwIP call and every lwIP callback runs.  lwIP is NEVER called from
  FIQ (the cardinal rule - see the AUN work and never-error-to-a-mounted-
  ADFS).  Async commands (connect/dns/close) return NET_PENDING; the Beeb
  re-issues the same command to poll, exactly like the AUN TX/TX_POLL idiom.

  TCP is written against lwIP's altcp API (altcp_*), which at LWIP_ALTCP==0
  compiles to plain tcp_* (altcp.h provides the macros) - so Stage 4 can add
  TLS by flipping one lwipopts flag rather than refactoring every callback.
*/

#include <string.h>
#include <stdio.h>

#include "Pi1MHz.h"
#include "ram_emulator.h"          /* DISC_RAM_BASE, DISC_RAM_SIZE, JIM_ram */
#include "services.h"
#include "net_service.h"
#include "net_tnfs.h"
#include "net_telnet.h"
#include "config.h"
#include "rpi/systimer.h"          /* RPI_GetSystemTime64 (ms clock for TNFS) */

#include "wifi/wifi_lwip.h"        /* wifi_lwip_get_context, wifi_lwip_rx_kick */
#include "lwip/altcp.h"
#include "lwip/tcp.h"              /* struct tcp_pcb fields (rcv_wnd clamp)  */
#include "lwip/udp.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "lwip/err.h"

/* Longest hostname accepted from the host, and the bound on the NUL scan. */
#define NET_MAX_HOSTNAME 256u

/* net_handle_t.url_phase - the N: device open state machine. */
#define URL_START      0u
#define URL_RESOLVING  1u
#define URL_CONNECTING 2u
#define URL_READY      3u
#define URL_FAIL       4u

typedef struct {
   uint8_t           state;         /* net_state_t                          */
   uint8_t           type;          /* NET_TYPE_TCP / NET_TYPE_UDP          */
   uint8_t           last_err;      /* NET_* sticky error for status        */
   bool              rx_eof;        /* peer sent FIN                        */
   bool              rx_parked;     /* a pbuf was ERR_MEM-parked            */
   bool              dns_done;      /* a resolve completed, result waiting  */
   bool              dns_ok;        /* that resolve succeeded               */
   bool              is_url;        /* opened via net_url_open (N: device)  */
   uint8_t           url_adapter;   /* NET_URL_* for a URL handle           */
   uint8_t           url_phase;     /* url_phase_t: the open state machine   */
   bool              http_hdr_done; /* HTTP adapter: response headers eaten */
   uint16_t          http_code;     /* HTTP adapter: parsed status code     */
   struct altcp_pcb *tpcb;          /* TCP pcb, NULL once freed by lwIP     */
   struct udp_pcb   *upcb;          /* UDP pcb                              */
   ip_addr_t         remote_ip;
   uint16_t          remote_port;
   uint16_t          bind_port;     /* local port for TCP listen / UDP bind */
   bool              accept_ready;  /* a listener has an accepted conn ready*/
   uint8_t           accept_h;      /* its handle index (when accept_ready) */
   ip_addr_t         dns_ip;        /* dns_gethostbyname target             */
   /* RX ring: single-context (the lwIP recv callbacks and the recv commands
      all run on the main loop), so no locking.  count distinguishes full
      from empty.  For TCP it is a byte stream; for UDP it holds datagram
      records: [4 ip][2 port][2 len][len payload]. */
   uint16_t          rx_head;
   uint16_t          rx_tail;
   uint16_t          rx_count;
   /* TNFS (N:TNFS://) session + one in-flight request, for the retry engine */
   uint8_t           tnfs_phase;    /* TNFS_PH_*                            */
   uint8_t           tnfs_seq;      /* sequence of the outstanding request  */
   uint8_t           tnfs_fd;       /* open file / directory handle         */
   uint8_t           tnfs_is_dir;   /* URL path ended in '/': a directory   */
   uint8_t           tnfs_wr;       /* url_open mode had the write bit      */
   uint8_t           tnfs_retries;  /* resends left on the outstanding req  */
   uint8_t           tnfs_eagain;   /* server-busy (EAGAIN) budget left     */
   uint16_t          tnfs_connid;   /* session id (from MOUNT)              */
   uint16_t          tnfs_retry_ms; /* base resend timeout (from MOUNT)     */
   uint32_t          tnfs_deadline; /* ms: resend/timeout deadline          */
   uint16_t          tnfs_req_len;  /* outstanding request length           */
   uint8_t           tnfs_req[256]; /* buffered request, for resend         */
   telnet_ctx_t      telnet;        /* TELNET: IAC filter state (zeroed = reset) */
} net_handle_t;

/* TNFS session phases (net_handle_t.tnfs_phase) */
#define TNFS_PH_IDLE     0u
#define TNFS_PH_MOUNT    1u   /* MOUNT sent, awaiting reply            */
#define TNFS_PH_OPEN     2u   /* OPEN sent, awaiting reply             */
#define TNFS_PH_READY    3u   /* mounted + file open                   */
#define TNFS_PH_READING  4u   /* READ sent, awaiting reply             */
#define TNFS_PH_WRITING  5u   /* WRITE sent, awaiting reply            */
#define TNFS_REQ_MAX     256u
#define TNFS_PKT_MAX     600u /* largest reply datagram we parse       */
#define TNFS_RETRIES     4u   /* resends before giving up              */
#define TNFS_EAGAIN_MAX  8u   /* server-busy backoffs before giving up */
#define TNFS_TIMEOUT_MS  800u /* fallback resend timeout               */
#define TNFS_READ_CHUNK  512u /* cap a READ so its reply fits a datagram */
#define TNFS_WRITE_CHUNK 240u /* cap a WRITE so req (data + 7 hdr) fits tnfs_req */

static void net_tnfs_send_raw(net_handle_t *h, const uint8_t *req, uint16_t len);

/* Handle table in BSS (zeroed at boot: all NET_ST_FREE, tpcb NULL - so the
   first reset teardown finds nothing live to abort).  The 8x8 KB RX rings
   are NOINIT to keep them out of the boot-zeroing path; rx_count resets to 0
   on open, so their contents never matter. */
static net_handle_t net_h[NET_MAX_HANDLES];
NOINIT_SECTION static uint8_t net_rx_ring[NET_MAX_HANDLES][NET_RX_RING_SIZE];

static uint8_t  net_source;         /* nIRQ source id (emulator instance)   */
static bool     net_enabled;        /* net_enable=1 in Pi1MHz.cfg           */
static bool     net_irq_armed;      /* nIRQ opt-in (NET_CMD_IRQ); see below  */

/* One-slot command mailbox latched in FIQ, drained by the poll. */
static volatile bool     net_pending;
static volatile uint32_t net_pending_cp;
static volatile uint32_t net_pending_addr;
static volatile uint8_t  net_pending_data;
static volatile bool     net_reset_pending;

/* ---- little-endian JIM accessors (byte-wise: fields are not all aligned) - */
static inline uint8_t jim_rd8(uint32_t off) { return Pi1MHz->JIM_ram[off]; }
static inline uint32_t jim_rd24(uint32_t off)
{
   const uint8_t *p = &Pi1MHz->JIM_ram[off];
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}
static inline uint32_t jim_rd32(uint32_t off)
{
   const uint8_t *p = &Pi1MHz->JIM_ram[off];
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void jim_wr24(uint32_t off, uint32_t v)
{
   uint8_t *p = &Pi1MHz->JIM_ram[off];
   p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16);
}

/* ---- untrusted-input bounds checks (clones of the FAT service's) --------- */
/* [offset, offset+length) inside the disc RAM region; offset is relative to
   DISC_RAM_BASE.  The subtraction cannot underflow (offset bounded first). */
static bool net_buffer_ok(uint32_t offset, uint32_t length)
{
   if (offset > DISC_RAM_SIZE)
      return false;
   return length <= (DISC_RAM_SIZE - offset);
}
/* A NUL terminator exists within NET_MAX_HOSTNAME bytes of the absolute JIM
   offset `start`, and before the end of the disc RAM region. */
static bool net_string_ok(uint32_t start)
{
   uint32_t limit = start + NET_MAX_HOSTNAME;
   if (limit > (uint32_t)(DISC_RAM_BASE + DISC_RAM_SIZE))
      limit = (uint32_t)(DISC_RAM_BASE + DISC_RAM_SIZE);
   for (uint32_t i = start; i < limit; i++)
      if (Pi1MHz->JIM_ram[i] == 0)
         return true;
   return false;
}

/* ---- RX ring ------------------------------------------------------------- */
static inline uint16_t ring_free(const net_handle_t *h)
{
   return (uint16_t)(NET_RX_RING_SIZE - h->rx_count);
}
/* Copy a whole pbuf chain into the ring.  Caller guarantees it fits. */
static void ring_put_pbuf(net_handle_t *h, const struct pbuf *p)
{
   uint8_t *ring = net_rx_ring[h - net_h];
   for (const struct pbuf *q = p; q != NULL; q = q->next) {
      const uint8_t *src = (const uint8_t *)q->payload;
      uint16_t n = q->len;
      for (uint16_t i = 0; i < n; i++) {
         ring[h->rx_head] = src[i];
         h->rx_head = (uint16_t)((h->rx_head + 1u) & (NET_RX_RING_SIZE - 1u));
      }
      h->rx_count = (uint16_t)(h->rx_count + n);
   }
}
/* Drain up to max bytes from the ring into a JIM destination. */
static uint32_t ring_get(net_handle_t *h, uint32_t jim_dst, uint32_t max)
{
   const uint8_t *ring = net_rx_ring[h - net_h];
   uint32_t n = (max < h->rx_count) ? max : h->rx_count;
   for (uint32_t i = 0; i < n; i++) {
      Pi1MHz->JIM_ram[jim_dst + i] = ring[h->rx_tail];
      h->rx_tail = (uint16_t)((h->rx_tail + 1u) & (NET_RX_RING_SIZE - 1u));
   }
   h->rx_count = (uint16_t)(h->rx_count - n);
   return n;
}
/* Append raw bytes to the ring (UDP record framing).  Caller ensures fit. */
static void ring_put_mem(net_handle_t *h, const uint8_t *src, uint16_t len)
{
   uint8_t *ring = net_rx_ring[h - net_h];
   for (uint16_t i = 0; i < len; i++) {
      ring[h->rx_head] = src[i];
      h->rx_head = (uint16_t)((h->rx_head + 1u) & (NET_RX_RING_SIZE - 1u));
   }
   h->rx_count = (uint16_t)(h->rx_count + len);
}
/* Read len bytes from the ring into a local buffer (UDP record header). */
static void ring_get_mem(net_handle_t *h, uint8_t *dst, uint16_t len)
{
   const uint8_t *ring = net_rx_ring[h - net_h];
   for (uint16_t i = 0; i < len; i++) {
      dst[i] = ring[h->rx_tail];
      h->rx_tail = (uint16_t)((h->rx_tail + 1u) & (NET_RX_RING_SIZE - 1u));
   }
   h->rx_count = (uint16_t)(h->rx_count - len);
}

/* ---- IPv4 <-> wire (network-order octets [b0,b1,b2,b3]) ------------------ */
static void net_ip_from_wire(ip_addr_t *ip, uint32_t off)
{
   const uint8_t *p = &Pi1MHz->JIM_ram[off];
   IP_ADDR4(ip, p[0], p[1], p[2], p[3]);
}
static void net_ip_to_wire(const ip_addr_t *ip, uint32_t off)
{
   uint32_t u = ip4_addr_get_u32(ip_2_ip4(ip));   /* network byte order */
   uint8_t *p = &Pi1MHz->JIM_ram[off];
   p[0] = (uint8_t)u; p[1] = (uint8_t)(u >> 8);
   p[2] = (uint8_t)(u >> 16); p[3] = (uint8_t)(u >> 24);
}

/* ---- lwIP callbacks (all main-loop context, arg = &net_h[i]) ------------- */

static void net_handle_reset(net_handle_t *h)
{
   memset(h, 0, sizeof *h);        /* NET_ST_FREE, tpcb NULL, ring empty */
}

/* Detach every callback and drop the pcb without a use-after-free, then mark
   the handle free.  Mirrors the webserver's conn_close ordering. */
static void net_pcb_release(net_handle_t *h, bool abort_pcb)
{
   struct altcp_pcb *pcb = h->tpcb;
   if (pcb != NULL) {
      h->tpcb = NULL;
      altcp_arg(pcb, NULL);
      altcp_recv(pcb, NULL);
      altcp_sent(pcb, NULL);
      altcp_poll(pcb, NULL, 0);
      altcp_err(pcb, NULL);
      altcp_accept(pcb, NULL);      /* a listener's accept cb too (harmless otherwise) */
      if (abort_pcb)
         altcp_abort(pcb);
      else if (altcp_close(pcb) != ERR_OK)
         altcp_abort(pcb);
   }
   if (h->upcb != NULL) {           /* UDP has no graceful close */
      udp_remove(h->upcb);
      h->upcb = NULL;
   }
}

/* Append an inbound datagram to the ring as [4 ip][2 port][2 len][payload],
   or drop it whole if the record would not fit (UDP has no back-pressure). */
static void net_udp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                         const ip_addr_t *addr, u16_t port)
{
   net_handle_t *h = (net_handle_t *)arg;
   (void)pcb;
   if (h == NULL || p == NULL) {
      if (p != NULL) pbuf_free(p);
      return;
   }
   if ((uint32_t)p->tot_len + 8u <= ring_free(h)) {
      uint8_t hdr[8];
      uint32_t u = ip4_addr_get_u32(ip_2_ip4(addr));
      hdr[0] = (uint8_t)u; hdr[1] = (uint8_t)(u >> 8);
      hdr[2] = (uint8_t)(u >> 16); hdr[3] = (uint8_t)(u >> 24);
      hdr[4] = (uint8_t)port; hdr[5] = (uint8_t)(port >> 8);
      hdr[6] = (uint8_t)p->tot_len; hdr[7] = (uint8_t)(p->tot_len >> 8);
      ring_put_mem(h, hdr, 8u);
      ring_put_pbuf(h, p);
   }
   pbuf_free(p);
}

static err_t net_tcp_recv(void *arg, struct altcp_pcb *pcb, struct pbuf *p,
                          err_t err)
{
   net_handle_t *h = (net_handle_t *)arg;
   if (h == NULL) {
      if (p != NULL) pbuf_free(p);
      return ERR_OK;
   }
   if (err != ERR_OK) {
      if (p != NULL) pbuf_free(p);
      h->last_err = NET_ERR_CONN;
      h->state = NET_ST_ERROR;
      return ERR_OK;
   }
   if (p == NULL) {                 /* peer FIN */
      h->rx_eof = true;
      return ERR_OK;
   }
   /* Drop-free back-pressure: only accept the segment if the whole chain
      fits, else return ERR_MEM WITHOUT freeing and WITHOUT acking - lwIP
      parks the pbuf and re-presents it once the Beeb has drained the ring. */
   if (p->tot_len > ring_free(h)) {
      h->rx_parked = true;
      return ERR_MEM;
   }
   if (h->is_url && h->url_adapter == NET_URL_TELNET) {
      /* Run the segment through the TELNET IAC filter: clean text to the ring,
         option-negotiation replies straight back to the server.  Filtered
         output is <= input, so it always fits the ring we just checked. */
      uint16_t off = 0;
      while (off < p->tot_len) {
         uint8_t chunk[256];
         uint8_t clean[256];
         uint8_t rep[300];         /* a 256-byte chunk yields at most ~255 reply bytes */
         size_t  cl, rl;
         uint16_t want = (uint16_t)(p->tot_len - off);
         if (want > sizeof chunk) want = (uint16_t)sizeof chunk;
         want = pbuf_copy_partial(p, chunk, want, off);
         if (want == 0u) break;
         telnet_filter(&h->telnet, chunk, want, clean, sizeof clean, &cl,
                       rep, sizeof rep, &rl);
         if (cl != 0u) ring_put_mem(h, clean, (uint16_t)cl);
         if (rl != 0u && h->tpcb != NULL             /* flush this chunk's replies */
             && altcp_write(h->tpcb, rep, (u16_t)rl, TCP_WRITE_FLAG_COPY) == ERR_OK)
            altcp_output(h->tpcb);
         off = (uint16_t)(off + want);
      }
      h->rx_parked = false;
      altcp_recved(pcb, p->tot_len);
      pbuf_free(p);
      return ERR_OK;
   }
   ring_put_pbuf(h, p);
   h->rx_parked = false;
   altcp_recved(pcb, p->tot_len);
   pbuf_free(p);
   return ERR_OK;
}

static err_t net_tcp_sent(void *arg, struct altcp_pcb *pcb, u16_t len)
{
   (void)arg; (void)pcb; (void)len;
   return ERR_OK;                   /* the Beeb re-issues send to make progress */
}

static err_t net_tcp_poll(void *arg, struct altcp_pcb *pcb)
{
   (void)arg; (void)pcb;
   return ERR_OK;
}

static void net_tcp_err(void *arg, err_t err)
{
   net_handle_t *h = (net_handle_t *)arg;
   (void)err;
   if (h == NULL)
      return;
   /* lwIP has already freed the pcb - NULL it, never touch it again. */
   h->tpcb = NULL;
   h->last_err = NET_ERR_CONN;
   h->state = NET_ST_ERROR;
}

/* Attach this service's callbacks to a TCP pcb - shared by an outbound
   connect and an inbound accept. */
static void net_tcp_bind_callbacks(net_handle_t *h, struct altcp_pcb *pcb)
{
   /* Deliberately NOT narrowing rcv_wnd to the ring size. It never worked
    * on the connect path - tcp_connect() reassigns rcv_wnd/rcv_ann_wnd from
    * TCP_WND unconditionally (lwip/src/core/tcp.c) - and where it DID stick,
    * on an accepted pcb, it was actively harmful: tcp_close_shutdown() sends
    * an RST instead of a FIN whenever rcv_wnd != TCP_WND_MAX(pcb), so every
    * close of an inbound connection reset the peer and discarded whatever
    * was still queued. The ERR_MEM park in net_tcp_recv() is the real
    * back-pressure: lwIP re-presents a refused pbuf, so nothing is lost. */
   altcp_arg (pcb, h);
   altcp_recv(pcb, net_tcp_recv);
   altcp_sent(pcb, net_tcp_sent);
   altcp_poll(pcb, net_tcp_poll, 4u);
   altcp_err (pcb, net_tcp_err);
}

/* Inbound connection on a listening handle: claim a FREE handle for it and
   record it in the listener's one-deep backlog for the Beeb to collect. */
static err_t net_tcp_accept(void *arg, struct altcp_pcb *newpcb, err_t err)
{
   net_handle_t *lh = (net_handle_t *)arg;   /* the listening handle */
   net_handle_t *nh = NULL;
   unsigned int  idx = 0;

   if (lh == NULL || err != ERR_OK || newpcb == NULL)
      return ERR_VAL;
   if (lh->accept_ready)                      /* backlog full: refuse */
      { altcp_abort(newpcb); return ERR_ABRT; }
   for (unsigned int i = 0; i < NET_MAX_HANDLES; i++)
      if (net_h[i].state == NET_ST_FREE) { nh = &net_h[i]; idx = i; break; }
   if (nh == NULL)                            /* no free handle: refuse */
      { altcp_abort(newpcb); return ERR_ABRT; }

   net_handle_reset(nh);
   nh->type  = NET_TYPE_TCP;
   nh->tpcb  = newpcb;
   nh->state = NET_ST_CONNECTED;
   nh->remote_ip   = newpcb->remote_ip;      /* peer, for URL_STATUS (see rcv_wnd */
   nh->remote_port = newpcb->remote_port;    /* clamp above: altcp_pcb is tcp_pcb) */
   net_tcp_bind_callbacks(nh, newpcb);
   lh->accept_ready = true;
   lh->accept_h     = (uint8_t)idx;
   return ERR_OK;
}

static err_t net_tcp_connected(void *arg, struct altcp_pcb *pcb, err_t err)
{
   net_handle_t *h = (net_handle_t *)arg;
   (void)pcb;
   if (h == NULL)
      return ERR_OK;
   if (err == ERR_OK)
      h->state = NET_ST_CONNECTED;
   else {
      h->tpcb = NULL;              /* connect failure frees the pcb */
      h->last_err = NET_ERR_CONN;
      h->state = NET_ST_ERROR;
   }
   return ERR_OK;
}

static void net_dns_found(const char *name, const ip_addr_t *ipaddr, void *arg)
{
   net_handle_t *h = (net_handle_t *)arg;
   (void)name;
   if (h == NULL)
      return;
   h->dns_done = true;
   if (ipaddr != NULL) {
      h->dns_ip = *ipaddr;
      h->dns_ok = true;
   } else {
      h->dns_ok = false;
   }
   if (h->state == NET_ST_RESOLVING)
      h->state = NET_ST_IDLE;
}

/* ---- N: device (Stage 2): URL parsing ------------------------------------ */

typedef struct { uint8_t adapter; uint16_t port; } net_url_t;

/* Case-insensitive compare of s[0..len) against an upper-case NUL literal. */
static bool net_ci_eq(const char *s, size_t len, const char *lit)
{
   size_t i;
   for (i = 0; i < len && lit[i] != '\0'; i++) {
      char c = s[i];
      if (c >= 'a' && c <= 'z') c = (char)(c - 32);
      if (c != lit[i]) return false;
   }
   return i == len && lit[i] == '\0';
}

/* "a.b.c.d" -> ip_addr_t; false if not a dotted quad. */
static bool net_parse_dotted(const char *s, ip_addr_t *ip)
{
   unsigned int oct[4], n = 0, v = 0, digits = 0;
   for (; ; s++) {
      if (*s >= '0' && *s <= '9') {
         v = v * 10u + (unsigned int)(*s - '0');
         if (v > 255u || ++digits > 3u) return false;
      } else if (*s == '.' || *s == '\0') {
         if (digits == 0u || n >= 4u) return false;
         oct[n++] = v; v = 0; digits = 0;
         if (*s == '\0') break;
      } else {
         return false;
      }
   }
   if (n != 4u) return false;
   IP_ADDR4(ip, oct[0], oct[1], oct[2], oct[3]);
   return true;
}

/* Parse "scheme://host[:port][/path]" into adapter/port + copies of host and
   path (NUL-terminated; path defaults to "/").  false on malformed/overflow. */
static bool net_url_parse(const char *url, net_url_t *out,
                          char *host, size_t host_sz, char *path, size_t path_sz)
{
   const char *sep = strstr(url, "://");
   const char *h, *e;
   size_t i;
   if (sep == NULL) return false;
   {
      size_t sl = (size_t)(sep - url);
      if      (net_ci_eq(url, sl, "HTTP")) { out->adapter = NET_URL_HTTP; out->port = 80u; }
      else if (net_ci_eq(url, sl, "TCP"))  { out->adapter = NET_URL_TCP;  out->port = 0u;  }
      else if (net_ci_eq(url, sl, "UDP"))  { out->adapter = NET_URL_UDP;  out->port = 0u;  }
      else if (net_ci_eq(url, sl, "TNFS")) { out->adapter = NET_URL_TNFS; out->port = (uint16_t)TNFS_PORT; }
      else if (net_ci_eq(url, sl, "TELNET")) { out->adapter = NET_URL_TELNET; out->port = (uint16_t)TELNET_PORT; }
      else return false;
   }
   h = sep + 3;
   for (e = h; *e != '\0' && *e != ':' && *e != '/'; e++) { }
   if (e == h || (size_t)(e - h) >= host_sz) return false;
   for (i = 0; h + i < e; i++) {
      if ((unsigned char)h[i] < 0x20u || (unsigned char)h[i] == 0x7Fu)
         return false;                          /* no control chars in host */
      host[i] = h[i];
   }
   host[i] = '\0';

   if (*e == ':') {
      uint32_t port = 0u;
      for (e++; *e >= '0' && *e <= '9'; e++) {
         port = port * 10u + (uint32_t)(*e - '0');
         if (port > 65535u) return false;       /* reject before it wraps */
      }
      if (port == 0u) return false;
      out->port = (uint16_t)port;
   }
   if (out->port == 0u) return false;          /* TCP/UDP need an explicit port */

   if (*e == '/') {
      const char *q;
      if (strlen(e) >= path_sz) return false;
      for (q = e; *q != '\0'; q++)
         if ((unsigned char)*q < 0x20u || (unsigned char)*q == 0x7Fu)
            return false;                        /* no control chars in path */
      strcpy(path, e);
   } else if (*e == '\0') {
      if (path_sz < 2u) return false;
      path[0] = '/'; path[1] = '\0';
   } else {
      return false;
   }
   return true;
}

/* ---- command handlers (main-loop context) -------------------------------- */

static uint8_t do_open(net_handle_t *h, uint32_t cp)
{
   uint8_t type = jim_rd8(cp + 1u);
   if (h->state != NET_ST_FREE)
      return NET_ERR_INUSE;
   if (type != NET_TYPE_TCP && type != NET_TYPE_UDP)
      return NET_ERR_PARAM;
   net_handle_reset(h);
   h->type  = type;
   h->state = NET_ST_IDLE;
   if (type == NET_TYPE_UDP) {
      h->upcb = udp_new();
      if (h->upcb == NULL) {
         net_handle_reset(h);
         return NET_ERR_NOMEM;
      }
      udp_recv(h->upcb, net_udp_recv, h);
   }
   return NET_OK;
}

/* UDP: bind a local port so inbound datagrams reach this handle (+1..2). */
static uint8_t do_bind(net_handle_t *h, uint32_t cp)
{
   uint16_t port = (uint16_t)(jim_rd8(cp + 1u) | (jim_rd8(cp + 2u) << 8));
   if (h->state == NET_ST_FREE)
      return NET_ERR_NOTOPEN;
   h->bind_port = port;              /* TCP: kept for a later listen */
   if (h->type == NET_TYPE_UDP) {
      if (h->upcb == NULL || udp_bind(h->upcb, IP_ANY_TYPE, port) != ERR_OK)
         return NET_ERR_CONN;
   }
   return NET_OK;
}

/* listen (49): start accepting inbound TCP on the bound port.  First call
   opens the listener (NET_PENDING); each later call yields the next accepted
   connection's handle index in [1] (NET_OK), or NET_PENDING while none. */
static uint8_t do_listen(net_handle_t *h, uint32_t cp)
{
   if (h->type != NET_TYPE_TCP || h->state == NET_ST_FREE)
      return NET_ERR_NOTOPEN;

   if (h->state == NET_ST_IDLE) {
      struct altcp_pcb *lp = altcp_new_ip_type(NULL, IPADDR_TYPE_V4);
      if (lp == NULL)
         return NET_ERR_NOMEM;
      if (altcp_bind(lp, IP_ANY_TYPE, h->bind_port) != ERR_OK) {
         altcp_close(lp);
         return NET_ERR_CONN;
      }
      h->tpcb = altcp_listen(lp);   /* consumes lp, returns the listen pcb */
      if (h->tpcb == NULL) {
         altcp_close(lp);
         return NET_ERR_NOMEM;
      }
      altcp_arg(h->tpcb, h);
      altcp_accept(h->tpcb, net_tcp_accept);
      h->state = NET_ST_LISTENING;
      return NET_PENDING;
   }
   if (h->state == NET_ST_LISTENING) {
      if (h->accept_ready) {
         h->accept_ready = false;
         Pi1MHz->JIM_ram[cp + 1u] = h->accept_h;
         return NET_OK;
      }
      return NET_PENDING;
   }
   return NET_ERR_NOTOPEN;
}

/* UDP send: +1..4 IPv4, +5..6 port, +7..9 length, +10..13 JIM source. */
static uint8_t do_udp_sendto(net_handle_t *h, uint32_t cp)
{
   ip_addr_t ip;
   uint16_t  port = (uint16_t)(jim_rd8(cp + 5u) | (jim_rd8(cp + 6u) << 8));
   uint32_t  len    = jim_rd24(cp + 7u);
   uint32_t  jimoff = jim_rd32(cp + 10u);
   struct pbuf *p;

   if (h->type != NET_TYPE_UDP || h->upcb == NULL)
      return NET_ERR_NOTOPEN;
   if (len > 0xFFFFu || !net_buffer_ok(jimoff, len))
      return NET_ERR_PARAM;
   if (!wifi_lwip_get_context()->address_ready)
      return NET_PENDING;

   net_ip_from_wire(&ip, cp + 1u);
   p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
   if (p == NULL)
      return NET_ERR_NOMEM;
   pbuf_take(p, &Pi1MHz->JIM_ram[jimoff + DISC_RAM_BASE], (u16_t)len);
   {
      err_t e = udp_sendto(h->upcb, p, &ip, port);
      pbuf_free(p);
      if (e != ERR_OK)
         return NET_ERR_CONN;
   }
   wifi_lwip_rx_kick();
   jim_wr24(cp + 7u, len);
   return NET_OK;
}

/* UDP receive one datagram: out +1..4 peer IPv4, +5..6 port, +7..9 length,
   payload into the JIM offset at +10..13.  length 0 = nothing waiting. */
static uint8_t do_udp_recvfrom(net_handle_t *h, uint32_t cp)
{
   uint32_t jimoff = jim_rd32(cp + 10u);
   uint32_t maxlen = jim_rd24(cp + 7u);
   uint8_t  hdr[8];
   uint16_t dglen;

   if (h->type != NET_TYPE_UDP)
      return NET_ERR_NOTOPEN;
   if (!net_buffer_ok(jimoff, maxlen))
      return NET_ERR_PARAM;
   if (h->rx_count < 8u) {           /* no complete record */
      jim_wr24(cp + 7u, 0u);
      return NET_OK;
   }
   ring_get_mem(h, hdr, 8u);
   Pi1MHz->JIM_ram[cp + 1u] = hdr[0]; Pi1MHz->JIM_ram[cp + 2u] = hdr[1];
   Pi1MHz->JIM_ram[cp + 3u] = hdr[2]; Pi1MHz->JIM_ram[cp + 4u] = hdr[3];
   Pi1MHz->JIM_ram[cp + 5u] = hdr[4]; Pi1MHz->JIM_ram[cp + 6u] = hdr[5];
   dglen = (uint16_t)(hdr[6] | (hdr[7] << 8));
   {
      uint32_t copy = (dglen < maxlen) ? dglen : maxlen;
      ring_get(h, jimoff + DISC_RAM_BASE, copy);
      /* discard any of the datagram that didn't fit the caller's buffer */
      if (dglen > copy) {
         uint8_t junk[64];
         uint16_t left = (uint16_t)(dglen - copy);
         while (left > 0u) {
            uint16_t n = (left < sizeof junk) ? left : (uint16_t)sizeof junk;
            ring_get_mem(h, junk, n);
            left = (uint16_t)(left - n);
         }
      }
      jim_wr24(cp + 7u, copy);
   }
   return NET_OK;
}

static uint8_t do_dns(net_handle_t *h, uint32_t cp)
{
   if (h->state == NET_ST_FREE)
      return NET_ERR_NOTOPEN;
   if (h->state == NET_ST_RESOLVING)
      return NET_PENDING;
   if (h->dns_done) {               /* a resolve finished */
      h->dns_done = false;
      if (!h->dns_ok)
         return NET_ERR_DNS;
      net_ip_to_wire(&h->dns_ip, cp + 4u);
      return NET_OK;
   }
   if (!net_string_ok(cp + 1u))
      return NET_ERR_PARAM;
   {
      const char *name = (const char *)&Pi1MHz->JIM_ram[cp + 1u];
      err_t e = dns_gethostbyname(name, &h->dns_ip, net_dns_found, h);
      if (e == ERR_OK) {            /* cache hit, resolved synchronously */
         net_ip_to_wire(&h->dns_ip, cp + 4u);
         return NET_OK;
      }
      if (e == ERR_INPROGRESS) {
         h->state = NET_ST_RESOLVING;
         return NET_PENDING;
      }
      return NET_ERR_DNS;
   }
}

/* Create a pcb and start an outbound connect to h->remote_ip:remote_port.
   Returns NET_PENDING (CONNECTING) or an error.  The ERR_MEM park in
   net_tcp_recv() is what keeps RX drop-free (see net_tcp_bind_callbacks). */
static uint8_t net_start_connect(net_handle_t *h)
{
   h->tpcb = altcp_new_ip_type(NULL, IPADDR_TYPE_V4);
   if (h->tpcb == NULL)
      return NET_ERR_NOMEM;
   net_tcp_bind_callbacks(h, h->tpcb);
   if (altcp_connect(h->tpcb, &h->remote_ip, h->remote_port,
                     net_tcp_connected) != ERR_OK) {
      net_pcb_release(h, true);
      h->state = NET_ST_ERROR;
      h->last_err = NET_ERR_CONN;
      return NET_ERR_CONN;
   }
   h->state = NET_ST_CONNECTING;
   return NET_PENDING;
}

static uint8_t do_connect(net_handle_t *h, uint32_t cp)
{
   if (h->type != NET_TYPE_TCP || h->state == NET_ST_FREE)
      return NET_ERR_NOTOPEN;
   if (h->state == NET_ST_CONNECTED)
      return NET_OK;
   if (h->state == NET_ST_ERROR)
      return NET_ERR_CONN;
   if (h->state == NET_ST_CONNECTING)
      return NET_PENDING;
   if (h->state != NET_ST_IDLE)
      return NET_ERR_NOTOPEN;

   if (!wifi_lwip_get_context()->address_ready)
      return NET_PENDING;          /* no IP yet - keep polling */

   net_ip_from_wire(&h->remote_ip, cp + 1u);
   h->remote_port = (uint16_t)(jim_rd8(cp + 5u) | (jim_rd8(cp + 6u) << 8));
   return net_start_connect(h);
}

static uint8_t do_send(net_handle_t *h, uint32_t cp)
{
   uint32_t len    = jim_rd24(cp + 1u);
   uint32_t jimoff = jim_rd32(cp + 4u);
   uint32_t want;
   u16_t    avail;

   if (h->type != NET_TYPE_TCP || h->tpcb == NULL
       || h->state != NET_ST_CONNECTED)
      return NET_ERR_NOTOPEN;
   if (!net_buffer_ok(jimoff, len))
      return NET_ERR_PARAM;

   avail = altcp_sndbuf(h->tpcb);
   want  = len;
   if (want > avail) want = avail;
   if (want == 0u) {                /* send buffer full - retry later */
      jim_wr24(cp + 1u, 0u);
      return NET_OK;
   }
   {
      const void *src = &Pi1MHz->JIM_ram[jimoff + DISC_RAM_BASE];
      err_t e = altcp_write(h->tpcb, src, (u16_t)want, TCP_WRITE_FLAG_COPY);
      if (e == ERR_MEM) {           /* heap shortfall despite sndbuf - retry */
         jim_wr24(cp + 1u, 0u);
         return NET_OK;
      }
      if (e != ERR_OK)
         return NET_ERR_CONN;
      altcp_output(h->tpcb);
      wifi_lwip_rx_kick();          /* a send usually precedes a reply */
   }
   jim_wr24(cp + 1u, want);
   return NET_OK;
}

static uint8_t do_recv(net_handle_t *h, uint32_t cp)
{
   uint32_t max    = jim_rd24(cp + 1u);
   uint32_t jimoff = jim_rd32(cp + 4u);
   uint32_t got;

   if (h->state == NET_ST_FREE)
      return NET_ERR_NOTOPEN;
   if (!net_buffer_ok(jimoff, max))
      return NET_ERR_PARAM;

   got = ring_get(h, jimoff + DISC_RAM_BASE, max);
   jim_wr24(cp + 1u, got);
   /* Once the ring is fully drained, surface the stream end so the Beeb's
      read loop terminates: a clean FIN as EOF, a reset as the error code
      (otherwise a reset with no FIN reads as OK/0 bytes forever). */
   if (got == 0u && h->rx_count == 0u) {
      if (h->rx_eof)                 return NET_EOF;
      if (h->state == NET_ST_ERROR)  return h->last_err ? h->last_err : NET_ERR_CONN;
   }
   return NET_OK;
}

static uint8_t do_recv_avail(net_handle_t *h, uint32_t cp)
{
   if (h->state == NET_ST_FREE)
      return NET_ERR_NOTOPEN;
   jim_wr24(cp + 1u, h->rx_count);
   return NET_OK;
}

static uint8_t do_close(net_handle_t *h)
{
   if (h->state == NET_ST_FREE) {
      /* Nothing to tear down, but clear any latched N: state so CLOSE is
       * always a way back to a usable handle. */
      h->url_phase = URL_START;
      h->last_err  = NET_OK;
      return NET_OK;
   }
   /* TNFS: best-effort CLOSE (if a file is open) + UMOUNT before dropping the
      socket, so the server reclaims the fd/session promptly.  Fire-and-forget:
      a lost teardown just leaves the server to time the session out. */
   if (h->is_url && h->url_adapter == NET_URL_TNFS
       && h->upcb != NULL && h->tnfs_connid != 0u) {
      uint8_t  req[16];
      size_t   n;
      if (h->tnfs_phase == TNFS_PH_READY || h->tnfs_phase == TNFS_PH_READING) {
         if (h->tnfs_is_dir)
            n = tnfs_build_closedir(req, sizeof req, h->tnfs_connid, ++h->tnfs_seq, h->tnfs_fd);
         else
            n = tnfs_build_close(req, sizeof req, h->tnfs_connid, ++h->tnfs_seq, h->tnfs_fd);
         net_tnfs_send_raw(h, req, (uint16_t)n);
      }
      n = tnfs_build_umount(req, sizeof req, h->tnfs_connid, ++h->tnfs_seq);
      net_tnfs_send_raw(h, req, (uint16_t)n);
   }
   net_pcb_release(h, false);       /* graceful (falls back to abort) */
   net_handle_reset(h);
   return NET_OK;
}

static uint8_t do_status(net_handle_t *h, uint32_t cp)
{
   uint8_t flags = 0u;
   if (h->state == NET_ST_CONNECTED) flags |= NET_FLAG_CONNECTED;
   if (h->rx_eof)                    flags |= NET_FLAG_RX_EOF;
   if (h->state == NET_ST_ERROR)     flags |= NET_FLAG_ERROR;
   if (h->rx_count != 0u)            flags |= NET_FLAG_RX_READY;

   Pi1MHz->JIM_ram[cp + 1u] = h->state;
   Pi1MHz->JIM_ram[cp + 2u] = flags;
   if (h->state >= NET_ST_CONNECTING)
      net_ip_to_wire(&h->remote_ip, cp + 3u);
   else {
      Pi1MHz->JIM_ram[cp + 3u] = 0u; Pi1MHz->JIM_ram[cp + 4u] = 0u;
      Pi1MHz->JIM_ram[cp + 5u] = 0u; Pi1MHz->JIM_ram[cp + 6u] = 0u;
   }
   Pi1MHz->JIM_ram[cp + 7u] = (uint8_t)h->remote_port;
   Pi1MHz->JIM_ram[cp + 8u] = (uint8_t)(h->remote_port >> 8);
   jim_wr24(cp + 9u, h->rx_count);
   return NET_OK;
}

/* ---- N: device adapters -------------------------------------------------- */

/* HTTP: send "GET <path> HTTP/1.0" with Host + Connection: close. */
static uint8_t net_http_send_request(net_handle_t *h, const char *host,
                                     const char *path)
{
   char req[NET_MAX_HOSTNAME + 256u];
   int n = snprintf(req, sizeof req,
                    "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                    path, host);
   if (n < 0 || (size_t)n >= sizeof req)
      return NET_ERR_PARAM;
   if (h->tpcb == NULL)
      return NET_ERR_CONN;
   if (altcp_write(h->tpcb, req, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK)
      return NET_ERR_NOMEM;
   altcp_output(h->tpcb);
   wifi_lwip_rx_kick();
   return NET_OK;
}

/* Bytes of headers (incl. the terminating CRLFCRLF) waiting in the ring, or 0
   if the end-of-headers has not arrived yet. */
static uint16_t net_http_find_headers(const net_handle_t *h)
{
   const uint8_t *ring = net_rx_ring[h - net_h];
   uint16_t i;
   if (h->rx_count < 4u)
      return 0u;
   for (i = 0; (unsigned int)i + 4u <= (unsigned int)h->rx_count; i++) {
      uint16_t t = (uint16_t)(((unsigned int)h->rx_tail + i) & (NET_RX_RING_SIZE - 1u));
      if (ring[t] == '\r'
          && ring[(uint16_t)((t + 1u) & (NET_RX_RING_SIZE - 1u))] == '\n'
          && ring[(uint16_t)((t + 2u) & (NET_RX_RING_SIZE - 1u))] == '\r'
          && ring[(uint16_t)((t + 3u) & (NET_RX_RING_SIZE - 1u))] == '\n')
         return (uint16_t)(i + 4u);
   }
   return 0u;
}

/* Parse the status code from the ring's first line ("HTTP/1.x NNN ..."). */
static void net_http_parse_status(net_handle_t *h)
{
   const uint8_t *ring = net_rx_ring[h - net_h];
   uint16_t i, code = 0u;
   bool after_space = false;
   for (i = 0; i < h->rx_count && i < 64u; i++) {
      uint8_t c = ring[(uint16_t)(((unsigned int)h->rx_tail + i) & (NET_RX_RING_SIZE - 1u))];
      if (!after_space) {
         if (c == ' ') after_space = true;
      } else if (c >= '0' && c <= '9') {
         code = (uint16_t)(code * 10u + (uint16_t)(c - '0'));
      } else {
         break;
      }
   }
   h->http_code = code;
}

/* ---- N: device commands -------------------------------------------------- */

/* Start the URL handle's connect and advance to URL_CONNECTING; on any
   immediate failure (e.g. pcb exhaustion) land in URL_FAIL rather than a
   stuck URL_CONNECTING that polls NET_PENDING forever. */
static uint8_t url_begin_connect(net_handle_t *h)
{
   uint8_t r = net_start_connect(h);
   if (r == NET_PENDING) h->url_phase = URL_CONNECTING;
   else { h->url_phase = URL_FAIL; h->last_err = r; }
   return r;
}

/* UDP: scheme - connectionless, so once the peer address is resolved there is
   nothing to connect: bind an ephemeral local port so replies come back, and
   go straight to READY.  url_write sends to the URL's host:port; url_read
   returns the next datagram's payload. */
static uint8_t net_udp_url_ready(net_handle_t *h)
{
   h->upcb = udp_new();
   if (h->upcb == NULL) {
      h->url_phase = URL_FAIL; h->last_err = NET_ERR_NOMEM; return NET_ERR_NOMEM;
   }
   udp_recv(h->upcb, net_udp_recv, h);
   if (udp_bind(h->upcb, IP_ANY_TYPE, 0u) != ERR_OK) {   /* ephemeral local port */
      udp_remove(h->upcb); h->upcb = NULL;
      h->url_phase = URL_FAIL; h->last_err = NET_ERR_CONN; return NET_ERR_CONN;
   }
   /* Filter to the URL's host:port, as the TNFS engine does. url_read hands
      the Beeb the payload with no source address attached, so without this an
      off-path datagram that guessed the ephemeral port would be consumed as
      though the server had sent it. (The raw-socket path is free to stay
      unconnected: recvfrom reports the peer, so the caller can judge.) */
   if (udp_connect(h->upcb, &h->remote_ip, h->remote_port) != ERR_OK) {
      udp_remove(h->upcb); h->upcb = NULL;
      h->url_phase = URL_FAIL; h->last_err = NET_ERR_CONN; return NET_ERR_CONN;
   }
   h->state = NET_ST_CONNECTED;      /* the N: handle is ready to send/recv */
   h->url_phase = URL_READY;
   return NET_OK;
}

/* ---- TNFS (N:TNFS://) reliable-transaction engine ------------------------ */

static uint32_t net_now_ms(void)
{
   return (uint32_t)(RPI_GetSystemTime64() / 1000u);
}

/* Pop one datagram's payload from the RX ring (dropping the [ip][port] record
   header net_udp_recv prepends) into dst.  Returns the payload length, or 0 if
   no complete record is buffered. */
static uint16_t net_udp_pop(net_handle_t *h, uint8_t *dst, size_t cap)
{
   uint8_t  hdr[8];
   uint16_t dglen, copy;
   if (h->rx_count < 8u) return 0u;
   ring_get_mem(h, hdr, 8u);
   dglen = (uint16_t)(hdr[6] | (hdr[7] << 8));
   copy  = (dglen < cap) ? dglen : (uint16_t)cap;
   ring_get_mem(h, dst, copy);
   if (dglen > copy) {                          /* discard what didn't fit */
      uint8_t  junk[64];
      uint16_t left = (uint16_t)(dglen - copy);
      while (left != 0u) {
         uint16_t k = (left < sizeof junk) ? left : (uint16_t)sizeof junk;
         ring_get_mem(h, junk, k);
         left = (uint16_t)(left - k);
      }
   }
   return copy;
}

/* Fire a datagram at the mounted server, no retry bookkeeping (close/umount). */
static void net_tnfs_send_raw(net_handle_t *h, const uint8_t *req, uint16_t len)
{
   struct pbuf *p;
   if (h->upcb == NULL || len == 0u) return;
   p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
   if (p == NULL) return;
   pbuf_take(p, req, len);
   (void)udp_sendto(h->upcb, p, &h->remote_ip, h->remote_port);
   pbuf_free(p);
   wifi_lwip_rx_kick();
}

/* (Re)transmit the buffered request and (re)arm the resend/timeout deadline. */
static void net_tnfs_fire(net_handle_t *h)
{
   net_tnfs_send_raw(h, h->tnfs_req, h->tnfs_req_len);
   h->tnfs_deadline = net_now_ms()
                    + (h->tnfs_retry_ms ? h->tnfs_retry_ms : TNFS_TIMEOUT_MS);
}

/* Begin a new transaction: the caller has already built the request into
   h->tnfs_req[] with sequence h->tnfs_seq.  Sends it and enters `phase`. */
static uint8_t net_tnfs_start(net_handle_t *h, uint8_t phase, size_t req_len)
{
   if (req_len == 0u || req_len > TNFS_REQ_MAX) return NET_ERR_PARAM;
   h->tnfs_phase   = phase;
   h->tnfs_req_len = (uint16_t)req_len;
   h->tnfs_retries = TNFS_RETRIES;
   h->tnfs_eagain  = TNFS_EAGAIN_MAX;
   net_tnfs_fire(h);
   return NET_PENDING;
}

/* Poll the outstanding transaction.  Fills *pkt (caller-owned, so the reply
   body pointers in *rep stay valid after return) and returns:
   NET_OK  - a matching reply arrived (rep->status may still be an error),
   NET_PENDING - still waiting / backing off / just resent,
   NET_ERR_CONN - retries exhausted with no reply. */
static uint8_t net_tnfs_poll(net_handle_t *h, uint8_t *pkt, size_t pktcap,
                             tnfs_reply_t *rep)
{
   uint8_t cmd = h->tnfs_req[3];                /* the command we sent */
   if (h->rx_count >= 8u) {
      uint16_t plen = net_udp_pop(h, pkt, pktcap);
      /* Accept only a well-formed reply to our seq/cmd on this session - a
         reply carrying a different connid (once one is assigned) is not ours,
         so it is drained and ignored (defence-in-depth: the pcb is also
         udp_connect'd to the server, so foreign sources never arrive). */
      if (plen != 0u && tnfs_parse_reply(pkt, plen, h->tnfs_seq, cmd, rep)
          && (h->tnfs_connid == 0u || rep->connid == h->tnfs_connid)) {
         if (rep->status == TNFS_EAGAIN) {       /* server busy: back off, resend */
            uint32_t back = rep->backoff_ms ? rep->backoff_ms : h->tnfs_retry_ms;
            if (h->tnfs_eagain == 0u) return NET_ERR_CONN;   /* bound sustained EAGAIN */
            h->tnfs_eagain--;
            h->tnfs_retries = TNFS_RETRIES;      /* link is proven up - restore loss budget */
            h->tnfs_deadline = net_now_ms() + (back ? back : TNFS_TIMEOUT_MS);
            return NET_PENDING;
         }
         return NET_OK;
      }
      /* a stale/duplicate/foreign datagram - ignore it and keep waiting */
   }
   if ((int32_t)(net_now_ms() - h->tnfs_deadline) >= 0) {
      if (h->tnfs_retries == 0u) return NET_ERR_CONN;   /* gave up */
      h->tnfs_retries--;
      net_tnfs_fire(h);
   }
   return NET_PENDING;
}

/* Map a TNFS status byte to a net result code. */
static uint8_t net_tnfs_status(uint8_t status)
{
   if (status == TNFS_OK)  return NET_OK;
   if (status == TNFS_EOF) return NET_EOF;
   return NET_ERR_CONN;
}

/* TNFS url_open step 1: create the UDP socket to the server and send MOUNT. */
static uint8_t net_tnfs_begin(net_handle_t *h)
{
   size_t n;
   h->upcb = udp_new();
   if (h->upcb == NULL) {
      h->url_phase = URL_FAIL; h->last_err = NET_ERR_NOMEM; return NET_ERR_NOMEM;
   }
   udp_recv(h->upcb, net_udp_recv, h);
   if (udp_bind(h->upcb, IP_ANY_TYPE, 0u) != ERR_OK) {
      udp_remove(h->upcb); h->upcb = NULL;
      h->url_phase = URL_FAIL; h->last_err = NET_ERR_CONN; return NET_ERR_CONN;
   }
   /* Only accept datagrams from the TNFS server itself - a session id lives in
      every reply, so an off-path spoof of the ephemeral port must not be able
      to hijack the mount.  (udp_sendto below still overrides the destination.) */
   (void)udp_connect(h->upcb, &h->remote_ip, h->remote_port);
   h->type          = NET_TYPE_UDP;
   h->state         = NET_ST_CONNECTED;
   h->tnfs_retry_ms = TNFS_TIMEOUT_MS;          /* until MOUNT tells us better */
   h->tnfs_seq      = 1u;
   n = tnfs_build_mount(h->tnfs_req, TNFS_REQ_MAX, h->tnfs_seq, "/", NULL, NULL);
   if (n == 0u) { h->url_phase = URL_FAIL; h->last_err = NET_ERR_PARAM; return NET_ERR_PARAM; }
   h->url_phase = URL_CONNECTING;               /* reuse: "TNFS handshaking" */
   return net_tnfs_start(h, TNFS_PH_MOUNT, n);
}

/* Once the URL's host is resolved into remote_ip, hand off to the adapter:
   UDP opens a datagram socket, TNFS mounts, everything else does a TCP connect. */
static uint8_t url_after_resolve(net_handle_t *h)
{
   if (h->url_adapter == NET_URL_UDP)  return net_udp_url_ready(h);
   if (h->url_adapter == NET_URL_TNFS) return net_tnfs_begin(h);
   return url_begin_connect(h);
}

static uint8_t do_url_open(net_handle_t *h, uint32_t cp)
{
   char       host[NET_MAX_HOSTNAME];
   char       path[192];
   net_url_t  u;
   const char *url;

   if (h->url_phase == URL_READY) return NET_OK;
   if (h->url_phase == URL_FAIL)  return h->last_err ? h->last_err : NET_ERR_CONN;

   if (!net_string_ok(cp + 2u))   return NET_ERR_PARAM;
   url = (const char *)&Pi1MHz->JIM_ram[cp + 2u];
   if (!net_url_parse(url, &u, host, sizeof host, path, sizeof path)) {
      /* Only latch the failure once this open OWNS the handle. A malformed
       * URL on a still-FREE handle must not stick: URL_FAIL is answered
       * before anything else on the next poll, and do_close() returns early
       * on a FREE handle, so latching here would kill the handle for every
       * later N: verb until a machine reset. */
      if (h->state != NET_ST_FREE) {
         h->url_phase = URL_FAIL;
         h->last_err  = NET_ERR_PARAM;
      }
      return NET_ERR_PARAM;
   }

   switch (h->url_phase) {
      case URL_START:
         if (h->state != NET_ST_FREE)  return NET_ERR_INUSE;
         if (!wifi_lwip_get_context()->address_ready)
            return NET_PENDING;                   /* no IP yet - keep polling */
         net_handle_reset(h);
         h->is_url = true; h->url_adapter = u.adapter;
         { uint8_t m = jim_rd8(cp + 1u);                     /* FujiNet aux1 open mode */
           h->tnfs_wr = (m == NET_OPEN_WRITE || m == NET_OPEN_RW) ? 1u : 0u; }
         h->type = (u.adapter == NET_URL_UDP) ? NET_TYPE_UDP : NET_TYPE_TCP;
         h->state = NET_ST_IDLE;
         h->remote_port = u.port;
         if (net_parse_dotted(host, &h->remote_ip))
            return url_after_resolve(h);
         {
            err_t e = dns_gethostbyname(host, &h->dns_ip, net_dns_found, h);
            if (e == ERR_OK) {
               h->remote_ip = h->dns_ip;
               return url_after_resolve(h);
            }
            if (e == ERR_INPROGRESS) {
               h->url_phase = URL_RESOLVING; h->state = NET_ST_RESOLVING;
               return NET_PENDING;
            }
            h->url_phase = URL_FAIL; h->last_err = NET_ERR_DNS; return NET_ERR_DNS;
         }
      case URL_RESOLVING:
         if (!h->dns_done) return NET_PENDING;
         h->dns_done = false;
         if (!h->dns_ok) { h->url_phase = URL_FAIL; h->last_err = NET_ERR_DNS; return NET_ERR_DNS; }
         h->remote_ip = h->dns_ip; h->state = NET_ST_IDLE;
         return url_after_resolve(h);
      case URL_CONNECTING:
         if (h->url_adapter == NET_URL_TNFS) {
            uint8_t      pkt[TNFS_PKT_MAX];
            tnfs_reply_t rep;
            uint8_t      r = net_tnfs_poll(h, pkt, sizeof pkt, &rep);
            if (r == NET_PENDING) return NET_PENDING;
            if (r != NET_OK) { h->url_phase = URL_FAIL; h->last_err = r; return r; }
            if (rep.status != TNFS_OK) {
               h->url_phase = URL_FAIL;
               h->last_err = net_tnfs_status(rep.status);
               return h->last_err;
            }
            if (h->tnfs_phase == TNFS_PH_MOUNT) {      /* mounted -> OPEN the file/dir */
               uint16_t rms = 0;
               size_t   plen = strlen(path);
               size_t   n;
               (void)tnfs_reply_mount(&rep, NULL, &rms);
               h->tnfs_connid = rep.connid;
               if (rms >= 100u && rms <= 5000u) h->tnfs_retry_ms = rms;
               /* directory if the mode is DIR (13) or the path ends in '/' */
               uint8_t m = jim_rd8(cp + 1u);
               h->tnfs_is_dir = ((plen != 0u && path[plen - 1u] == '/')
                                 || m == NET_OPEN_DIR) ? 1u : 0u;
               h->tnfs_seq++;
               if (h->tnfs_is_dir) {
                  n = tnfs_build_opendir(h->tnfs_req, TNFS_REQ_MAX, h->tnfs_connid,
                                         h->tnfs_seq, path);
               } else {
                  uint16_t oflags = (m == NET_OPEN_RW)
                        ? (uint16_t)(TNFS_O_RDWR | TNFS_O_CREAT)
                        : h->tnfs_wr
                              ? (uint16_t)(TNFS_O_WRONLY | TNFS_O_CREAT | TNFS_O_TRUNC)
                              : (uint16_t)TNFS_O_RDONLY;
                  uint16_t omode = h->tnfs_wr ? 0x01A4u : 0u;   /* 0644 on create */
                  n = tnfs_build_open(h->tnfs_req, TNFS_REQ_MAX, h->tnfs_connid,
                                      h->tnfs_seq, oflags, omode, path);
               }
               if (n == 0u) { h->url_phase = URL_FAIL; h->last_err = NET_ERR_PARAM; return NET_ERR_PARAM; }
               return net_tnfs_start(h, TNFS_PH_OPEN, n);
            }
            /* TNFS_PH_OPEN: file/dir is open -> READY */
            if (h->tnfs_is_dir) (void)tnfs_reply_opendir(&rep, &h->tnfs_fd);
            else                (void)tnfs_reply_open(&rep, &h->tnfs_fd);
            h->tnfs_phase = TNFS_PH_READY;
            h->url_phase  = URL_READY;
            return NET_OK;
         }
         if (h->state == NET_ST_CONNECTING) return NET_PENDING;
         if (h->state == NET_ST_ERROR) { h->url_phase = URL_FAIL; h->last_err = NET_ERR_CONN; return NET_ERR_CONN; }
         if (h->state == NET_ST_CONNECTED) {
            if (h->url_adapter == NET_URL_HTTP) {
               uint8_t r = net_http_send_request(h, host, path);
               if (r != NET_OK) { h->url_phase = URL_FAIL; h->last_err = r; return r; }
            }
            h->url_phase = URL_READY;
            return NET_OK;
         }
         return NET_PENDING;
      default:
         return NET_PENDING;
   }
}

static uint8_t do_url_read(net_handle_t *h, uint32_t cp)
{
   uint32_t max    = jim_rd24(cp + 1u);
   uint32_t jimoff = jim_rd32(cp + 4u);
   uint32_t got;

   if (h->state == NET_ST_FREE) return NET_ERR_NOTOPEN;
   if (!net_buffer_ok(jimoff, max)) return NET_ERR_PARAM;

   if (h->url_adapter == NET_URL_TNFS) {
      uint8_t      pkt[TNFS_PKT_MAX];
      tnfs_reply_t rep;
      uint8_t      r;
      jim_wr24(cp + 1u, 0u);                     /* default: 0 bytes read */
      if (h->tnfs_phase == TNFS_PH_READY) {       /* start a READ / READDIR */
         size_t n;
         if (max == 0u) return NET_OK;            /* no room - don't burn a dir entry */
         h->tnfs_seq++;
         if (h->tnfs_is_dir) {                    /* one directory entry per read */
            n = tnfs_build_readdir(h->tnfs_req, TNFS_REQ_MAX, h->tnfs_connid,
                                   h->tnfs_seq, h->tnfs_fd);
         } else {
            uint16_t want = (max > TNFS_READ_CHUNK) ? (uint16_t)TNFS_READ_CHUNK : (uint16_t)max;
            if (want == 0u) return NET_OK;
            n = tnfs_build_read(h->tnfs_req, TNFS_REQ_MAX, h->tnfs_connid,
                                h->tnfs_seq, h->tnfs_fd, want);
         }
         return net_tnfs_start(h, TNFS_PH_READING, n);
      }
      if (h->tnfs_phase != TNFS_PH_READING) return NET_ERR_NOTOPEN;
      r = net_tnfs_poll(h, pkt, sizeof pkt, &rep);
      if (r == NET_PENDING) return NET_PENDING;
      h->tnfs_phase = TNFS_PH_READY;             /* transaction over either way */
      if (r != NET_OK) return r;                 /* timed out */
      if (rep.status == TNFS_EOF) return NET_EOF; /* end of file / end of directory */
      if (rep.status != TNFS_OK)  return net_tnfs_status(rep.status);
      if (h->tnfs_is_dir) {                       /* deliver the entry name */
         const char *name;
         uint32_t    copy;
         if (!tnfs_reply_readdir(&rep, &name)) return NET_ERR_CONN;
         copy = (uint32_t)strlen(name);
         if (copy > max) copy = max;
         memcpy(&Pi1MHz->JIM_ram[jimoff + DISC_RAM_BASE], name, copy);
         jim_wr24(cp + 1u, copy);
      } else {                                    /* deliver file bytes */
         const uint8_t *data;
         uint16_t       dlen;
         uint32_t       copy;
         if (!tnfs_reply_read(&rep, &data, &dlen)) return NET_ERR_CONN;
         copy = (dlen < max) ? dlen : max;
         memcpy(&Pi1MHz->JIM_ram[jimoff + DISC_RAM_BASE], data, copy);
         jim_wr24(cp + 1u, copy);
      }
      return NET_OK;
   }

   if (h->url_adapter == NET_URL_UDP) {
      /* Return one datagram's payload; the peer is the fixed URL host, so the
         [4 ip][2 port] record header is dropped.  No connection => no EOF; a
         client polls and terminates on its own (timeout/count). */
      uint8_t  hdr[8];
      uint16_t dglen;
      uint32_t copy;
      if (h->rx_count < 8u) { jim_wr24(cp + 1u, 0u); return NET_OK; }
      ring_get_mem(h, hdr, 8u);
      dglen = (uint16_t)(hdr[6] | (hdr[7] << 8));
      copy  = (dglen < max) ? dglen : max;
      ring_get(h, jimoff + DISC_RAM_BASE, copy);
      if (dglen > copy) {                        /* discard the tail that didn't fit */
         uint8_t  junk[64];
         uint16_t left = (uint16_t)(dglen - copy);
         while (left != 0u) {
            uint16_t k = (left < sizeof junk) ? left : (uint16_t)sizeof junk;
            ring_get_mem(h, junk, k);
            left = (uint16_t)(left - k);
         }
      }
      jim_wr24(cp + 1u, copy);
      return NET_OK;
   }

   if (h->url_adapter == NET_URL_HTTP && !h->http_hdr_done) {
      uint16_t hdr = net_http_find_headers(h);
      if (hdr == 0u) {                       /* end-of-headers not seen yet */
         jim_wr24(cp + 1u, 0u);
         /* Ring full without CRLFCRLF = oversized/non-HTTP headers: fail
            rather than deadlock (a full ring parks all further RX, so the
            terminator - and the FIN - can never arrive).
            rx_parked is the exact signal: back-pressure works at SEGMENT
            granularity (net_tcp_recv refuses a whole chain that will not
            fit), so rx_count plateaus wherever the next segment stopped
            fitting - typically ~7 KB, never the 8191 a byte-count test
            waits for. While the headers are incomplete we return 0 bytes
            to the Beeb, so nothing will ever drain the ring and release
            the parked pbuf: that is the deadlock, and this is it arriving. */
         if (h->rx_parked || h->rx_count >= NET_RX_RING_SIZE - 1u) {
            h->last_err = NET_ERR_CONN;
            return NET_ERR_CONN;
         }
         if (h->rx_eof) return NET_EOF;      /* peer closed before end-of-headers */
         if (h->state == NET_ST_ERROR)       /* reset before end-of-headers */
            return h->last_err ? h->last_err : NET_ERR_CONN;
         return NET_OK;                       /* wait for more header bytes */
      }
      net_http_parse_status(h);
      { uint8_t junk[64]; uint16_t left = hdr;
        while (left != 0u) { uint16_t k = (left < sizeof junk) ? left : (uint16_t)sizeof junk;
                             ring_get_mem(h, junk, k); left = (uint16_t)(left - k); } }
      h->http_hdr_done = true;
   }
   got = ring_get(h, jimoff + DISC_RAM_BASE, max);
   jim_wr24(cp + 1u, got);
   if (got == 0u && h->rx_count == 0u) {
      if (h->rx_eof)                 return NET_EOF;
      if (h->state == NET_ST_ERROR)  return h->last_err ? h->last_err : NET_ERR_CONN;
   }
   return NET_OK;
}

/* url_write (62): UDP sends the payload to the URL's host:port; TCP/HTTP reuse
   the stream send (same [1..3] len, [4..7] JIM src command-block layout). */
static uint8_t do_url_write(net_handle_t *h, uint32_t cp)
{
   uint32_t     len;
   uint32_t     jimoff;
   struct pbuf *p;

   if (h->url_adapter == NET_URL_TNFS) {         /* WRITE a chunk to the file */
      uint8_t      pkt[TNFS_PKT_MAX];
      tnfs_reply_t rep;
      uint8_t      r;
      len    = jim_rd24(cp + 1u);
      jimoff = jim_rd32(cp + 4u);
      if (!h->tnfs_wr)                 return NET_ERR_NOTOPEN;   /* read-only open */
      if (!net_buffer_ok(jimoff, len)) return NET_ERR_PARAM;
      if (h->tnfs_phase == TNFS_PH_READY) {
         uint16_t want = (len > TNFS_WRITE_CHUNK) ? (uint16_t)TNFS_WRITE_CHUNK : (uint16_t)len;
         size_t   n;
         jim_wr24(cp + 1u, 0u);
         if (want == 0u) return NET_OK;
         h->tnfs_seq++;
         n = tnfs_build_write(h->tnfs_req, TNFS_REQ_MAX, h->tnfs_connid, h->tnfs_seq,
                              h->tnfs_fd, &Pi1MHz->JIM_ram[jimoff + DISC_RAM_BASE], want);
         return net_tnfs_start(h, TNFS_PH_WRITING, n);
      }
      if (h->tnfs_phase != TNFS_PH_WRITING) { jim_wr24(cp + 1u, 0u); return NET_ERR_NOTOPEN; }
      r = net_tnfs_poll(h, pkt, sizeof pkt, &rep);
      if (r == NET_PENDING) { jim_wr24(cp + 1u, 0u); return NET_PENDING; }
      h->tnfs_phase = TNFS_PH_READY;
      if (r != NET_OK) { jim_wr24(cp + 1u, 0u); return r; }
      if (rep.status != TNFS_OK) { jim_wr24(cp + 1u, 0u); return net_tnfs_status(rep.status); }
      /* Clamp the server's count to the most we can ever have sent: a hostile
         or buggy server answering a 240-byte WRITE with "wrote 65535" would
         underflow a Beeb-side `rem% -= wrote%` loop and walk its pointer off
         the end. The request was issued on an earlier poll, so bound it by
         TNFS_WRITE_CHUNK - true regardless of what the command block holds
         now - rather than by a re-read length. */
      { uint16_t wrote = 0u;
        if (!tnfs_reply_write(&rep, &wrote)) wrote = 0u;
        if (wrote > (uint16_t)TNFS_WRITE_CHUNK) wrote = (uint16_t)TNFS_WRITE_CHUNK;
        jim_wr24(cp + 1u, wrote); }
      return NET_OK;
   }

   if (h->url_adapter == NET_URL_TELNET) {        /* escape 0xFF as IAC IAC, then send */
      uint8_t esc[512];
      size_t  consumed = 0u, elen;
      u16_t   avail, oc;
      len    = jim_rd24(cp + 1u);
      jimoff = jim_rd32(cp + 4u);
      if (h->type != NET_TYPE_TCP || h->tpcb == NULL || h->state != NET_ST_CONNECTED)
         return NET_ERR_NOTOPEN;
      if (!net_buffer_ok(jimoff, len))
         return NET_ERR_PARAM;
      avail = altcp_sndbuf(h->tpcb);
      oc = (avail < sizeof esc) ? avail : (u16_t)sizeof esc;
      if (oc == 0u) { jim_wr24(cp + 1u, 0u); return NET_OK; }   /* sndbuf full - retry */
      elen = telnet_escape(&Pi1MHz->JIM_ram[jimoff + DISC_RAM_BASE], len, esc, oc, &consumed);
      if (elen != 0u) {
         err_t e = altcp_write(h->tpcb, esc, (u16_t)elen, TCP_WRITE_FLAG_COPY);
         if (e == ERR_MEM) { jim_wr24(cp + 1u, 0u); return NET_OK; }
         if (e != ERR_OK)  return NET_ERR_CONN;
         altcp_output(h->tpcb);
         wifi_lwip_rx_kick();
      }
      jim_wr24(cp + 1u, (uint32_t)consumed);      /* input bytes consumed */
      return NET_OK;
   }

   if (h->type != NET_TYPE_UDP)
      return do_send(h, cp);

   len    = jim_rd24(cp + 1u);
   jimoff = jim_rd32(cp + 4u);
   if (h->upcb == NULL)
      return NET_ERR_NOTOPEN;
   if (len > 0xFFFFu || !net_buffer_ok(jimoff, len))
      return NET_ERR_PARAM;

   p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
   if (p == NULL)
      return NET_ERR_NOMEM;
   pbuf_take(p, &Pi1MHz->JIM_ram[jimoff + DISC_RAM_BASE], (u16_t)len);
   {
      err_t e = udp_sendto(h->upcb, p, &h->remote_ip, h->remote_port);
      pbuf_free(p);
      if (e != ERR_OK)
         return NET_ERR_CONN;
   }
   wifi_lwip_rx_kick();
   jim_wr24(cp + 1u, len);
   return NET_OK;
}

static uint8_t do_url_status(net_handle_t *h, uint32_t cp)
{
   uint8_t flags = 0u;
   uint8_t err   = 0u;
   if (h->url_phase == URL_READY)               flags |= NET_FLAG_CONNECTED;
   if (h->rx_eof)                               flags |= NET_FLAG_RX_EOF;
   if (h->url_phase == URL_FAIL || h->state == NET_ST_ERROR) {
      flags |= NET_FLAG_ERROR;
      err = h->last_err ? h->last_err : NET_ERR_CONN;
   }
   if (h->rx_count != 0u)                       flags |= NET_FLAG_RX_READY;
   /* [1..4] = FujiNet DVSTAT {bytes_waiting_lo, hi, connected, error} - what
      fujinet-lib's network_status() reads; [5..8] are our native extensions. */
   Pi1MHz->JIM_ram[cp + 1u] = (uint8_t)h->rx_count;
   Pi1MHz->JIM_ram[cp + 2u] = (uint8_t)(h->rx_count >> 8);
   Pi1MHz->JIM_ram[cp + 3u] = (h->url_phase == URL_READY) ? 1u : 0u;
   Pi1MHz->JIM_ram[cp + 4u] = err;
   Pi1MHz->JIM_ram[cp + 5u] = h->state;
   Pi1MHz->JIM_ram[cp + 6u] = flags;
   Pi1MHz->JIM_ram[cp + 7u] = (uint8_t)h->http_code;
   Pi1MHz->JIM_ram[cp + 8u] = (uint8_t)(h->http_code >> 8);
   return NET_OK;
}

/* Dispatch one latched command; returns the result byte for the Beeb. */
static uint8_t net_dispatch(uint32_t cp, uint8_t data)
{
   uint8_t cmd    = jim_rd8(cp);
   uint8_t handle = (uint8_t)(data & 0x0Fu);
   net_handle_t *h;

   if (!net_enabled)
      return NET_ERR_DISABLED;
   if (handle >= NET_MAX_HANDLES)
      return NET_ERR_PARAM;
   h = &net_h[handle];

   switch (cmd) {
      case NET_CMD_OPEN:         return do_open(h, cp);
      case NET_CMD_DNS:          return do_dns(h, cp);
      case NET_CMD_CONNECT:      return do_connect(h, cp);
      case NET_CMD_BIND:         return do_bind(h, cp);
      case NET_CMD_LISTEN:       return do_listen(h, cp);
      case NET_CMD_SEND:         return do_send(h, cp);
      case NET_CMD_RECV:         return do_recv(h, cp);
      case NET_CMD_RECV_AVAIL:   return do_recv_avail(h, cp);
      case NET_CMD_CLOSE:        return do_close(h);
      case NET_CMD_STATUS:       return do_status(h, cp);
      case NET_CMD_UDP_SENDTO:   return do_udp_sendto(h, cp);
      case NET_CMD_UDP_RECVFROM: return do_udp_recvfrom(h, cp);
      case NET_CMD_IRQ:          net_irq_armed = (jim_rd8(cp + 1u) != 0u);
                                 return NET_OK;
      case NET_CMD_URL_OPEN:     return do_url_open(h, cp);
      case NET_CMD_URL_READ:     return do_url_read(h, cp);
      case NET_CMD_URL_WRITE:    return do_url_write(h, cp);
      case NET_CMD_URL_CLOSE:    return do_close(h);
      case NET_CMD_URL_STATUS:   return do_url_status(h, cp);
      default:                   return NET_ERR_UNSUPPORTED;
   }
}

/* Drive the shared nIRQ line: assert whenever any handle has buffered RX the
   Beeb has not drained (level-triggered; the Beeb reads status/recv to learn
   which handle and clears it by draining).  Terminal states (EOF/error with
   no data) are discovered by polling and do not raise nIRQ in this stage.

   nIRQ is OPT-IN (NET_CMD_IRQ, default disarmed).  A purely polling client
   installs no IRQ handler, so a level-triggered nIRQ that stays asserted while
   RX sits unread would lock the Beeb in an IRQ storm - disarmed by default,
   the line is held clear no matter how much RX is buffered. */
static uint8_t net_irq_state;
static void net_update_irq(void)
{
   uint8_t any = 0u;
   if (net_irq_armed)
      for (unsigned int i = 0; i < NET_MAX_HANDLES; i++)
         if (net_h[i].rx_count != 0u) { any = 1u; break; }
   if (any != net_irq_state) {
      services_irq_set(net_source, any != 0u);
      net_irq_state = any;
   }
}

/* ---- FIQ latch + main-loop poll ----------------------------------------- */

static void net_service_command(uint32_t command_pointer, uint32_t addr,
                                uint8_t data)
{
   /* FIQ context: latch only.  Publish NET_BUSY (bit 7) so a Beeb that reads
      the result register before the poll runs spins rather than seeing a
      stale value; the poll overwrites it with the real result. */
   net_pending_cp   = command_pointer;
   net_pending_addr = addr;
   net_pending_data = data;
   net_pending      = true;
   Pi1MHz_MemoryWrite(addr, NET_BUSY);
}

static void net_service_poll(void)
{
   if (net_reset_pending) {
      /* A BBC reset re-ran net_service_init.  Tear down every live pcb here,
         on the main loop - never from init/FIQ (lwIP may not be up at init,
         and is never callable from FIQ).  The table is valid (BSS), so on the
         first boot this finds only NET_ST_FREE handles and aborts nothing. */
      for (unsigned int i = 0; i < NET_MAX_HANDLES; i++) {
         net_pcb_release(&net_h[i], true);
         net_handle_reset(&net_h[i]);
      }
      net_irq_armed = false;         /* a reset removes any Beeb IRQ handler */
      net_reset_pending = false;
      /* Deliberately do NOT clear net_pending here.  A command the FIQ latches
         *during* this teardown loop would otherwise be dropped, stranding the
         NET_BUSY byte the FIQ wrote to the result register forever - the Beeb
         spins on bit 7 and never re-issues, so only a reflash recovers.  Let it
         fall through to be dispatched below: every handle is FREE now, so it
         returns a clean bit-7-clear result and BUSY always clears. */
   }

   if (net_pending) {
      uint32_t cp   = net_pending_cp;
      uint32_t addr = net_pending_addr;
      uint8_t  data = net_pending_data;
      net_pending = false;
      Pi1MHz_MemoryWrite(addr, net_dispatch(cp, data));
   }

   net_update_irq();
}

void net_service_init(uint8_t instance, uint8_t address)
{
   (void)address;                   /* the services framework owns the base */
   net_source = instance;

   {
      net_enabled = config_get_bool("net_enable");
   }

   /* Defer all pcb teardown to the first poll (see net_service_poll).  Do NOT
      clear net_pending here either: a command latched around a BBC-reset
      re-init must be dispatched (against the freshly-reset, all-FREE handles),
      never dropped - dropping strands NET_BUSY in the result register.  It is
      already 0 on the first-ever boot (BSS, before the service is registered). */
   net_reset_pending = true;

   /* Both dedupe, so re-running on a BBC reset is safe. */
   (void)services_register(SERVICE_CMD_NET_FIRST, SERVICE_CMD_NET_LAST,
                           net_service_command);
   Pi1MHz_Register_Poll(net_service_poll, "net");

   services_irq_set(net_source, false);   /* start with our nIRQ line clear */
}
