/*
  Host tests for the IP/net service (net_service.c).  lwIP, the Pi1MHz core
  and the services framework are stubbed (stubs/), so the tests drive the real
  FRED command dispatch and replay lwIP events (connected/recv/sent/err/dns)
  to exercise the full socket lifecycle under ASan/UBSan.  Mirrors the
  tests/services harness.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Pi1MHz.h"
#include "ram_emulator.h"
#include "services.h"
#include "net_service.h"
#include "net_tnfs.h"
#include "net_telnet.h"
#include "config.h"
#include "wifi/wifi_lwip.h"
#include "lwip/altcp.h"
#include "lwip/udp.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "lwip/err.h"

/* ---- harness state / stub implementations -------------------------------- */
static Pi1MHz_t g_pi;
Pi1MHz_t *Pi1MHz = &g_pi;

static uint8_t   g_reg[256];
static func_ptr  g_poll;
static service_command_fn g_cmd;
static int       g_nirq_asserted;
uint64_t         g_now_us;        /* the mock system clock (see stubs/rpi/systimer.h) */

void Pi1MHz_MemoryWrite(uint32_t addr, uint8_t data) { g_reg[addr & 0xffu] = data; }
void Pi1MHz_Register_Poll(func_ptr f, const char *name) { (void)name; g_poll = f; }
void Pi1MHz_nIRQ_ASSERT(uint8_t src) { (void)src; g_nirq_asserted = 1; }
void Pi1MHz_nIRQ_CLEAR (uint8_t src) { (void)src; g_nirq_asserted = 0; }

bool services_register(uint8_t first, uint8_t last, service_command_fn h)
{ (void)first; (void)last; g_cmd = h; return true; }
void services_irq_set(uint8_t source, bool asserted)
{ (void)source; g_nirq_asserted = asserted ? 1 : 0; }

static const char *g_net_enable = "1";
const char *config_get(const char *prop)
{ return (strcmp(prop, "net_enable") == 0) ? g_net_enable : NULL; }
bool config_get_bool(const char *key)
{ const char *v = config_get(key);
  return v && (v[0]=='1'||v[0]=='y'||v[0]=='Y'||v[0]=='t'||v[0]=='T'); }

static wifi_lwip_context_t g_ctx;
static int g_kicks;
const wifi_lwip_context_t *wifi_lwip_get_context(void) { return &g_ctx; }
void wifi_lwip_rx_kick(void) { g_kicks++; }

/* altcp stub machinery */
static struct altcp_pcb *g_pcbs[1024];
static int g_npcbs;
static struct altcp_pcb *g_last_pcb;
static int   g_force_new_null;
static u16_t g_default_sndbuf = 4096u;
static err_t g_connect_ret = ERR_OK;
static uint8_t g_tx[65536];
static uint32_t g_tx_len;

struct altcp_pcb *altcp_new_ip_type(void *a, u8_t t)
{
   (void)a; (void)t;
   if (g_force_new_null) return NULL;
   struct altcp_pcb *p = calloc(1, sizeof *p);
   p->t_sndbuf = g_default_sndbuf;
   p->t_write_err = ERR_OK;
   g_pcbs[g_npcbs++] = p;
   g_last_pcb = p;
   return p;
}
void altcp_arg (struct altcp_pcb *c, void *a) { c->arg = a; }
void altcp_recv(struct altcp_pcb *c, altcp_recv_fn f) { c->recv = f; }
void altcp_sent(struct altcp_pcb *c, altcp_sent_fn f) { c->sent = f; }
void altcp_poll(struct altcp_pcb *c, altcp_poll_fn f, u8_t i) { (void)i; c->poll = f; }
void altcp_err (struct altcp_pcb *c, altcp_err_fn f) { c->err = f; }
err_t altcp_connect(struct altcp_pcb *c, const ip_addr_t *ip, u16_t port,
                    altcp_connected_fn f)
{ (void)ip; (void)port; c->connected = f; return g_connect_ret; }
err_t altcp_bind(struct altcp_pcb *c, const ip_addr_t *ip, u16_t port)
{ (void)ip; c->bound_port = port; return ERR_OK; }
struct altcp_pcb *altcp_listen(struct altcp_pcb *c) { c->listening = 1; return c; }
void altcp_accept(struct altcp_pcb *c, altcp_accept_fn f) { c->accept = f; }
u16_t altcp_sndbuf(struct altcp_pcb *c) { return c->t_sndbuf; }
err_t altcp_write(struct altcp_pcb *c, const void *d, u16_t len, u8_t fl)
{
   (void)fl;
   if (c->t_write_err != ERR_OK) return c->t_write_err;
   if (g_tx_len + len <= sizeof g_tx) { memcpy(g_tx + g_tx_len, d, len); g_tx_len += len; }
   return ERR_OK;
}
void altcp_output(struct altcp_pcb *c) { (void)c; }
void altcp_recved(struct altcp_pcb *c, u16_t len) { c->t_recved += len; }
err_t altcp_close(struct altcp_pcb *c) { c->t_closed = 1; return ERR_OK; }
void altcp_abort(struct altcp_pcb *c) { c->t_closed = 1; }

/* udp stub machinery */
static struct udp_pcb *g_upcbs[64];
static int g_nupcbs;
static struct udp_pcb *g_last_upcb;
static uint8_t  g_udp_tx[2048];
static uint16_t g_udp_tx_len;
static u32_t    g_udp_tx_ip;
static u16_t    g_udp_tx_port;

struct udp_pcb *udp_new(void)
{
   struct udp_pcb *p = calloc(1, sizeof *p);
   g_upcbs[g_nupcbs++] = p; g_last_upcb = p; return p;
}
err_t udp_bind(struct udp_pcb *pcb, const ip_addr_t *ip, u16_t port)
{ (void)ip; pcb->bound_port = port; return ERR_OK; }
void udp_recv(struct udp_pcb *pcb, udp_recv_fn f, void *arg)
{ pcb->recv = f; pcb->arg = arg; }
err_t udp_sendto(struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *dst,
                 u16_t port)
{
   (void)pcb;
   g_udp_tx_len = 0;
   for (struct pbuf *q = p; q; q = q->next) {
      if (g_udp_tx_len + q->len <= sizeof g_udp_tx)
      { memcpy(g_udp_tx + g_udp_tx_len, q->payload, q->len); g_udp_tx_len += q->len; }
   }
   g_udp_tx_ip = dst ? dst->addr : 0; g_udp_tx_port = port;
   return ERR_OK;                 /* does NOT free p (caller frees) */
}
err_t udp_connect(struct udp_pcb *pcb, const ip_addr_t *ip, u16_t port)
{ pcb->connected_ip = ip ? ip->addr : 0u; pcb->connected_port = port; return ERR_OK; }
void udp_disconnect(struct udp_pcb *pcb) { pcb->connected_port = 0u; }
void udp_remove(struct udp_pcb *pcb) { pcb->removed = 1; }

/* dns stub */
static int       g_dns_sync;
static ip_addr_t g_dns_result;
static const char *g_dns_name;
static dns_found_callback g_dns_cb;
static void      *g_dns_arg;
err_t dns_gethostbyname(const char *name, ip_addr_t *addr, dns_found_callback cb,
                        void *arg)
{
   g_dns_name = name; g_dns_cb = cb; g_dns_arg = arg;
   if (g_dns_sync) { *addr = g_dns_result; return ERR_OK; }
   return ERR_INPROGRESS;
}

/* pbuf stub */
static int g_pbuf_live;
static struct pbuf *make_pbuf(const void *data, u16_t len)
{
   struct pbuf *p = calloc(1, sizeof *p);
   p->payload = malloc(len ? len : 1u);
   if (data) memcpy(p->payload, data, len);
   p->len = p->tot_len = len;
   g_pbuf_live++;
   return p;
}

/* Build a pbuf CHAIN from one buffer, cutting it into <=seg-byte links, so the
   tests can exercise the multi-pbuf path (real lwIP hands segments as chains).
   tot_len on the head is the whole length, as lwIP guarantees. */
static struct pbuf *make_pbuf_split(const void *data, u16_t len, u16_t seg)
{
   const uint8_t *src = data;
   struct pbuf *head = NULL, *tail = NULL;
   u16_t done = 0;
   if (seg == 0u) seg = 1u;
   do {
      u16_t n = (u16_t)(len - done);
      struct pbuf *p;
      if (n > seg) n = seg;
      p = make_pbuf(src ? src + done : NULL, n);
      p->tot_len = (u16_t)(len - done);         /* remaining length, lwIP-style */
      if (tail) tail->next = p; else head = p;
      tail = p;
      done = (u16_t)(done + n);
   } while (done < len);
   return head;
}

/* Chain-walking copy, matching lwIP's pbuf_copy_partial (the earlier single-
   buffer stub silently mis-read chains). */
u16_t pbuf_copy_partial(const struct pbuf *p, void *dst, u16_t len, u16_t offset)
{
   uint8_t *out = dst;
   u16_t copied = 0;
   if (p == NULL || dst == NULL) return 0u;
   for (; len != 0u && p != NULL; p = p->next) {
      if (offset >= p->len) { offset = (u16_t)(offset - p->len); continue; }
      u16_t n = (u16_t)(p->len - offset);
      if (n > len) n = len;
      memcpy(out + copied, (const uint8_t *)p->payload + offset, n);
      copied = (u16_t)(copied + n);
      len = (u16_t)(len - n);
      offset = 0u;
   }
   return copied;
}
u8_t pbuf_free(struct pbuf *p)
{
   while (p) { struct pbuf *n = p->next; free(p->payload); free(p); g_pbuf_live--; p = n; }
   return 1;
}
struct pbuf *pbuf_alloc(pbuf_layer l, u16_t len, pbuf_type t)
{ (void)l; (void)t; return make_pbuf(NULL, len); }
err_t pbuf_take(struct pbuf *b, const void *data, u16_t len)
{ memcpy(b->payload, data, len); return ERR_OK; }

/* ---- test framework ------------------------------------------------------ */
static int checks, fails;
#define CHECK(cond, msg) do { checks++; \
   if (!(cond)) { fails++; printf("  FAIL: %s\n", (msg)); } \
   else printf("  ok: %s\n", (msg)); } while (0)

#define RES 0xA6u
static uint32_t CP(unsigned h) { return DISC_RAM_BASE + 0x100u + h * 0x100u; }

