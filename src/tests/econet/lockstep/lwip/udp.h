#pragma once
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
struct udp_pcb;
typedef void (*udp_recv_fn)(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port);
struct udp_pcb { udp_recv_fn cb; void *arg; uint16_t port; };
struct udp_pcb *udp_new(void);
err_t udp_bind(struct udp_pcb *pcb, const ip_addr_t *ip, uint16_t port);
void udp_recv(struct udp_pcb *pcb, udp_recv_fn cb, void *arg);
void udp_remove(struct udp_pcb *pcb);
err_t udp_sendto(struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *ip, uint16_t port);
