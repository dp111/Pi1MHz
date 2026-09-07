/* byteorder.h - little-endian access to unaligned wire and file bytes.

   The one copy of the byte-shuffling that ZIP/UEF headers, WAV headers,
   the CYW43 firmware image and the MTP kernel check all need.  Plain byte
   loads and stores, so any alignment is fine and there is no aliasing. */
#ifndef BYTEORDER_H
#define BYTEORDER_H

#include <stdint.h>

static inline uint16_t get_le16(const uint8_t *p)
{
   return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static inline uint32_t get_le32(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void put_le16(uint8_t *p, uint16_t v)
{
   p[0] = (uint8_t)v;
   p[1] = (uint8_t)(v >> 8);
}

static inline void put_le32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)v;
   p[1] = (uint8_t)(v >> 8);
   p[2] = (uint8_t)(v >> 16);
   p[3] = (uint8_t)(v >> 24);
}

#endif
