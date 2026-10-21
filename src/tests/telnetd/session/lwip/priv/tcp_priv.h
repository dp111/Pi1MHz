/* Host stand-in for lwIP's tcp_priv.h (tests/telnetd session harness). */
#ifndef LWIP_HDR_TCP_PRIV_H
#define LWIP_HDR_TCP_PRIV_H
#include "lwip/tcp.h"
void tcp_rexmit_rto(struct tcp_pcb *pcb);
#endif
