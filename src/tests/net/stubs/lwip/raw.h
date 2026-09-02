#ifndef LWIP_RAW_H
#define LWIP_RAW_H
#include "lwip/arch.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"

struct raw_pcb;
/* Returns non-zero when the callback has taken (and freed) the pbuf. */
typedef u8_t (*raw_recv_fn)(void *arg, struct raw_pcb *pcb, struct pbuf *p,
                            const ip_addr_t *addr);
struct raw_pcb {
   void        *arg;
   raw_recv_fn  recv;
   u8_t         proto;
   int          removed;
};

#define IP_PROTO_ICMP 1u

struct raw_pcb *raw_new(u8_t proto);
void  raw_recv(struct raw_pcb *pcb, raw_recv_fn recv, void *arg);
err_t raw_bind(struct raw_pcb *pcb, const ip_addr_t *ipaddr);
err_t raw_sendto(struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *ipaddr);
void  raw_remove(struct raw_pcb *pcb);
#endif
