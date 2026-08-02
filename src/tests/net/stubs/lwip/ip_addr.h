#ifndef LWIP_IP_ADDR_H
#define LWIP_IP_ADDR_H
#include "lwip/arch.h"
/* IPv4-only stub: addr held in network byte order (first octet in the low
   byte), matching ip4_addr_get_u32 semantics used by net_service.c. */
typedef struct { u32_t addr; } ip_addr_t;
#define IPADDR_TYPE_V4 0
#define IP_ADDR4(ip, a, b, c, d) \
   ((ip)->addr = (u32_t)(a) | ((u32_t)(b) << 8) \
               | ((u32_t)(c) << 16) | ((u32_t)(d) << 24))
static inline const ip_addr_t *ip_2_ip4(const ip_addr_t *ip) { return ip; }
static inline u32_t ip4_addr_get_u32(const ip_addr_t *ip) { return ip->addr; }
#endif
