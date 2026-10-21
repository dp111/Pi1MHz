#pragma once
/* Host-test stub of wifi/wifi_lwip.h: the context fields and lwIP address
   helpers wifi_service.c reads (IFCFG/ONLINE). */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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
#define IP4_ADDR(ip, a, b, c, d) \
   ((ip)->addr = (uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
/* lwIP's ip4addr_ntoa_r: NULL when the text does not fit. */
static inline char *ip4addr_ntoa_r(const ip4_addr_t *a, char *buf, int len)
{
   int n = snprintf(buf, (size_t)len, "%u.%u.%u.%u", ip4_addr1(a), ip4_addr2(a),
                    ip4_addr3(a), ip4_addr4(a));
   return (n < 0 || n >= len) ? NULL : buf;
}
const wifi_lwip_context_t *wifi_lwip_get_context(void);
void wifi_lwip_rx_kick(void);
const char *wifi_lwip_dhcp_timezone(void);
