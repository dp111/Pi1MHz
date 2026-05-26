#ifndef PI1MHZ_LWIP_ARCH_CC_H
#define PI1MHZ_LWIP_ARCH_CC_H

#include <stdint.h>

typedef int sys_prot_t;

#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_RAND() ((uint32_t)0x12345678u)

#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT __attribute__((__packed__))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

#define LWIP_PLATFORM_ASSERT(x) do { if (!(x)) while (1) { } } while (0)

#endif