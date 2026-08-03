#ifndef LWIP_PBUF_H
#define LWIP_PBUF_H
#include "lwip/arch.h"
typedef enum { PBUF_TRANSPORT, PBUF_IP, PBUF_LINK, PBUF_RAW } pbuf_layer;
typedef enum { PBUF_RAM, PBUF_POOL } pbuf_type;
struct pbuf {
   struct pbuf *next;
   void        *payload;
   u16_t        tot_len;
   u16_t        len;
};
struct pbuf *pbuf_alloc(pbuf_layer layer, u16_t length, pbuf_type type);
err_t        pbuf_take(struct pbuf *buf, const void *dataptr, u16_t len);
u16_t        pbuf_copy_partial(const struct pbuf *p, void *dst, u16_t len, u16_t offset);
u8_t         pbuf_free(struct pbuf *p);   /* all defined in test_net.c */
#endif
