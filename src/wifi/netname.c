/* Local-network name resolution for the Pi1MHz webserver.
 *
 * Two mechanisms let the webserver be reached by name:
 *
 *   NetBIOS - answers NetBIOS name-service queries on UDP 137.  Windows
 *             broadcasts these, so "http://<hostname>/" resolves with no
 *             suffix.  Broadcast reception is reliable on this stack
 *             (it is the same path DHCP already uses).
 *
 *   mDNS    - this lwIP build has no IGMP, so it cannot RECEIVE the
 *             multicast mDNS queries.  Instead the Pi periodically
 *             MULTICASTS an unsolicited A-record announcement for
 *             "<hostname>.local"; mDNS resolvers cache that, which makes
 *             "http://<hostname>.local/" resolve on macOS/iOS/Linux and
 *             Windows 10+.  Multicast transmission needs no IGMP.
 */

#include "netname.h"

#include "wifi.h"
#include "wifi_lwip.h"
#include "../Pi1MHz.h"
#include "../rpi/systimer.h"

#include "lwip/err.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define NETNAME_HOST_MAX     32u
#define NETBIOS_PORT         137u
#define MDNS_PORT            5353u
#define MDNS_TTL_SECONDS     120u
#define NETBIOS_TTL_SECONDS  300u
#define NETNAME_ANNOUNCE_US  (20u * 1000000u)  /* re-announce interval */
#define NBNS_NAME_FIELD_LEN  34u   /* 0x20 length + 32 encoded + 0x00 */
#define NBNS_REQUEST_MIN     50u   /* 12 header + 34 name + 4 type/class */
#define NBNS_BUFFER_MAX      96u

static struct udp_pcb *g_nbns_pcb;
static struct udp_pcb *g_mdns_pcb;
static ip_addr_t       g_mdns_group;
static char            g_host[NETNAME_HOST_MAX + 1u];
static uint8_t         g_host_len;
static bool            g_ready;
static bool            g_announced;
static uint32_t        g_last_announce_us;

static uint8_t netname_to_upper(uint8_t c)
{
   return (c >= 'a' && c <= 'z') ? (uint8_t)(c - 32u) : c;
}

/* Fetch the WiFi netif's current IPv4 address as four wire-order octets.
   Returns false until DHCP (or static config) has assigned an address. */
static bool netname_get_ip(uint8_t out[4])
{
   const wifi_lwip_context_t *ctx = wifi_lwip_get_context();
   const ip4_addr_t          *ip;

   if (ctx == NULL)
      return false;

   ip = netif_ip4_addr(&ctx->netif);
   if (ip4_addr_isany_val(*ip))
      return false;

   out[0] = ip4_addr1_val(*ip);
   out[1] = ip4_addr2_val(*ip);
   out[2] = ip4_addr3_val(*ip);
   out[3] = ip4_addr4_val(*ip);
   return true;
}

