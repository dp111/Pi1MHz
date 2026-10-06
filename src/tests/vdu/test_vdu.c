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

// ---- V2: VDU 20 must not change the cell ----------------------------------

static void test_v2_vdu20_keeps_font(void)
{
   /* MODE 3 cells are 8x10 (two gap rows).  VDU 23,19,2,0,0 drops the gap:
      8x8 cells, 31 rows.  VDU 20 then must not put the gap back behind the
      text grid's back. */
   mode(3);
   VDU(23, 19, 2, 0, 0, 0, 0, 0, 0, 0);
   int rows = fb_read_vdu_variable(V_WINDOWHEIGHT);
   CHECK(rows == 31, "V2 setup: MODE 3 with no spacing has %d rows, want 31", rows);
   VDU(26);                        /* the text window to the new grid */
   VDU(31, 0, 30);
   CHECK(fb_get_cursor_y() == 30, "V2 setup: cursor row %d, want 30", fb_get_cursor_y());
   VDU(20);
   VDU('X');
   long d = guard_damage();
   CHECK(d == 0, "V2: VDU 20 then a character on row 30 wrote %ld bytes outside the screen", d);
   int h = fb_read_vdu_variable(V_TCHARSIZEY);
   CHECK(h == 8, "V2: VDU 20 changed the cell height to %d (want 8, as VDU 23,19 left it)", h);

   /* VDU 20 is colours, palette and GCOL actions (MOS 3.20). */
   mode(1);
   VDU(18, 3, 2);                  /* GCOL 3,2: EOR */
   VDU(18, 1, 129);                /* GCOL 1,129: OR, background */
   VDU(17, 2);
   VDU(20);
   CHECK(fb_read_vdu_variable(V_GPLFMD) == 0, "V2: VDU 20 left the foreground GCOL action at %d",
         (int)fb_read_vdu_variable(V_GPLFMD));
   CHECK(fb_read_vdu_variable(V_GPLBMD) == 0, "V2: VDU 20 left the background GCOL action at %d",
         (int)fb_read_vdu_variable(V_GPLBMD));
   CHECK(fb_read_vdu_variable(V_TFORECOL) == 3, "V2: VDU 20 text colour %d, want 3",
         (int)fb_read_vdu_variable(V_TFORECOL));
   VDU(23, 19, 2, 0xff, 0xff, 0, 0, 0, 0, 0);
}

// ---- V3: a MODE change must leave a cell that fits ------------------------

static void test_v3_mode_shrinks_below_cell(void)
{
   /* Rounding doubles the BBC font to 16x16, which MODE 0 holds; a custom
      8x8 mode does not. */
   mode(0);
   VDU(23, 19, 3, 1, 0, 0, 0, 0, 0, 0);
   CHECK(fb_read_vdu_variable(V_TCHARSIZEY) == 16, "V3 setup: rounded cell is %d high",
         (int)fb_read_vdu_variable(V_TCHARSIZEY));
   VDU(23, 22, 8, 0, 8, 0, 1, 1, 2, 0);
   (void)guard_damage();
   screen_mode_t *s = fb_get_current_screen_mode();
   CHECK(s->width == 8 && s->height == 8, "V3 setup: custom mode is %dx%d", s->width, s->height);
   int w = fb_read_vdu_variable(V_TCHARSIZEX), h = fb_read_vdu_variable(V_TCHARSIZEY);
   CHECK(w <= 8 && h <= 8, "V3: an 8x8 mode was left with a %dx%d cell", w, h);
   VDU('A');
   VDU(12);
   long d = guard_damage();
   CHECK(d == 0, "V3: a character in the 8x8 mode wrote %ld bytes outside the screen", d);
   mode(0);
   VDU(23, 19, 3, 0, 0, 0, 0, 0, 0, 0);
}

// ---- V4: teletext scroll vs the cached line state --------------------------

static void test_v4_teletext_scroll(void)
{
   mode(7);
   /* Red alphanumerics at the start of row 23, then "A" at the top left. */
   VDU(31, 0, 23, 129);
   VDU(31, 0, 0, 'A');
   /* VDU 11 on the top row scrolls the screen down: row 24 now starts red,
      and re-rendering it leaves the renderer's colour state red. */
   VDU(11);
   VDU('B');                       /* lands at column 1 of the (blank) top row */
   screen_mode_t *s = fb_get_current_screen_mode();
   font_t *f = s->font;
   int cw = f->get_overall_w(f), ch = f->get_overall_h(f);
   pixel_t ink = first_ink(1 * cw, s->height - 1, cw, ch, 0);
   CHECK(ink == 0x3f, "V4: 'B' after a scroll drawn in colour %02x, want white (3f)", (unsigned)ink);

   /* The same through VDU 23,7 (here: scroll the whole screen down). */
   mode(7);
   VDU(31, 0, 23, 129);            /* red at the start of row 23, row 24 after the scroll */
   VDU(31, 0, 2, 'A');             /* "A" at (0,2): the cursor is at (1,2) */
   VDU(23, 7, 1, 2, 0, 0, 0, 0, 0, 0);
   VDU('B');                       /* (1,2), on a row the scroll filled from row 1 */
   ink = first_ink(1 * cw, s->height - 1 - 2 * ch, cw, ch, 0);
   CHECK(ink == 0x3f, "V4: 'B' after VDU 23,7 drawn in colour %02x, want white (3f)", (unsigned)ink);
}

// ---- V6: VDU 19's logical colour is masked to the mode ---------------------

