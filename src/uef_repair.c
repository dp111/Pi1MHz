/* FILEV stamp repair for UEF cassette images.  See uef_repair.h.
 *
 * Self-contained: it reads UEF chunks and Acorn cassette blocks directly and
 * recomputes the one CRC it disturbs, so it can run on a window of a tape
 * that is still being decompressed. */

#include <stdbool.h>
#include <string.h>

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

#define STAMP_LENGTH 9u      /* ?&212=&D6 */
#define TOKEN_ELSE   0x8Bu
#define TOKEN_THEN   0x8Cu

/* Is `at` a whole `?&21x=&vv` statement (x/vv = 2/D6 or 3/F1) in tokenised
 * BBC BASIC?  It must start a statement - a line's first text, or after a
 * colon, THEN or ELSE - and end one, before a colon, ELSE or the line's CR.
 * `IF ?&212=&D6 THEN` reads the vector and is not a stamp: blanking that
 * would be a syntax error.  A stamp cut by the end of the payload is left. */
static bool stamp_at(const uint8_t *data, size_t length, size_t at,
                     uint8_t which)
{
   static const char *const text[2] = { "?&212=&D6", "?&213=&F1" };
   size_t s = at;
   size_t e = at + STAMP_LENGTH;

   if (e > length || memcmp(&data[at], text[which], STAMP_LENGTH) != 0)
      return false;
   while (e < length && data[e] == ' ')
      e++;
   if (e >= length || (data[e] != ':' && data[e] != 0x0Du && data[e] != TOKEN_ELSE))
      return false;
   while (s > 0u && data[s - 1u] == ' ')
      s--;
   if (s > 0u && (data[s - 1u] == ':' || data[s - 1u] == TOKEN_THEN
                  || data[s - 1u] == TOKEN_ELSE))
      return true;
   /* A line starts CR, line number high and low, length; the next line's
      CR is that length on from this one, which a stray CR would not be. */
   if (s >= 4u && data[s - 4u] == 0x0Du && data[s - 1u] >= 4u) {
      size_t next = s - 4u + data[s - 1u];
      return next >= length || data[next] == 0x0Du;
   }
   return false;
}

/* Blank the loader's FILEV stamp statements with spaces.  The statements
 * then do nothing, as BASIC runs an empty statement, and nothing is written
 * anywhere; the program keeps its length, so only the block CRC changes.
 * Both halves or neither: one half blanked would leave FILEV pointing at
 * neither the filing system nor the MOS.  A cassette block can end in the
 * middle of a line, so a pair may be split across two blocks: a ?&212 here
 * with its ?&213 at the start of the next block, where it cannot be seen to
 * start a statement and is left.  So a block whose last ?&212 has no ?&213
 * after it, on a line that runs on past the block, is left whole. */
static unsigned repair_block_payload(uint8_t *data, size_t length)
{
   unsigned found[2] = { 0u, 0u };
   unsigned repaired = 0u;
   bool open_pair = false;      /* the last ?&212 still waits for its ?&213 */
   size_t last = 0u;
   for (size_t at = 0u; at < length; at++)
      for (uint8_t w = 0u; w < 2u; w++)
         if (stamp_at(data, length, at, w)) {
            found[w]++;
            if (w == 0u) {
               open_pair = true;
               last = at;
            } else {
               open_pair = false;
            }
         }
   if (found[0] == 0u || found[1] == 0u)
      return 0u;
   if (open_pair && memchr(&data[last], 0x0D, length - last) == NULL)
      return 0u;               /* its line, and maybe its ?&213, run on */
   for (size_t at = 0u; at < length; at++)
      for (uint8_t w = 0u; w < 2u; w++)
         if (stamp_at(data, length, at, w)) {
            memset(&data[at], ' ', STAMP_LENGTH);
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
