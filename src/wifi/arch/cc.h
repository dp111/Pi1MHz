#ifndef PI1MHZ_LWIP_ARCH_CC_H
#define PI1MHZ_LWIP_ARCH_CC_H

#include <stdint.h>

#include "../../rpi/systimer.h"

typedef int sys_prot_t;

#define BYTE_ORDER LITTLE_ENDIAN
/* lwIP uses LWIP_RAND() for DHCP transaction IDs and TCP initial sequence
   numbers, both of which break (or become hijackable) when constant.  Mix
   the free-running 1 MHz system timer with a fixed golden-ratio
   multiplier so callers across a single boot get distinct values and
   the per-boot starting point is non-deterministic.  This is not
   cryptographically strong - lwIP's API does not require it to be - but
   it removes the predictable-ISN attack and the DHCP-XID collision. */
#define LWIP_RAND() ((uint32_t)(RPI_GetSystemTime() * 2654435761u))

#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT __attribute__((__packed__))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

#define LWIP_PLATFORM_ASSERT(x) do { if (!(x)) while (1) { } } while (0)

#endif