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

#include "Pi1MHz.h"
#include "ram_emulator.h"          /* DISC_RAM_BASE, DISC_RAM_SIZE, JIM_ram */
#include "services.h"
#include "net_service.h"
#include "config.h"

#include "wifi/wifi_lwip.h"        /* wifi_lwip_get_context, wifi_lwip_rx_kick */
#include "lwip/altcp.h"
#include "lwip/tcp.h"              /* struct tcp_pcb fields (rcv_wnd clamp)  */
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "lwip/err.h"

/* Longest hostname accepted from the host, and the bound on the NUL scan. */
#define NET_MAX_HOSTNAME 256u

typedef struct {
   uint8_t           state;         /* net_state_t                          */
   uint8_t           type;          /* NET_TYPE_TCP / NET_TYPE_UDP          */
   uint8_t           last_err;      /* NET_* sticky error for status        */
   bool              rx_eof;        /* peer sent FIN                        */
   bool              rx_parked;     /* a pbuf was ERR_MEM-parked            */
   bool              dns_done;      /* a resolve completed, result waiting  */
   bool              dns_ok;        /* that resolve succeeded               */
   struct altcp_pcb *tpcb;          /* TCP pcb, NULL once freed by lwIP     */
   ip_addr_t         remote_ip;
   uint16_t          remote_port;
   ip_addr_t         dns_ip;        /* dns_gethostbyname target             */
   /* RX ring: single-context (recv_cb and the recv command both run on the
      main loop), so no locking.  count distinguishes full from empty. */
   uint16_t          rx_head;
   uint16_t          rx_tail;
   uint16_t          rx_count;
} net_handle_t;

/* Handle table in BSS (zeroed at boot: all NET_ST_FREE, tpcb NULL - so the
   first reset teardown finds nothing live to abort).  The 8x8 KB RX rings
   are NOINIT to keep them out of the boot-zeroing path; rx_count resets to 0
   on open, so their contents never matter. */
static net_handle_t net_h[NET_MAX_HANDLES];
NOINIT_SECTION static uint8_t net_rx_ring[NET_MAX_HANDLES][NET_RX_RING_SIZE];

static uint8_t  net_source;         /* nIRQ source id (emulator instance)   */
static bool     net_enabled;        /* net_enable=1 in Pi1MHz.cfg           */

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
      if (abort_pcb)
         altcp_abort(pcb);
      else if (altcp_close(pcb) != ERR_OK)
         altcp_abort(pcb);
   }
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

/* ---- command handlers (main-loop context) -------------------------------- */

static uint8_t do_open(net_handle_t *h, uint32_t cp)
{
   uint8_t type = jim_rd8(cp + 1u);
   if (h->state != NET_ST_FREE)
      return NET_ERR_INUSE;
   if (type != NET_TYPE_TCP)        /* UDP arrives in a later stage */
      return NET_ERR_UNSUPPORTED;
   net_handle_reset(h);
   h->type  = type;
   h->state = NET_ST_IDLE;
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

   h->tpcb = altcp_new_ip_type(NULL, IPADDR_TYPE_V4);
   if (h->tpcb == NULL)
      return NET_ERR_NOMEM;
#if !LWIP_ALTCP
   /* Advertise a window no larger than the ring so the peer can't outrun the
      Beeb's drain (the ERR_MEM park keeps it drop-free even if lwIP resets
      these; this just cuts wasted retransmits).  Direct field access is a
      LWIP_ALTCP==0 shortcut - revisit for TLS.  TODO: Wireshark-verify. */
   h->tpcb->rcv_wnd     = NET_RX_RING_SIZE;
   h->tpcb->rcv_ann_wnd = NET_RX_RING_SIZE;
#endif
   altcp_arg (h->tpcb, h);
   altcp_recv(h->tpcb, net_tcp_recv);
   altcp_sent(h->tpcb, net_tcp_sent);
   altcp_poll(h->tpcb, net_tcp_poll, 4u);
   altcp_err (h->tpcb, net_tcp_err);

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
   if (got == 0u && h->rx_count == 0u && h->rx_eof)
      return NET_EOF;
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
   if (h->state == NET_ST_FREE)
      return NET_OK;
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
      case NET_CMD_OPEN:       return do_open(h, cp);
      case NET_CMD_DNS:        return do_dns(h, cp);
      case NET_CMD_CONNECT:    return do_connect(h, cp);
      case NET_CMD_SEND:       return do_send(h, cp);
      case NET_CMD_RECV:       return do_recv(h, cp);
      case NET_CMD_RECV_AVAIL: return do_recv_avail(h, cp);
      case NET_CMD_CLOSE:      return do_close(h);
      case NET_CMD_STATUS:     return do_status(h, cp);
      default:                 return NET_ERR_UNSUPPORTED;
   }
}

/* ---- FIQ latch + main-loop poll ----------------------------------------- */

static void net_service_command(uint32_t command_pointer, uint32_t addr,
                                uint8_t data)
{
   /* FIQ context: latch only.  Publish NET_PENDING so a Beeb that reads the
      result register before the poll runs sees "busy", not a stale value. */
   net_pending_cp   = command_pointer;
   net_pending_addr = addr;
   net_pending_data = data;
   net_pending      = true;
   Pi1MHz_MemoryWrite(addr, NET_PENDING);
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
      net_reset_pending = false;
      net_pending = false;           /* drop any command latched pre-reset */
   }

   if (net_pending) {
      uint32_t cp   = net_pending_cp;
      uint32_t addr = net_pending_addr;
      uint8_t  data = net_pending_data;
      net_pending = false;
      Pi1MHz_MemoryWrite(addr, net_dispatch(cp, data));
   }
}

void net_service_init(uint8_t instance, uint8_t address)
{
   (void)address;                   /* the services framework owns the base */
   net_source = instance;

   {
      const char *v = config_get("net_enable");
      net_enabled = (v != NULL) && (v[0] == '1' || v[0] == 'y' || v[0] == 't');
   }

   /* Defer all pcb teardown to the first poll (see net_service_poll). */
   net_reset_pending = true;
   net_pending       = false;

   /* Both dedupe, so re-running on a BBC reset is safe. */
   (void)services_register(SERVICE_CMD_NET_FIRST, SERVICE_CMD_NET_LAST,
                           net_service_command);
   Pi1MHz_Register_Poll(net_service_poll);

   services_irq_set(net_source, false);   /* start with our nIRQ line clear */
}
