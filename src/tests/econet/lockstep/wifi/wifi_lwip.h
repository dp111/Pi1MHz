#pragma once
#include <stdbool.h>
#include "lwip/ip_addr.h"
struct netif { uint32_t ip; };
typedef struct { bool address_ready; bool netif_added; struct netif netif; } wifi_lwip_context_t;
const wifi_lwip_context_t *wifi_lwip_get_context(void);
#define netif_ip4_addr(n) (&(n)->ip)
#define ip4_addr_get_u32(p) (*(p))
