#ifndef LWIP_INET_CHKSUM_H
#define LWIP_INET_CHKSUM_H
#include "lwip/arch.h"
u16_t inet_chksum(const void *dataptr, u16_t len);
#endif
