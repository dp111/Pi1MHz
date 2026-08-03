#ifndef LWIP_UDP_H
#define LWIP_UDP_H
#include "lwip/arch.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"

struct udp_pcb;
typedef void (*udp_recv_fn)(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                            const ip_addr_t *addr, u16_t port);
struct udp_pcb {
   void        *arg;
   udp_recv_fn  recv;
   u16_t        bound_port;
   u32_t        connected_ip;
   u16_t        connected_port;
   int          removed;
};

struct udp_pcb *udp_new(void);
err_t udp_bind(struct udp_pcb *pcb, const ip_addr_t *ipaddr, u16_t port);
void  udp_recv(struct udp_pcb *pcb, udp_recv_fn recv, void *arg);
err_t udp_sendto(struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *dst,
                 u16_t port);
err_t udp_connect(struct udp_pcb *pcb, const ip_addr_t *ipaddr, u16_t port);
void  udp_disconnect(struct udp_pcb *pcb);
void  udp_remove(struct udp_pcb *pcb);
#endif
