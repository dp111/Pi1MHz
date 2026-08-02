#ifndef LWIP_HDR_ALTCP_H
#define LWIP_HDR_ALTCP_H
/* Minimal altcp stub: struct altcp_pcb exposes the registered callbacks so
   the test can replay lwIP events (connected/recv/sent/err), plus the
   rcv_wnd fields net_service.c clamps under LWIP_ALTCP==0. */
#include "lwip/arch.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"

struct altcp_pcb;
typedef err_t (*altcp_recv_fn)(void *arg, struct altcp_pcb *conn,
                               struct pbuf *p, err_t err);
typedef err_t (*altcp_sent_fn)(void *arg, struct altcp_pcb *conn, u16_t len);
typedef err_t (*altcp_poll_fn)(void *arg, struct altcp_pcb *conn);
typedef void  (*altcp_err_fn)(void *arg, err_t err);
typedef err_t (*altcp_connected_fn)(void *arg, struct altcp_pcb *conn, err_t err);
typedef err_t (*altcp_accept_fn)(void *arg, struct altcp_pcb *new_conn, err_t err);

struct altcp_pcb {
   void            *arg;
   altcp_recv_fn    recv;
   altcp_sent_fn    sent;
   altcp_poll_fn    poll;
   altcp_err_fn     err;
   altcp_connected_fn connected;
   altcp_accept_fn  accept;
   u16_t            bound_port;
   int              listening;
   u16_t            rcv_wnd;
   u16_t            rcv_ann_wnd;
   /* test control / capture */
   u16_t            t_sndbuf;      /* value altcp_sndbuf returns             */
   err_t            t_write_err;   /* value altcp_write returns              */
   u32_t            t_recved;      /* total bytes altcp_recved acked         */
   int              t_closed;      /* altcp_close/abort seen                 */
   int              t_freed;       /* backing memory released                */
};

struct altcp_pcb *altcp_new_ip_type(void *allocator, u8_t ip_type);
void  altcp_arg  (struct altcp_pcb *conn, void *arg);
void  altcp_recv (struct altcp_pcb *conn, altcp_recv_fn recv);
void  altcp_sent (struct altcp_pcb *conn, altcp_sent_fn sent);
void  altcp_poll (struct altcp_pcb *conn, altcp_poll_fn poll, u8_t interval);
void  altcp_err  (struct altcp_pcb *conn, altcp_err_fn err);
err_t altcp_connect(struct altcp_pcb *conn, const ip_addr_t *ipaddr,
                    u16_t port, altcp_connected_fn connected);
err_t altcp_bind(struct altcp_pcb *conn, const ip_addr_t *ipaddr, u16_t port);
struct altcp_pcb *altcp_listen(struct altcp_pcb *conn);
void  altcp_accept(struct altcp_pcb *conn, altcp_accept_fn accept);
u16_t altcp_sndbuf(struct altcp_pcb *conn);
err_t altcp_write(struct altcp_pcb *conn, const void *dataptr, u16_t len,
                  u8_t apiflags);
void  altcp_output(struct altcp_pcb *conn);
void  altcp_recved(struct altcp_pcb *conn, u16_t len);
err_t altcp_close(struct altcp_pcb *conn);
void  altcp_abort(struct altcp_pcb *conn);
#endif
