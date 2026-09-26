/* fujinet_tnfs_io.c - the FujiNet device's TNFS backend (fujinet/fn_tnfs.c)
   on the Pi's network: one lwIP UDP socket, the lwIP resolver and the
   system timer.  Everything here runs on the main loop - lwIP's callbacks
   come from the WiFi poll - so fn_tnfs_input is called directly.

   Like the N: device, it needs net_enable=1 in Pi1MHz.cfg; without it
   every server fails to resolve and a TNFS URI is simply refused. */

#include <string.h>

#include "Pi1MHz.h"
#include "config.h"
#include "rpi/systimer.h"
#include "wifi/wifi_lwip.h"        /* wifi_lwip_rx_kick */
#include "lwip/udp.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "fujinet/fn_tnfs.h"

#define TNFS_DGRAM_MAX 600u         /* the largest reply fn_tnfs parses */

static struct udp_pcb *s_pcb;

/* One resolve at a time (the device runs one job).  A late answer for a
   name no longer wanted is dropped by comparing the name. */
static struct {
   char      name[64];
   bool      busy, done, ok;
   ip_addr_t ip;
} s_dns;

uint32_t fn_tnfs_io_now_ms(void)
{
   return (uint32_t)(RPI_GetSystemTime64() / 1000u);
}

static void tnfs_udp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                          const ip_addr_t *addr, u16_t port)
{
   static uint8_t buf[TNFS_DGRAM_MAX];
   (void)arg;
   (void)pcb;
   if (p == NULL)
      return;
   uint16_t len = (uint16_t)pbuf_copy_partial(p, buf, sizeof buf, 0);
   uint32_t ip = ip4_addr_get_u32(ip_2_ip4(addr));
   pbuf_free(p);
   /* A datagram longer than any reply is not one of ours. */
   if (len < sizeof buf)
      fn_tnfs_input(ip, port, buf, len);
}

static bool socket_ready(void)
{
   if (s_pcb)
      return true;
   s_pcb = udp_new();
   if (!s_pcb)
      return false;
   if (udp_bind(s_pcb, IP_ANY_TYPE, 0u) != ERR_OK) {
      udp_remove(s_pcb);
      s_pcb = NULL;
      return false;
   }
   udp_recv(s_pcb, tnfs_udp_recv, NULL);
   return true;
}

static void tnfs_dns_found(const char *name, const ip_addr_t *ipaddr, void *arg)
{
   (void)arg;
   if (!s_dns.busy || strcmp(name, s_dns.name) != 0)
      return;
   s_dns.busy = false;
   s_dns.done = true;
   s_dns.ok = ipaddr != NULL;
   if (ipaddr)
      s_dns.ip = *ipaddr;
}

fn_io fn_tnfs_io_resolve(const char *host, uint32_t *ip)
{
   ip_addr_t a;
   if (!config_get_bool("net_enable"))
      return FN_IO_FAIL;
   if (ipaddr_aton(host, &a)) {
      *ip = ip4_addr_get_u32(ip_2_ip4(&a));
      return FN_IO_OK;
   }
   if (s_dns.done && strcmp(host, s_dns.name) == 0) {
      s_dns.done = false;
      if (!s_dns.ok)
         return FN_IO_FAIL;
      *ip = ip4_addr_get_u32(ip_2_ip4(&s_dns.ip));
      return FN_IO_OK;
   }
   if (s_dns.busy && strcmp(host, s_dns.name) == 0)
      return FN_IO_PENDING;
   if (strlen(host) >= sizeof s_dns.name)
      return FN_IO_FAIL;
   strcpy(s_dns.name, host);
   s_dns.done = false;
   s_dns.busy = true;
   err_t e = dns_gethostbyname(host, &s_dns.ip, tnfs_dns_found, NULL);
   if (e == ERR_OK) {                  /* cached */
      s_dns.busy = false;
      *ip = ip4_addr_get_u32(ip_2_ip4(&s_dns.ip));
      return FN_IO_OK;
   }
   if (e == ERR_INPROGRESS)
      return FN_IO_PENDING;
   s_dns.busy = false;
   return FN_IO_FAIL;
}

bool fn_tnfs_io_send(uint32_t ip, uint16_t port, const uint8_t *pkt, uint16_t len)
{
   ip_addr_t to;
   if (!config_get_bool("net_enable") || !socket_ready())
      return false;
   struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
   if (!p)
      return false;
   pbuf_take(p, pkt, len);
   ip_addr_set_ip4_u32(&to, ip);
   err_t e = udp_sendto(s_pcb, p, &to, port);
   pbuf_free(p);
   wifi_lwip_rx_kick();                /* a send usually precedes a reply */
   return e == ERR_OK;
}
