/* lwip_util.c - see lwip_util.h. */
#include "lwip_util.h"

#include <string.h>

/* Clamp to the send buffer, then queue, settling for less rather than
   nothing.  tcp_write() is transactional: it queues the whole chunk or it
   queues nothing, and it allocates from the same MEM_SIZE heap that every
   other connection's unacked data is sitting in.  tcp_sndbuf() does not see
   that heap - it reports TCP window space - so on a busy server the sizes it
   blesses routinely fail to allocate.

   Retrying at one segment is what stops that becoming starvation.  With
   several concurrent webserver downloads, two saturated senders owned nearly
   all of the 32 KB heap; the rest asked for 8 KB, got ERR_MEM every time,
   queued not one byte, and were killed ~30 s later by the idle watchdog - the
   client seeing a 200 with an empty body.  A 1460-byte write fits in what is
   left, so the slow connections make progress instead of dying.  Deliberately
   not fixed by enlarging MEM_SIZE: 64 KB was measured to make all four
   streams crawl, because the heap is also what limits how far the SDIO
   transmit path can be oversubscribed. */
err_t lwip_tcp_write_some(struct tcp_pcb *pcb, const void *data, size_t len,
                          u16_t *queued)
{
   u16_t want = tcp_sndbuf(pcb);
   err_t err;

   *queued = 0u;
   if (len < want)
      want = (u16_t)len;
   if (want == 0u)
      return ERR_OK;                    /* send buffer full */
   err = tcp_write(pcb, data, want, TCP_WRITE_FLAG_COPY);
   if (err == ERR_MEM && want > TCP_MSS) {
      want = TCP_MSS;
      err = tcp_write(pcb, data, want, TCP_WRITE_FLAG_COPY);
   }
   if (err == ERR_OK)
      *queued = want;
   return err;
}

/* As lwip_tcp_write_some.  Under TLS the heap shortfall is the same one: the
   record is encrypted into a buffer from that heap before tcp_write. */
err_t lwip_altcp_write_some(struct altcp_pcb *pcb, const void *data, size_t len,
                            u16_t *queued)
{
   u16_t want = altcp_sndbuf(pcb);
   err_t err;

   *queued = 0u;
   if (len < want)
      want = (u16_t)len;
   if (want == 0u)
      return ERR_OK;                    /* send buffer full */
   err = altcp_write(pcb, data, want, TCP_WRITE_FLAG_COPY);
   if (err == ERR_MEM && want > TCP_MSS) {
      want = TCP_MSS;
      err = altcp_write(pcb, data, want, TCP_WRITE_FLAG_COPY);
   }
   if (err == ERR_OK)
      *queued = want;
   return err;
}

uint32_t byte_ring_put(uint8_t *ring, uint32_t mask, uint32_t head,
                       const void *src, uint32_t len)
{
   const uint8_t *s = src;
   uint32_t first = mask + 1u - head;   /* room before the wrap */

   if (first > len)
      first = len;
   memcpy(&ring[head], s, first);
   memcpy(ring, s + first, len - first);
   return (head + len) & mask;
}

uint32_t byte_ring_put_pbuf(uint8_t *ring, uint32_t mask, uint32_t head,
                            const struct pbuf *p)
{
   for (const struct pbuf *q = p; q != NULL; q = q->next)
      head = byte_ring_put(ring, mask, head, q->payload, q->len);
   return head;
}
