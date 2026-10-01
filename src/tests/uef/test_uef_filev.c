/* The FILEV stamp repair, driven through the service the way the host ROM
   drives it.

   A large minority of Electron titles load with `?&212=&D6:?&213=&F1`, which
   stamps the MOS 1.00 cassette entry over whatever filing system owns FILEV.
   The Pi redirects the address token to &900/&901 as the tape goes past.

   The case worth a test is the one the streaming rewrite created: the tape is
   published to the Beeb a 63 KB window at a time, and the block carrying the
   idiom can straddle a window boundary.  Repairing it needs the whole block -
   the payload changes and its CRC has to be recomputed - so the service holds
   the tail of an incomplete chunk back and re-offers it at the head of the
   next window.  This builds tapes that put the block at, before and after the
   boundary, reassembles what the service publishes, and checks the idiom is
   gone and the block CRC still verifies. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "Pi1MHz.h"
#include "uef_service.h"
#include "wifi_service.h"

static Pi1MHz_t jim_storage;
Pi1MHz_t *Pi1MHz = &jim_storage;

void Pi1MHz_MemoryWrite(uint32_t addr, uint8_t data) { (void)addr; (void)data; }
void Pi1MHz_Register_Poll(func_ptr f, const char *n) { (void)f; (void)n; }
void Pi1MHz_nIRQ_ASSERT(uint8_t src) { (void)src; }
void Pi1MHz_nIRQ_CLEAR(uint8_t src) { (void)src; }

/* The repair is on by default on a Pi; say so explicitly here. */
const char *config_get(const char *key)
{
   return strcmp(key, "wifi_service_uef_filev_repair") == 0 ? "1" : NULL;
}

void response_string(uint32_t cp, const char *value)
{
   size_t n = strlen(value);
   if (n > 200u) n = 200u;
   memcpy(&Pi1MHz->JIM_ram[cp + 1u], value, n);
   Pi1MHz->JIM_ram[cp + 1u + n] = 0u;
}

#define CP           0x10000u
#define FIRST_PAGE   1u
#define FLAT_WINDOW  ((256u - FIRST_PAGE - 1u) * 256u)
#define TRAILER      0xfffeu
#define APPEND_MAX   0xff00u

static int failures;

static void check(const char *what, bool ok)
{
   printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
   if (!ok) failures++;
}

