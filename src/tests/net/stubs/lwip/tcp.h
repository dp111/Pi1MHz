#ifndef LWIP_TCP_H
#define LWIP_TCP_H
/* net_service.c includes this only for TCP_WRITE_FLAG_COPY; the rcv_wnd
   clamp fields live on struct altcp_pcb in the stub (see altcp.h).
   wifi/lwip_util.c, built with it, also has a raw-TCP sender: its two calls
   are declared here and defined (unused) by each suite. */
#include "lwip/arch.h"
#include "lwip/err.h"
#define TCP_WRITE_FLAG_COPY 0x01
#define TCP_MSS 1460
struct tcp_pcb;
u16_t tcp_sndbuf(const struct tcp_pcb *pcb);
err_t tcp_write(struct tcp_pcb *pcb, const void *dataptr, u16_t len, u8_t apiflags);
#endif
