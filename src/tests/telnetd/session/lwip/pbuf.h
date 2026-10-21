/* Host stand-in for lwIP's pbuf (tests/telnetd session harness): one malloc per buffer. */
#ifndef LWIP_HDR_PBUF_H
#define LWIP_HDR_PBUF_H
#include "lwip/err.h"
struct pbuf {
   struct pbuf *next;
   void        *payload;
   u16_t        tot_len;
   u16_t        len;
};
u8_t         pbuf_free(struct pbuf *p);
void         pbuf_cat(struct pbuf *head, struct pbuf *tail);
struct pbuf *pbuf_free_header(struct pbuf *q, u16_t size);
#endif
