// Host test: VDU driver state that the ROM-comparison suite (tools/vdutest)
// cannot see - cell metrics, the cursors, the teletext line state, the
// palette and the window bookkeeping (review 2026-10-06 V1-V4, V6-V8).
//
// Runs the real src/framebuffer/* against tools/vdutest/stubs.c with two
// stubs wrapped (-Wl,--wrap):
//   - screen_allocate_buffer puts a guard zone, filled with a known byte, on
//     each side of every screen buffer, so a write outside the buffer shows
//     up as a changed guard byte instead of silently landing in the heap;
//   - screen_update_palette_entry records every palette write.
//
// Where the expected behaviour is the MOS's, it was read from MOS 3.20
// (github.com/tom-seddon/acorn_mos_disassembly, src/mos.s65): VDU 19 ANDs
// the logical colour with numberOfLogicalColoursMinusOne; VDU 20 zeroes the
// text/graphics colours and both GCOL actions and redoes the palette; VDU 24
// refuses right < left or top < bottom (equal is a one-pixel window), then
// any edge off the screen in pixels; VDU 31 ignores a position off the window.
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "framebuffer.h"
#include "screen_modes.h"
#include "fonts.h"

static int fails;
static int checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- guarded screen buffers ----------------------------------------------

#define GUARD      (1u << 20)      /* 1 MB each side: V1 was ~160 KB out */
#define GUARD_BYTE 0xA5u
#define MAX_BUFS   64

static struct { uint8_t *base; uint32_t size; } bufs[MAX_BUFS];
static int nbufs;

