/* Drive the UEF service protocol the way the ElkWiFi ROM does, against a fake
   JIM aperture, and check that the windows it publishes reassemble into
   exactly the tape gunzip would produce.

   This is the part that cannot be checked by streaming alone: whether BEGIN /
   APPEND / FINALIZE / REFILL / REWIND / REPUBLISH / CLOSE actually hand the
   host its tape, in the right order, with the right generation handshake, and
   with the guard laid out on top without eating any of it. */
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

/* uef_service.c borrows this from wifi_service.c. */
void response_string(uint32_t cp, const char *value)
{
   size_t n = strlen(value);
   if (n > 200u) n = 200u;
   memcpy(&Pi1MHz->JIM_ram[cp + 1u], value, n);
   Pi1MHz->JIM_ram[cp + 1u + n] = 0u;
}

#define CP            0x10000u   /* command block, clear of the aperture */
#define UEF_BASE      0u
#define FIRST_PAGE    1u
#define GUARD_OFFSET  0x97u
#define GUARD_LENGTH  (256u - GUARD_OFFSET)
#define FLAT_WINDOW   ((256u - FIRST_PAGE - 1u) * 256u)
#define GUARD_WINDOW  ((256u - FIRST_PAGE - 1u) * GUARD_OFFSET)
#define TRAILER       0xfffeu

static int failures;