static void test_v6_vdu19_mask(void)
{
   mode(3);
   pal_forget();
   VDU(19, 2, 1, 0, 0, 0);         /* MODE 3 has two colours: this is 0 */
   CHECK(pal[2].writes == 0 && pal[0x102].writes == 0,
         "V6: VDU 19,2 in MODE 3 wrote palette entry 2 (the gap line colour)");
   CHECK(pal[0].writes > 0 && pal[0].r == 0xff && pal[0].g == 0 && pal[0].b == 0,
         "V6: VDU 19,2,1 in MODE 3 did not make logical colour 0 red");

   mode(1);
   pal_forget();
   VDU(19, 6, 4, 0, 0, 0);         /* MODE 1: 6 AND 3 = 2 */
   CHECK(pal[6].writes == 0, "V6: VDU 19,6 in MODE 1 wrote entry 6");
   CHECK(pal[2].writes > 0 && pal[2].b == 0xff && pal[2].r == 0, "V6: VDU 19,6,4 in MODE 1 did not set colour 2 blue");

   mode(2);
   pal_forget();
   VDU(19, 0x13, 2, 0, 0, 0);      /* MODE 2: 19 AND 15 = 3 */
   CHECK(pal[0x13].writes == 0 && pal[3].writes > 0, "V6: VDU 19,19 in MODE 2 not taken as colour 3");
   VDU(20);
}

// ---- V7: VDU 24 - one rule, and a one-pixel window -------------------------

static void test_v7_graphics_window(void)
{
   /* MODE 1 pixels are 4 units wide: left 0, right 3 is a one-pixel-wide
      window, which the MOS accepts (right - left >= 0). */
   mode(1);
   VDU(24, 0, 0, 0, 0, 3, 0, 100, 0);
   int l = fb_read_vdu_variable(V_GWLCOL), r = fb_read_vdu_variable(V_GWRCOL);
   int b = fb_read_vdu_variable(V_GWBROW), t = fb_read_vdu_variable(V_GWTROW);
   CHECK(l == 0 && r == 0 && b == 0 && t == 25, "V7: VDU 24,0;0;3;100; window (%d,%d)-(%d,%d), want (0,0)-(0,25)",
         l, b, r, t);
   VDU(18, 0, 129);                /* graphics background colour 1 */
   VDU(16);                        /* CLG: must fill exactly that window */
   CHECK(px(0, 0) == 1 && px(0, 25) == 1, "V7: CLG did not fill the one-pixel window");
   CHECK(px(1, 0) == 0 && px(0, 26) == 0 && px(100, 100) == 0,
         "V7: CLG filled outside the one-pixel window (the drawing clip and VDU 24 disagree)");

   /* A one-pixel-high window (bottom == top) is accepted as well. */
   mode(1);
   VDU(24, 0, 0, 40, 0, 40, 0, 40, 0);
   CHECK(fb_read_vdu_variable(V_GWBROW) == 10 && fb_read_vdu_variable(V_GWTROW) == 10,
         "V7: VDU 24,0;40;40;40; refused");

   /* A refused window changes nothing, in either layer. */
   mode(1);
   VDU(24, 20, 0, 20, 0, 80, 0, 60, 0);   /* (20,20)-(80,60): pixels (5,5)-(20,15) */
   VDU(24, 80, 0, 20, 0, 20, 0, 60, 0);   /* right < left: refused */
   CHECK(fb_read_vdu_variable(V_GWLCOL) == 5 && fb_read_vdu_variable(V_GWRCOL) == 20,
         "V7: a refused VDU 24 changed the window (%d-%d)",
         (int)fb_read_vdu_variable(V_GWLCOL), (int)fb_read_vdu_variable(V_GWRCOL));
   VDU(24, 20, 0, 20, 0, 0, 10, 60, 0);   /* right 2560: off screen, refused */
   CHECK(fb_read_vdu_variable(V_GWRCOL) == 20, "V7: an off-screen VDU 24 changed the window");
   VDU(18, 0, 129);
   VDU(16);
   CHECK(px(5, 5) == 1 && px(20, 15) == 1 && px(4, 5) == 0 && px(21, 15) == 0,
         "V7: CLG after refused windows does not fill (5,5)-(20,15)");
}

// ---- V8: VDU 31 off the window --------------------------------------------

static void test_v8_vdu31_range(void)
{
   mode(0);
   VDU(28, 10, 31, 79, 0);         /* text window from column 10 */
   VDU(31, 2, 3);
   CHECK(fb_get_cursor_x() == 2 && fb_get_cursor_y() == 3, "V8 setup: cursor at %d,%d",
         fb_get_cursor_x(), fb_get_cursor_y());
   VDU(31, 250, 3);                /* 250 + 10 wraps to 4 in a byte */
   CHECK(fb_get_cursor_x() == 2, "V8: VDU 31,250,3 with the window at column 10 moved the cursor to %d",
         fb_get_cursor_x());
   VDU(28, 0, 31, 79, 10);         /* window from row 10 */
   VDU(31, 1, 250);
   CHECK(fb_get_cursor_y() >= 0, "V8: VDU 31,1,250 put the cursor above the window (row %d)",
         fb_get_cursor_y());
   VDU(26);
}

int main(void)
{
   fb_emulator_init(0, 0xd0);
   (void)guard_damage();

   test_v1_cursor_vs_metrics();
   test_v2_vdu20_keeps_font();
   test_v3_mode_shrinks_below_cell();
   test_v4_teletext_scroll();
   test_v6_vdu19_mask();
   test_v7_graphics_window();
   test_v8_vdu31_range();

   printf("%d checks, %d failed\n", checks, fails);
   if (fails)
      return 1;
   printf("VDU TESTS PASSED\n");
   return 0;
}