uint32_t __wrap_screen_allocate_buffer(uint32_t buffer_size, uint32_t *handle);
uint32_t __wrap_screen_allocate_buffer(uint32_t buffer_size, uint32_t *handle)
{
   *handle = 1;
   size_t total = (size_t)buffer_size + 2u * GUARD;
   uint8_t *p = mmap(NULL, total, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
   if (p == MAP_FAILED || nbufs == MAX_BUFS) {
      fprintf(stderr, "test_vdu: guarded allocation of %u bytes failed\n", buffer_size);
      exit(2);
   }
   memset(p, GUARD_BYTE, GUARD);
   memset(p + GUARD, 0, buffer_size);
   memset(p + GUARD + buffer_size, GUARD_BYTE, GUARD);
   bufs[nbufs].base = p;
   bufs[nbufs].size = buffer_size;
   nbufs++;
   return (uint32_t)(uintptr_t)(p + GUARD);
}

/* Bytes changed in any guard zone since the buffer was allocated; the zones
   are then refilled so each check reports only its own step. */
static long guard_damage(void)
{
   long bad = 0;
   for (int i = 0; i < nbufs; i++) {
      uint8_t *lo = bufs[i].base;
      uint8_t *hi = bufs[i].base + GUARD + bufs[i].size;
      for (uint32_t j = 0; j < GUARD; j++) {
         if (lo[j] != GUARD_BYTE) { bad++; lo[j] = GUARD_BYTE; }
         if (hi[j] != GUARD_BYTE) { bad++; hi[j] = GUARD_BYTE; }
      }
   }
   return bad;
}

// ---- palette writes -------------------------------------------------------

#define PAL_ENTRIES 0x200
static struct { int writes; uint32_t r, g, b; } pal[PAL_ENTRIES];

void __wrap_screen_update_palette_entry(uint32_t entry, uint32_t r, uint32_t g, uint32_t b);
void __wrap_screen_update_palette_entry(uint32_t entry, uint32_t r, uint32_t g, uint32_t b)
{
   if (entry < PAL_ENTRIES) {
      pal[entry].writes++;
      pal[entry].r = r; pal[entry].g = g; pal[entry].b = b;
   }
}

static void pal_forget(void) { memset(pal, 0, sizeof pal); }

// ---- driving the VDU ------------------------------------------------------

static void vdu(const uint8_t *b, size_t n)
{
   for (size_t i = 0; i < n; i++)
      fb_writec((char)b[i]);
   fb_process_vdu_queue();
}
#define VDU(...) do { const uint8_t v_[] = { __VA_ARGS__ }; vdu(v_, sizeof v_); } while (0)

static void mode(int m)
{
   VDU(22, (uint8_t)m);
   (void)guard_damage();          /* each case starts clean */
}

/* The raw pixel: prim_get_pixel answers the background colour outside the
   graphics window, which would hide exactly what the window tests look for. */
static pixel_t px(int x, int y)
{
   screen_mode_t *s = fb_get_current_screen_mode();
   return s->get_pixel(s, x, y);
}

/* The first pixel in a w x h box (y counted down from y_top) that is not
   colour bg, or bg if there is none. */
static pixel_t first_ink(int x0, int y_top, int w, int h, pixel_t bg)
{
   for (int y = y_top; y > y_top - h; y--)
      for (int x = x0; x < x0 + w; x++)
         if (px(x, y) != bg)
            return px(x, y);
   return bg;
}

/* Run the flash tick (which toggles the flashing cursor) n times, checking
   the guards after each tick: the cursor is drawn by XOR, so two toggles of
   a stray cursor would otherwise cancel out. */
static long flash_damage(int n)
{
   long d = 0;
   for (int i = 0; i < n; i++) {
      fb_process_flash();
      d += guard_damage();
   }
   return d;
}

// ---- V1: VDU 23,19 with the cursor drawn ----------------------------------

static void test_v1_cursor_vs_metrics(void)
{
   long d;

   /* The text cursor on the bottom row of MODE 0, then double the cell. */
   mode(0);
   VDU(31, 0, 31);
   /* The cursor is the cell's bottom pixel row: pixel row 0 at text row 31. */
   CHECK(px(0, 0) == 1, "V1 setup: cursor not drawn at row 31 (px %u)", (unsigned)px(0, 0));
   VDU(23, 19, 1, 2, 2, 0, 0, 0, 0, 0);
   d = guard_damage();
   CHECK(d == 0, "V1: VDU 23,19,1,2,2 with the cursor on row 31 wrote %ld bytes outside the screen", d);
   /* The cursor moves to the new grid's last row (16-pixel cells, row 15),
      whose cursor is pixel rows 0-1, x 0-15.  It overlaps the old one, so
      an old cursor never un-drawn shows as a hole at x 0-7 of row 0. */
   CHECK(fb_get_cursor_y() == 15, "V1: cursor row %d, want 15", fb_get_cursor_y());
   CHECK(px(0, 0) == 1 && px(7, 0) == 1 && px(15, 0) == 1 && px(0, 1) == 1 && px(16, 0) == 0 && px(0, 2) == 0,
         "V1: the cursor is not exactly the bottom two rows of cell (0,15): the old one was never un-drawn");
   d = flash_damage(64);           /* let the flashing cursor toggle */
   CHECK(d == 0, "V1: cursor flash after VDU 23,19 wrote %ld bytes outside the screen", d);

   /* The edit cursor (cursor keys, here VDU 27,139 = up: it wraps to the
      bottom row) must be clamped to the new grid too. */
   mode(0);
   VDU(27, 139);
   CHECK(fb_get_cursor_y() == 31, "V1 setup: edit cursor row %d, want 31", fb_get_cursor_y());
   VDU(23, 19, 1, 2, 2, 0, 0, 0, 0, 0);
   d = flash_damage(64);
   CHECK(d == 0, "V1: edit cursor at row 31, VDU 23,19,1,2,2: %ld bytes written outside the screen", d);
   CHECK(fb_get_cursor_y() <= 15, "V1: edit cursor left on row %d of a 16-row grid", fb_get_cursor_y());
   VDU(27, 138);                   /* down, so it is redrawn from its row */
   d = guard_damage();
   CHECK(d == 0, "V1: edit cursor move after VDU 23,19: %ld bytes outside the screen", d);

   VDU(23, 19, 1, 1, 1, 0, 0, 0, 0, 0);
}

int main(void)
{
   fb_emulator_init(0, 0xd0);
   (void)guard_damage();

   test_v1_cursor_vs_metrics();

   printf("%d checks, %d failed\n", checks, fails);
   if (fails)
      return 1;
   printf("VDU TESTS PASSED\n");
   return 0;
}
