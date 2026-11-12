#ifndef LWIP_IP_ADDR_H
#define LWIP_IP_ADDR_H
#include <stdint.h>
typedef struct { uint32_t addr; } ip_addr_t;     /* network order */
int   ipaddr_aton(const char *cp, ip_addr_t *addr);
char *ipaddr_ntoa(const ip_addr_t *addr);
#define ip_addr_copy(dst, src) ((dst) = (src))
/* as lwIP: a.b.c.d in network order (the host is little-endian) */
#define IP_ADDR4(ip, a, b, c, d) ((ip)->addr = (uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#endif
