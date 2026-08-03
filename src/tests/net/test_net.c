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
#include "services.h"
#include "net_service.h"
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

void Pi1MHz_MemoryWrite(uint32_t addr, uint8_t data) { g_reg[addr & 0xffu] = data; }
void Pi1MHz_Register_Poll(func_ptr f) { g_poll = f; }
void Pi1MHz_nIRQ_ASSERT(uint8_t src) { (void)src; g_nirq_asserted = 1; }
void Pi1MHz_nIRQ_CLEAR (uint8_t src) { (void)src; g_nirq_asserted = 0; }

bool services_register(uint8_t first, uint8_t last, service_command_fn h)
{ (void)first; (void)last; g_cmd = h; return true; }
void services_irq_set(uint8_t source, bool asserted)
{ (void)source; g_nirq_asserted = asserted ? 1 : 0; }

static const char *g_net_enable = "1";
const char *config_get(const char *prop)
{ return (strcmp(prop, "net_enable") == 0) ? g_net_enable : NULL; }

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
static uint32_t CP(unsigned h) { return 0x100u + h * 0x100u; }

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
   CHECK(g_last_pcb->rcv_wnd == NET_RX_RING_SIZE, "rcv_wnd clamped to ring size");
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
   CHECK(issue(NET_CMD_CONNECT, 0) == NET_ERR_CONN, "connect refused -> ERR_CONN");

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
      CHECK(issue(NET_CMD_RECV, 0) == NET_ERR_CONN, "recv after drain + reset -> ERR_CONN");
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

   printf("== N: device - TCP scheme ==\n");
   world_reset();
   strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u], "TCP://1.2.3.4:5000");
   { int before = g_npcbs;
     CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_PENDING, "url_open TCP:// -> PENDING (connecting)");
     CHECK(g_npcbs == before + 1, "dotted IP skipped DNS, went straight to connect"); }
   g_last_pcb->connected(g_last_pcb->arg, g_last_pcb, ERR_OK);
   CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_OK, "url_open TCP:// -> OK after connect");
   issue(NET_CMD_URL_STATUS, 0);
   CHECK((jrd8(CP(0)+2) & NET_FLAG_CONNECTED) != 0, "url status CONNECTED");

   printf("== N: device - HTTP scheme ==\n");
   world_reset();
   strcpy((char *)&Pi1MHz->JIM_ram[CP(0) + 2u], "HTTP://1.2.3.4/index.html");
   CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_PENDING, "url_open HTTP:// -> PENDING");
   g_last_pcb->connected(g_last_pcb->arg, g_last_pcb, ERR_OK);
   CHECK(issue(NET_CMD_URL_OPEN, 0) == NET_OK, "url_open HTTP:// -> OK (request sent)");
   g_tx[g_tx_len] = 0;
   CHECK(strstr((char *)g_tx, "GET /index.html HTTP/1.0") != NULL, "HTTP GET line sent");
   CHECK(strstr((char *)g_tx, "Host: 1.2.3.4") != NULL, "Host header sent");
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
   CHECK((jrd8(CP(0)+3) | (jrd8(CP(0)+4)<<8)) == 200u, "url status reports HTTP 200");

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
   { uint8_t *big = malloc(NET_RX_RING_SIZE); memset(big, 'A', NET_RX_RING_SIZE);
     struct pbuf *p = make_pbuf(big, (u16_t)NET_RX_RING_SIZE);
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

   /* free the last test's pcbs so LSan is clean */
   for (int i = 0; i < g_npcbs; i++) free(g_pcbs[i]);
   for (int i = 0; i < g_nupcbs; i++) free(g_upcbs[i]);
   CHECK(g_pbuf_live == 0, "no pbuf leaked across the suite");

   printf("\n%d checks, %d failures\n", checks, fails);
   printf(fails ? "NET SERVICE TESTS FAILED\n" : "NET SERVICE TESTS PASSED\n");
   return fails ? 1 : 0;
}
