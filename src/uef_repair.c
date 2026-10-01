/* FILEV stamp repair for UEF cassette images.  See uef_repair.h.
 *
 * Self-contained: it reads UEF chunks and Acorn cassette blocks directly and
 * recomputes the one CRC it disturbs, so it can run on a window of a tape
 * that is still being decompressed. */

#include "uef_repair.h"

static uint16_t rd16(const uint8_t *p)
{
   return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
          ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Acorn cassette CRC: CCITT with the polynomial applied high byte first. */
static uint16_t tape_crc(const uint8_t *data, size_t length)
{
   uint16_t crc = 0u;
   for (size_t i = 0u; i < length; i++) {
      crc ^= (uint16_t)((uint16_t)data[i] << 8);
      for (unsigned bit = 0u; bit < 8u; bit++)
         crc = (crc & 0x8000u) ? (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u)
                               : (uint16_t)(crc << 1);
   }
   return crc;
}

static int stamp_hex_digit(uint8_t c)
{
   return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')
       || (c >= 'a' && c <= 'f');
}

/* Rewrite `&212`/`&213` address tokens in one block's payload. BBC BASIC
 * tokenises keywords but leaves `&` and digits as ASCII, so the address
 * survives verbatim and a three-digit substitution is exact. A following hex
 * digit means the token is really a longer address such as &2120, which must
 * be left alone. Both `?` and `!` forms are redirected: a loader which saves
 * and restores the vector then does both through scratch and leaves ours
 * untouched. */
static unsigned repair_block_payload(uint8_t *data, size_t length)
{
   unsigned repaired = 0u;
   if (length < 4u) return 0u;
   for (size_t at = 0u; at + 4u <= length; at++) {
      if (data[at] != '&' || data[at + 1] != '2' || data[at + 2] != '1')
         continue;
      if (data[at + 3] != '2' && data[at + 3] != '3')
         continue;
      if (at + 4u < length && stamp_hex_digit(data[at + 4]))
         continue;
      if (at == 0u || (data[at - 1] != '?' && data[at - 1] != '!'))
         continue;
      data[at + 1] = '9';
      data[at + 2] = '0';
      data[at + 3] = (data[at + 3] == '2') ? '0' : '1';
      repaired++;
   }
   return repaired;
}

size_t uef_repair_filev_span(uint8_t *window, size_t length, size_t start,
                             unsigned *repaired)
{
   size_t position = start;
   unsigned hits_total = 0u;
   if (repaired != NULL) *repaired = 0u;
   if (window == NULL || length < position) return length;
   while (position + 6u <= length) {
      uint16_t chunk = rd16(&window[position]);
      uint32_t chunk_length = rd32(&window[position + 2]);
      size_t block = position + 6u;
      if (chunk_length > length || block + chunk_length > length)
         break;
      if (chunk == 0x0100u && chunk_length > 1u
          && window[block] == (uint8_t)'*') {
         /* Standard cassette block: '*', NUL-terminated name of 1 to 10
          * characters, 17-byte descriptor, header CRC, payload, payload CRC. */
         size_t name_end = block + 1u;
         size_t limit = block + chunk_length;
         while (name_end < limit && name_end - block <= 11u
                && window[name_end] != 0u)
            name_end++;
         if (name_end < limit && window[name_end] == 0u) {
            size_t descriptor = name_end + 1u;
            size_t header_crc = descriptor + 17u;
            size_t data_at = header_crc + 2u;
            if (data_at <= limit && descriptor + 13u <= limit) {
               uint16_t data_length = rd16(&window[descriptor + 10]);
               size_t data_crc = data_at + data_length;
               /* A zero-length catalogue marker carries no payload CRC. */
               if (data_length != 0u && data_crc + 2u <= limit) {
                  unsigned hits = repair_block_payload(&window[data_at],
                                                       data_length);
                  if (hits != 0u) {
                     uint16_t crc = tape_crc(&window[data_at], data_length);
                     window[data_crc] = (uint8_t)(crc >> 8);
                     window[data_crc + 1u] = (uint8_t)(crc & 0xffu);
                     hits_total += hits;
                  }
               }
            }
         }
      }
      position = block + chunk_length;
   }
   if (repaired != NULL) *repaired = hits_total;
   return position;
}

unsigned uef_repair_filev_stamp(uint8_t *window, size_t length)
{
   unsigned repaired = 0u;
   (void)uef_repair_filev_span(window, length, UEF_REPAIR_HEADER, &repaired);
   return repaired;
}