static void wr16(uint8_t *p, unsigned v)
{
   p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void wr32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
   p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t rd32(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Acorn cassette CRC: CCITT, high byte first. */
static uint16_t tape_crc(const uint8_t *data, size_t length)
{
   uint16_t crc = 0;
   for (size_t i = 0; i < length; i++) {
      crc ^= (uint16_t)((uint16_t)data[i] << 8);
      for (unsigned bit = 0; bit < 8; bit++)
         crc = (crc & 0x8000u) ? (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u)
                               : (uint16_t)(crc << 1);
   }
   return crc;
}

/* ---- tape construction ------------------------------------------------- */

/* A &0100 chunk holding one standard cassette block whose payload is `text`
   padded out to `payload_length` bytes.  Appends to `tape`. */
static size_t put_block(uint8_t *tape, size_t at, const char *name,
                        const char *text, size_t payload_length)
{
   size_t chunk_at = at;
   size_t body = at + 6u;
   size_t p = body;
   size_t name_length = strlen(name);
   size_t text_length = strlen(text);
   size_t header_start;
   size_t data_at;

   tape[p++] = (uint8_t)'*';
   header_start = p;
   memcpy(&tape[p], name, name_length); p += name_length;
   tape[p++] = 0u;
   memset(&tape[p], 0, 17u);
   wr32(&tape[p], 0x1900u);            /* load address  */
   wr32(&tape[p + 4], 0x1900u);        /* exec address  */
   wr16(&tape[p + 8], 0u);             /* block number  */
   wr16(&tape[p + 10], (unsigned)payload_length);
   p += 17u;
   wr16(&tape[p], tape_crc(&tape[header_start], p - header_start));
   /* The header CRC is stored high byte first, like the payload CRC. */
   tape[p] = (uint8_t)(tape_crc(&tape[header_start], p - header_start) >> 8);
   tape[p + 1] = (uint8_t)(tape_crc(&tape[header_start], p - header_start) & 0xffu);
   p += 2u;

   data_at = p;
   memset(&tape[p], ' ', payload_length);
   memcpy(&tape[p], text, text_length < payload_length ? text_length
                                                       : payload_length);
   p += payload_length;
   wr16(&tape[p], 0u);
   tape[p] = (uint8_t)(tape_crc(&tape[data_at], payload_length) >> 8);
   tape[p + 1] = (uint8_t)(tape_crc(&tape[data_at], payload_length) & 0xffu);
   p += 2u;

   wr16(&tape[chunk_at], 0x0100u);
   wr32(&tape[chunk_at + 2], (uint32_t)(p - body));
   return p;
}

/* A &0110 carrier-tone chunk, used as filler to place the next block at a
   chosen offset.  Costs 6 bytes of header plus `payload`. */
static size_t put_filler(uint8_t *tape, size_t at, size_t payload)
{
   wr16(&tape[at], 0x0110u);
   wr32(&tape[at + 2], (uint32_t)payload);
   memset(&tape[at + 6u], 0x5au, payload);
   return at + 6u + payload;
}

/* Build a tape whose one interesting block starts at `block_at`. */
static size_t build_tape(uint8_t *tape, size_t capacity, size_t block_at,
                         size_t payload_length)
{
   size_t at = 0u;
   memcpy(&tape[at], "UEF File!\0\012\000", 12u); at += 12u;
   if (block_at > at) {
      /* Filler chunks are 6 bytes of header plus payload, and the smallest
         useful one is 6 bytes long, so any gap of 6 or more is reachable. */
      size_t gap = block_at - at;
      while (gap >= 12u) {
         size_t take = gap - 6u > 60000u ? 60000u : gap - 6u;
         at = put_filler(tape, at, take);
         gap = block_at - at;
      }
      if (gap != 0u)
         at = put_filler(tape, at, gap - 6u);
   }
   at = put_block(tape, at, "LOADER",
                  "10?&212=&D6:?&213=&F1:REM stamp", payload_length);
   at = put_filler(tape, at, 300u);
   if (at > capacity) { printf("tape overran the buffer\n"); exit(2); }
   return at;
}

/* ---- driving the service ------------------------------------------------ */

static uint8_t *request(void)   { return &Pi1MHz->JIM_ram[CP + 1u]; }

static void send(uint8_t op, uint32_t token, uint32_t value, uint16_t length)
{
   uint8_t *p = request();
   memset(p, 0, 32u);
   memcpy(p, "IUEF", 4u);
   p[4] = 1u;
   p[5] = op;
   wr32(p + 6u, token);
   wr32(p + 10u, value);
   wr16(p + 14u, length);
}

static uint32_t reply_token(void)      { return rd32(request() + 5u); }
static uint32_t reply_generation(void) { return rd32(request() + 9u); }
static unsigned reply_length(void)
{
   return (unsigned)request()[13] | ((unsigned)request()[14] << 8);
}
static bool reply_final(void) { return request()[15] != 0u; }

/* Upload `tape` and read every window back, returning the reassembled image
   in `out`.  Returns its length, or SIZE_MAX on a protocol failure. */
static size_t round_trip(const uint8_t *tape, size_t length, uint8_t *out,
                         size_t capacity)
{
   size_t produced = 0u;
   size_t sent = 0u;
   uint32_t token;

   uef_service_reset();
   send(1u, 0u, 0u, 0u);                      /* BEGIN  */
   if (uef_service_stream_command(CP) != WIFI_SVC_OK) return SIZE_MAX;
   token = reply_token();

   while (sent < length) {
      size_t take = length - sent > APPEND_MAX ? APPEND_MAX : length - sent;
      memcpy(&Pi1MHz->JIM_ram[0], tape + sent, take);
      send(2u, token, reply_generation(), (uint16_t)take);
      if (uef_service_stream_command(CP) != WIFI_SVC_OK) return SIZE_MAX;
      sent += take;
   }

   send(3u, token, 0u, 0u);                   /* FINALIZE */
   if (uef_service_stream_command(CP) != WIFI_SVC_OK) return SIZE_MAX;

   for (;;) {
      unsigned window = reply_length();
      if (produced + window > capacity) return SIZE_MAX;
      memcpy(out + produced,
             &Pi1MHz->JIM_ram[FIRST_PAGE << 8], window);
      produced += window;
      if (reply_final()) break;
      send(5u, token, reply_generation(), 0u);   /* REFILL */
      if (uef_service_stream_command(CP) != WIFI_SVC_OK) return SIZE_MAX;
      if (reply_length() == 0u && reply_final()) break;
   }
   send(6u, token, 0u, 0u);                   /* CLOSE */
   (void)uef_service_stream_command(CP);
   return produced;
}

/* ---- the checks --------------------------------------------------------- */

/* Walk the reassembled tape and report on the one block that matters. */
static void inspect(const uint8_t *image, size_t length, bool *found_old,
                    bool *found_new, bool *crc_ok)
{
   size_t at = 12u;
   *found_old = false;
   *found_new = false;
   *crc_ok = true;
   while (at + 6u <= length) {
      unsigned chunk = (unsigned)image[at] | ((unsigned)image[at + 1] << 8);
      uint32_t chunk_length = rd32(&image[at + 2]);
      size_t body = at + 6u;
      if (chunk_length > length || body + chunk_length > length) break;
      if (chunk == 0x0100u && chunk_length > 1u && image[body] == '*') {
         size_t name_end = body + 1u;
         size_t limit = body + chunk_length;
         while (name_end < limit && name_end - body <= 11u
                && image[name_end] != 0u)
            name_end++;
         if (name_end < limit && image[name_end] == 0u) {
            size_t descriptor = name_end + 1u;
            size_t data_at = descriptor + 19u;
            unsigned payload = (unsigned)image[descriptor + 10]
                             | ((unsigned)image[descriptor + 11] << 8);
            if (payload != 0u && data_at + payload + 2u <= limit) {
               uint16_t want = tape_crc(&image[data_at], payload);
               uint16_t got = (uint16_t)(((unsigned)image[data_at + payload] << 8)
                                        | image[data_at + payload + 1u]);
               if (memmem(&image[data_at], payload, "?&212", 5u) != NULL)
                  *found_old = true;
               if (memmem(&image[data_at], payload, "?&900", 5u) != NULL)
                  *found_new = true;
               if (want != got) *crc_ok = false;
            }
         }
      }
      at = body + chunk_length;
   }
}

static void one_case(const char *what, size_t block_at, size_t payload_length)
{
   static uint8_t tape[4u * 1024u * 1024u];
   static uint8_t out[4u * 1024u * 1024u];
   bool old_token, new_token, crc_ok;
   size_t length = build_tape(tape, sizeof tape, block_at, payload_length);
   size_t produced = round_trip(tape, length, out, sizeof out);
   char label[128];

   if (produced == SIZE_MAX) {
      snprintf(label, sizeof label, "%s: protocol round trip", what);
      check(label, false);
      return;
   }
   snprintf(label, sizeof label, "%s: tape reassembles to its full length", what);
   check(label, produced == length);
   inspect(out, produced, &old_token, &new_token, &crc_ok);
   snprintf(label, sizeof label, "%s: the FILEV stamp is gone", what);
   check(label, !old_token);
   snprintf(label, sizeof label, "%s: redirected to &900", what);
   check(label, new_token);
   snprintf(label, sizeof label, "%s: the block CRC still verifies", what);
   check(label, crc_ok);
}

int main(void)
{
   /* Well inside the first window. */
   one_case("early block", 4096u, 256u);
   /* The block's header lands in one window and its payload in the next. */
   one_case("block header on the boundary", FLAT_WINDOW - 20u, 256u);
   /* The chunk header itself is split across the boundary. */
   one_case("chunk header split", FLAT_WINDOW - 3u, 256u);
   /* Payload straddles the boundary, CRC lands in the next window. */
   one_case("payload straddles the boundary", FLAT_WINDOW - 150u, 256u);
   /* Second window onwards, so the walk starts at offset 0, not 12. */
   one_case("block in the second window", FLAT_WINDOW + 5000u, 256u);
   /* A chunk far too long to carry: framing must resume after it, and the
      block that follows is still repaired. */
   one_case("after a chunk too long to carry", 3u * FLAT_WINDOW / 2u, 256u);

   printf("%s\n", failures ? "FILEV REPAIR TESTS FAILED"
                           : "FILEV REPAIR TESTS PASSED");
   return failures ? 1 : 0;
}
