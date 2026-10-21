/* lwip_util.h - small lwIP helpers the network services share: queue as
   much as a connection's send buffer will take, and copy a received pbuf
   chain into a byte ring. */
#ifndef WIFI_LWIP_UTIL_H
#define WIFI_LWIP_UTIL_H

#include <stddef.h>
#include <stdint.h>

#include "lwip/altcp.h"
#include "lwip/err.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

/* Queue up to len bytes of data on pcb (TCP_WRITE_FLAG_COPY), as many as its
   send buffer has room for, settling for one segment when the heap cannot
   take more (see lwip_util.c).  Does not call tcp_output: when to push is
   the caller's.  Returns:
     ERR_OK  with *queued the bytes taken - 0 when the send buffer is full;
     ERR_MEM with *queued 0 - the heap could not take even one segment, so
             try again later;
     any other tcp_write error with *queued 0 - the connection is unusable. */
err_t lwip_tcp_write_some(struct tcp_pcb *pcb, const void *data, size_t len,
                          u16_t *queued);

/* The same through the altcp layer (plain TCP or TLS). */
err_t lwip_altcp_write_some(struct altcp_pcb *pcb, const void *data, size_t len,
                            u16_t *queued);

/* Append len bytes at ring[head], wrapping with mask (the ring's size is
   mask + 1, a power of two), and return the new head.  The caller has
   checked that they fit and keeps its own count. */
uint32_t byte_ring_put(uint8_t *ring, uint32_t mask, uint32_t head,
                       const void *src, uint32_t len);

/* byte_ring_put for every buffer of a pbuf chain (p->tot_len bytes). */
uint32_t byte_ring_put_pbuf(uint8_t *ring, uint32_t mask, uint32_t head,
                            const struct pbuf *p);

#endif /* WIFI_LWIP_UTIL_H */
