#include "wifi_lwip.h"

#include "netname.h"
#include "sdio.h"
#include "wifi.h"

#include "../Pi1MHz.h"
#include "../rpi/rpi.h"
#include "../rpi/systimer.h"

#include "lwip/dhcp.h"
#include "lwip/prot/dhcp.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/init.h"
#include "lwip/ip.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/timeouts.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static wifi_lwip_context_t g_wifi_lwip_context;
static bool g_wifi_lwip_link_logged;
static bool g_wifi_lwip_last_link_up;
static bool g_wifi_lwip_address_logged;
static bool g_wifi_lwip_last_address_ready;
static u8_t g_wifi_lwip_last_dhcp_state = 0xFFu;   /* 0xFF = not seen yet */
static void wifi_lwip_debug_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#define WIFI_LWIP_RX_FRAME_MAX_LEN 1600u
#define WIFI_LWIP_RX_FRAME_BUDGET 8u

/* If the WiFi link has still not associated this long after the lwIP
   stack was brought up, treat the boot as failed: report the error and
   stop polling.  Association is normally a sub-second operation once the
   radio works, so this window is deliberately generous - it only needs
   to cover a join whose WLC_E_LINK event arrives late in the poll loop. */
#define WIFI_LWIP_LINK_TIMEOUT_US (30u * 1000000u)

u32_t sys_now(void)
{
   return RPI_GetSystemTime() / 1000u;
}

u32_t sys_jiffies(void)
{
   return RPI_GetSystemTime();
}

static void wifi_lwip_debug_log(const char *format, ...)
{
   va_list args;
   char line[192];
   int written;

   if (!wifi_debug_enabled())
      return;

   va_start(args, format);
   written = vsnprintf(line, sizeof(line), format, args);
   va_end(args);

   if (written <= 0)
      return;

   wifi_debug_printf("WIFI-LWIP: %s\r\n", line);
}

static bool wifi_lwip_address_ready(const struct netif *netif)
{
   if (netif == NULL)
      return false;

   return !ip4_addr_isany_val(*netif_ip4_addr(netif));
}

static void wifi_lwip_update_runtime_state(void)
{
   char ipaddr_text[20];

   if (g_wifi_lwip_context.netif_added) {
      if (sdio_runtime_link_is_up())
         netif_set_link_up(&g_wifi_lwip_context.netif);
      else
         netif_set_link_down(&g_wifi_lwip_context.netif);
   }

   g_wifi_lwip_context.link_up = g_wifi_lwip_context.netif_added
      && netif_is_link_up(&g_wifi_lwip_context.netif);
   g_wifi_lwip_context.address_ready = g_wifi_lwip_context.netif_added
      && wifi_lwip_address_ready(&g_wifi_lwip_context.netif);

   /* Latch the first time the link comes up.  Once set, the boot-time
      link-up timeout in wifi_lwip_poll() is disabled, so a later
      transient link drop keeps polling and is allowed to recover. */
   if (g_wifi_lwip_context.link_up)
      g_wifi_lwip_context.link_established = true;

   if (!g_wifi_lwip_link_logged || g_wifi_lwip_last_link_up != g_wifi_lwip_context.link_up) {
      wifi_lwip_debug_log("link %s", g_wifi_lwip_context.link_up ? "up" : "down");
      g_wifi_lwip_last_link_up = g_wifi_lwip_context.link_up;
      g_wifi_lwip_link_logged = true;
   }

   if (!g_wifi_lwip_address_logged || g_wifi_lwip_last_address_ready != g_wifi_lwip_context.address_ready) {
      if (g_wifi_lwip_context.address_ready) {
         wifi_lwip_debug_log("address ready ip=%s",
                             ip4addr_ntoa_r(netif_ip4_addr(&g_wifi_lwip_context.netif),
                                            ipaddr_text,
                                            (int)sizeof(ipaddr_text)));
      } else {
         wifi_lwip_debug_log("address not ready");
      }

      g_wifi_lwip_last_address_ready = g_wifi_lwip_context.address_ready;
      g_wifi_lwip_address_logged = true;
   }

   if (g_wifi_lwip_context.address_ready)
      wifi_note_network_ready();
}

