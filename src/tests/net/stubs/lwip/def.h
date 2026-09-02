#ifndef LWIP_DEF_H
#define LWIP_DEF_H
#include "lwip/arch.h"
/* Host is little-endian; the firmware only uses this for the ICMP sequence,
   and the comparison against it is symmetric, so a plain swap is enough. */
static inline u16_t lwip_htons(u16_t x)
{
   return (u16_t)(((x & 0x00ffu) << 8) | ((x & 0xff00u) >> 8));
}
static inline u16_t lwip_ntohs(u16_t x) { return lwip_htons(x); }
#endif
