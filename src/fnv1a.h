/* fnv1a.h - 32-bit FNV-1a, the hash behind the FAT service's short names
   and directory ids, MTP's object handles and TNFS's write-retry check. */
#ifndef FNV1A_H
#define FNV1A_H

#include <stddef.h>
#include <stdint.h>

#define FNV1A_BASIS 2166136261u

static inline uint32_t fnv1a_step(uint32_t h, uint8_t b)
{
   return (h ^ b) * 16777619u;
}

static inline uint32_t fnv1a_str(uint32_t h, const char *s)
{
   while (*s != '\0')
      h = fnv1a_step(h, (uint8_t)*s++);
   return h;
}

static inline uint32_t fnv1a_buf(uint32_t h, const void *p, size_t n)
{
   const uint8_t *b = p;
   while (n-- != 0u)
      h = fnv1a_step(h, *b++);
   return h;
}

#endif