static err_t wifi_lwip_link_output(struct netif *netif, struct pbuf *p)
{
   /* static: this is on the cooperative poll path and is large (~1.6 KB).
      Keeping it off the stack avoids a deep RX->TX nesting blowing the
      bare-metal stack.  The function is never re-entered. */
   static uint8_t frame[WIFI_LWIP_RX_FRAME_MAX_LEN];
   uint16_t offset = 0u;
   const struct pbuf *cursor = p;

   (void)netif;

   if (p == NULL || p->tot_len > sizeof(frame))
      return ERR_IF;

   while (cursor != NULL) {
      if ((uint16_t)(offset + cursor->len) > sizeof(frame))
         return ERR_IF;
      memcpy(&frame[offset], cursor->payload, cursor->len);
      offset = (uint16_t)(offset + cursor->len);
      cursor = cursor->next;
   }

   return sdio_runtime_send_ethernet_frame(frame, offset) ? ERR_OK : ERR_IF;
}

static void wifi_lwip_drain_rx_frames(void)
{
   /* static: this is on the cooperative poll path and is large (~1.6 KB).
      Keeping it off the stack avoids a deep RX->TX nesting blowing the
      bare-metal stack.  The function is never re-entered. */
   static uint8_t frame[WIFI_LWIP_RX_FRAME_MAX_LEN];
   uint16_t frame_length;
   uint8_t frame_index;

   if (!g_wifi_lwip_context.netif_added)
      return;

   for (frame_index = 0u; frame_index < WIFI_LWIP_RX_FRAME_BUDGET; ++frame_index) {
      struct pbuf *packet;

      frame_length = 0u;
      if (!sdio_runtime_poll_ethernet_frame(frame, sizeof(frame), &frame_length))
         return;

      if (frame_length == 0u)
         continue;

      packet = pbuf_alloc(PBUF_RAW, frame_length, PBUF_POOL);
      if (packet == NULL)
         return;

      if (pbuf_take(packet, frame, frame_length) != ERR_OK) {
         pbuf_free(packet);
         return;
      }

      if (g_wifi_lwip_context.netif.input(packet, &g_wifi_lwip_context.netif) != ERR_OK) {
         pbuf_free(packet);
         return;
      }
   }
}

static err_t wifi_lwip_netif_init(struct netif *netif)
{
   netif->name[0] = 'w';
   netif->name[1] = 'f';
   netif->output = etharp_output;
   netif->linkoutput = wifi_lwip_link_output;
   netif->hwaddr_len = 6;
   /* Use the chip's real WiFi MAC: the firmware associates with that
      address, so the lwIP netif must present the same one or DHCP and
      ARP replies are addressed to a station the access point does not
      know.  Fall back to a fixed locally-administered address only if
      the chip MAC could not be read. */
   if (!sdio_runtime_get_chip_mac(netif->hwaddr)) {
      netif->hwaddr[0] = 0x02;
      netif->hwaddr[1] = 0x50;
      netif->hwaddr[2] = 0x31;
      netif->hwaddr[3] = 0x4d;
      netif->hwaddr[4] = 0x48;
      netif->hwaddr[5] = 0x7a;
   }
   netif->mtu = 1500;
   netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
   return ERR_OK;
}

static void wifi_lwip_copy_ip4(ip4_addr_t *target, const wifi_ipv4_addr_t *source)
{
   if (target == NULL || source == NULL)
      return;

   IP4_ADDR(target,
            source->octets[0],
            source->octets[1],
            source->octets[2],
            source->octets[3]);
}

