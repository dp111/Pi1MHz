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

#endif