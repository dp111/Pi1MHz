/* Host stand-in for lwIP's altcp.h (tests/telnetd session harness): lwip_util.c's TLS half
   is compiled, never called. */
#ifndef LWIP_HDR_ALTCP_H
#define LWIP_HDR_ALTCP_H
#include "lwip/tcp.h"
struct altcp_pcb;
u16_t altcp_sndbuf(struct altcp_pcb *pcb);
err_t altcp_write(struct altcp_pcb *pcb, const void *dataptr, u16_t len, u8_t apiflags);
#endif