void wifi_lwip_prepare(void)
{
   const wifi_config_t *config = wifi_get_config();
   const wifi_network_config_t *network_config = wifi_get_network_config();

   memset(&g_wifi_lwip_context, 0, sizeof(g_wifi_lwip_context));
   g_wifi_lwip_link_logged = false;
   g_wifi_lwip_address_logged = false;
   g_wifi_lwip_context.use_dhcp = network_config->ip_mode == WIFI_IP_MODE_DHCP;
   g_wifi_lwip_context.prepared = network_config->valid;
#if LWIP_NETIF_HOSTNAME
   g_wifi_lwip_context.netif.hostname = config->hostname;
#endif

   ip4_addr_set_zero(&g_wifi_lwip_context.ipaddr);
   ip4_addr_set_zero(&g_wifi_lwip_context.netmask);
   ip4_addr_set_zero(&g_wifi_lwip_context.gateway);
   ip4_addr_set_zero(&g_wifi_lwip_context.dns);
   ip_addr_set_zero_ip4(&g_wifi_lwip_context.netif.ip_addr);
   ip_addr_set_zero_ip4(&g_wifi_lwip_context.netif.netmask);
   ip_addr_set_zero_ip4(&g_wifi_lwip_context.netif.gw);

   if (!g_wifi_lwip_context.prepared || g_wifi_lwip_context.use_dhcp)
   {
      wifi_lwip_debug_log("prepare mode=%s prepared=%u",
                          g_wifi_lwip_context.use_dhcp ? "dhcp" : "static",
                          g_wifi_lwip_context.prepared ? 1u : 0u);
      return;
   }

   wifi_lwip_copy_ip4(&g_wifi_lwip_context.ipaddr, &network_config->address);
   wifi_lwip_copy_ip4(&g_wifi_lwip_context.netmask, &network_config->netmask);

   if (network_config->has_gateway)
      wifi_lwip_copy_ip4(&g_wifi_lwip_context.gateway, &network_config->gateway);

   if (network_config->has_dns) {
      wifi_lwip_copy_ip4(&g_wifi_lwip_context.dns, &network_config->dns);
      g_wifi_lwip_context.has_dns = true;
   }

   ip_addr_set_ip4_u32(&g_wifi_lwip_context.netif.ip_addr, ip4_addr_get_u32(&g_wifi_lwip_context.ipaddr));
   ip_addr_set_ip4_u32(&g_wifi_lwip_context.netif.netmask, ip4_addr_get_u32(&g_wifi_lwip_context.netmask));
   ip_addr_set_ip4_u32(&g_wifi_lwip_context.netif.gw, ip4_addr_get_u32(&g_wifi_lwip_context.gateway));
   wifi_lwip_debug_log("prepare mode=static ip=%s",
                       ip4addr_ntoa(&g_wifi_lwip_context.ipaddr));
}

void wifi_lwip_init_stack(void)
{
   ip_addr_t dns_address;
   err_t dhcp_result;

   if (!g_wifi_lwip_context.prepared)
      return;

   lwip_init();
   wifi_lwip_debug_log("lwip core initialised");
   g_wifi_lwip_context.initialized = true;
   g_wifi_lwip_context.init_time_us = RPI_GetSystemTime();

   if (netif_add(&g_wifi_lwip_context.netif,
                 g_wifi_lwip_context.use_dhcp ? NULL : &g_wifi_lwip_context.ipaddr,
                 g_wifi_lwip_context.use_dhcp ? NULL : &g_wifi_lwip_context.netmask,
                 g_wifi_lwip_context.use_dhcp ? NULL : &g_wifi_lwip_context.gateway,
                 &g_wifi_lwip_context,
                 wifi_lwip_netif_init,
                 ethernet_input) == NULL) {
      return;
   }

   g_wifi_lwip_context.netif_added = true;
   netif_set_default(&g_wifi_lwip_context.netif);
   netif_set_up(&g_wifi_lwip_context.netif);
   netif_set_link_down(&g_wifi_lwip_context.netif);
   g_wifi_lwip_context.timers_running = true;
   /* No poll registration: wifi_lwip_poll is called from
      wifi_dispatch_poll in wifi.c so the whole WiFi stack costs a
      single slot in the main Pi1MHz poll table.  timers_running
      gates the poll so it stays a no-op until this point. */
   wifi_lwip_debug_log("netif added");

   /* Start the NetBIOS / mDNS name responders so the Pi can be reached
      by name as well as by IP address. */
   netname_init();

   if (g_wifi_lwip_context.use_dhcp) {
      dhcp_result = dhcp_start(&g_wifi_lwip_context.netif);
      if (dhcp_result == ERR_OK)
         g_wifi_lwip_context.dhcp_started = true;
      wifi_lwip_debug_log("dhcp_start result=%d", (int)dhcp_result);
      wifi_lwip_update_runtime_state();
      return;
   }

   g_wifi_lwip_context.static_configured = true;
   if (g_wifi_lwip_context.has_dns) {
      ip_addr_set_ip4_u32(&dns_address, ip4_addr_get_u32(&g_wifi_lwip_context.dns));
      dns_setserver(0, &dns_address);
   }

   wifi_lwip_debug_log("static network configured");

   wifi_lwip_update_runtime_state();
}

