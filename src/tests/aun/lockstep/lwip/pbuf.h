#ifndef LOCKSTEP_LWIP_PBUF_H
#define LOCKSTEP_LWIP_PBUF_H
#include <stdint.h>
#include <stddef.h>
typedef uint16_t u16_t;
typedef int err_t;
#define ERR_OK 0
#define PBUF_TRANSPORT 0
#define PBUF_RAM 0
struct pbuf { u16_t tot_len; void *payload; struct pbuf *next; };
struct pbuf *pbuf_alloc(int layer, u16_t len, int type);
err_t pbuf_take(struct pbuf *p, const void *d, u16_t len);
uint8_t pbuf_free(struct pbuf *p);
u16_t pbuf_copy_partial(const struct pbuf *p, void *dst, u16_t len, u16_t off);
#endif