static void check(const char *what, bool ok)
{
   printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
   if (!ok) failures++;
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
static uint16_t rd16(const uint8_t *p)
{
   return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* Build an "IUEF" request in the command block, exactly as the ROM lays it. */
static uint8_t op(uint8_t operation, uint32_t token, uint32_t value,
                  uint16_t length, uint32_t crc)
{
   uint8_t *p = &Pi1MHz->JIM_ram[CP + 1u];
   memcpy(p, "IUEF", 4u);
   p[4] = 1u;                    /* protocol version */
   p[5] = operation;
   wr32(p + 6u, token);
   wr32(p + 10u, value);
   p[14] = (uint8_t)length; p[15] = (uint8_t)(length >> 8);
   wr32(p + 16u, crc);
   return uef_service_stream_command(CP);
}

static uint32_t reply_token(void)      { return rd32(&Pi1MHz->JIM_ram[CP + 1u + 5u]); }
static uint32_t reply_generation(void) { return rd32(&Pi1MHz->JIM_ram[CP + 1u + 9u]); }
static uint16_t reply_length(void)     { return rd16(&Pi1MHz->JIM_ram[CP + 1u + 13u]); }
static bool     reply_final(void)      { return Pi1MHz->JIM_ram[CP + 1u + 15u] != 0u; }
static uint8_t  reply_format(void)     { return Pi1MHz->JIM_ram[CP + 1u + 16u]; }

/* Read the published window back out of the aperture the way the host would,
   honouring the guard layout when one is up. */
static size_t collect_window(uint8_t *out, size_t count, bool guarded)
{
   size_t done = 0u, page = 0u;
   if (!guarded) {
      memcpy(out, &Pi1MHz->JIM_ram[UEF_BASE + (FIRST_PAGE << 8)], count);
      return count;
   }
   while (done < count) {
      size_t chunk = count - done < GUARD_OFFSET ? count - done : GUARD_OFFSET;
      memcpy(out + done,
             &Pi1MHz->JIM_ram[UEF_BASE + ((page + FIRST_PAGE) << 8)], chunk);
      done += chunk;
      page++;
   }
   return done;
}

/* Upload a file with BEGIN/APPEND/FINALIZE, then drain it with REFILL, and
   return the reassembled tape. */
static uint8_t *play_tape(const uint8_t *image, uint32_t image_length,
                          bool guarded, size_t *out_length, uint8_t *format)
{
   uint8_t *out = malloc(16u * 1024u * 1024u);
   size_t total = 0u;
   uint32_t token, generation, offset = 0u;
   size_t limit = guarded ? GUARD_WINDOW : FLAT_WINDOW;

   if (op(1u /*BEGIN*/, 0u, 0u, 0u, 0u) != WIFI_SVC_OK) { free(out); return NULL; }
   token = reply_token();
   generation = reply_generation();

   while (offset < image_length) {
      uint32_t chunk = image_length - offset;
      if (chunk > 0xff00u) chunk = 0xff00u;
      memcpy(&Pi1MHz->JIM_ram[UEF_BASE], image + offset, chunk);
      if (op(2u /*APPEND*/, token, generation, (uint16_t)chunk, 0u) != WIFI_SVC_OK) {
         free(out); return NULL;
      }
      generation = reply_generation();
      offset += chunk;
   }

   if (op(3u /*FINALIZE*/, token, 0u, 0u, 0u) != WIFI_SVC_OK) { free(out); return NULL; }
   *format = reply_format();

   for (;;) {
      uint16_t n = reply_length();
      if (n != 0u) total += collect_window(out + total, n, guarded);
      if (reply_final()) break;
      if (op(5u /*REFILL*/, token, reply_generation(), 0u, 0u) != WIFI_SVC_OK) {
         free(out); return NULL;
      }
      if (reply_length() == 0u && reply_final()) break;
   }
   (void)limit;
   *out_length = total;
   return out;
}

static uint8_t *slurp(const char *path, uint32_t *length)
{
   FILE *fp = fopen(path, "rb");
   uint8_t *data;
   long size;
   if (!fp) return NULL;
   fseek(fp, 0, SEEK_END); size = ftell(fp); rewind(fp);
   data = malloc((size_t)size);
   if (fread(data, 1, (size_t)size, fp) != (size_t)size) { fclose(fp); free(data); return NULL; }
   fclose(fp);
   *length = (uint32_t)size;
   return data;
}

int main(int argc, char **argv)
{
   uint8_t *image, *reference, *played;
   uint32_t image_length, reference_length;
   size_t played_length;
   uint8_t format;

   if (argc < 3) { fprintf(stderr, "usage: test_uef_service <in.uef> <expected.bin>\n"); return 2; }
   image = slurp(argv[1], &image_length);
   reference = slurp(argv[2], &reference_length);
   if (!image || !reference) { perror("open"); return 2; }

   printf("== %s (%u bytes in, %u expected out) ==\n",
          argv[1], image_length, reference_length);

   /* 1. Flat window, no guard. */
   played = play_tape(image, image_length, false, &played_length, &format);
   check("upload, finalize and drain reassembles the tape",
         played != NULL && played_length == reference_length
         && memcmp(played, reference, reference_length) == 0);
   check("format letter is R, G or Z",
         format == 'R' || format == 'G' || format == 'Z');
   free(played);

   /* 2. REPUBLISH must re-lay the same window without advancing. */
   {
      uint8_t first[FLAT_WINDOW], again[FLAT_WINDOW];
      uint16_t n1, n2;
      uint32_t token, generation;
      op(1u, 0u, 0u, 0u, 0u);
      token = reply_token(); generation = reply_generation();
      memcpy(&Pi1MHz->JIM_ram[UEF_BASE], image,
             image_length < 0xff00u ? image_length : 0xff00u);
      op(2u, token, generation,
         (uint16_t)(image_length < 0xff00u ? image_length : 0xff00u), 0u);
      if (image_length < 0xff00u) {
         op(3u, token, 0u, 0u, 0u);
         n1 = reply_length();
         collect_window(first, n1, false);
         generation = reply_generation();
         check("REPUBLISH is accepted", op(7u, token, 0u, 0u, 0u) == WIFI_SVC_OK);
         n2 = reply_length();
         collect_window(again, n2, false);
         check("REPUBLISH re-lays the same window", n1 == n2
               && memcmp(first, again, n1) == 0);
         check("REPUBLISH does not advance the generation",
               reply_generation() == generation);
         /* 3. REWIND returns to the first window. */
         check("REWIND is accepted", op(4u, token, 0u, 0u, 0u) == WIFI_SVC_OK);
         check("REWIND republishes the first window",
               reply_length() == n1
               && collect_window(again, reply_length(), false) == n1
               && memcmp(first, again, n1) == 0);
      }
      op(6u /*CLOSE*/, token, 0u, 0u, 0u);
   }

   /* 4. With a guard up, the tape must still arrive whole - the guard takes
         the top of each page and the stream is laid out around it. */
   {
      uint8_t *p = &Pi1MHz->JIM_ram[CP + 1u];
      unsigned int i;
      p[0] = GUARD_LENGTH;
      for (i = 0u; i < GUARD_LENGTH; i++) p[1u + i] = (uint8_t)(0xa0u + i);
      check("guard image accepted", uef_service_guard_command(CP) == WIFI_SVC_OK);
   }
   played = play_tape(image, image_length, true, &played_length, &format);
   check("tape still arrives whole with a guard published",
         played != NULL && played_length == reference_length
         && memcmp(played, reference, reference_length) == 0);
   {
      /* The guard must be intact at the top of a page the stream used. */
      const uint8_t *g = &Pi1MHz->JIM_ram[UEF_BASE + (FIRST_PAGE << 8) + GUARD_OFFSET];
      bool intact = true;
      unsigned int i;
      for (i = 0u; i < GUARD_LENGTH; i++)
         if (g[i] != (uint8_t)(0xa0u + i)) intact = false;
      check("guard survives the window being published over the page", intact);
   }
   free(played);

   /* 5. Withdraw the guard, then close, and check the tape really is gone -
         a REFILL afterwards must be refused rather than serving stale
         windows from a tape the host has finished with. */
   Pi1MHz->JIM_ram[CP + 1u] = 0u;
   check("guard withdrawn", uef_service_guard_command(CP) == WIFI_SVC_OK);
   check("CLOSE is accepted", op(6u, 0u, 0u, 0u, 0u) == WIFI_SVC_OK);
   check("REFILL after CLOSE is refused",
         op(5u, 0u, 0u, 0u, 0u) == WIFI_SVC_ERR_PARAM);
   check("REWIND after CLOSE is refused",
         op(4u, 0u, 0u, 0u, 0u) == WIFI_SVC_ERR_PARAM);
   check("PROBE after CLOSE still answers, with a zero token",
         op(0u, 0u, 0u, 0u, 0u) == WIFI_SVC_OK && reply_token() == 0u);

   free(image);
   free(reference);
   printf("\n%s\n", failures ? "FAILURES" : "service protocol correct");
   return failures != 0;
}