static const char *wifi_lwip_dhcp_state_name(u8_t state)
{
   switch (state) {
      case DHCP_STATE_OFF:         return "off";
      case DHCP_STATE_INIT:        return "init";
      case DHCP_STATE_SELECTING:   return "selecting (Discover sent)";
      case DHCP_STATE_REQUESTING:  return "requesting (Offer in, Request sent)";
      case DHCP_STATE_CHECKING:    return "checking (Ack in, conflict check)";
      case DHCP_STATE_BOUND:       return "bound (address acquired)";
      case DHCP_STATE_RENEWING:    return "renewing";
      case DHCP_STATE_REBINDING:   return "rebinding";
      case DHCP_STATE_REBOOTING:   return "rebooting";
      case DHCP_STATE_RELEASING:   return "releasing";
      case DHCP_STATE_BACKING_OFF: return "backing off (timed out, retrying)";
      case DHCP_STATE_PERMANENT:   return "permanent";
      case DHCP_STATE_INFORMING:   return "informing";
      default:                     return "unknown";
   }
}

/* Log every DHCP state change with a millisecond timestamp, so the time
   from link-up to address acquisition - and any retries - can be seen.
   wifi_lwip_debug_log() self-gates, so this is silent unless wifi_debug
   is enabled. */
static void wifi_lwip_log_dhcp_state(void)
{
   struct dhcp *d = netif_dhcp_data(&g_wifi_lwip_context.netif);

   if (d == NULL || d->state == g_wifi_lwip_last_dhcp_state)
      return;

   g_wifi_lwip_last_dhcp_state = d->state;
   wifi_lwip_debug_log("dhcp %s  t=%lu ms  tries=%u",
                       wifi_lwip_dhcp_state_name(d->state),
                       (unsigned long)(RPI_GetSystemTime() / 1000u),
                       (unsigned int)d->tries);
}

void wifi_lwip_poll(void)
{
   if (!g_wifi_lwip_context.timers_running)
      return;

   /* Stop polling once the WiFi boot has failed.  There is no point
      hammering the SDIO bus and lwIP timers when the stack can never
      come up; timers_running latches false so this is permanent. */
   if (wifi_get_state() == WIFI_STATE_ERROR) {
      g_wifi_lwip_context.timers_running = false;
      wifi_lwip_debug_log("wifi in error state - polling stopped");
      return;
   }

   /* Boot-time link-up timeout: if the link never associated within the
      window, the join has failed - report it as an error and stop
      polling rather than spinning on the SDIO bus forever.  Disabled
      once the link has come up even once (link_established). */
   if (!g_wifi_lwip_context.link_established
       && (RPI_GetSystemTime() - g_wifi_lwip_context.init_time_us) > WIFI_LWIP_LINK_TIMEOUT_US) {
      wifi_lwip_debug_log("link-up timeout - WiFi join failed, polling stopped");
      wifi_set_error("WiFi failed to associate within the boot timeout");
      g_wifi_lwip_context.timers_running = false;
      return;
   }

   /* Drain inbound frames and hand them to lwIP.  sdio_runtime_poll_-
      ethernet_frame() also processes the chip's async events (WLC_E_LINK,
      WLC_E_SET_SSID, etc.) as a side effect, so this single drain covers
      both events and data.

      A separate sdio_runtime_poll_events() call used to run here first -
      but it read frames into a throwaway buffer, so every inbound TCP/UDP
      data frame it touched was consumed from the chip and silently
      discarded before lwIP could see it.  That crippled throughput
      (constant retransmits); draining straight into lwIP fixes it. */
   wifi_lwip_drain_rx_frames();
   sys_check_timeouts();
   wifi_lwip_log_dhcp_state();
   g_wifi_lwip_context.last_service_time_us = RPI_GetSystemTime();
   g_wifi_lwip_context.service_calls++;
   wifi_lwip_update_runtime_state();
}

const wifi_lwip_context_t *wifi_lwip_get_context(void)
{
   return &g_wifi_lwip_context;
}