#pragma once
/* Host-test stub of wifi/wifi_lwip.h: the context fields and lwIP address
   helpers wifi_service.c reads (IFCFG/ONLINE). */
#include <stdbool.h>
#include <stdint.h>
typedef struct { uint32_t addr; } ip4_addr_t;
struct netif { ip4_addr_t ip_addr; };
typedef struct {
   bool netif_added;
   bool address_ready;
   struct netif netif;
} wifi_lwip_context_t;
#define netif_ip4_addr(n) (&(n)->ip_addr)
#define ip4_addr1(a) ((unsigned)((a)->addr & 0xffu))
#define ip4_addr2(a) ((unsigned)(((a)->addr >> 8) & 0xffu))
#define ip4_addr3(a) ((unsigned)(((a)->addr >> 16) & 0xffu))
#define ip4_addr4(a) ((unsigned)((a)->addr >> 24))
#define ip4_addr_isany_val(a) ((a).addr == 0u)
const wifi_lwip_context_t *wifi_lwip_get_context(void);
void wifi_lwip_rx_kick(void);
