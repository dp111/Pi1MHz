#ifndef LOCKSTEP_LWIP_IP_ADDR_H
#define LOCKSTEP_LWIP_IP_ADDR_H
#include <stdint.h>
typedef struct { uint32_t addr; } ip_addr_t;
#define ip_addr_set_ip4_u32(d,v) ((d)->addr=(v))
#define ip_addr_get_ip4_u32(p) ((p)->addr)
extern const ip_addr_t ip_addr_any;
#define IP_ADDR_ANY (&ip_addr_any)
#endif
