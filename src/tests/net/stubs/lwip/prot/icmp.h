#ifndef LWIP_PROT_ICMP_H
#define LWIP_PROT_ICMP_H
#include "lwip/arch.h"
/* Same field order and width as lwIP's, so the bytes the firmware builds and
   the bytes this suite delivers back agree. */
struct icmp_echo_hdr {
   u8_t  type;
   u8_t  code;
   u16_t chksum;
   u16_t id;
   u16_t seqno;
};
#define ICMPH_TYPE(hdr)          ((hdr)->type)
#define ICMPH_TYPE_SET(hdr, t)   ((hdr)->type = (u8_t)(t))
#define ICMPH_CODE_SET(hdr, c)   ((hdr)->code = (u8_t)(c))
#define ICMP_ER   0u   /* echo reply   */
#define ICMP_ECHO 8u   /* echo request */
#endif
