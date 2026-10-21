/* Host stand-in for lwIP's IPv4 addresses (tests/telnetd session harness). */
#ifndef LWIP_HDR_IP4_ADDR_H
#define LWIP_HDR_IP4_ADDR_H
#include "lwip/err.h"
typedef struct ip4_addr { u32_t addr; } ip4_addr_t;
typedef ip4_addr_t ip_addr_t;
#define ip_2_ip4(a)            (a)
#define ip4_addr_get_u32(a)    ((a)->addr)
#define ip_addr_cmp(a, b)      ((a)->addr == (b)->addr)
#define IPADDR_TYPE_V4         0u
extern const ip_addr_t ip_addr_any;
#define IP_ADDR_ANY            (&ip_addr_any)
char *ip4addr_ntoa(const ip4_addr_t *addr);
#endif
