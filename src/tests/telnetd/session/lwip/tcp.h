/* Host stand-in for lwIP's raw TCP API (tests/telnetd session harness).  The pcb carries the
   fields telnetd.c reads, and the harness's own: the callbacks, and what the client was sent. */
#ifndef LWIP_HDR_TCP_H
#define LWIP_HDR_TCP_H
#include <stddef.h>
#include "lwip/err.h"
#include "lwip/ip4_addr.h"
#include "lwip/pbuf.h"

#define TCP_MSS              1460u
#define TCP_SND_QUEUELEN     128u     /* lwIP's (4 * TCP_SND_BUF + TCP_MSS - 1) / TCP_MSS, TCP_SND_BUF 32 MSS */
#define TCP_WRITE_FLAG_COPY  0x01u
enum tcp_state { CLOSED = 0, LISTEN = 1, ESTABLISHED = 4 };

struct tcp_pcb;
typedef err_t (*tcp_accept_fn)(void *arg, struct tcp_pcb *newpcb, err_t err);
typedef err_t (*tcp_recv_fn)(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
typedef err_t (*tcp_sent_fn)(void *arg, struct tcp_pcb *tpcb, u16_t len);
typedef err_t (*tcp_poll_fn)(void *arg, struct tcp_pcb *tpcb);
typedef void  (*tcp_err_fn)(void *arg, err_t err);

struct tcp_pcb {
   ip_addr_t      remote_ip;
   u16_t          snd_queuelen;
   u32_t          lastack;
   u8_t           nrtx;
   void          *unacked;
   enum tcp_state state;
   u16_t          rcv_wnd, snd_wnd;
   s16_t          rto, rtime;
   /* the harness's */
   int            id;
   u16_t          port;
   void          *arg;
   tcp_accept_fn  accept;
   tcp_recv_fn    recv;
   tcp_sent_fn    sent;
   tcp_err_fn     errf;
   u16_t          sndbuf;
   unsigned       recved;
   int            closed, aborted;
   char           got[65536];
   size_t         got_len;
};

struct tcp_pcb *tcp_new_ip_type(u8_t type);
err_t  tcp_bind(struct tcp_pcb *pcb, const ip_addr_t *ipaddr, u16_t port);
struct tcp_pcb *tcp_listen_with_backlog(struct tcp_pcb *pcb, u8_t backlog);
void   tcp_arg(struct tcp_pcb *pcb, void *arg);
void   tcp_accept(struct tcp_pcb *pcb, tcp_accept_fn accept);
void   tcp_recv(struct tcp_pcb *pcb, tcp_recv_fn recv);
void   tcp_sent(struct tcp_pcb *pcb, tcp_sent_fn sent);
void   tcp_poll(struct tcp_pcb *pcb, tcp_poll_fn poll, u8_t interval);
void   tcp_err(struct tcp_pcb *pcb, tcp_err_fn err);
err_t  tcp_write(struct tcp_pcb *pcb, const void *dataptr, u16_t len, u8_t apiflags);
err_t  tcp_output(struct tcp_pcb *pcb);
void   tcp_recved(struct tcp_pcb *pcb, u16_t len);
err_t  tcp_close(struct tcp_pcb *pcb);
void   tcp_abort(struct tcp_pcb *pcb);
u16_t  tcp_sndbuf(const struct tcp_pcb *pcb);
void   tcp_nagle_disable(struct tcp_pcb *pcb);
#endif
