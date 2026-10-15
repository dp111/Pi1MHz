#ifndef LWIP_DNS_H
#define LWIP_DNS_H
#include "lwip/err.h"
#include "lwip/ip_addr.h"
typedef void (*dns_found_callback)(const char *name, const ip_addr_t *ipaddr, void *callback_arg);
#endif