/* Multicast an unsolicited mDNS response advertising <hostname>.local. */
static void netname_send_mdns(const uint8_t ip[4])
{
   uint8_t      msg[96];
   uint32_t     len;
   struct pbuf *p;

   if (g_mdns_pcb == NULL)
      return;

   /* DNS header: response, authoritative, one answer record. */
   msg[0] = 0u;  msg[1] = 0u;          /* transaction id           */
   msg[2] = 0x84u; msg[3] = 0x00u;     /* flags: QR=1, AA=1        */
   msg[4] = 0u;  msg[5] = 0u;          /* QDCOUNT = 0              */
   msg[6] = 0u;  msg[7] = 1u;          /* ANCOUNT = 1              */
   msg[8] = 0u;  msg[9] = 0u;          /* NSCOUNT = 0              */
   msg[10] = 0u; msg[11] = 0u;         /* ARCOUNT = 0              */
   len = 12u;

   /* Answer NAME: <hostname>.local */
   msg[len++] = g_host_len;
   memcpy(&msg[len], g_host, g_host_len);
   len += g_host_len;
   msg[len++] = 5u;
   msg[len++] = 'l'; msg[len++] = 'o'; msg[len++] = 'c';
   msg[len++] = 'a'; msg[len++] = 'l';
   msg[len++] = 0u;

   msg[len++] = 0u; msg[len++] = 1u;     /* TYPE  = A                */
   msg[len++] = 0x80u; msg[len++] = 1u;  /* CLASS = IN, cache-flush  */
   msg[len++] = 0u; msg[len++] = 0u;
   msg[len++] = (uint8_t)((MDNS_TTL_SECONDS >> 8) & 0xFFu);
   msg[len++] = (uint8_t)(MDNS_TTL_SECONDS & 0xFFu);   /* TTL        */
   msg[len++] = 0u; msg[len++] = 4u;     /* RDLENGTH = 4             */
   msg[len++] = ip[0]; msg[len++] = ip[1];
   msg[len++] = ip[2]; msg[len++] = ip[3];

   p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
   if (p == NULL)
      return;
   if (pbuf_take(p, msg, (u16_t)len) == ERR_OK)
      (void)udp_sendto(g_mdns_pcb, p, &g_mdns_group, MDNS_PORT);
   pbuf_free(p);
}

/* UDP 137 receive callback: answer a NetBIOS name query for our name. */
static void netname_nbns_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                              const ip_addr_t *addr, u16_t port)
{
   uint8_t      req[NBNS_BUFFER_MAX];
   uint8_t      resp[12u + NBNS_NAME_FIELD_LEN + 16u];
   uint8_t      decoded[16];
   uint8_t      ip[4];
   uint16_t     reqlen;
   uint16_t     flags;
   uint16_t     qdcount;
   uint32_t     i;
   uint32_t     namelen;
   struct pbuf *out;

   (void)arg;

   if (p == NULL)
      return;

   reqlen = pbuf_copy_partial(p, req, (u16_t)sizeof req, 0u);
   pbuf_free(p);

   if (reqlen < NBNS_REQUEST_MIN)
      return;

   flags   = (uint16_t)(((uint16_t)req[2] << 8) | req[3]);
   qdcount = (uint16_t)(((uint16_t)req[4] << 8) | req[5]);
   if ((flags & 0x8000u) != 0u)          /* a response, not a query  */
      return;
   if (((flags >> 11) & 0x0Fu) != 0u)    /* opcode is not "query"    */
      return;
   if (qdcount == 0u)
      return;
   if (req[12] != 0x20u)                 /* 32-byte encoded name     */
      return;

   /* NetBIOS first-level decode: 32 chars 'A'..'P' -> 16 bytes. */
   for (i = 0u; i < 16u; ++i) {
      uint8_t hi = req[13u + i * 2u];
      uint8_t lo = req[13u + i * 2u + 1u];
      if (hi < 'A' || hi > 'P' || lo < 'A' || lo > 'P')
         return;
      decoded[i] = (uint8_t)(((uint32_t)(hi - 'A') << 4) | (uint32_t)(lo - 'A'));
   }

   /* The name is the first 15 bytes, space-padded; the 16th is a suffix
      byte we deliberately ignore so any service suffix matches. */
   namelen = 15u;
   while (namelen > 0u && decoded[namelen - 1u] == (uint8_t)' ')
      --namelen;

   if (namelen != (uint32_t)g_host_len)
      return;
   for (i = 0u; i < namelen; ++i) {
      if (netname_to_upper(decoded[i])
          != netname_to_upper((uint8_t)g_host[i]))
         return;
   }

   if (!netname_get_ip(ip))
      return;

   /* Positive name-query response. */
   memset(resp, 0, sizeof resp);
   resp[0] = req[0];                     /* echo transaction id      */
   resp[1] = req[1];
   resp[2] = 0x85u;                      /* QR=1, AA=1, RD=1, query  */
   resp[3] = 0x00u;
   resp[7] = 1u;                         /* ANCOUNT = 1              */
   memcpy(&resp[12], &req[12], NBNS_NAME_FIELD_LEN);  /* echo name   */
   resp[12u + NBNS_NAME_FIELD_LEN + 1u]  = 0x20u;     /* type  NB    */
   resp[12u + NBNS_NAME_FIELD_LEN + 3u]  = 0x01u;     /* class IN    */
   resp[12u + NBNS_NAME_FIELD_LEN + 6u]  =
      (uint8_t)((NETBIOS_TTL_SECONDS >> 8) & 0xFFu);
   resp[12u + NBNS_NAME_FIELD_LEN + 7u]  =
      (uint8_t)(NETBIOS_TTL_SECONDS & 0xFFu);         /* TTL         */
   resp[12u + NBNS_NAME_FIELD_LEN + 9u]  = 6u;        /* RDLENGTH=6  */
   /* bytes 10..11 are the NB flags - left zero (unique, B-node)     */
   resp[12u + NBNS_NAME_FIELD_LEN + 12u] = ip[0];
   resp[12u + NBNS_NAME_FIELD_LEN + 13u] = ip[1];
   resp[12u + NBNS_NAME_FIELD_LEN + 14u] = ip[2];
   resp[12u + NBNS_NAME_FIELD_LEN + 15u] = ip[3];

   out = pbuf_alloc(PBUF_TRANSPORT, (u16_t)sizeof resp, PBUF_RAM);
   if (out == NULL)
      return;
   if (pbuf_take(out, resp, (u16_t)sizeof resp) == ERR_OK)
      (void)udp_sendto(pcb, out, addr, port);
   pbuf_free(out);
}

