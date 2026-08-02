#ifndef LWIP_TCP_H
#define LWIP_TCP_H
/* net_service.c includes this only for TCP_WRITE_FLAG_COPY; the rcv_wnd
   clamp fields live on struct altcp_pcb in the stub (see altcp.h). */
#define TCP_WRITE_FLAG_COPY 0x01
struct tcp_pcb;
#endif
