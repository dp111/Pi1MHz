// Host test: the colour set-up of a VDU 23,22 custom mode (review 2026-10-06
// R14).  Runs the real src/framebuffer/* against tools/vdutest/stubs.c, with
// screen_set_palette wrapped (-Wl,--wrap) so the plane-1 bank the framebuffer
// selects can be seen.
//
//   - the text colour (white) of a custom mode is worked out from the mode's
//     OWN colour count, including the very first custom mode;
//   - a 256-colour custom mode does not flash, and a 16-colour one does;
//   - with no flash the plane sits on bank 0, the one VDU 19,l,17 (first
//     flashing colour) writes; flags 1 + palette 0 selects it.
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "framebuffer.h"
#include "screen_modes.h"

static int      fails;
static int      pal_calls;
static uint32_t pal_last = 99u;

void __wrap_screen_set_palette(uint32_t planeno, uint32_t palette, uint32_t flags);
void __wrap_screen_set_palette(uint32_t planeno, uint32_t palette, uint32_t flags)
{
   if (planeno == 1u && flags == 1u) {     /* update_palette: the flash bank */
      pal_calls++;
      pal_last = palette;
   }
}

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                printf(__VA_ARGS__); printf("\n"); } } while (0)

static void vdu(const uint8_t *b, size_t n)
{
   for (size_t i = 0; i < n; i++)
      fb_writec((char)b[i]);
   fb_process_vdu_queue();
}

/* VDU 23,22,w;h;xchars,ychars,colours,flags */
static void custom_mode(unsigned w, unsigned h, unsigned colours)
{
   const uint8_t c[] = { 23, 22, (uint8_t)(w & 0xff), (uint8_t)(w >> 8),
                         (uint8_t)(h & 0xff), (uint8_t)(h >> 8),
                         (uint8_t)(w / 8), (uint8_t)(h / 8), (uint8_t)colours, 0 };
   vdu(c, sizeof c);
}

/* Let the flash tick run for a few seconds' worth of frames. */
static int flash_ticks(void)
{
   int before = pal_calls;
   for (int i = 0; i < 200; i++)
      fb_process_flash();
   return pal_calls - before;
}

int main(void)
{
   fb_emulator_init(0, 0xd0);
   /* Read the mode in force, never through get_screen_mode(): that would
      re-derive the fields and hide what the mode change actually left. */
   screen_mode_t *cm;

   /* 1. The first custom mode after boot, 16 colours. */
   custom_mode(640, 256, 16);
   cm = fb_get_current_screen_mode();
   CHECK(cm->mode_num == CUSTOM_8BPP_SCREEN_MODE, "mode %d", cm->mode_num);
   CHECK(cm->ncolour == 15u, "ncolour %u", (unsigned)cm->ncolour);
   CHECK(cm->white == 7u, "first 16-colour custom mode: white = %u, want 7", (unsigned)cm->white);
   CHECK(cm->flash != NULL, "16-colour custom mode does not flash");
   CHECK(flash_ticks() > 0, "16-colour custom mode: no flash tick reached the palette");

   /* 2. Then a 256-colour custom mode. */
   custom_mode(640, 256, 0);
   cm = fb_get_current_screen_mode();
   CHECK(cm->ncolour == 255u, "ncolour %u", (unsigned)cm->ncolour);
   CHECK(cm->white == 255u, "256-colour custom mode: white = %u, want 255", (unsigned)cm->white);
   CHECK(cm->flash == NULL, "256-colour custom mode still flashes");
   CHECK(pal_last == 0u, "256-colour custom mode: plane on bank %u, want 0 (VDU 19,l,17 bank)",
         (unsigned)pal_last);
   int t = flash_ticks();
   CHECK(t == 0, "256-colour custom mode: %d flash ticks reached the palette", t);
   CHECK(pal_last == 0u, "256-colour custom mode: bank %u after flash ticks", (unsigned)pal_last);

   /* 3. Back to 2 colours: white is 1, and it flashes again. */
   custom_mode(640, 256, 2);
   cm = fb_get_current_screen_mode();
   CHECK(cm->white == 1u, "2-colour custom mode: white = %u, want 1", (unsigned)cm->white);
   CHECK(cm->flash != NULL, "2-colour custom mode does not flash");

   /* 4. A standard 256-colour mode (MODE 21, the default) does not flash and
         is on bank 0 too. */
   { const uint8_t c[] = { 22, 21 }; vdu(c, sizeof c); }
   cm = fb_get_current_screen_mode();
   CHECK(cm->flash == NULL, "MODE 21 flashes");
   CHECK(pal_last == 0u, "MODE 21: plane on bank %u, want 0", (unsigned)pal_last);

   /* 5. MODE 1 (4 colours) flashes. */
   { const uint8_t c[] = { 22, 1 }; vdu(c, sizeof c); }
   cm = fb_get_current_screen_mode();
   CHECK(cm->flash != NULL, "MODE 1 does not flash");
   CHECK(cm->white == 3u, "MODE 1 white = %u", (unsigned)cm->white);

   if (fails) {
      printf("%d FAILED\n", fails);
      return 1;
   }
   printf("FB MODE TESTS PASSED\n");
   return 0;
}
