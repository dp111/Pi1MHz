#ifndef LWIP_IP_ADDR_H
#define LWIP_IP_ADDR_H
#include <stdint.h>
typedef struct { uint32_t addr; } ip_addr_t;     /* network order */
int   ipaddr_aton(const char *cp, ip_addr_t *addr);
char *ipaddr_ntoa(const ip_addr_t *addr);
#define ip_addr_copy(dst, src) ((dst) = (src))
#endif