static void jwr8 (uint32_t o, uint8_t v)  { Pi1MHz->JIM_ram[o] = v; }
static void jwr24(uint32_t o, uint32_t v) { uint8_t *p=&Pi1MHz->JIM_ram[o]; p[0]=v; p[1]=v>>8; p[2]=v>>16; }
static void jwr32(uint32_t o, uint32_t v) { uint8_t *p=&Pi1MHz->JIM_ram[o]; p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static uint8_t  jrd8 (uint32_t o) { return Pi1MHz->JIM_ram[o]; }
static uint32_t jrd24(uint32_t o) { const uint8_t*p=&Pi1MHz->JIM_ram[o]; return p[0]|(p[1]<<8)|(p[2]<<16); }

/* Issue a command on handle h and return the result byte. */
static uint8_t issue(uint8_t cmd, unsigned h)
{
   jwr8(CP(h), cmd);
   g_cmd(CP(h), RES, (uint8_t)h);
   g_poll();
   return g_reg[RES];
}

static void world_reset(void)
{
   /* Do NOT free pcbs here: the net_h handle table (static in net_service.c)
      still points at the previous test's pcbs, and the reset teardown run by
      net_service_init below will abort them - freeing first would be a
      use-after-free.  Every pcb ever allocated is tracked in g_pcbs and freed
      once at program exit. */
   memset(&g_pi, 0, sizeof g_pi);
   memset(g_reg, 0, sizeof g_reg);
   g_tx_len = 0; g_kicks = 0; g_force_new_null = 0;
   g_default_sndbuf = 4096u; g_connect_ret = ERR_OK; g_dns_sync = 0;
   g_net_enable = "1"; g_ctx.address_ready = true;
   net_service_init(3u, 0u);
   g_poll();                 /* clear the initial reset_pending */
}

/* Open handle h as TCP and drive it to CONNECTED. */
static void connect_handle(unsigned h)
{
   jwr8(CP(h) + 1u, NET_TYPE_TCP); issue(NET_CMD_OPEN, h);
   jwr8(CP(h)+1,1); jwr8(CP(h)+2,2); jwr8(CP(h)+3,3); jwr8(CP(h)+4,4);
   jwr8(CP(h)+5,0x50); jwr8(CP(h)+6,0);   /* 1.2.3.4:80 */
   issue(NET_CMD_CONNECT, h);             /* -> CONNECTING */
   g_last_pcb->connected(g_last_pcb->arg, g_last_pcb, ERR_OK);
   issue(NET_CMD_CONNECT, h);             /* poll -> CONNECTED */
}

int main(void)
{
   printf("== net service: open / status / gate ==\n");
   world_reset();
   jwr8(CP(0) + 1u, NET_TYPE_TCP);
   CHECK(issue(NET_CMD_OPEN, 0) == NET_OK, "open TCP handle 0 -> OK");
   CHECK(issue(NET_CMD_STATUS, 0) == NET_OK, "status -> OK");
   CHECK(jrd8(CP(0) + 1u) == NET_ST_IDLE, "status state = IDLE after open");
   jwr8(CP(0) + 1u, NET_TYPE_TCP);
   CHECK(issue(NET_CMD_OPEN, 0) == NET_ERR_INUSE, "re-open same handle -> INUSE");
   jwr8(CP(1) + 1u, NET_TYPE_UDP);
   CHECK(issue(NET_CMD_OPEN, 1) == NET_OK, "open UDP -> OK");
   CHECK(issue(NET_CMD_LISTEN, 1) == NET_ERR_NOTOPEN, "listen on a UDP handle -> NOTOPEN");

   printf("== disabled gate ==\n");
   world_reset();
   g_net_enable = NULL; net_service_init(3u, 0u); g_poll();
   jwr8(CP(0) + 1u, NET_TYPE_TCP);
   CHECK(issue(NET_CMD_OPEN, 0) == NET_ERR_DISABLED, "net_enable off -> DISABLED");

   printf("== connect lifecycle ==\n");
   world_reset();
   jwr8(CP(0) + 1u, NET_TYPE_TCP); issue(NET_CMD_OPEN, 0);
   g_ctx.address_ready = false;
   CHECK(issue(NET_CMD_CONNECT, 0) == NET_PENDING, "connect with no IP -> PENDING (no pcb)");
   CHECK(g_npcbs == 0, "no pcb created before address_ready");
   g_ctx.address_ready = true;
   jwr8(CP(0)+1,93); jwr8(CP(0)+2,184); jwr8(CP(0)+3,216); jwr8(CP(0)+4,34);
   jwr8(CP(0)+5,0x50); jwr8(CP(0)+6,0);
   CHECK(issue(NET_CMD_CONNECT, 0) == NET_PENDING, "connect -> PENDING (CONNECTING)");
   CHECK(g_npcbs == 1 && g_last_pcb->connected != NULL, "pcb created, connected cb registered");
   /* rcv_wnd is deliberately LEFT ALONE. Narrowing it to the ring size did
    * nothing on this path anyway (real lwIP's tcp_connect reassigns
    * rcv_wnd/rcv_ann_wnd from TCP_WND, which this stub does not model), and
    * on an accepted pcb it made tcp_close_shutdown() send an RST instead of
    * a FIN - rcv_wnd != TCP_WND_MAX(pcb) selects the reset branch - so every
    * close of an inbound connection discarded queued data. The ERR_MEM park
    * in net_tcp_recv() is the real back-pressure. */
   CHECK(g_last_pcb->rcv_wnd != NET_RX_RING_SIZE, "rcv_wnd left at the lwIP default");
   g_last_pcb->connected(g_last_pcb->arg, g_last_pcb, ERR_OK);
   CHECK(issue(NET_CMD_CONNECT, 0) == NET_OK, "after connected cb -> OK");
   issue(NET_CMD_STATUS, 0);
   CHECK(jrd8(CP(0)+1) == NET_ST_CONNECTED, "status state CONNECTED");
   CHECK((jrd8(CP(0)+2) & NET_FLAG_CONNECTED) != 0, "status flag CONNECTED set");
   CHECK(jrd8(CP(0)+3)==93 && jrd8(CP(0)+6)==34, "status reports remote IP 93.184.216.34");

   printf("== connect failure ==\n");
   world_reset();
   jwr8(CP(0)+1, NET_TYPE_TCP); issue(NET_CMD_OPEN, 0);
   jwr8(CP(0)+1,1); jwr8(CP(0)+5,0x50);
   issue(NET_CMD_CONNECT, 0);
   g_last_pcb->connected(g_last_pcb->arg, g_last_pcb, ERR_ABRT);  /* refused */
   CHECK(issue(NET_CMD_CONNECT, 0) == NET_ERR_TCP_ABORT,
         "connect abort retains lwIP cause");

   printf("== send ==\n");
   world_reset();
   connect_handle(0);
   {
      static const uint8_t payload[] = "GET / HTTP/1.0\r\n\r\n";
      uint32_t len = (uint32_t)sizeof payload - 1u;
      memcpy(&Pi1MHz->JIM_ram[0x8000], payload, len);
      jwr24(CP(0)+1, len); jwr32(CP(0)+4, 0x8000);
      CHECK(issue(NET_CMD_SEND, 0) == NET_OK, "send -> OK");
      CHECK(jrd24(CP(0)+1) == len, "send reports full length queued");
      CHECK(g_tx_len == len && memcmp(g_tx, payload, len) == 0, "TX bytes match payload");
      CHECK(g_kicks > 0, "send kicked the RX drain");
   }
   g_last_pcb->t_sndbuf = 0;
   jwr24(CP(0)+1, 10); jwr32(CP(0)+4, 0x8000);
   issue(NET_CMD_SEND, 0);
   CHECK(jrd24(CP(0)+1) == 0, "send with full sndbuf reports 0 queued");

   printf("== recv + ring ==\n");
   world_reset();
   connect_handle(0);
   {
      static const uint8_t data[] = "HTTP/1.0 200 OK";
      uint16_t len = (uint16_t)(sizeof data - 1u);
      struct pbuf *p = make_pbuf(data, len);
      err_t r = g_last_pcb->recv(g_last_pcb->arg, g_last_pcb, p, ERR_OK);
      CHECK(r == ERR_OK, "recv_cb accepted the segment");
      CHECK(g_last_pcb->t_recved == len, "recv_cb acked the whole segment");
      issue(NET_CMD_RECV_AVAIL, 0);
      CHECK(jrd24(CP(0)+1) == len, "recv_avail reports the buffered bytes");
      jwr24(CP(0)+1, 64); jwr32(CP(0)+4, 0x9000);
      CHECK(issue(NET_CMD_RECV, 0) == NET_OK, "recv -> OK");
      CHECK(jrd24(CP(0)+1) == len, "recv returns the byte count");
      CHECK(memcmp(&Pi1MHz->JIM_ram[0x9000], data, len) == 0, "recv copied the bytes to JIM");
      jwr24(CP(0)+1, 64); jwr32(CP(0)+4, 0x9000);
      issue(NET_CMD_RECV, 0);
      CHECK(jrd24(CP(0)+1) == 0, "second recv returns 0 (ring drained)");
   }

   printf("== back-pressure (ERR_MEM park, redeliver) ==\n");
   world_reset();
   connect_handle(0);
   {
      uint8_t *big = malloc(6000); memset(big, 'A', 6000);
      struct pbuf *p1 = make_pbuf(big, 6000);
      CHECK(g_last_pcb->recv(g_last_pcb->arg, g_last_pcb, p1, ERR_OK) == ERR_OK,
            "first 6000-byte segment fits");
      struct pbuf *p2 = make_pbuf(big, 6000);
      CHECK(g_last_pcb->recv(g_last_pcb->arg, g_last_pcb, p2, ERR_OK) == ERR_MEM,
            "second segment does not fit -> ERR_MEM (parked)");
      issue(NET_CMD_RECV_AVAIL, 0);
      CHECK(jrd24(CP(0)+1) == 6000, "ring still holds only the first segment");
      jwr24(CP(0)+1, 6000); jwr32(CP(0)+4, 0x2000);
      issue(NET_CMD_RECV, 0);
      CHECK(jrd24(CP(0)+1) == 6000, "drained the first segment");
      CHECK(g_last_pcb->recv(g_last_pcb->arg, g_last_pcb, p2, ERR_OK) == ERR_OK,
            "redelivered parked segment now fits");
      free(big);
   }

   printf("== FIN / EOF ==\n");
   world_reset();
   connect_handle(0);
   {
      static const uint8_t tail[] = "bye";
      struct pbuf *p = make_pbuf(tail, 3);
      g_last_pcb->recv(g_last_pcb->arg, g_last_pcb, p, ERR_OK);
      CHECK(g_last_pcb->recv(g_last_pcb->arg, g_last_pcb, NULL, ERR_OK) == ERR_OK,
            "recv_cb NULL pbuf (FIN) accepted");
      jwr24(CP(0)+1, 64); jwr32(CP(0)+4, 0x9000);
      CHECK(issue(NET_CMD_RECV, 0) == NET_OK, "recv drains the last 3 bytes -> OK");
      CHECK(jrd24(CP(0)+1) == 3, "got the 3 trailing bytes");
      jwr24(CP(0)+1, 64); jwr32(CP(0)+4, 0x9000);
      CHECK(issue(NET_CMD_RECV, 0) == NET_EOF, "recv after drain + FIN -> NET_EOF");
      issue(NET_CMD_STATUS, 0);
      CHECK((jrd8(CP(0)+2) & NET_FLAG_RX_EOF) != 0, "status flag RX_EOF set");
   }

   printf("== reset surfaces on recv ==\n");
   world_reset();
   connect_handle(0);
   {
      static const uint8_t part[] = "data";
      struct pbuf *p = make_pbuf(part, 4);
      g_last_pcb->recv(g_last_pcb->arg, g_last_pcb, p, ERR_OK);
      /* peer RSTs: lwIP frees the pcb and calls the err callback (no FIN) */
      g_last_pcb->err(g_last_pcb->arg, ERR_ABRT);
      jwr24(CP(0)+1, 64); jwr32(CP(0)+4, 0x9000);
      CHECK(issue(NET_CMD_RECV, 0) == NET_OK, "recv drains buffered bytes despite reset -> OK");
      CHECK(jrd24(CP(0)+1) == 4, "got the 4 buffered bytes before signalling the error");
      jwr24(CP(0)+1, 64); jwr32(CP(0)+4, 0x9000);
      CHECK(issue(NET_CMD_RECV, 0) == NET_ERR_TCP_ABORT,
            "recv after drain retains lwIP abort cause");
   }

   printf("== DNS ==\n");
   world_reset();
   jwr8(CP(0)+1, NET_TYPE_TCP); issue(NET_CMD_OPEN, 0);
   g_dns_sync = 1; IP_ADDR4(&g_dns_result, 8, 8, 8, 8);
   memcpy(&Pi1MHz->JIM_ram[CP(0)+1], "dns.test", 9);
   CHECK(issue(NET_CMD_DNS, 0) == NET_OK, "dns cache hit -> OK");
   CHECK(jrd8(CP(0)+4)==8 && jrd8(CP(0)+7)==8, "resolved IP 8.8.8.8 written back");
   world_reset();
   jwr8(CP(0)+1, NET_TYPE_TCP); issue(NET_CMD_OPEN, 0);
   g_dns_sync = 0;
   memcpy(&Pi1MHz->JIM_ram[CP(0)+1], "slow.test", 10);
   CHECK(issue(NET_CMD_DNS, 0) == NET_PENDING, "dns async -> PENDING");
   {
      ip_addr_t ip; IP_ADDR4(&ip, 1, 1, 1, 1);
      g_dns_cb(g_dns_name, &ip, g_dns_arg);
   }
   CHECK(issue(NET_CMD_DNS, 0) == NET_OK, "dns after callback -> OK");
   CHECK(jrd8(CP(0)+4)==1 && jrd8(CP(0)+7)==1, "resolved async IP 1.1.1.1 written back");

   printf("== bounds checks ==\n");
   world_reset();
   connect_handle(0);
   jwr24(CP(0)+1, 100); jwr32(CP(0)+4, 0xFFFFF0);   /* offset past region */
   CHECK(issue(NET_CMD_SEND, 0) == NET_ERR_PARAM, "send with OOB JIM offset -> PARAM");
   jwr24(CP(0)+1, 100); jwr32(CP(0)+4, 0xFFFFF0);
   CHECK(issue(NET_CMD_RECV, 0) == NET_ERR_PARAM, "recv with OOB JIM offset -> PARAM");

   printf("== close ==\n");
   world_reset();
   connect_handle(0);
   {
      struct altcp_pcb *pcb = g_last_pcb;
      CHECK(issue(NET_CMD_CLOSE, 0) == NET_OK, "close -> OK");
      CHECK(pcb->t_closed == 1, "pcb was closed");
      issue(NET_CMD_STATUS, 0);
      CHECK(jrd8(CP(0)+1) == NET_ST_FREE, "handle FREE after close");
   }

   printf("== BBC reset teardown ==\n");
   world_reset();
   connect_handle(0);
   {
      struct altcp_pcb *pcb = g_last_pcb;
      net_service_init(3u, 0u);     /* simulate a BBC reset re-running init */
      g_poll();                     /* first poll does the teardown */
      CHECK(pcb->t_closed == 1, "reset aborted the live pcb");
      issue(NET_CMD_STATUS, 0);
      CHECK(jrd8(CP(0)+1) == NET_ST_FREE, "handle FREE after reset");
   }

   printf("== UDP send ==\n");
   world_reset();
   jwr8(CP(2)+1, NET_TYPE_UDP);
   CHECK(issue(NET_CMD_OPEN, 2) == NET_OK, "open UDP -> OK");
   CHECK(g_last_upcb != NULL && g_last_upcb->recv != NULL, "udp pcb created + recv registered");
   {
      static const uint8_t dg[] = "ntp-request";
      uint16_t len = (uint16_t)(sizeof dg - 1u);
      memcpy(&Pi1MHz->JIM_ram[0x8000], dg, len);
      jwr8(CP(2)+1,192); jwr8(CP(2)+2,168); jwr8(CP(2)+3,0); jwr8(CP(2)+4,1);
      jwr8(CP(2)+5,123); jwr8(CP(2)+6,0);        /* :123 */
      jwr24(CP(2)+7, len); jwr32(CP(2)+10, 0x8000);
      CHECK(issue(NET_CMD_UDP_SENDTO, 2) == NET_OK, "udp_sendto -> OK");
      CHECK(g_udp_tx_len == len && memcmp(g_udp_tx, dg, len) == 0, "datagram payload sent");
      CHECK(g_udp_tx_port == 123, "datagram sent to port 123");
      CHECK(g_udp_tx_ip == (192u | (168u<<8) | (0u<<16) | (1u<<24)), "datagram sent to 192.168.0.1");
   }

   printf("== UDP recvfrom ==\n");
   world_reset();
   jwr8(CP(2)+1, NET_TYPE_UDP); issue(NET_CMD_OPEN, 2);
   jwr8(CP(2)+1, 88); jwr8(CP(2)+2, 0); issue(NET_CMD_BIND, 2);   /* bind :88 */
   {
      static const uint8_t reply[] = "PONG";
      struct pbuf *p = make_pbuf(reply, 4);
      ip_addr_t peer; IP_ADDR4(&peer, 10, 0, 0, 5);
      g_last_upcb->recv(g_last_upcb->arg, g_last_upcb, p, &peer, 4000);
      jwr24(CP(2)+7, 64); jwr32(CP(2)+10, 0x9000);
      CHECK(issue(NET_CMD_UDP_RECVFROM, 2) == NET_OK, "udp_recvfrom -> OK");
      CHECK(jrd8(CP(2)+1)==10 && jrd8(CP(2)+4)==5, "peer IP 10.0.0.5 reported");
      CHECK((jrd8(CP(2)+5) | (jrd8(CP(2)+6)<<8)) == 4000, "peer port 4000 reported");
      CHECK(jrd24(CP(2)+7) == 4, "datagram length 4 reported");
      CHECK(memcmp(&Pi1MHz->JIM_ram[0x9000], reply, 4) == 0, "datagram payload copied to JIM");
      jwr24(CP(2)+7, 64); jwr32(CP(2)+10, 0x9000);
      issue(NET_CMD_UDP_RECVFROM, 2);
      CHECK(jrd24(CP(2)+7) == 0, "second recvfrom -> length 0 (empty)");
   }

   printf("== nIRQ is opt-in (disarmed by default) ==\n");
   world_reset();
   connect_handle(0);
   CHECK(g_nirq_asserted == 0, "nIRQ clear with no data");
   {
      static const uint8_t data[] = "async-data";
      struct pbuf *p = make_pbuf(data, 10);
      g_last_pcb->recv(g_last_pcb->arg, g_last_pcb, p, ERR_OK);
      g_poll();
      /* Disarmed default: buffered RX must NOT raise nIRQ (a polling client
         installs no handler; a stuck level-triggered nIRQ freezes the Beeb). */
      CHECK(g_nirq_asserted == 0, "disarmed: buffered RX does NOT assert nIRQ");

      jwr8(CP(0)+1, 1); issue(NET_CMD_IRQ, 0);   /* arm */
      CHECK(g_nirq_asserted == 1, "armed: nIRQ asserts while RX is buffered");
      jwr24(CP(0)+1, 64); jwr32(CP(0)+4, 0x9000);
      issue(NET_CMD_RECV, 0);          /* drain (poll recomputes nIRQ) */
      CHECK(g_nirq_asserted == 0, "nIRQ cleared after the Beeb drains");

      /* re-buffer, then disarm: the line must drop even with data waiting */
      struct pbuf *p2 = make_pbuf(data, 10);
      g_last_pcb->recv(g_last_pcb->arg, g_last_pcb, p2, ERR_OK);
      g_poll();
      CHECK(g_nirq_asserted == 1, "still armed: nIRQ back up with new RX");
      jwr8(CP(0)+1, 0); issue(NET_CMD_IRQ, 0);   /* disarm */
      CHECK(g_nirq_asserted == 0, "disarm drops nIRQ even with RX buffered");
   }

   printf("== listen / accept ==\n");
   world_reset();
   jwr8(CP(0)+1, NET_TYPE_TCP); issue(NET_CMD_OPEN, 0);
   jwr8(CP(0)+1, 0x40); jwr8(CP(0)+2, 0x1F);        /* bind port 8000 = 0x1F40 */
   CHECK(issue(NET_CMD_BIND, 0) == NET_OK, "bind port 8000 -> OK");
   CHECK(issue(NET_CMD_LISTEN, 0) == NET_PENDING, "listen -> PENDING (listener up)");
   {
      struct altcp_pcb *lp = g_last_pcb;
      struct altcp_pcb *np;
      uint8_t nh;
      CHECK(lp->listening && lp->accept != NULL, "listen pcb created, accept cb set");
      CHECK(lp->bound_port == 8000u, "listener bound to port 8000");
      CHECK(issue(NET_CMD_LISTEN, 0) == NET_PENDING, "listen poll -> PENDING (no conn yet)");
      np = altcp_new_ip_type(NULL, 0u);              /* simulate an inbound conn */
      CHECK(lp->accept(lp->arg, np, ERR_OK) == ERR_OK, "accept cb accepted the connection");
      CHECK(issue(NET_CMD_LISTEN, 0) == NET_OK, "listen poll -> OK (connection ready)");
      nh = jrd8(CP(0)+1);
      CHECK(nh != 0u && nh < NET_MAX_HANDLES, "yielded a fresh handle index");
      CHECK(np->arg != NULL, "new pcb's arg wired to the accepted handle");
      issue(NET_CMD_STATUS, nh);
      CHECK(jrd8(CP(nh)+1) == NET_ST_CONNECTED, "accepted handle is CONNECTED");
   }

   printf("== stress: listen/accept/reset churn (teardown invariants) ==\n");
   {
      int stuck = 0, leaked = 0, base = g_npcbs;
      for (int cycle = 0; cycle < 40; cycle++) {
         world_reset();
         jwr8(CP(0)+1, NET_TYPE_TCP); issue(NET_CMD_OPEN, 0);
         jwr8(CP(0)+1, 0x40); jwr8(CP(0)+2, 0x1F);
         issue(NET_CMD_BIND, 0);
         issue(NET_CMD_LISTEN, 0);                 /* listener up */
         struct altcp_pcb *lp = g_last_pcb;
         /* fire several inbound connections; collect some, leave some in the
            backlog, feed one a segment - then abandon the lot for teardown */
         for (int k = 0; k < 6; k++) {
            struct altcp_pcb *np = altcp_new_ip_type(NULL, 0u);
            lp->accept(lp->arg, np, ERR_OK);        /* accepted or refused */
            if ((k & 1) && np->recv)                /* poke half with data */
               np->recv(np->arg, np, make_pbuf("x", 1), ERR_OK);
            if (k == 2) issue(NET_CMD_LISTEN, 0);   /* collect one accepted handle */
         }
         /* no closes: the next world_reset()'s teardown must reclaim it all */
      }
      world_reset();                               /* final teardown */
      for (unsigned i = 0; i < NET_MAX_HANDLES; i++) {
         issue(NET_CMD_STATUS, i);
         if (jrd8(CP(i)+1) != NET_ST_FREE) stuck++;
      }
      for (int i = base; i < g_npcbs; i++)
         if (!g_pcbs[i]->t_closed) leaked++;
      CHECK(stuck == 0, "every handle back to FREE after listen/accept churn");
      CHECK(leaked == 0, "every TCP pcb closed/aborted after listen/accept churn");
   }

   printf("== reset teardown must not drop a command latched during it ==\n");
   /* The hardware wedge: a BBC reset arms the teardown, and if the FIQ latches
      a command (writing NET_BUSY) *during* the teardown pass, the old code
      cleared net_pending and the command was never dispatched - NET_BUSY stayed
      set in the result register forever and the Beeb spun on bit 7.  Reproduce
      it: re-arm net_reset_pending with a bare net_service_init (no trailing
      poll, unlike world_reset), so the next issue() latches AND tears down in
      the same poll. */
   world_reset();
   net_service_init(3u, 0u);
   jwr8(CP(0)+1, NET_TYPE_TCP);
   CHECK(issue(NET_CMD_OPEN, 0) == NET_OK,
         "command latched during reset teardown is dispatched, not dropped (no stuck NET_BUSY)");
   issue(NET_CMD_STATUS, 0);
   CHECK(jrd8(CP(0)+1) == NET_ST_IDLE, "and the handle actually opened through the teardown");

   printf("== N: device - TCP scheme ==\n");
   world_reset();
   strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u], "TCP://1.2.3.4:5000");
   { int before = g_npcbs;
     CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_PENDING, "url_open TCP:// -> PENDING (connecting)");
     CHECK(g_npcbs == before + 1, "dotted IP skipped DNS, went straight to connect"); }
   g_last_pcb->connected(g_last_pcb->arg, g_last_pcb, ERR_OK);
   CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_OK, "url_open TCP:// -> OK after connect");
   issue(NET_CMD_URL_STATUS, 0);
   CHECK(jrd8(CP(0)+3) == 1u, "url status CONNECTED");

   printf("== N: device - UDP scheme ==\n");
   world_reset();
   strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u], "UDP://192.168.0.9:5300");
   CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_OK, "url_open UDP:// -> OK (connectionless, no connect)");
   CHECK(g_last_upcb != NULL && g_last_upcb->recv != NULL, "UDP url created a bound pcb + recv cb");
   issue(NET_CMD_URL_STATUS, 0);
   CHECK(jrd8(CP(0)+3) == 1u, "UDP url status ready (CONNECTED flag)");
   {
      static const uint8_t msg[]   = "hello-udp-url";
      static const uint8_t reply[] = "UDP-REPLY";
      uint16_t len = (uint16_t)(sizeof msg - 1u);
      struct pbuf *p;
      ip_addr_t peer;
      memcpy(&Pi1MHz->JIM_ram[0x8000], msg, len);
      jwr24(CP(0)+1, len); jwr32(CP(0)+4, 0x8000);
      CHECK(issue(NET_CMD_URL_WRITE, 0) == NET_OK, "url_write UDP -> OK");
      CHECK(g_udp_tx_len == len && memcmp(g_udp_tx, msg, len) == 0, "url_write sent the payload");
      CHECK(g_udp_tx_port == 5300, "url_write went to the URL port 5300");
      CHECK(g_udp_tx_ip == (192u|(168u<<8)|(0u<<16)|(9u<<24)), "url_write went to the URL host");
      /* inbound datagram -> url_read returns the payload only (peer header dropped) */
      p = make_pbuf(reply, 9);
      IP_ADDR4(&peer, 192, 168, 0, 9);
      g_last_upcb->recv(g_last_upcb->arg, g_last_upcb, p, &peer, 5300);
      jwr24(CP(0)+1, 64); jwr32(CP(0)+4, 0x9000);
      CHECK(issue(NET_CMD_URL_READ, 0) == NET_OK, "url_read UDP -> OK");
      CHECK(jrd24(CP(0)+1) == 9, "url_read reported the datagram length");
      CHECK(memcmp(&Pi1MHz->JIM_ram[0x9000], reply, 9) == 0, "url_read returned the payload, no peer header");
      jwr24(CP(0)+1, 64); jwr32(CP(0)+4, 0x9000);
      issue(NET_CMD_URL_READ, 0);
      CHECK(jrd24(CP(0)+1) == 0, "second url_read -> 0 (ring drained)");
      CHECK(issue(NET_CMD_URL_CLOSE, 0) == NET_OK, "url_close UDP -> OK");
      CHECK(g_last_upcb->removed == 1, "UDP url pcb removed on close");
   }

   printf("== N: device - TNFS scheme (mount/open/read/close) ==\n");
   world_reset();
   strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u], "TNFS://192.168.0.9/games/game.dsk");
   CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_PENDING, "url_open TNFS -> PENDING (MOUNT sent)");
   CHECK(g_last_upcb != NULL, "TNFS opened a UDP socket");
   CHECK(g_udp_tx_port == (u16_t)TNFS_PORT, "MOUNT sent to TNFS port 16384");
   CHECK(g_udp_tx[3] == TNFS_CMD_MOUNT, "first datagram is MOUNT");
   {
      ip_addr_t peer; IP_ADDR4(&peer, 192, 168, 0, 9);
      uint8_t sm = g_udp_tx[2];
      uint8_t mrep[] = { 0x42,0x00, sm, TNFS_CMD_MOUNT, TNFS_OK, 0x02,0x01, 0x2C,0x01 };
      g_last_upcb->recv(g_last_upcb->arg, g_last_upcb, make_pbuf(mrep, sizeof mrep), &peer, (u16_t)TNFS_PORT);
      CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_PENDING, "after MOUNT reply -> PENDING (OPEN sent)");
      CHECK(g_udp_tx[3] == TNFS_CMD_OPEN, "second datagram is OPEN");
      CHECK((g_udp_tx[0] | (g_udp_tx[1] << 8)) == 0x0042, "OPEN carries the session id from MOUNT");
      {
         uint8_t so = g_udp_tx[2];
         uint8_t orep[] = { 0x42,0x00, so, TNFS_CMD_OPEN, TNFS_OK, 0x07 };
         g_last_upcb->recv(g_last_upcb->arg, g_last_upcb, make_pbuf(orep, sizeof orep), &peer, (u16_t)TNFS_PORT);
      }
      CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_OK, "after OPEN reply -> READY (OK)");
      issue(NET_CMD_URL_STATUS, 0);
      CHECK(jrd8(CP(0)+3) == 1u, "TNFS url status READY");

      /* url_read: first call sends a READ, second delivers the data */
      jwr24(CP(0)+1, 64); jwr32(CP(0)+4, 0x9000);
      CHECK(issue(NET_CMD_URL_READ, 0) == NET_PENDING, "url_read -> PENDING (READ sent)");
      CHECK(g_udp_tx[3] == TNFS_CMD_READ && g_udp_tx[4] == 0x07, "READ carries the fd");
      {
         uint8_t sr = g_udp_tx[2];
         uint8_t rrep[] = { 0x42,0x00, sr, TNFS_CMD_READ, TNFS_OK, 0x05,0x00, 'H','e','l','l','o' };
         g_last_upcb->recv(g_last_upcb->arg, g_last_upcb, make_pbuf(rrep, sizeof rrep), &peer, (u16_t)TNFS_PORT);
      }
      jwr24(CP(0)+1, 64); jwr32(CP(0)+4, 0x9000);
      CHECK(issue(NET_CMD_URL_READ, 0) == NET_OK, "url_read delivers the data -> OK");
      CHECK(jrd24(CP(0)+1) == 5, "url_read returned 5 bytes");
      CHECK(memcmp(&Pi1MHz->JIM_ram[0x9000], "Hello", 5) == 0, "TNFS file bytes delivered to JIM");

      /* next read hits EOF */
      jwr24(CP(0)+1, 64); jwr32(CP(0)+4, 0x9000);
      CHECK(issue(NET_CMD_URL_READ, 0) == NET_PENDING, "next url_read -> PENDING (READ sent)");
      {
         uint8_t sr2 = g_udp_tx[2];
         uint8_t erep[] = { 0x42,0x00, sr2, TNFS_CMD_READ, TNFS_EOF };
         g_last_upcb->recv(g_last_upcb->arg, g_last_upcb, make_pbuf(erep, sizeof erep), &peer, (u16_t)TNFS_PORT);
      }
      CHECK(issue(NET_CMD_URL_READ, 0) == NET_EOF, "url_read at end of file -> NET_EOF");

      /* close fires a best-effort CLOSE + UMOUNT */
      CHECK(issue(NET_CMD_URL_CLOSE, 0) == NET_OK, "url_close TNFS -> OK");
      CHECK(g_udp_tx[3] == TNFS_CMD_UMOUNT, "UMOUNT sent to the server on close");
   }

   printf("== TNFS retry on silence, then give up ==\n");
   world_reset();
   strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u], "TNFS://192.168.0.9/f");
   CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_PENDING, "url_open TNFS -> PENDING (MOUNT sent)");
   {
      uint8_t sm = g_udp_tx[2];
      uint8_t r  = NET_PENDING;
      int i;
      g_udp_tx_len = 0;
      g_now_us += 1000000ull;                    /* past the 800 ms deadline */
      CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_PENDING, "timeout with no reply -> PENDING (resend)");
      CHECK(g_udp_tx_len != 0 && g_udp_tx[3] == TNFS_CMD_MOUNT && g_udp_tx[2] == sm,
            "MOUNT resent with the same sequence");
      for (i = 0; i < 8 && r == NET_PENDING; i++) { g_now_us += 1000000ull; r = issue(NET_CMD_URL_OPEN, 0); }
      CHECK(r == NET_ERR_CONN, "retries exhausted -> ERR_CONN");
   }

   printf("== N: device - TNFS directory listing ==\n");
   world_reset();
   strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u], "TNFS://192.168.0.9/games/");
   CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_PENDING, "url_open TNFS dir -> PENDING (MOUNT)");
   {
      ip_addr_t peer; IP_ADDR4(&peer, 192, 168, 0, 9);
      uint8_t sm = g_udp_tx[2];
      uint8_t mrep[] = { 0x55,0x00, sm, TNFS_CMD_MOUNT, TNFS_OK, 0x02,0x01, 0x2C,0x01 };
      g_last_upcb->recv(g_last_upcb->arg, g_last_upcb, make_pbuf(mrep, sizeof mrep), &peer, (u16_t)TNFS_PORT);
      CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_PENDING, "after MOUNT -> PENDING (OPENDIR sent)");
      CHECK(g_udp_tx[3] == TNFS_CMD_OPENDIR, "a trailing-slash URL sends OPENDIR, not OPEN");
      {
         uint8_t so = g_udp_tx[2];
         uint8_t orep[] = { 0x55,0x00, so, TNFS_CMD_OPENDIR, TNFS_OK, 0x03 };  /* dir handle 3 */
         g_last_upcb->recv(g_last_upcb->arg, g_last_upcb, make_pbuf(orep, sizeof orep), &peer, (u16_t)TNFS_PORT);
      }
      CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_OK, "OPENDIR reply -> READY");

      jwr24(CP(0)+1, 64); jwr32(CP(0)+4, 0x9000);
      CHECK(issue(NET_CMD_URL_READ, 0) == NET_PENDING, "url_read dir -> PENDING (READDIR sent)");
      CHECK(g_udp_tx[3] == TNFS_CMD_READDIR && g_udp_tx[4] == 0x03, "READDIR carries the dir handle");
      {
         uint8_t sd = g_udp_tx[2];
         uint8_t drep[] = { 0x55,0x00, sd, TNFS_CMD_READDIR, TNFS_OK, 'G','A','M','E','.','D','S','K', 0 };
         g_last_upcb->recv(g_last_upcb->arg, g_last_upcb, make_pbuf(drep, sizeof drep), &peer, (u16_t)TNFS_PORT);
      }
      jwr24(CP(0)+1, 64); jwr32(CP(0)+4, 0x9000);
      CHECK(issue(NET_CMD_URL_READ, 0) == NET_OK, "readdir delivers an entry -> OK");
      CHECK(jrd24(CP(0)+1) == 8, "entry name length 8");
      CHECK(memcmp(&Pi1MHz->JIM_ram[0x9000], "GAME.DSK", 8) == 0, "entry name delivered (no NUL)");

      jwr24(CP(0)+1, 64); jwr32(CP(0)+4, 0x9000);
      CHECK(issue(NET_CMD_URL_READ, 0) == NET_PENDING, "next url_read -> PENDING (READDIR sent)");
      {
         uint8_t sd2 = g_udp_tx[2];
         uint8_t erep[] = { 0x55,0x00, sd2, TNFS_CMD_READDIR, TNFS_EOF };
         g_last_upcb->recv(g_last_upcb->arg, g_last_upcb, make_pbuf(erep, sizeof erep), &peer, (u16_t)TNFS_PORT);
      }
      CHECK(issue(NET_CMD_URL_READ, 0) == NET_EOF, "readdir at end of directory -> NET_EOF");

      CHECK(issue(NET_CMD_URL_CLOSE, 0) == NET_OK, "url_close dir -> OK");
      CHECK(g_udp_tx[3] == TNFS_CMD_UMOUNT, "UMOUNT sent last on dir close (CLOSEDIR before it)");
   }

   printf("== N: device - TNFS write ==\n");
   world_reset();
   jwr8(CP(0)+1, 0x08);                                     /* open-mode write bit */
   strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u], "TNFS://192.168.0.9/out.dat");
   CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_PENDING, "url_open TNFS write -> PENDING (MOUNT)");
   {
      ip_addr_t peer; IP_ADDR4(&peer, 192, 168, 0, 9);
      uint8_t sm = g_udp_tx[2];
      uint8_t mrep[] = { 0x66,0x00, sm, TNFS_CMD_MOUNT, TNFS_OK, 0x02,0x01, 0x2C,0x01 };
      g_last_upcb->recv(g_last_upcb->arg, g_last_upcb, make_pbuf(mrep, sizeof mrep), &peer, (u16_t)TNFS_PORT);
      CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_PENDING, "after MOUNT -> PENDING (OPEN sent)");
      CHECK(g_udp_tx[3] == TNFS_CMD_OPEN, "OPEN sent for a write handle");
      CHECK((g_udp_tx[4] | (g_udp_tx[5] << 8)) == (TNFS_O_WRONLY | TNFS_O_CREAT | TNFS_O_TRUNC),
            "OPEN flags = WRONLY|CREAT|TRUNC");
      {
         uint8_t so = g_udp_tx[2];
         uint8_t orep[] = { 0x66,0x00, so, TNFS_CMD_OPEN, TNFS_OK, 0x04 };  /* fd 4 */
         g_last_upcb->recv(g_last_upcb->arg, g_last_upcb, make_pbuf(orep, sizeof orep), &peer, (u16_t)TNFS_PORT);
      }
      CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_OK, "OPEN reply -> READY");

      {
         static const uint8_t payload[] = "SAVE-ME";
         memcpy(&Pi1MHz->JIM_ram[0x8000], payload, 7);
         jwr24(CP(0)+1, 7); jwr32(CP(0)+4, 0x8000);
         CHECK(issue(NET_CMD_URL_WRITE, 0) == NET_PENDING, "url_write -> PENDING (WRITE sent)");
         CHECK(g_udp_tx[3] == TNFS_CMD_WRITE && g_udp_tx[4] == 0x04, "WRITE carries the fd");
         CHECK((g_udp_tx[5] | (g_udp_tx[6] << 8)) == 7, "WRITE size = 7");
         CHECK(memcmp(&g_udp_tx[7], payload, 7) == 0, "WRITE payload sent");
         {
            uint8_t sw = g_udp_tx[2];
            uint8_t wrep[] = { 0x66,0x00, sw, TNFS_CMD_WRITE, TNFS_OK, 0x07,0x00 };
            g_last_upcb->recv(g_last_upcb->arg, g_last_upcb, make_pbuf(wrep, sizeof wrep), &peer, (u16_t)TNFS_PORT);
         }
         jwr24(CP(0)+1, 7); jwr32(CP(0)+4, 0x8000);
         CHECK(issue(NET_CMD_URL_WRITE, 0) == NET_OK, "WRITE reply -> OK");
         CHECK(jrd24(CP(0)+1) == 7, "url_write reports 7 bytes written");
      }
      CHECK(issue(NET_CMD_URL_CLOSE, 0) == NET_OK, "url_close TNFS write -> OK");
      CHECK(g_udp_tx[3] == TNFS_CMD_UMOUNT, "UMOUNT sent on close");
   }

   /* a read-only TNFS handle refuses url_write */
   world_reset();
   jwr8(CP(0)+1, 0x00);                                     /* read mode */
   strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u], "TNFS://192.168.0.9/ro.dat");
   issue(NET_CMD_URL_OPEN, 0);
   {
      ip_addr_t peer; IP_ADDR4(&peer, 192, 168, 0, 9);
      uint8_t sm = g_udp_tx[2];
      uint8_t mrep[] = { 0x66,0x00, sm, TNFS_CMD_MOUNT, TNFS_OK, 0x02,0x01, 0x2C,0x01 };
      g_last_upcb->recv(g_last_upcb->arg, g_last_upcb, make_pbuf(mrep, sizeof mrep), &peer, (u16_t)TNFS_PORT);
      issue(NET_CMD_URL_OPEN, 0);
      { uint8_t so = g_udp_tx[2]; uint8_t orep[] = { 0x66,0x00, so, TNFS_CMD_OPEN, TNFS_OK, 0x04 };
        g_last_upcb->recv(g_last_upcb->arg, g_last_upcb, make_pbuf(orep, sizeof orep), &peer, (u16_t)TNFS_PORT); }
      issue(NET_CMD_URL_OPEN, 0);
      jwr24(CP(0)+1, 4); jwr32(CP(0)+4, 0x8000);
      CHECK(issue(NET_CMD_URL_WRITE, 0) == NET_ERR_NOTOPEN, "url_write on a read-only TNFS handle -> NOTOPEN");
   }

   printf("== TNFS ignores replies from a foreign session (connid) ==\n");
   world_reset();
   strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u], "TNFS://192.168.0.9/f.dat");
   CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_PENDING, "url_open -> PENDING (MOUNT)");
   {
      ip_addr_t peer; IP_ADDR4(&peer, 192, 168, 0, 9);
      uint8_t sm = g_udp_tx[2];
      uint8_t mrep[] = { 0x77,0x00, sm, TNFS_CMD_MOUNT, TNFS_OK, 0x02,0x01, 0x2C,0x01 };
      g_last_upcb->recv(g_last_upcb->arg, g_last_upcb, make_pbuf(mrep, sizeof mrep), &peer, (u16_t)TNFS_PORT);
      CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_PENDING, "after MOUNT -> PENDING (OPEN sent)");
      CHECK((g_udp_tx[0] | (g_udp_tx[1] << 8)) == 0x0077, "OPEN carries the session id 0x0077");
      {
         uint8_t so = g_udp_tx[2];
         /* right seq+cmd, WRONG connid: a stray/spoofed datagram - must be dropped */
         uint8_t bad[]  = { 0x99,0x99, so, TNFS_CMD_OPEN, TNFS_OK, 0x04 };
         g_last_upcb->recv(g_last_upcb->arg, g_last_upcb, make_pbuf(bad, sizeof bad), &peer, (u16_t)TNFS_PORT);
         CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_PENDING, "foreign-connid reply ignored -> still PENDING");
         uint8_t good[] = { 0x77,0x00, so, TNFS_CMD_OPEN, TNFS_OK, 0x04 };
         g_last_upcb->recv(g_last_upcb->arg, g_last_upcb, make_pbuf(good, sizeof good), &peer, (u16_t)TNFS_PORT);
      }
      CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_OK, "matching-connid reply -> READY");
   }

   printf("== TNFS bounds sustained EAGAIN (server busy) then gives up ==\n");
   world_reset();
   strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u], "TNFS://192.168.0.9/busy.dat");
   issue(NET_CMD_URL_OPEN, 0);
   {
      ip_addr_t peer; IP_ADDR4(&peer, 192, 168, 0, 9);
      uint8_t sm = g_udp_tx[2];
      uint8_t mrep[] = { 0x88,0x00, sm, TNFS_CMD_MOUNT, TNFS_OK, 0x02,0x01, 0x2C,0x01 };
      g_last_upcb->recv(g_last_upcb->arg, g_last_upcb, make_pbuf(mrep, sizeof mrep), &peer, (u16_t)TNFS_PORT);
      issue(NET_CMD_URL_OPEN, 0);
      { uint8_t so = g_udp_tx[2]; uint8_t orep[] = { 0x88,0x00, so, TNFS_CMD_OPEN, TNFS_OK, 0x04 };
        g_last_upcb->recv(g_last_upcb->arg, g_last_upcb, make_pbuf(orep, sizeof orep), &peer, (u16_t)TNFS_PORT); }
      CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_OK, "READY");

      jwr24(CP(0)+1, 5); jwr32(CP(0)+4, 0x9000);
      CHECK(issue(NET_CMD_URL_READ, 0) == NET_PENDING, "url_read -> PENDING (READ sent)");
      {
         /* feed EAGAIN forever: each is re-issued as PENDING until the budget
            (TNFS_EAGAIN_MAX) runs out, then the read fails rather than hanging */
         uint8_t r = NET_PENDING;
         int busy = 0, i;
         for (i = 0; i < 20 && r == NET_PENDING; i++) {
            uint8_t sr = g_udp_tx[2];
            uint8_t eagain[] = { 0x88,0x00, sr, TNFS_CMD_READ, TNFS_EAGAIN };
            g_last_upcb->recv(g_last_upcb->arg, g_last_upcb, make_pbuf(eagain, sizeof eagain), &peer, (u16_t)TNFS_PORT);
            r = issue(NET_CMD_URL_READ, 0);
            if (r == NET_PENDING) busy++;
         }
         CHECK(r == NET_ERR_CONN, "sustained EAGAIN eventually -> ERR_CONN (no infinite spin)");
         CHECK(busy >= 1 && busy <= 8, "EAGAIN budget (TNFS_EAGAIN_MAX=8) bounded the backoffs");
      }
   }

   printf("== N: device - TELNET scheme (IAC filter) ==\n");
   world_reset();
   strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u], "TELNET://1.2.3.4:23");
   CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_PENDING, "url_open TELNET -> PENDING (connecting)");
   g_last_pcb->connected(g_last_pcb->arg, g_last_pcb, ERR_OK);
   CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_OK, "url_open TELNET -> OK (connected)");
   {
      /* server stream: "Hi" IAC WILL ECHO "!" -> clean "Hi!" + a DO ECHO reply */
      uint8_t seg[] = { 'H','i', TN_IAC, TN_WILL, TN_OPT_ECHO, '!' };
      g_tx_len = 0;
      CHECK(g_last_pcb->recv(g_last_pcb->arg, g_last_pcb, make_pbuf(seg, sizeof seg), ERR_OK) == ERR_OK,
            "recv_cb accepts + filters the TELNET segment");
      CHECK(g_tx_len == 3 && g_tx[0] == TN_IAC && g_tx[1] == TN_DO && g_tx[2] == TN_OPT_ECHO,
            "negotiation reply IAC DO ECHO sent back to the server");
      jwr24(CP(0)+1, 64); jwr32(CP(0)+4, 0x9000);
      CHECK(issue(NET_CMD_URL_READ, 0) == NET_OK, "url_read TELNET -> OK");
      CHECK(jrd24(CP(0)+1) == 3 && memcmp(&Pi1MHz->JIM_ram[0x9000], "Hi!", 3) == 0,
            "IAC command stripped: the Beeb sees clean text \"Hi!\"");

      /* url_write escapes a literal 0xFF as IAC IAC on the wire */
      { uint8_t data[] = { 'X', 0xFF, 'Y' };
        memcpy(&Pi1MHz->JIM_ram[0x8000], data, 3);
        jwr24(CP(0)+1, 3); jwr32(CP(0)+4, 0x8000);
        g_tx_len = 0;
        CHECK(issue(NET_CMD_URL_WRITE, 0) == NET_OK, "url_write TELNET -> OK");
        CHECK(jrd24(CP(0)+1) == 3, "url_write reports 3 input bytes consumed");
        CHECK(g_tx_len == 4 && g_tx[0]=='X' && g_tx[1]==0xFF && g_tx[2]==0xFF && g_tx[3]=='Y',
              "outbound 0xFF escaped to IAC IAC on the wire"); }

      /* url_status DVSTAT: bytes_waiting (rx) + connected byte */
      { static const uint8_t seg[] = "abcde";
        g_last_pcb->recv(g_last_pcb->arg, g_last_pcb, make_pbuf(seg, 5), ERR_OK);
        issue(NET_CMD_URL_STATUS, 0);
        CHECK((jrd8(CP(0)+1) | (jrd8(CP(0)+2)<<8)) == 5u, "DVSTAT bytes_waiting reflects the RX ring");
        CHECK(jrd8(CP(0)+3) == 1u && jrd8(CP(0)+4) == 0u, "DVSTAT connected=1, error=0"); }
   }

   printf("== TELNET over a >256B pbuf chain, IAC straddling boundaries ==\n");
   world_reset();
   strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u], "TELNET://1.2.3.4:23");
   issue(NET_CMD_URL_OPEN, 0);
   g_last_pcb->connected(g_last_pcb->arg, g_last_pcb, ERR_OK);
   CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_OK, "TELNET connected");
   {
      /* 280-byte stream: 255 'A', then IAC WILL SGA (the IAC is byte 255 - the
         last of the filter's first 256-byte chunk - so the command spans the
         internal chunk boundary), 'B' 'C', IAC IAC (-> one 0xFF), 18 'D'. */
      uint8_t in[280]; size_t n = 0, k;
      for (k = 0; k < 255u; k++) in[n++] = 'A';
      in[n++] = TN_IAC; in[n++] = TN_WILL; in[n++] = TN_OPT_SGA;   /* 255,256,257 */
      in[n++] = 'B'; in[n++] = 'C';
      in[n++] = TN_IAC; in[n++] = TN_IAC;                          /* -> literal 0xFF */
      while (n < sizeof in) in[n++] = 'D';                         /* pad to 280 */

      g_tx_len = 0;
      /* feed it as a chain of 64-byte pbufs: the IAC at byte 255 also lands on a
         pbuf boundary (WILL is the first byte of the 5th pbuf) */
      CHECK(g_last_pcb->recv(g_last_pcb->arg, g_last_pcb,
                             make_pbuf_split(in, (u16_t)n, 64), ERR_OK) == ERR_OK,
            "recv_cb filters a chained >256B TELNET segment");
      CHECK(g_tx_len == 3 && g_tx[0] == TN_IAC && g_tx[1] == TN_DO && g_tx[2] == TN_OPT_SGA,
            "WILL SGA (spanning chunk+pbuf boundary) answered with DO SGA");

      jwr24(CP(0)+1, 0x400); jwr32(CP(0)+4, 0x9000);              /* read up to 1024 */
      CHECK(issue(NET_CMD_URL_READ, 0) == NET_OK, "url_read the filtered stream -> OK");
      {
         uint32_t got = jrd24(CP(0)+1);
         const uint8_t *o = &Pi1MHz->JIM_ram[0x9000];
         int a_ok = 1; unsigned i;
         for (i = 0; i < 255u; i++) if (o[i] != 'A') a_ok = 0;
         CHECK(got == 276u, "filtered length = 276 (3-byte cmd stripped, IAC IAC folded)");
         CHECK(a_ok && o[255] == 'B' && o[256] == 'C' && o[257] == 0xFFu && o[275] == 'D',
               "chained filter output byte-correct across both boundaries");
      }
   }

   printf("== N: device - TNFS aux modes (FujiNet-aligned) ==\n");
   world_reset();
   jwr8(CP(0)+1, NET_OPEN_DIR);                             /* mode 13 -> directory */
   strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u], "TNFS://192.168.0.9/games");  /* no trailing / */
   issue(NET_CMD_URL_OPEN, 0);
   {
      ip_addr_t peer; IP_ADDR4(&peer, 192, 168, 0, 9);
      uint8_t sm = g_udp_tx[2];
      uint8_t mrep[] = { 0x77,0x00, sm, TNFS_CMD_MOUNT, TNFS_OK, 0x02,0x01, 0x2C,0x01 };
      g_last_upcb->recv(g_last_upcb->arg, g_last_upcb, make_pbuf(mrep, sizeof mrep), &peer, (u16_t)TNFS_PORT);
      issue(NET_CMD_URL_OPEN, 0);
      CHECK(g_udp_tx[3] == TNFS_CMD_OPENDIR, "open mode 13 -> OPENDIR (no trailing slash needed)");
   }
   world_reset();
   jwr8(CP(0)+1, NET_OPEN_RW);                              /* mode 12 -> read-write */
   strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u], "TNFS://192.168.0.9/rw.dat");
   issue(NET_CMD_URL_OPEN, 0);
   {
      ip_addr_t peer; IP_ADDR4(&peer, 192, 168, 0, 9);
      uint8_t sm = g_udp_tx[2];
      uint8_t mrep[] = { 0x77,0x00, sm, TNFS_CMD_MOUNT, TNFS_OK, 0x02,0x01, 0x2C,0x01 };
      g_last_upcb->recv(g_last_upcb->arg, g_last_upcb, make_pbuf(mrep, sizeof mrep), &peer, (u16_t)TNFS_PORT);
      issue(NET_CMD_URL_OPEN, 0);
      CHECK(g_udp_tx[3] == TNFS_CMD_OPEN
            && (g_udp_tx[4] | (g_udp_tx[5]<<8)) == (TNFS_O_RDWR | TNFS_O_CREAT),
            "open mode 12 -> OPEN O_RDWR|O_CREAT (no truncate)");
   }

   printf("== N: device - HTTP scheme ==\n");
   world_reset();
   strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u], "HTTP://1.2.3.4/index.html");
   CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_PENDING, "url_open HTTP:// -> PENDING");
   g_last_pcb->connected(g_last_pcb->arg, g_last_pcb, ERR_OK);
   CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_OK, "url_open HTTP:// -> OK (request sent)");
   g_tx[g_tx_len] = 0;
   CHECK(strstr((char *)g_tx, "GET /index.html HTTP/1.0") != NULL, "HTTP GET line sent");
   CHECK(strstr((char *)g_tx, "Host: 1.2.3.4") != NULL, "Host header sent");
   CHECK(strstr((char *)g_tx, "User-Agent: Pi1MHz/") != NULL,
         "Pi1MHz User-Agent sent");
   {
      static const char resp[] =
         "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n<html>hi</html>";
      struct pbuf *p = make_pbuf(resp, (u16_t)(sizeof resp - 1u));
      g_last_pcb->recv(g_last_pcb->arg, g_last_pcb, p, ERR_OK);
   }
   jwr24(CP(0)+1, 200); jwr32(CP(0)+4, 0x9000);
   CHECK(issue(NET_CMD_URL_READ, 0) == NET_OK, "url_read -> OK");
   {
      uint32_t n = jrd24(CP(0)+1);
      CHECK(n == strlen("<html>hi</html>"), "url_read returned only the body (headers stripped)");
      CHECK(memcmp(&Pi1MHz->JIM_ram[0x9000], "<html>hi</html>", n) == 0, "body bytes correct");
   }
   issue(NET_CMD_URL_STATUS, 0);
   CHECK((jrd8(CP(0)+7) | (jrd8(CP(0)+8)<<8)) == 200u, "url status reports HTTP 200 (+7..8)");
   CHECK(jrd8(CP(0)+3) == 1u, "DVSTAT connected byte set");

   printf("== N: device - truncated HTTP body ==\n");
   world_reset();
   strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u], "HTTP://1.2.3.4/game.uef");
   CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_PENDING, "truncated HTTP open -> PENDING");
   g_last_pcb->connected(g_last_pcb->arg, g_last_pcb, ERR_OK);
   CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_OK, "truncated HTTP open -> OK");
   {
      static const char resp[] =
         "HTTP/1.0 200 OK\r\nContent-Length: 10\r\n\r\nshort";
      struct pbuf *p = make_pbuf(resp, (u16_t)(sizeof resp - 1u));
      g_last_pcb->recv(g_last_pcb->arg, g_last_pcb, p, ERR_OK);
      g_last_pcb->recv(g_last_pcb->arg, g_last_pcb, NULL, ERR_OK);
   }
   jwr24(CP(0)+1, 200); jwr32(CP(0)+4, 0x9000);
   CHECK(issue(NET_CMD_URL_READ, 0) == NET_OK,
         "truncated HTTP returns bytes received before FIN");
   CHECK(jrd24(CP(0)+1) == 5u, "truncated HTTP returned five-byte partial body");
   jwr24(CP(0)+1, 200); jwr32(CP(0)+4, 0x9100);
   CHECK(issue(NET_CMD_URL_READ, 0) == NET_ERR_TCP_CLOSED,
         "truncated HTTP body -> TCP_CLOSED, not successful EOF");

   printf("== N: device - exact MENU TITLES transfer shape ==\n");
   world_reset();
   strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u],
          "HTTP://acornelectron.nl/uefarchive/TITLES");
   g_dns_sync = 1;
   IP_ADDR4(&g_dns_result, 192, 0, 2, 80);
   CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_PENDING,
         "TITLES open -> PENDING");
   g_last_pcb->connected(g_last_pcb->arg, g_last_pcb, ERR_OK);
   CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_OK,
         "TITLES open -> OK");
   {
      enum { TITLES_LENGTH = 11498, SEGMENT = 1460, READ_MAX = 240 };
      static const char header[] =
         "HTTP/1.0 200 OK\r\nContent-Length: 11498\r\n"
         "Content-Type: application/octet-stream\r\n\r\n";
      uint8_t *wire = malloc((sizeof header - 1u) + TITLES_LENGTH);
      uint8_t *received = malloc(TITLES_LENGTH);
      size_t wire_length = (sizeof header - 1u) + TITLES_LENGTH;
      size_t sent = 0u, total = 0u;
      int transfer_ok = 1;
      CHECK(wire != NULL && received != NULL,
            "TITLES test buffers allocated");
      if (wire != NULL && received != NULL) {
         memcpy(wire, header, sizeof header - 1u);
         for (size_t i = 0u; i < TITLES_LENGTH; i++)
            wire[sizeof header - 1u + i] = (uint8_t)(i * 37u + 11u);
         /* The segmented shape first; the coalesced single-chain shape is
            exercised separately below, now that a handle can borrow the
            shared ring for a chain too large for its own. */
         while (sent < wire_length) {
            u16_t length = (u16_t)(wire_length - sent);
            struct pbuf *p;
            err_t accepted;
            unsigned retries = 64u;
            if (length > SEGMENT) length = SEGMENT;
            p = make_pbuf(wire + sent, length);
            accepted = g_last_pcb->recv(g_last_pcb->arg, g_last_pcb,
                                        p, ERR_OK);
            while (accepted == ERR_MEM && retries-- != 0u) {
               jwr24(CP(0) + 1u, READ_MAX);
               jwr32(CP(0) + 4u, 0x9000u);
               CHECK(issue(NET_CMD_URL_READ, 0) == NET_OK,
                     "TITLES drains while lwIP retains refused pbuf");
               {
                  uint32_t got = jrd24(CP(0) + 1u);
                  CHECK(total + got <= TITLES_LENGTH,
                        "TITLES drain remains within declared length");
                  if (got == 0u || total + got > TITLES_LENGTH) {
                     transfer_ok = 0;
                     break;
                  }
                  memcpy(received + total, &Pi1MHz->JIM_ram[0x9000], got);
                  total += got;
               }
               if (!transfer_ok) break;
               accepted = g_last_pcb->recv(g_last_pcb->arg, g_last_pcb,
                                            p, ERR_OK);
            }
            CHECK(accepted != ERR_MEM,
                  "TITLES refused pbuf retry is bounded");
            if (accepted != ERR_OK) {
               if (accepted == ERR_MEM) pbuf_free(p);
               transfer_ok = 0;
               break;
            }
            sent += length;
         }
         if (transfer_ok)
            g_last_pcb->recv(g_last_pcb->arg, g_last_pcb, NULL, ERR_OK);
         {
            unsigned reads = 64u;
            uint8_t result = NET_OK;
            while (transfer_ok && result != NET_EOF && reads-- != 0u) {
               uint32_t got;
               jwr24(CP(0) + 1u, READ_MAX);
               jwr32(CP(0) + 4u, 0x9000u);
               result = issue(NET_CMD_URL_READ, 0);
               if (result == NET_EOF) continue;
               CHECK(result == NET_OK,
                     "TITLES read completes without HTTP error");
               if (result != NET_OK) {
                  transfer_ok = 0;
                  break;
               }
               got = jrd24(CP(0) + 1u);
               CHECK(got != 0u && total + got <= TITLES_LENGTH,
                     "TITLES read makes bounded progress");
               if (got == 0u || total + got > TITLES_LENGTH) {
                  transfer_ok = 0;
                  break;
               }
               memcpy(received + total, &Pi1MHz->JIM_ram[0x9000], got);
               total += got;
            }
            CHECK(result == NET_EOF, "TITLES reaches bounded EOF");
         }
         CHECK(total == TITLES_LENGTH, "TITLES returns exactly 11498 bytes");
         CHECK(memcmp(received, wire + sizeof header - 1u,
                      TITLES_LENGTH) == 0, "TITLES payload is byte-exact");

         /* The same payload delivered as ONE coalesced chain.  The CYW43 path
            can present several TCP segments that way, and lwIP re-presents the
            whole chain rather than a prefix, so a receiver sized for a single
            Ethernet segment refuses it for ever however much the host drains.
            This is the shape an 8 KB ring cannot accept. */
         world_reset();
         strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u],
                "HTTP://acornelectron.nl/uefarchive/TITLES");
         g_dns_sync = 1;
         IP_ADDR4(&g_dns_result, 192, 0, 2, 80);
         CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_PENDING,
               "coalesced TITLES open -> PENDING");
         g_last_pcb->connected(g_last_pcb->arg, g_last_pcb, ERR_OK);
         CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_OK,
               "coalesced TITLES open -> OK");
         {
            struct pbuf *whole = make_pbuf_split(wire, (u16_t)wire_length,
                                                 SEGMENT);
            err_t accepted = g_last_pcb->recv(g_last_pcb->arg, g_last_pcb,
                                              whole, ERR_OK);
            CHECK(accepted == ERR_OK,
                  "TITLES fits as one coalesced lwIP pbuf chain");
            if (accepted == ERR_OK) {
               size_t one_total = 0u;
               unsigned reads = 256u;
               g_last_pcb->recv(g_last_pcb->arg, g_last_pcb, NULL, ERR_OK);
               while (one_total < TITLES_LENGTH && reads-- != 0u) {
                  uint32_t got;
                  jwr24(CP(0) + 1u, READ_MAX);
                  jwr32(CP(0) + 4u, 0x9000u);
                  CHECK(issue(NET_CMD_URL_READ, 0) == NET_OK,
                        "coalesced TITLES read completes");
                  got = jrd24(CP(0) + 1u);
                  CHECK(got != 0u && one_total + got <= TITLES_LENGTH,
                        "coalesced TITLES read makes bounded progress");
                  if (got == 0u || one_total + got > TITLES_LENGTH) break;
                  CHECK(memcmp(wire + (sizeof header - 1u) + one_total,
                               &Pi1MHz->JIM_ram[0x9000], got) == 0,
                        "coalesced TITLES bytes are exact");
                  one_total += got;
               }
               CHECK(one_total == TITLES_LENGTH,
                     "coalesced TITLES returns exactly 11498 bytes");
            } else {
               pbuf_free(whole);
            }
            CHECK(issue(NET_CMD_URL_CLOSE, 0) == NET_OK,
                  "coalesced TITLES closes cleanly");
         }
      }
      free(received);
      free(wire);
   }

   printf("== N: device - malformed URL ==\n");
   world_reset();
   strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u], "notaurl");
   CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_ERR_PARAM, "malformed URL -> PARAM");
   strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u], "TCP://host.only");
   CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_ERR_PARAM, "TCP:// without a port -> PARAM");
   strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u], "TCP://h:99999");
   CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_ERR_PARAM, "port > 65535 -> PARAM");
   strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u], "TCP://h:4294967297");
   CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_ERR_PARAM, "port that would wrap -> PARAM");
   { char *u = (char *)&Pi1MHz->JIM_ram[CP(0) + 2u];
     strcpy(u, "TCP://haXst:80"); u[8] = '\r';       /* control char in host */
     CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_ERR_PARAM, "control char in host -> PARAM"); }

   printf("== N: device - oversized HTTP headers don't deadlock ==\n");
   world_reset();
   strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u], "HTTP://1.2.3.4/");
   issue(NET_CMD_URL_OPEN, 0);
   g_last_pcb->connected(g_last_pcb->arg, g_last_pcb, ERR_OK);
   issue(NET_CMD_URL_OPEN, 0);
   /* A pbuf length is 16-bit, so a 65536-byte ring cannot be represented by
      one pbuf. Filling its usable 65535 bytes exercises the same deadlock. */
   { uint8_t *big = malloc(NET_RX_RING_SIZE - 1u); memset(big, 'A', NET_RX_RING_SIZE - 1u);
     struct pbuf *p = make_pbuf(big, (u16_t)(NET_RX_RING_SIZE - 1u));
     if (g_last_pcb->recv(g_last_pcb->arg, g_last_pcb, p, ERR_OK) == ERR_MEM) pbuf_free(p);
     free(big); }
   jwr24(CP(0)+1, 200); jwr32(CP(0)+4, 0x9000);
   CHECK(issue(NET_CMD_URL_READ, 0) == NET_ERR_CONN, "ring-full-without-CRLFCRLF -> ERR (not deadlock)");

   printf("== N: device - connect NOMEM doesn't wedge ==\n");
   world_reset();
   strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u], "TCP://1.2.3.4:5000");
   g_force_new_null = 1;                              /* pcb exhaustion */
   CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_ERR_NOMEM, "url_open with no free pcb -> NOMEM");
   CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_ERR_NOMEM, "re-issue stays NOMEM (not stuck PENDING)");
   g_force_new_null = 0;

   printf("== fuzz: random command blocks + RX (ASan/UBSan) ==\n");
   {
      uint32_t s = 0x1234567u;
      #define RND() (s = s * 1103515245u + 12345u, (uint8_t)(s >> 17))
      for (int it = 0; it < 40000; it++) {
         unsigned h = it & 7u;
         world_reset();                 /* teardown uses last round's pcbs... */
         for (int i = 0; i < g_npcbs; i++) free(g_pcbs[i]);   /* ...now free */
         g_npcbs = 0; g_last_pcb = NULL;

         /* random command block for handle h, then dispatch it */
         for (int b = 0; b < 40; b++) Pi1MHz->JIM_ram[CP(h) + (uint32_t)b] = RND();
         Pi1MHz->JIM_ram[CP(h)] = (uint8_t)(45u + (RND() % 20u));  /* a net command */
         g_cmd(CP(h), RES, (uint8_t)(RND() & 0x0Fu));
         g_poll();

         /* sometimes shove a random inbound segment at whatever pcb exists */
         if (g_last_pcb != NULL && g_last_pcb->recv != NULL && (RND() & 1u)) {
            uint16_t n = (uint16_t)(RND() % 48u);
            struct pbuf *p = make_pbuf(NULL, n);
            for (uint16_t k = 0; k < n; k++) ((uint8_t *)p->payload)[k] = RND();
            if (g_last_pcb->recv(g_last_pcb->arg, g_last_pcb, p, ERR_OK) == ERR_MEM)
               pbuf_free(p);            /* parked: the harness still owns it */
         } else if (g_last_pcb != NULL && g_last_pcb->recv != NULL && (RND() & 1u)) {
            g_last_pcb->recv(g_last_pcb->arg, g_last_pcb, NULL, ERR_OK);  /* FIN */
         }
      }
      #undef RND
      CHECK(1, "40000 random command blocks + RX survived (no crash / UB / leak)");
   }

   /* The command-block fuzzer above rarely forms a "TELNET://" or "TNFS://"
      URL from random bytes, so it barely touches those adapters' recv paths.
      These two loops set the adapter up deterministically, then hammer its
      recv callback with adversarial input. */
   printf("== fuzz: TELNET IAC filter over random chained segments ==\n");
   {
      uint32_t s = 0x9E3779B9u;
      #define RND() (s = s * 1103515245u + 12345u, (uint8_t)(s >> 17))
      for (int it = 0; it < 8000; it++) {
         world_reset();
         for (int i = 0; i < g_npcbs; i++) free(g_pcbs[i]);
         g_npcbs = 0; g_last_pcb = NULL;
         strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u], "TELNET://1.2.3.4:23");
         issue(NET_CMD_URL_OPEN, 0);
         if (g_last_pcb != NULL && g_last_pcb->connected != NULL)
            g_last_pcb->connected(g_last_pcb->arg, g_last_pcb, ERR_OK);
         issue(NET_CMD_URL_OPEN, 0);                        /* -> READY */

         for (int r = 0; r < 3 && g_last_pcb != NULL && g_last_pcb->recv != NULL; r++) {
            uint16_t len = (uint16_t)(((RND() << 8) | RND()) % 400u);
            uint16_t seg = (uint16_t)(1u + RND() % 80u);
            struct pbuf *p = make_pbuf_split(NULL, len, seg);
            for (struct pbuf *q = p; q != NULL; q = q->next)
               for (uint16_t k = 0; k < q->len; k++) ((uint8_t *)q->payload)[k] = RND();
            if (g_last_pcb->recv(g_last_pcb->arg, g_last_pcb, p, ERR_OK) == ERR_MEM)
               pbuf_free(p);                                /* parked: harness owns it */
            if (RND() & 1u) { jwr24(CP(0)+1, 0x400); jwr32(CP(0)+4, 0x9000);
                              issue(NET_CMD_URL_READ, 0); } /* drain the ring */
         }
      }
      #undef RND
      /* Do NOT free g_pcbs here: the handle table still points at the last
         pcb; the final suite cleanup (after the last world_reset) frees it. */
      CHECK(1, "TELNET filter survived random chained segments (no crash / UB / leak)");
   }

   printf("== fuzz: TNFS reply parser over random datagrams ==\n");
   {
      uint32_t s = 0xB5297A4Du;
      #define RND() (s = s * 1103515245u + 12345u, (uint8_t)(s >> 17))
      for (int it = 0; it < 8000; it++) {
         world_reset();
         for (int i = 0; i < g_nupcbs; i++) free(g_upcbs[i]);
         g_nupcbs = 0; g_last_upcb = NULL;
         strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u], "TNFS://192.168.0.9/f");
         issue(NET_CMD_URL_OPEN, 0);                        /* MOUNT sent -> PENDING */

         for (int r = 0; r < 4 && g_last_upcb != NULL && g_last_upcb->recv != NULL; r++) {
            ip_addr_t peer; IP_ADDR4(&peer, 192, 168, 0, 9);
            uint16_t len = (uint16_t)(RND() % 64u);
            struct pbuf *p = make_pbuf(NULL, len);
            for (uint16_t k = 0; k < len; k++) ((uint8_t *)p->payload)[k] = RND();
            /* half the time echo the seq/cmd we last sent so parse gets deeper
               (past the seq/cmd gate) into the status/body handling */
            if (len >= 4u && (RND() & 1u)) {
               ((uint8_t *)p->payload)[2] = g_udp_tx[2];
               ((uint8_t *)p->payload)[3] = g_udp_tx[3];
            }
            g_last_upcb->recv(g_last_upcb->arg, g_last_upcb, p, &peer, (u16_t)TNFS_PORT);
            g_now_us += (uint64_t)RND() * 5000ull;          /* sometimes cross a deadline */
            issue(NET_CMD_URL_OPEN, 0);
         }
      }
      #undef RND
      /* Same as above: leave g_upcbs for the final cleanup (no teardown runs
         after it), so the live handle->upcb ref is never touched post-free. */
      CHECK(1, "TNFS parser survived random datagrams (no crash / UB / leak)");
   }

   /* free the last test's pcbs so LSan is clean */
   for (int i = 0; i < g_npcbs; i++) free(g_pcbs[i]);
   for (int i = 0; i < g_nupcbs; i++) free(g_upcbs[i]);
   CHECK(g_pbuf_live == 0, "no pbuf leaked across the suite");

   printf("\n%d checks, %d failures\n", checks, fails);
   printf(fails ? "NET SERVICE TESTS FAILED\n" : "NET SERVICE TESTS PASSED\n");
   return fails ? 1 : 0;
}
