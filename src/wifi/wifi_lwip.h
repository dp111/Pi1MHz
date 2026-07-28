#ifndef WIFI_WIFI_LWIP_H
#define WIFI_WIFI_LWIP_H

#include <stdbool.h>

#include "lwip/ip4_addr.h"
#include "lwip/netif.h"

typedef struct {
   bool prepared;
   bool initialized;
   bool netif_added;
   bool static_configured;
   bool dhcp_started;
   bool timers_running;
   bool use_dhcp;
   bool has_dns;
   bool link_up;
   bool link_established;
   bool address_ready;
   struct netif netif;
   ip4_addr_t ipaddr;
   ip4_addr_t netmask;
   ip4_addr_t gateway;
   ip4_addr_t dns;
   uint32_t init_time_us;
   uint32_t last_service_time_us;
   uint32_t service_calls;
} wifi_lwip_context_t;

void wifi_lwip_prepare(void);
void wifi_lwip_init_stack(void);
void wifi_lwip_poll(void);
const wifi_lwip_context_t *wifi_lwip_get_context(void);

/* Drain the RX at full rate for a short window: call after transmitting a
 * datagram that expects a prompt reply, so the reply is not held by the
 * idle-throttle backoff. */
void wifi_lwip_rx_kick(void);
void wifi_lwip_icmp_probe_counts(uint32_t *rx_seen, uint32_t *tx_seen);
void wifi_lwip_tx_path_counts(uint32_t *queued, uint32_t *stale,
                              uint32_t *direct_fail, uint32_t *hold_max_us);
uint32_t wifi_lwip_icmp_probe_read(uint32_t *gap_ms, uint32_t *turnaround_us,
                                  uint32_t max, uint32_t *total);

#endif