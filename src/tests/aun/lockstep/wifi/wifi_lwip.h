#pragma once
#include <stdbool.h>
#include "lwip/ip_addr.h"
struct netif { uint32_t ip; uint32_t mask; };
typedef struct { bool address_ready; bool netif_added; struct netif netif; } wifi_lwip_context_t;
const wifi_lwip_context_t *wifi_lwip_get_context(void);
/* aun_udp_send() kicks the RX into full-rate draining after a send; a no-op
 * in the host harness (there is no real lwIP RX poll to accelerate). */
static inline void wifi_lwip_rx_kick(void) { }
#define netif_ip4_addr(n) (&(n)->ip)
#define netif_ip4_netmask(n) (&(n)->mask)
#define ip4_addr_get_u32(p) (*(p))