/* Poll hook: re-announce over mDNS at a steady interval. */
void netname_poll(void)
{
   uint8_t  ip[4];
   uint32_t now;

   if (!g_ready)
      return;
   if (!netname_get_ip(ip))
      return;                            /* no address assigned yet */

   now = RPI_GetSystemTime();
   if (g_announced && (now - g_last_announce_us) < NETNAME_ANNOUNCE_US)
      return;

   netname_send_mdns(ip);
   g_last_announce_us = now;
   g_announced = true;
}

void netname_init(void)
{
   const wifi_config_t *cfg;
   const char          *src;
   size_t               i;

   if (g_ready)
      return;

   cfg = wifi_get_config();
   src = (cfg != NULL && cfg->hostname[0] != '\0') ? cfg->hostname : "Pi1MHz";
   for (i = 0u; i < NETNAME_HOST_MAX && src[i] != '\0'; ++i)
      g_host[i] = src[i];
   g_host[i] = '\0';
   g_host_len = (uint8_t)i;
   if (g_host_len == 0u)
      return;

   /* NetBIOS name responder - broadcast queries arrive on UDP 137. */
   g_nbns_pcb = udp_new();
   if (g_nbns_pcb != NULL) {
      if (udp_bind(g_nbns_pcb, IP_ADDR_ANY, NETBIOS_PORT) == ERR_OK) {
         udp_recv(g_nbns_pcb, netname_nbns_recv, NULL);
      } else {
         udp_remove(g_nbns_pcb);
         g_nbns_pcb = NULL;
      }
   }

   /* mDNS announcer - bound to 5353 so announcements carry that source
      port; sent to the 224.0.0.251 multicast group. */
   IP4_ADDR(&g_mdns_group, 224, 0, 0, 251);
   g_mdns_pcb = udp_new();
   if (g_mdns_pcb != NULL) {
      if (udp_bind(g_mdns_pcb, IP_ADDR_ANY, MDNS_PORT) != ERR_OK) {
         udp_remove(g_mdns_pcb);
         g_mdns_pcb = NULL;
      }
   }

   /* No poll registration: netname_poll is called from
      wifi_dispatch_poll in wifi.c so the whole WiFi stack costs a
      single slot in the main Pi1MHz poll table. */
   g_ready = true;
}
