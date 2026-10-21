/* Host stand-in for lwIP's err.h and its base types (tests/telnetd session harness). */
#ifndef LWIP_HDR_ERR_H
#define LWIP_HDR_ERR_H
#include <stdint.h>
typedef uint8_t  u8_t;
typedef uint16_t u16_t;
typedef uint32_t u32_t;
typedef int16_t  s16_t;
typedef int8_t   err_t;
#define ERR_OK    0
#define ERR_MEM  -1
#define ERR_VAL  -6
#define ERR_ABRT -13
#endif
