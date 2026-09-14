#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "primitives.h"
#include "framebuffer.h"
#include "fonts.h"

static pixel_t    max_col;
static plotmode_t g_fg_plotmode;
static pixel_t    g_fg_col;
static plotmode_t g_bg_plotmode;
static pixel_t    g_bg_col;

static int16_t g_x_min;
static int16_t g_y_min;
static int16_t g_x_max;
static int16_t g_y_max;

// Default ECF Patterns for 2 colour modes variant A (used only in mode 0)
static const uint8_t ECF1_DEFAULT_2COLS_A[] = {0xCC, 0x00, 0xCC, 0x00, 0xCC, 0x00, 0xCC, 0x00};
static const uint8_t ECF2_DEFAULT_2COLS_A[] = {0xCC, 0x33, 0xCC, 0x33, 0xCC, 0x33, 0xCC, 0x33};
static const uint8_t ECF3_DEFAULT_2COLS_A[] = {0xFF, 0x33, 0xFF, 0x33, 0xFF, 0x33, 0xFF, 0x33};
static const uint8_t ECF4_DEFAULT_2COLS_A[] = {0x03, 0x0C, 0x30, 0xC0, 0x03, 0x0C, 0x30, 0xC0};

// Default ECF Patterns for 2 colour modes variant B (used in all other 2 colour modes)
static const uint8_t ECF1_DEFAULT_2COLS_B[] = {0xAA, 0x00, 0xAA, 0x00, 0xAA, 0x00, 0xAA, 0x00};
static const uint8_t ECF2_DEFAULT_2COLS_B[] = {0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55};
static const uint8_t ECF3_DEFAULT_2COLS_B[] = {0xFF, 0x55, 0xFF, 0x55, 0xFF, 0x55, 0xFF, 0x55};
static const uint8_t ECF4_DEFAULT_2COLS_B[] = {0x11, 0x22, 0x44, 0x88, 0x11, 0x22, 0x44, 0x88};

// Default ECF Patterns for 4 colour modes
static const uint8_t ECF1_DEFAULT_4COLS[] = {0xA5, 0x0F, 0xA5, 0x0F, 0xA5, 0x0F, 0xA5, 0x0F};
static const uint8_t ECF2_DEFAULT_4COLS[] = {0xA5, 0x5A, 0xA5, 0x5A, 0xA5, 0x5A, 0xA5, 0x5A};
static const uint8_t ECF3_DEFAULT_4COLS[] = {0xF0, 0x5A, 0xF0, 0x5A, 0xF0, 0x5A, 0xF0, 0x5A};
static const uint8_t ECF4_DEFAULT_4COLS[] = {0xF5, 0xFA, 0xF5, 0xFA, 0xF5, 0xFA, 0xF5, 0xFA};

// Default ECF Patterns for 16 colour modes
static const uint8_t ECF1_DEFAULT_16COLS[] = {0x0B, 0x07, 0x0B, 0x07, 0x0B, 0x07, 0x0B, 0x07};
static const uint8_t ECF2_DEFAULT_16COLS[] = {0x23, 0x13, 0x23, 0x13, 0x23, 0x13, 0x23, 0x13};
static const uint8_t ECF3_DEFAULT_16COLS[] = {0x0E, 0x0D, 0x0E, 0x0D, 0x0E, 0x0D, 0x0E, 0x0D};
static const uint8_t ECF4_DEFAULT_16COLS[] = {0x1F, 0x2F, 0x1F, 0x2F, 0x1F, 0x2F, 0x1F, 0x2F};

// Default ECF Patterns for 256 colour modes (RISCOS 3.11)
static const uint8_t ECF1_DEFAULT_256COLS[] = {0xFC, 0xFD, 0xFE, 0xFF, 0xFC, 0xFD, 0xFE, 0xFF};
static const uint8_t ECF2_DEFAULT_256COLS[] = {0x03, 0x02, 0x01, 0x00, 0x03, 0x02, 0x01, 0x00};
static const uint8_t ECF3_DEFAULT_256COLS[] = {0x23, 0x22, 0x21, 0x20, 0x23, 0x22, 0x21, 0x20};
static const uint8_t ECF4_DEFAULT_256COLS[] = {0xDC, 0xDD, 0xDE, 0xDF, 0xDC, 0xDD, 0xDE, 0xDF};

// ECF Pattern state
__attribute__ ((section (".noinit"))) static pixel_t  g_ecf_pattern[4][64];
static int16_t  g_ecf_origin_x;
static int16_t  g_ecf_origin_y;
static int      g_ecf_giant_shift;
static int      g_ecf_mask;
static int      g_ecf_mode;

// Default Dot Patterns
static const uint8_t DEFAULT_DOT_PATTERN[] = {0xAA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// Dot Pattern state
__attribute__ ((section (".noinit"))) static uint8_t g_dot_pattern[64];
static int     g_dot_pattern_len;
static int     g_dot_pattern_index;

#define NUM_SPRITES 256

typedef struct {  
   void *data;
   uint16_t width;
   uint16_t height;
} sprite_t;

// This needs to be in initialized memory, otherwise prim_reset_sprites()
// will erroneously call free on memory that wasn't malloced.
static sprite_t sprites[NUM_SPRITES];

// ==========================================================================
// Static methods (operate at screen resolution)
// ==========================================================================

static inline int max(int a, int b) {
   return (a > b) ? a : b;
}

static inline int min(int a, int b) {
   return (a < b) ? a : b;
}

// The colour an ECF plot mode gives pixel (x,y): patterns 1-4, or 5 (giant)
// taking the pattern from the pixel's column
static inline pixel_t ecf_colour(plotmode_t plotmode, int x, int y) {
   int ecfnum = (plotmode >> 4) - 1;
   if (ecfnum >= 4) {
      ecfnum = ((x - g_ecf_origin_x) >> g_ecf_giant_shift) & 3;
   }
   return g_ecf_pattern[ecfnum][(((y - g_ecf_origin_y) & 7) << 3) + ((x - g_ecf_origin_x) & g_ecf_mask)];
}

static pixel_t get_pixel(screen_mode_t *screen, int x, int y) {
   if (x < g_x_min  || x > g_x_max || y < g_y_min || y > g_y_max) {
      // Return the graphics background colour if off the screen
      return g_bg_col;
   } else {
      return screen->get_pixel(screen, x, y);
   }
}

static void set_pixel(screen_mode_t *screen, int x, int y, plotcol_t col) {
   plotmode_t plotmode;
   pixel_t colour;
   if (x < g_x_min  || x > g_x_max || y < g_y_min || y > g_y_max) {
      return;
   }
   switch (col) {
   default :
   case PC_FG:
      plotmode = g_fg_plotmode;
      colour   = g_fg_col;
      break;
   case PC_BG:
      plotmode = g_bg_plotmode;
      colour   = g_bg_col;
      break;
   case PC_INV:
      plotmode = PM_INVERT;
      colour   = 0; // not used
      break;
   }
   if (plotmode >= PM_ECF) {
      colour = ecf_colour(plotmode, x, y);
      plotmode &= 0x0F;
   }
   if (plotmode != PM_NORMAL) {
      pixel_t existing = screen->get_pixel(screen, x, y);
      switch (plotmode) {
      case PM_OR:
         colour |= existing;
         break;
      case PM_AND:
         colour &= existing;
         break;
      case PM_XOR:
         colour ^= existing;
         break;
      case PM_INVERT:
         colour = max_col - existing;
         break;
      case PM_UNCHANGED:
         colour = existing;
         break;
      case PM_AND_INVERTED:
         colour = existing & (max_col - colour);
         break;
      case PM_OR_INVERTED:
         colour = existing | (max_col - colour);
         break;
      case PM_NORMAL:
      case PM_ECF:
      default:
         break;
      }
   }
   screen->set_pixel(screen, x, y, colour);
}

static void draw_hline(screen_mode_t *screen, int x1, int x2, int y, plotcol_t colour) {
   if (x1 > x2) {
      int tmp = x1;
      x1 = x2;
      x2 = tmp;
   }
   // Clip once here (set_pixel would otherwise re-clip per pixel)
   if (y < g_y_min || y > g_y_max) {
      return;
   }
   if (x1 < g_x_min) {
      x1 = g_x_min;
   }
   if (x2 > g_x_max) {
      x2 = g_x_max;
   }
   if (x1 > x2) {
      return;
   }
   // Fast path: a PM_NORMAL fill is a straight row fill in the
   // framebuffer with no per-pixel plot-mode or ECF work. This feeds
   // every solid fill (triangles, circles, flood spans, CLG).
   plotmode_t plotmode = (colour == PC_FG) ? g_fg_plotmode :
                         (colour == PC_BG) ? g_bg_plotmode : PM_INVERT;
   if (plotmode == PM_NORMAL && screen->fill_hline) {
      screen->fill_hline(screen, x1, x2, y, (colour == PC_FG) ? g_fg_col : g_bg_col);
      return;
   }
   for (int x = x1; x <= x2; x++) {
      set_pixel(screen, x, y, colour);
   }
}

// ---- Ellipses: the GXR's own algorithm -------------------------------------
//
// Source: this is a C re-implementation of the ellipse routine (PLOT 192-207)
// as the BBC Master's MOS 3.20 has it, (C) Acorn Computers Ltd, worked out
// from Toby Nelson's annotated reassembly of the Graphics Extension ROM 1.20
// it came from (https://github.com/tobylobster/GXR-pages, docs/gxr120_acme.a,
// chapter 23) and Tom Seddon's MOS disassembly for where MOS 3.20 differs
// (https://github.com/tom-seddon/acorn_mos_disassembly, src/ext.s65, the
// outline's right-hand run).  No ROM code or data is included.
//
// Row n
// of the ellipse, counted from the centre towards the third PLOT point, has
// half-width (w/h)*sqrt(h*h - n*n) and a shear offset of n*s/h, computed in
// 8.8 fixed point with an integer square root and rounded half up, with the
// rounding errors accumulating exactly as the ROM's do - which is why no
// closed-form ellipse ever fitted the pixels.  Everything is in pixels, so
// modes with oblong pixels come out right by construction.  The outline
// joins each row to its neighbours the way the ROM does: the left run
// extends right until it meets the left ends of the rows above and below,
// and the right run extends left likewise.

// The largest n with n*n <= v, bit by bit: exact, and no floating point
static uint32_t isqrt_u64(uint64_t v) {
   uint64_t root = 0;
   uint64_t bit = (uint64_t)1 << 62;
   while (bit > v) {
      bit >>= 2;
   }
   while (bit) {
      if (v >= root + bit) {
         v -= root + bit;
         root = (root >> 1) + bit;
      } else {
         root >>= 1;
      }
      bit >>= 2;
   }
   return (uint32_t)root;
}

typedef struct {
   int64_t aspect;        // 256 * w / h                    (.ellipse256AspectRatio)
   int64_t shear;         // 256 * s / h, signed            (.ellipse256Shear)
   int64_t hh2;           // h * h                          (.ellipseHalfHeightSquared)
   int64_t acc;           // accumulated shear, 1/256 px   (.ellipseAccumulatedShear)
   int64_t squares;       // n * n                          (.ellipseCountSquares)
   int64_t odd;           // 2n + 1                         (.ellipseCountOddNumbers)
   int n;                 // current row                    (.ellipseCountHeight)
   int half;              // rows to go                     (.ellipseHalfHeightCounter)
   int L, R;              // this row's extent              (.ellipseLeftPoint/RightPoint)
   int A, B;              // next row's extent              (.ellipsePointA/B)
   int C, D;              // previous row's extent          (.ellipsePointC/D)
} gxr_ellipse_t;

static void gxr_ellipse_update(gxr_ellipse_t *e) {          // .updateEllipse
   e->C = e->L;
   e->D = e->R;
   e->L = e->A;
   e->R = e->B;
   int64_t left = e->hh2 - e->squares;                       // h*h - n*n, negative past the top
   int64_t root = (left > 0) ? isqrt_u64((uint64_t)left << 16) : 0;   // 256 * sqrt(h*h - n*n)
   int64_t p = ((e->aspect * root) & 0xFFFFFFFF) >> 8;      // 32-bit product, top 24 bits
   e->B = (int)((e->acc + p + 128) >> 8);                    // round half up, as the ROM
   e->A = (int)((e->acc - p + 128) >> 8);
   e->squares += e->odd;
   e->odd += 2;
   e->acc += e->shear;
   e->n++;
   e->half--;
}

static void gxr_ellipse_update_incrementally(gxr_ellipse_t *e) {   // .updateEllipseIncrementally
   gxr_ellipse_update(e);
   if (e->A > e->R) {
      e->R = e->A;
   } else if (e->B < e->L) {
      e->L = e->B;
   }
}

// Fill row n from a to b, and its reflection through the centre
static void gxr_ellipse_row(screen_mode_t *screen, int xc, int yc, int n, int a, int b, plotcol_t colour) {
   draw_hline(screen, xc + a, xc + b, yc + n, colour);
   if (n) {
      draw_hline(screen, xc - a, xc - b, yc - n, colour);
   }
}

static void gxr_ellipse_point(screen_mode_t *screen, int xc, int yc, int n, int x, plotcol_t colour) {
   set_pixel(screen, xc + x, yc + n, colour);
   if (n) {
      set_pixel(screen, xc - x, yc - n, colour);
   }
}

// width w and height h in pixels, shear s the x offset of the top row
static void gxr_ellipse(screen_mode_t *screen, int xc, int yc, int w, int h, int s, int fill, plotcol_t colour) {
   if (h == 0) {                                             // .zeroHeightEllipse
      draw_hline(screen, xc - w, xc + w, yc, colour);
      return;
   }
   gxr_ellipse_t e = { 0 };
   e.aspect = (256 * (int64_t)w) / h;                        // unsigned divides, then the sign
   e.shear  = (256 * (int64_t)abs(s)) / h;
   if (s < 0) e.shear = -e.shear;
   e.hh2    = (int64_t)h * h;
   e.odd    = 1;
   e.half   = h;
   gxr_ellipse_update(&e);
   gxr_ellipse_update(&e);
   e.n = 0;
   e.D = -e.A;
   e.C = -e.B;
   if (e.R < e.A) {
      e.R = e.A;
      e.L = e.D;
   } else if (e.L > e.B) {
      e.L = e.B;
      e.R = e.C;
   }
   for (;;) {
      if (fill) {                                            // .startFilledEllipse
         gxr_ellipse_row(screen, xc, yc, e.n, e.L, e.R, colour);
      } else {                                               // .startEllipseOutline
         int y = e.C > e.A ? e.C : e.A;                      // rightmost of C and A
         int x = e.L;
         gxr_ellipse_point(screen, xc, yc, e.n, x, colour);
         for (x++; x < y; x++) {
            gxr_ellipse_point(screen, xc, yc, e.n, x, colour);
         }
         // MOS 3.20 then runs from the right point down to one past the
         // lesser of D and B, whatever the left run covered.  (GXR 1.20 and
         // MOS 5 stop the right run where the left run ended, so each pixel
         // is plotted once; on a Master the overlap is plotted twice, which
         // is invisible except under EOR and invert, where it cancels.)
         y = e.D < e.B ? e.D : e.B;
         x = e.R;
         gxr_ellipse_point(screen, xc, yc, e.n, x, colour);
         for (x--; x > y; x--) {
            gxr_ellipse_point(screen, xc, yc, e.n, x, colour);
         }
      }
      if (e.half < 0) break;
      gxr_ellipse_update_incrementally(&e);
   }
   e.n++;                                                    // .finishEllipseLastRow
   gxr_ellipse_row(screen, xc, yc, e.n, e.A, e.B, colour);
}

void prim_init (const screen_mode_t *screen) {
   // max_col is used when calculating the logical inverse of the existing pixel
   max_col = (pixel_t) screen->ncolour;
}

void prim_set_fg_col(const screen_mode_t *screen, pixel_t colour) {
   g_fg_col = colour;
}

void prim_set_fg_plotmode(const screen_mode_t *screen, plotmode_t plotmode) {
   g_fg_plotmode = plotmode;
}

plotmode_t prim_get_fg_plotmode(void) {
   return g_fg_plotmode;
}

pixel_t prim_get_fg_col(void) {
   return g_fg_col;
}

void prim_set_bg_col(const screen_mode_t *screen, pixel_t colour) {
   g_bg_col = colour;
}

void prim_set_bg_plotmode(const screen_mode_t *screen, plotmode_t plotmode) {
   g_bg_plotmode = plotmode;
}

plotmode_t prim_get_bg_plotmode(void) {
   return g_bg_plotmode;
}
#if 0
pixel_t prim_get_bg_col(void) {
   return g_bg_col;
}
#endif
void prim_set_ecf_mode(const screen_mode_t *screen, int ecf_mode) {
   g_ecf_mode = ecf_mode;
}

void prim_set_ecf_origin(const screen_mode_t *screen, int16_t x, int16_t y) {
   g_ecf_origin_x = x;
   g_ecf_origin_y = y;
}

void prim_set_ecf_pattern(const screen_mode_t *screen, int num, const uint8_t *pattern) {
   // The pattern starts with the top row, which has the largest Y value
   pixel_t *ptr = g_ecf_pattern[num] + 8 * 7;
   // Expand pattern into array of pixels_t values
   for (int i = 0; i < 8; i++) {
      uint8_t p = *pattern++;
      //   2-colour modes: 7,6,5,4,3,2,1,0 (BBC) 0,1,2,3,4,5,6,7 (RISC OS)
      //   4-colour modes: 73,62,51,40     (BBC) 10,32,54,76     (RISC OS)
      //  16-colour modes: 7531,6420       (BBC) 3210,7654       (RISC OS)
      // 256-colour modes: 76543210        (BBC) 76543210        (RISC OS)
      if (!g_ecf_mode) {
         // BBC Mode
         switch (screen->ncolour) {
         case 1:
            for (int j = 0; j < 8; j++) {
               ptr[j] = (pixel_t)((p & 0x80) >> 7);
               p = (uint8_t)(p<<1);
            }
            break;
         case 3:
            for (int j = 0; j < 4; j++) {
               ptr[j] = (pixel_t)(((p & 0x80) >> 6) | ((p & 0x08) >> 3));
               p = (uint8_t)(p<<1);
            }
            break;
         case 15:
            for (int j = 0; j < 2; j++) {
               ptr[j] = (pixel_t)(((p & 0x80) >> 4) | ((p & 0x20) >> 3) | ((p & 0x08) >> 2) | ((p & 0x02) >> 1));
               p = (uint8_t)(p<<1);
            }
            break;
         default:
            ptr[0] = (pixel_t)p;
         }
      } else {
         // RISCOS Mode
         switch (screen->ncolour) {
         case 1:
            for (int j = 0; j < 8; j++) {
               ptr[j] = (pixel_t)(p & 0x01);
               p >>= 1;
            }
            break;
         case 3:
            for (int j = 0; j < 4; j++) {
               ptr[j] = (pixel_t)(p & 0x03);
               p >>= 2;
            }
            break;
         case 15:
            for (int j = 0; j < 2; j++) {
               ptr[j] = (pixel_t)(p & 0x0F);
               p >>= 4;
            }
            break;
         default:
            ptr[0] = (pixel_t)p;
         }
      }
      ptr -= 8;
   }
}

void prim_set_ecf_simple(screen_mode_t *screen, int num, const uint8_t *pattern) {
   // The pattern starts with the top row, which has the largest Y value
   // p0 p1
   // p2 p3
   // p4 p5
   // p6 p7
   pixel_t *ptr = g_ecf_pattern[num] + 8 * 7;
   // Expand pattern into array of 8x8 pixels_t values, repeating as necessary
   for (int i = 0; i < 8; i++) {
      for (int j = 0; j < 8; j++) {
         ptr[j] = pattern[((i & 3) << 1) + (j & 1)];
      }
      ptr -= 8;
   }
}

void prim_set_ecf_default(const screen_mode_t *screen) {
   g_ecf_mode = 0;
   g_ecf_origin_x = 0;
   g_ecf_origin_y = 0;
   switch (screen->ncolour) {
   case 1:
      if (screen->mode_num == 0) {
         prim_set_ecf_pattern(screen, 0, ECF1_DEFAULT_2COLS_A);
         prim_set_ecf_pattern(screen, 1, ECF2_DEFAULT_2COLS_A);
         prim_set_ecf_pattern(screen, 2, ECF3_DEFAULT_2COLS_A);
         prim_set_ecf_pattern(screen, 3, ECF4_DEFAULT_2COLS_A);
      } else {
         prim_set_ecf_pattern(screen, 0, ECF1_DEFAULT_2COLS_B);
         prim_set_ecf_pattern(screen, 1, ECF2_DEFAULT_2COLS_B);
         prim_set_ecf_pattern(screen, 2, ECF3_DEFAULT_2COLS_B);
         prim_set_ecf_pattern(screen, 3, ECF4_DEFAULT_2COLS_B);
      }
      g_ecf_mask = 7;
      g_ecf_giant_shift = 3;
      break;
   case 3:
      prim_set_ecf_pattern(screen, 0, ECF1_DEFAULT_4COLS);
      prim_set_ecf_pattern(screen, 1, ECF2_DEFAULT_4COLS);
      prim_set_ecf_pattern(screen, 2, ECF3_DEFAULT_4COLS);
      prim_set_ecf_pattern(screen, 3, ECF4_DEFAULT_4COLS);
      g_ecf_mask = 3;
      g_ecf_giant_shift = 2;
      break;
   case 15:
      prim_set_ecf_pattern(screen, 0, ECF1_DEFAULT_16COLS);
      prim_set_ecf_pattern(screen, 1, ECF2_DEFAULT_16COLS);
      prim_set_ecf_pattern(screen, 2, ECF3_DEFAULT_16COLS);
      prim_set_ecf_pattern(screen, 3, ECF4_DEFAULT_16COLS);
      g_ecf_mask = 1;
      g_ecf_giant_shift = 1;
      break;
   default:
      prim_set_ecf_pattern(screen, 0, ECF1_DEFAULT_256COLS);
      prim_set_ecf_pattern(screen, 1, ECF2_DEFAULT_256COLS);
      prim_set_ecf_pattern(screen, 2, ECF3_DEFAULT_256COLS);
      prim_set_ecf_pattern(screen, 3, ECF4_DEFAULT_256COLS);
      g_ecf_mask = 0;
      g_ecf_giant_shift = 0;
   }
}

void prim_set_dot_pattern(screen_mode_t *screen, const uint8_t *pattern) {
   // Expand the pattern into one byte per pixel for efficient access
   const uint8_t *ptr = pattern;
   uint8_t mask = 0x80;
   for (unsigned int i = 0; i < sizeof(g_dot_pattern); i++) {
      if (*ptr & mask) {
         g_dot_pattern[i] = 1;
      } else {
         g_dot_pattern[i] = 0;
      }
      mask >>= 1;
      if (!mask) {
         mask = 0x80;
         ptr++;
      }
   }
}

void prim_set_dot_pattern_len(screen_mode_t *screen, int len) {
#ifdef DEBUG_VDU
   printf("prim_set_dot_pattern_len = %d\r\n", len);
#endif
   if (len == 0) {
      prim_set_dot_pattern(screen, DEFAULT_DOT_PATTERN);
      g_dot_pattern_len = 8;
   } else if (len <= 64) {
      g_dot_pattern_len = len;
   }
   g_dot_pattern_index = 0;
}

void prim_set_graphics_area(const screen_mode_t *screen, int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
   // Reject illegal windows (this is what OS 1.20 does)
   if (x1 < 0 || x1 >= screen->width || y1 < 0 || y1 >= screen->height) {
      return;
   }
   if (x2 < 0 || x2 >= screen->width || y2 < 0 || y2 >= screen->height) {
      return;
   }
   if (x1 >= x2 || y1 >= y2) {
      return;
   }
   // Update the window
   g_x_min = x1;
   g_y_min = y1;
   g_x_max = x2;
   g_y_max = y2;
}

void prim_clear_graphics_area(screen_mode_t *screen) {
   prim_fill_rectangle(screen, g_x_min, g_y_min, g_x_max, g_y_max, PC_BG);
}

void prim_set_pixel(screen_mode_t *screen, int x, int y, plotcol_t colour) {
   set_pixel(screen, x, y, colour);
}

pixel_t prim_get_pixel(screen_mode_t *screen, int x, int y) {
   return get_pixel(screen, x, y);
}
#if 0
int prim_on_screen(screen_mode_t *screen, int x, int y) {
   return x >= g_x_min && x <= g_x_max && y >= g_y_min && y <= g_y_max;
}
#endif
// Rodders: Line mode support
// Implementation of Bresenham's line drawing algorithm from here:
// http://tech-algorithm.com/articles/drawing-line-using-bresenham-algorithm/
// The OS's line: Bresenham from (x1,y1), taking longest+1 steps.  One stepper
// serves PLOT lines and the span fills, so a filled triangle's edges are the
// same pixels as the lines that would outline it - which is the ROM's rule.
typedef struct {
   int x, y;                 // the current pixel
   int dx1, dy1;             // the diagonal step
   int dx2, dy2;             // the step along the major axis
   int longest, shortest;    // |major|, |minor|
   int numerator;            // the error term
} line_stepper_t;

static void line_stepper_init(line_stepper_t *s, int x1, int y1, int x2, int y2) {
   int w = x2 - x1;
   int h = y2 - y1;
   s->dx1 = (w < 0) ? -1 : (w > 0) ? 1 : 0;
   s->dy1 = (h < 0) ? -1 : (h > 0) ? 1 : 0;
   s->dx2 = s->dx1;
   s->dy2 = 0;
   s->longest = abs(w);
   s->shortest = abs(h);
   if (!(s->longest > s->shortest)) {
      s->longest = abs(h);
      s->shortest = abs(w);
      s->dy2 = s->dy1;
      s->dx2 = 0;
   }
   // dx2/dy2 is the step along the major axis, so their sum is its direction.
   // The OS starts the error term one lower when the major axis runs in the
   // positive direction; that asymmetry is what makes a real Beeb draw A to B
   // and B to A as exactly the same pixels.  Measured against OS 1.20 + GXR.
   int major_dir = s->dx2 + s->dy2;
   s->numerator = (major_dir > 0) ? ((s->longest - 1) >> 1) : (s->longest >> 1);
   s->x = x1;
   s->y = y1;
}

static inline void line_stepper_next(line_stepper_t *s) {
   s->numerator += s->shortest;
   if (!(s->numerator < s->longest)) {
      s->numerator -= s->longest;
      s->x += s->dx1;
      s->y += s->dy1;
   } else {
      s->x += s->dx2;
      s->y += s->dy2;
   }
}

void prim_draw_line(screen_mode_t *screen, int x1, int y1, int x2, int y2, plotcol_t colour, uint8_t linemode) {
   int mask = (linemode & 0x38);
   int dotted =     (mask == 0x10 || mask == 0x18 || mask == 0x30 || mask == 0x38); // Dotted line
   int omit_first = (mask == 0x20 || mask == 0x28 || mask == 0x30 || mask == 0x38); // Omit first
   int omit_last =  (mask == 0x08 || mask == 0x18 || mask == 0x28 || mask == 0x38); // Omit last
   line_stepper_t s;
   line_stepper_init(&s, x1, y1, x2, y2);
   // "longest" sets the Bresenham step ratio, so omitting the last point must
   // shorten the loop, not longest itself - decrementing it re-slopes the line
   int count = omit_last ? s.longest - 1 : s.longest;
   // restart the dot pattern if the first point is plotted
   if (dotted && !omit_first) {
      g_dot_pattern_index = 0;
   }
   for (int i = 0; i <= count; i++) {
      if (i > 0 || !omit_first) {
         if (dotted) {
            if (g_dot_pattern[g_dot_pattern_index++]) {
               set_pixel(screen, s.x, s.y, colour);
            }
            if (g_dot_pattern_index == g_dot_pattern_len) {
               g_dot_pattern_index = 0;
            }
         } else {
            set_pixel(screen, s.x, s.y, colour);
         }
      }
      line_stepper_next(&s);
   }
}

// ---- span fills: the OS fills a polygon row by row between the leftmost
// and rightmost pixel of its own edges, each drawn as a PLOT line.  The edges
// are walked in bands of SPAN_ROWS rows so the scratch stays small whatever
// the screen height; rows outside the graphics window are never walked.

#define SPAN_ROWS 256
__attribute__ ((section (".noinit"))) static int16_t span_min[SPAN_ROWS];
__attribute__ ((section (".noinit"))) static int16_t span_max[SPAN_ROWS];

static void span_edge(int x1, int y1, int x2, int y2, int ybase, int rows) {
   line_stepper_t s;
   line_stepper_init(&s, x1, y1, x2, y2);
   for (int i = 0; i <= s.longest; i++) {
      int r = s.y - ybase;
      if (r >= 0 && r < rows) {
         // Held to one pixel either side of the window, so a corner far off
         // screen cannot wrap int16; draw_hline clips the rest
         int x = (s.x < g_x_min - 1) ? g_x_min - 1 : (s.x > g_x_max + 1) ? g_x_max + 1 : s.x;
         if (x < span_min[r]) span_min[r] = (int16_t)x;
         if (x > span_max[r]) span_max[r] = (int16_t)x;
      }
      line_stepper_next(&s);
   }
}

// Fill the polygon whose n vertices are xs[]/ys[] (closed back to the first)
static void span_fill(screen_mode_t *screen, int n, const int *xs, const int *ys, plotcol_t colour) {
   int ymin = ys[0], ymax = ys[0];
   for (int i = 1; i < n; i++) {
      if (ys[i] < ymin) ymin = ys[i];
      if (ys[i] > ymax) ymax = ys[i];
   }
   if (ymin < g_y_min) ymin = g_y_min;
   if (ymax > g_y_max) ymax = g_y_max;
   for (int ybase = ymin; ybase <= ymax; ybase += SPAN_ROWS) {
      int rows = ymax - ybase + 1;
      if (rows > SPAN_ROWS) rows = SPAN_ROWS;
      for (int r = 0; r < rows; r++) {
         span_min[r] = INT16_MAX;
         span_max[r] = INT16_MIN;
      }
      for (int i = 0; i < n; i++) {
         int j = (i + 1 == n) ? 0 : i + 1;
         span_edge(xs[i], ys[i], xs[j], ys[j], ybase, rows);
      }
      for (int r = 0; r < rows; r++) {
         if (span_min[r] <= span_max[r]) {
            draw_hline(screen, span_min[r], span_max[r], ybase + r, colour);
         }
      }
   }
}


// Does (x,y) hold the foreground (or background) colour - or, if that is an
// ECF, the pattern's colour at that pixel?  The OS's line and flood fills
// test against the pattern, not a solid colour.
static bool pixel_matches_gcol(screen_mode_t *screen, bool fg, int x, int y) {
   plotmode_t plotmode = fg ? g_fg_plotmode : g_bg_plotmode;
   pixel_t colour = fg ? g_fg_col : g_bg_col;
   if (plotmode >= PM_ECF) {
      colour = ecf_colour(plotmode, x, y);
   }
   return screen->get_pixel(screen, x, y) == colour;
}

// ---- Flood fill: the Master's span queue ------------------------------------
//
// Source: this is a C re-implementation of the flood fill (PLOT 128-143) in
// Acorn's MOS 3.20 for the BBC Master 128, (C) Acorn Computers Ltd - &9CF9 in
// the utilities ROM with the line-fill helpers at &DC1C-&DD9E in the MOS -
// worked out from Tom Seddon's disassembly
// (https://github.com/tom-seddon/acorn_mos_disassembly, src/utils.s65 and
// src/mos.s65).  No ROM code or data is included.
//
// A pixel is fillable when it holds the background colour or pattern (PLOT
// 128-135), or does not hold the foreground (136-143), tested on the screen
// as it stands - so a fill in OR, EOR or invert can meet its own pixels, which
// is where the Master and a mark-then-paint fill part company.  The start
// span is filled and queued; each span taken from the queue then has the row
// above and the row below scanned between its ends, and every fillable run
// found there is filled at once and queued.  The Master's queue holds 255
// spans and the whole fill stops if it overflows; a screen taller than the
// BBC's 256 rows gets a proportionally longer queue.

#define FLOOD_SPANS_MAX 4096u         // a power of two: the ring wraps by masking

typedef struct {
   int16_t left, right, y;
} flood_span_t;

__attribute__ ((section (".noinit"))) static flood_span_t flood_spans[FLOOD_SPANS_MAX];

typedef struct {
   screen_mode_t *screen;
   plotcol_t colour;
   bool fg;                      // test against the foreground, else the background
   bool invert;                  // fill what does not match
   unsigned rd, wr, mask;        // ring indices; mask = queue size - 1 (a power of two)
} flood_t;

static bool flood_fillable(const flood_t *f, int x, int y) {
   if (x < g_x_min || x > g_x_max || y < g_y_min || y > g_y_max) {
      return false;
   }
   return pixel_matches_gcol(f->screen, f->fg, x, y) != f->invert;
}

static int flood_run_right(const flood_t *f, int x, int y) {
   while (x < g_x_max && flood_fillable(f, x + 1, y)) {
      x++;
   }
   return x;
}

static bool flood_queue(flood_t *f, int left, int right, int y) {
   unsigned next = (f->wr + 1u) & f->mask;
   if (next == f->rd) {
      return false;                              // full: the Master gives up
   }
   f->wr = next;
   flood_spans[next].left = (int16_t)left;
   flood_spans[next].right = (int16_t)right;
   flood_spans[next].y = (int16_t)y;
   return true;
}

// Fill the runs on row y within the parent span; false if the queue overflowed
static bool flood_row(flood_t *f, flood_span_t p, int y) {
   int x = p.left;
   if (flood_fillable(f, x, y)) {
      int right = flood_run_right(f, x, y);
      int left = x;
      while (left > g_x_min && flood_fillable(f, left - 1, y)) {
         left--;
      }
      draw_hline(f->screen, left, right, y, f->colour);
      if (!flood_queue(f, left, right, y)) {
         return false;
      }
      if (right >= p.right) {
         return true;
      }
      x = right + 1;
   }
   for (;;) {
      if (x > g_x_max) {
         return true;
      }
      // skip what cannot be filled, stopping at the parent's right end
      while (x < p.right && !flood_fillable(f, x + 1, y)) {
         x++;
      }
      if (x >= p.right) {
         return true;
      }
      x++;
      int right = flood_run_right(f, x, y);
      draw_hline(f->screen, x, right, y, f->colour);
      if (!flood_queue(f, x, right, y)) {
         return false;
      }
      if (right >= p.right) {
         return true;
      }
      x = right + 1;
   }
}

static void master_flood(screen_mode_t *screen, int x, int y, plotcol_t colour, fill_t mode) {
   flood_t f;
   f.screen = screen;
   f.colour = colour;
   f.fg = f.invert = (mode == AF_TOFGD);
   f.rd = f.wr = 0;
   f.mask = ((screen->height > 256) ? FLOOD_SPANS_MAX : 256u) - 1u;
   if (!flood_fillable(&f, x, y)) {
      return;
   }
   int right = flood_run_right(&f, x, y);
   int left = x;
   while (left > g_x_min && flood_fillable(&f, left - 1, y)) {
      left--;
   }
   draw_hline(screen, left, right, y, colour);
   flood_queue(&f, left, right, y);
   while (f.rd != f.wr) {
      f.rd = (f.rd + 1u) & f.mask;
      flood_span_t p = flood_spans[f.rd];
      if (p.y != g_y_max && !flood_row(&f, p, p.y + 1)) {
         return;
      }
      if (p.y != g_y_min && !flood_row(&f, p, p.y - 1)) {
         return;
      }
   }
}


void prim_fill_area(screen_mode_t *screen, int x, int y, plotcol_t colour, fill_t mode, fill_result_t *res) {
   int x_left = x;
   int x_right = x;


   int error = 0;

   int offscreen = (x < g_x_min  || x > g_x_max || y < g_y_min || y > g_y_max);

   switch(mode) {
   case HL_LR_NB:
      if (offscreen || !pixel_matches_gcol(screen, false, x, y)) {
         error = 1;
      } else {
         while (x_right < g_x_max && pixel_matches_gcol(screen, false, x_right + 1, y)) {
            x_right++;
         }
         while (x_left > g_x_min && pixel_matches_gcol(screen, false, x_left - 1, y)) {
            x_left--;
         }
         draw_hline(screen, x_left, x_right, y, colour);
      }

      break;

   case HL_RO_BG:
      if (offscreen) {
         error = 1;
      } else if (pixel_matches_gcol(screen, false, x, y)) {
         error = 2;
      } else {
         while (x_right < g_x_max && !pixel_matches_gcol(screen, false, x_right + 1, y)) {
            x_right++;
         }
         draw_hline(screen, x_left, x_right, y, colour);
      }

      break;

   case HL_LR_FG:
      if (offscreen || pixel_matches_gcol(screen, true, x, y)) {
         error = 1;
      } else {
         while (x_right < g_x_max && !pixel_matches_gcol(screen, true, x_right + 1, y)) {
            x_right++;
         }
         while (x_left > g_x_min && !pixel_matches_gcol(screen, true, x_left - 1, y)) {
            x_left--;
         }
         draw_hline(screen, x_left, x_right, y, colour);
      }

      break;

   case HL_RO_NF:
      if (offscreen) {
         error = 1;
      } else if (!pixel_matches_gcol(screen, true, x, y)) {
         error = 2;
      } else {
         while (x_right < g_x_max && pixel_matches_gcol(screen, true, x_right + 1, y)) {
            x_right++;
         }
         draw_hline(screen, x_left, x_right, y, colour);
      }

      break;

   case AF_NONBG:
   case AF_TOFGD:
      master_flood(screen, x, y, colour, mode);
      res->drawn = false;   // a flood leaves the graphics cursors alone
      return;

   default:
#ifdef DEBUG_VDU
      /* IRQ context via the plot dispatch - see framebuffer.c; no release
         printing from here. */
      printf( "Unknown fill mode %d\r\n", mode);
#endif
      res->drawn = false;   // unknown mode: the caller leaves the cursors alone
      return;
   }

   /* Report what happened; applying the OS's cursor rule is the VDU layer's
      job, not the rasteriser's - vdu_25 already owns that dispatch, and the
      rule is keyed to the PLOT code rather than to any property of the
      pixels. */
   res->drawn   = true;
   res->error   = error;
   res->x_left  = x_left;
   res->x_right = x_right;
}

// The OS's filled triangle is the span fill of its own three edges drawn as
// PLOT lines: measured identical on the ROM, the collinear case included.
void prim_fill_triangle(screen_mode_t *screen, int x1, int y1, int x2, int y2, int x3, int y3, plotcol_t colour) {
   int xs[3] = { x1, x2, x3 };
   int ys[3] = { y1, y2, y3 };
   span_fill(screen, 3, xs, ys, colour);
}


// ---- Circles, arcs, chords and sectors: the Master's own walk -------------
//
// Source: this is a C re-implementation of the circle, arc, chord and sector
// routines (PLOT 144-183) in Acorn's MOS 3.20 for the BBC Master 128, (C)
// Acorn Computers Ltd - &9923 sector, &9935 chord, &9999 arc, &9944 and
// &99A4 circles in the utilities ROM, with their helpers at &D24D-&D77B in
// the MOS - worked out from Tom Seddon's disassembly
// (https://github.com/tom-seddon/acorn_mos_disassembly, src/utils.s65 and
// src/mos.s65).  No ROM code or data is included.
//
// MOS 3.20 draws all five of these the same way, and this reproduces it
// pixel for pixel (measured with tools/vdutest and per-row traces of the ROM;
// see docs/dev/vdu-rom-conformance.md).  It walks the boundary of one disc,
// x*x + y*y <= r2 + isqrt(r2) in doubled units for oblong pixels, from its
// bottom pixel up the right-hand side to its top, gathering each row's
// extent.  An arc plots the walked pixels; a filled shape fills each row.
// A sector or chord also follows two edges with the OS line stepper - for a
// sector the two radii, for a chord the chord in both directions - holding
// edge A at its rightmost pixel on a row and edge B at its leftmost.  A flag
// byte, updated whenever the walk lands on the start point or on the point
// where the end radius leaves the circle, and again at the centre row,
// picks which of the circle's edges and the two lines bound each row, and
// can split a row in two.  Everything here is in pixels relative to the
// centre, y up, keeping the 16-bit wrap the ROM's arithmetic depends on.

typedef struct {
   int16_t  x, y;        // current pixel
   int16_t  tx, ty;      // where the edge ends
   uint16_t adx, ady;
   int16_t  err;
   int8_t   sx, sy;      // step directions
} walk_edge_t;

static void walk_edge_init(walk_edge_t *e, int fx, int fy, int tx, int ty) {
   int16_t dx = (int16_t)(tx - fx);
   int16_t dy = (int16_t)(ty - fy);
   e->x = (int16_t)fx;
   e->y = (int16_t)fy;
   e->tx = (int16_t)tx;
   e->ty = (int16_t)ty;
   e->sx = (dx < 0) ? -1 : 1;
   e->sy = (dy < 0) ? -1 : 1;
   e->adx = (uint16_t)((dx < 0) ? -dx : dx);
   e->ady = (uint16_t)((dy < 0) ? -dy : dy);
   uint16_t longest = (e->adx > e->ady) ? e->adx : e->ady;
   if (!((dy - dx) & 0x8000)) {          // the 6502's N flag of dy - dx
      longest = (uint16_t)(longest - 1);
   }
   e->err = (int16_t)((longest >> 1) - e->ady);
}

// One call moves either along x, or to the next row (possibly with an x step)
static void walk_edge_step(walk_edge_t *e) {
   if (e->err >= 0) {
      e->err = (int16_t)(e->err - e->ady);
      e->x = (int16_t)(e->x + e->sx);
      return;
   }
   e->err = (int16_t)(e->err + e->adx);
   if (e->err >= 0) {
      e->err = (int16_t)(e->err - e->ady);
      e->x = (int16_t)(e->x + e->sx);
   }
   e->y = (int16_t)(e->y + e->sy);
}

// Step along the current row to its last pixel, or until the edge ends.
// Never more than adx steps on one row; the bound only guards the IRQ path.
static void walk_edge_to_row_end(walk_edge_t *e) {
   for (unsigned n = e->adx + 1u; n && e->err >= 0 && !(e->x == e->tx && e->y == e->ty); n--) {
      walk_edge_step(e);
   }
}

static void walk_edge_next_row(walk_edge_t *e) {
   for (unsigned n = e->adx + 1u; n && e->err >= 0; n--) {
      walk_edge_step(e);
   }
   walk_edge_step(e);
}

// The square of a 16-bit value as the ROM computes it: |v| times |v|
static uint32_t square16(int v) {
   int16_t s = (int16_t)v;
   uint16_t a = (uint16_t)((s < 0) ? -s : s);
   return (uint32_t)a * a;
}

enum { WALK_CIRCLE = 0, WALK_ARC = 1, WALK_CHORD = 2, WALK_SECTOR = 3 };

#define WALK_B_ACTIVE   0x10u    // edge B is being followed
#define WALK_A_ACTIVE   0x20u    // edge A is being followed
#define WALK_LEFT_OFF   0x40u    // the circle's left edge is not part of the shape
#define WALK_RIGHT_OFF  0x80u    // nor its right edge

typedef struct {
   screen_mode_t *screen;
   int xc, yc;                   // centre, screen pixels
   plotcol_t colour;
   int kind;
   int xs, ys;                   // 1 for the doubled axis of an oblong-pixel mode
   int16_t sx, sy;               // start point, relative
   int16_t ex, ey;               // where the end radius meets the circle
   uint32_t r2;                  // r2 + isqrt(r2), in doubled units
   int16_t radius;               // walk radius in pixels of y
   int16_t cx, cy;               // the walk
   int16_t dx2, dy2, decision;   //   and its incremental terms
   int phase;                    // 0 below the centre row, 1 from it up
   int32_t budget;
   uint8_t flags;                // the shape's state
   uint8_t point_flags;          // what applies to the pixel being added
   uint8_t row_flags;            // what applies to the row being filled
   int16_t left_x, left_y, right_x, right_y;
   walk_edge_t a, b;
} master_walk_t;

// Radius from the start point; false if the ROM gives up (radius >= 8192)
static bool walk_radius(master_walk_t *w) {
   uint32_t r2 = square16(w->sx * (1 << w->xs)) + square16(w->sy * (1 << w->ys));
   w->r2 = r2 + isqrt_u64(r2);
   uint32_t r = isqrt_u64(w->r2);
   if (r >= 0x2000u) {
      return false;
   }
   w->radius = (int16_t)(r >> w->ys);
   return true;
}

// The walk's x on row y, as the ROM finds it when setting up
static void walk_x_at(master_walk_t *w, int y) {
   uint32_t x = isqrt_u64(w->r2 - square16(y * (1 << w->ys))) & 0xFFFFu;
   w->cy = (int16_t)y;
   w->cx = (int16_t)((int16_t)x >> w->xs);
}

static void walk_init(master_walk_t *w) {
   w->phase = 0;
   w->cx = 0;
   w->cy = (int16_t)-w->radius;
   w->dx2 = 0;
   w->dy2 = (int16_t)(2 * w->cy * (1 << w->ys));
   w->decision = (int16_t)(w->r2 - square16(w->cy * (1 << w->ys)));
   w->budget = 64 * ((int32_t)w->radius + 4);
}

static bool walk_x(master_walk_t *w, int d) {
   w->cx = (int16_t)(w->cx + d);
   for (int i = 0; i < (1 << w->xs); i++) {
      if (d > 0) {
         w->dx2 = (int16_t)(w->dx2 + 1); w->decision = (int16_t)(w->decision - w->dx2); w->dx2 = (int16_t)(w->dx2 + 1);
      } else {
         w->dx2 = (int16_t)(w->dx2 - 1); w->decision = (int16_t)(w->decision + w->dx2); w->dx2 = (int16_t)(w->dx2 - 1);
      }
   }
   return w->decision >= 0;
}

static bool walk_y(master_walk_t *w, int d) {
   w->cy = (int16_t)(w->cy + d);
   for (int i = 0; i < (1 << w->ys); i++) {
      if (d > 0) {
         w->dy2 = (int16_t)(w->dy2 + 1); w->decision = (int16_t)(w->decision - w->dy2); w->dy2 = (int16_t)(w->dy2 + 1);
      } else {
         w->dy2 = (int16_t)(w->dy2 - 1); w->decision = (int16_t)(w->decision + w->dy2); w->dy2 = (int16_t)(w->dy2 - 1);
      }
   }
   return w->decision >= 0;
}

// Next pixel of the walk.  False once the budget is spent: the Master never
// finishes some tiny shapes in MODE 2 and 5, and this must not hang instead.
static bool walk_step(master_walk_t *w) {
   if (--w->budget < 0) {
      return false;
   }
   if (w->phase == 0) {
      if (w->cy == 0) {
         w->phase = 1;
      } else if (walk_x(w, +1)) {
         return true;
      }
   }
   if (!walk_y(w, +1) && !walk_x(w, -1)) {
      walk_y(w, -1);
   }
   return true;
}

static void walk_init_edges(master_walk_t *w) {
   if (w->kind == WALK_SECTOR) {
      if (w->phase == 0) {          // below the centre: both radii inwards
         walk_edge_init(&w->a, w->ex, w->ey, 0, 0);
         walk_edge_init(&w->b, w->sx, w->sy, 0, 0);
      } else {                      // above: both outwards
         walk_edge_init(&w->a, 0, 0, w->sx, w->sy);
         walk_edge_init(&w->b, 0, 0, w->ex, w->ey);
      }
   } else {                         // a chord, both ways
      walk_edge_init(&w->a, w->ex, w->ey, w->sx, w->sy);
      walk_edge_init(&w->b, w->sx, w->sy, w->ex, w->ey);
   }
   if (w->a.sx > 0) {
      walk_edge_to_row_end(&w->a);   // A: rightmost pixel on its row
   }
   if (w->b.sx < 0) {
      walk_edge_to_row_end(&w->b);   // B: leftmost
   }
}

// The walk landing exactly on the start point or the end point
static void walk_event(master_walk_t *w, int px, int py, uint8_t on, uint8_t off) {
   if (py != w->cy) {
      return;
   }
   uint8_t side = WALK_RIGHT_OFF;
   if (px < 0) {
      side = WALK_LEFT_OFF;
      px = (int16_t)-px;
   }
   if (px != w->cx) {
      return;
   }
   if (w->flags & 2) {
      uint8_t mark = (uint8_t)(((w->flags & 1) ? on : 0x30u) >> 2);
      if (!(w->flags & mark)) {
         w->flags |= (uint8_t)(mark | on);
      } else {
         w->flags &= (uint8_t)~(mark | off);
      }
   }
   if (!(w->flags & side)) {
      w->flags |= side;              // takes effect from the next pixel
      return;
   }
   w->flags &= (uint8_t)~side;       // takes effect on this pixel
   w->point_flags = w->flags;
   w->row_flags = w->flags;
}

static void walk_events(master_walk_t *w) {
   w->point_flags = w->flags;
   if (w->flags & 3) {
      walk_event(w, w->sx, w->sy, WALK_B_ACTIVE, WALK_A_ACTIVE);
      walk_event(w, w->ex, w->ey, WALK_A_ACTIVE, WALK_B_ACTIVE);
   }
}

// Gather one row of the walk; true when the top of the circle is done
static bool walk_row(master_walk_t *w, bool *hung) {
   w->row_flags = w->flags;
   w->left_x = w->right_x = 0;
   w->left_y = w->right_y = w->cy;
   int16_t row = w->cy;
   for (;;) {
      walk_events(w);
      if (!(w->point_flags & WALK_RIGHT_OFF) && (uint16_t)w->cx >= (uint16_t)w->right_x) {
         w->right_x = w->cx;
      }
      if (!(w->point_flags & WALK_LEFT_OFF) && (uint16_t)w->cx >= (uint16_t)w->left_x) {
         w->left_x = w->cx;
      }
      if (w->cx == 0 && w->phase) {
         w->left_x = (int16_t)-w->left_x;
         return true;
      }
      if (!walk_step(w)) {
         *hung = true;
         return true;
      }
      if ((w->cy & 0xFF) != (row & 0xFF)) {
         w->left_x = (int16_t)-w->left_x;
         return false;
      }
   }
}

// At the centre row a sector swaps to its outward edges
static void walk_centre_row(master_walk_t *w) {
   if (w->left_y != 0 || w->kind != WALK_SECTOR) {
      return;
   }
   if ((w->row_flags & WALK_RIGHT_OFF) && (w->row_flags & WALK_A_ACTIVE)) {
      w->right_x = w->a.x; w->right_y = w->a.y;
   }
   if ((w->row_flags & WALK_LEFT_OFF) && (w->row_flags & WALK_B_ACTIVE)) {
      w->left_x = w->b.x; w->left_y = w->b.y;
   }
   walk_init_edges(w);
   uint8_t f = w->flags ^ 0x3Cu;
   w->flags = (uint8_t)((f & ~0x30u) | ((f & 0x10u) << 1) | ((f & 0x20u) >> 1));
   if ((w->flags & WALK_A_ACTIVE) && w->a.x > w->right_x) {
      w->right_x = w->a.x; w->right_y = w->a.y;
   }
   if ((w->flags & WALK_B_ACTIVE) && w->b.x <= w->left_x) {
      w->left_x = w->b.x; w->left_y = w->b.y;
   }
   w->row_flags = 0;
}

static void walk_fill(master_walk_t *w, int x1, int y, int x2) {
   draw_hline(w->screen, w->xc + x1, w->xc + x2, w->yc + y, w->colour);
}

// Set up an arc, chord or sector: the point where the end radius leaves the
// circle, and which way round the shape goes.  False if the ROM draws nothing
// here or falls back to *degenerate.
static bool walk_setup(master_walk_t *w, int ex, int ey, bool *degenerate) {
   *degenerate = false;
   w->flags = (uint8_t)w->kind;
   if (!walk_radius(w)) {
      return false;
   }
   if (w->radius == 0) {
      *degenerate = true;
      return false;
   }
   walk_edge_t e;
   walk_edge_init(&e, 0, 0, ex, ey);
   walk_x_at(w, e.y);
   bool inside = true;
   int16_t px, py, d;
   for (int32_t n = 4 * (int32_t)w->radius + 64; ; ) {
      px = e.x;
      py = e.y;
      bool new_row = e.err < 0;
      walk_edge_step(&e);
      if (new_row) {
         walk_x_at(w, e.y);
      }
      d = (int16_t)(w->cx - ((e.x < 0) ? -e.x : e.x));
      if (d < 0 || --n < 0) {
         break;
      }
      inside = (d != 0);
   }
   if (d == -1 && inside && (e.x & 0xFF) != (px & 0xFF)) {
      if ((e.sx < 0) != (e.sy < 0)) {
         py = e.y;
      } else {
         px = e.x;
      }
   }
   w->ex = px;
   w->ey = py;
   walk_radius(w);
   bool reverse;
   if ((px < 0) != (w->sx < 0)) {
      reverse = px < 0;
   } else if (w->sy != py) {
      reverse = (w->sy < py) != (px < 0);
   } else if (w->sx != px) {
      reverse = !(w->sx < px) != (py < 0);
   } else {
      *degenerate = true;
      return false;
   }
   if (reverse) {
      w->flags |= WALK_RIGHT_OFF | WALK_LEFT_OFF;
   }
   return true;
}

// kind: WALK_CIRCLE with fill false/true for PLOT 144/152, or an arc, chord
// or sector from centre (xc,yc) through start (x1,y1) towards end (x2,y2)
static void master_walk(screen_mode_t *screen, int kind, bool fill, int xc, int yc, int x1, int y1, int x2, int y2, plotcol_t colour) {
   master_walk_t w;
   memset(&w, 0, sizeof w);
   w.screen = screen;
   w.xc = xc;
   w.yc = yc;
   w.colour = colour;
   w.kind = kind;
   w.xs = (screen->xeigfactor > screen->yeigfactor) ? screen->xeigfactor - screen->yeigfactor : 0;
   w.ys = (screen->yeigfactor > screen->xeigfactor) ? screen->yeigfactor - screen->xeigfactor : 0;
   w.sx = (int16_t)(x1 - xc);
   w.sy = (int16_t)(y1 - yc);
   if (kind == WALK_CIRCLE) {
      if (!walk_radius(&w)) {
         return;
      }
      if (w.radius == 0) {
         set_pixel(screen, xc, yc, colour);
         return;
      }
   } else {
      bool degenerate;
      if (!walk_setup(&w, (int16_t)(x2 - xc), (int16_t)(y2 - yc), &degenerate)) {
         if (degenerate) {
            if (kind == WALK_SECTOR) {
               prim_draw_line(screen, x1, y1, xc, yc, colour, 0);   // the radius
            } else {
               set_pixel(screen, x1, y1, colour);
            }
         }
         return;
      }
      fill = (kind != WALK_ARC);
   }
   walk_init(&w);

   if (!fill) {
      walk_events(&w);
      if (!(w.point_flags & WALK_RIGHT_OFF)) {
         set_pixel(screen, xc + w.cx, yc + w.cy, colour);
      }
      for (;;) {
         if (!walk_step(&w)) {
            return;
         }
         walk_events(&w);
         if (!(w.point_flags & WALK_RIGHT_OFF)) {
            set_pixel(screen, xc + w.cx, yc + w.cy, colour);
         }
         if (w.cx == 0) {
            return;
         }
         if (!(w.point_flags & WALK_LEFT_OFF)) {
            set_pixel(screen, xc - w.cx, yc + w.cy, colour);
         }
      }
   }

   if (kind != WALK_CIRCLE) {
      walk_init_edges(&w);
   }
   for (;;) {
      if (w.flags & WALK_A_ACTIVE) {
         walk_edge_next_row(&w.a);
         if (w.a.sx > 0) {
            walk_edge_to_row_end(&w.a);
         }
      }
      if (w.flags & WALK_B_ACTIVE) {
         walk_edge_next_row(&w.b);
         if (w.b.sx < 0) {
            walk_edge_to_row_end(&w.b);
         }
      }
      bool hung = false;
      bool done = walk_row(&w, &hung);
      if (hung) {
         return;
      }
      walk_centre_row(&w);
      uint8_t f = w.row_flags;
      if (f & WALK_A_ACTIVE) {
         if (f & WALK_RIGHT_OFF) {
            if (f & WALK_LEFT_OFF) {
               walk_fill(&w, w.b.x, w.b.y, w.a.x);
            } else {
               walk_fill(&w, w.left_x, w.left_y, w.a.x);
            }
         } else if (w.a.x != w.b.x) {
            walk_fill(&w, w.b.x, w.b.y, w.right_x);
            walk_fill(&w, w.left_x, w.left_y, w.a.x);
         } else {
            walk_fill(&w, w.left_x, w.left_y, w.right_x);
         }
      } else if (!(f & WALK_RIGHT_OFF)) {
         if (f & WALK_LEFT_OFF) {
            walk_fill(&w, w.b.x, w.b.y, w.right_x);
         } else {
            walk_fill(&w, w.left_x, w.left_y, w.right_x);
         }
      }
      if (done) {
         return;
      }
   }
}

void prim_draw_arc(screen_mode_t *screen, int xc, int yc, int x1, int y1, int x2, int y2, plotcol_t colour) {
   master_walk(screen, WALK_ARC, false, xc, yc, x1, y1, x2, y2, colour);
}

void prim_fill_chord(screen_mode_t *screen, int xc, int yc, int x1, int y1, int x2, int y2, plotcol_t colour) {
   master_walk(screen, WALK_CHORD, true, xc, yc, x1, y1, x2, y2, colour);
}

void prim_fill_sector(screen_mode_t *screen, int xc, int yc, int x1, int y1, int x2, int y2, plotcol_t colour) {
   master_walk(screen, WALK_SECTOR, true, xc, yc, x1, y1, x2, y2, colour);
}

// Block Copy/Move
void prim_move_copy_rectangle(screen_mode_t *screen, int x1, int y1, int x2, int y2, int x3, int y3, int move) {

   // Make x1, y1 the bottom left and x2, y2 the top right of the source
   if (x1 > x2) {
      int tmp = x1;
      x1 = x2;
      x2 = tmp;
   }
   if (y1 > y2) {
      int tmp = y1;
      y1 = y2;
      y2 = tmp;
   }

   // Work out direction to copy in taking account of overlap of source and destination rectangles
   int x_overlap = x3 >= x1 && x3 <= x2;
   int y_overlap = y3 >= y1 && y3 <= y2;
   int xstep  = x_overlap ?     -1 :      1;
   int ystep  = y_overlap ?     -1 :      1;

   // Work out the offset between the source and destination rectangles
   int ox = x3 - x1;
   int oy = y3 - y1;

   // Clip before the loop, not inside it: this runs in IRQ context and the
   // Beeb's coordinates would otherwise set the iteration count (32768 x
   // 32768 for one PLOT). Only a destination pixel inside the graphics window
   // is ever written, so walk just the part of the source whose image lands
   // in the window - at most the window's area. Dropping pixels from the walk
   // keeps the relative order of the rest, so the overlap direction still
   // reads every source pixel before the copy overwrites it.
   int cx1 = x1, cx2 = x2, cy1 = y1, cy2 = y2;
   if (cx1 < g_x_min - ox) cx1 = g_x_min - ox;
   if (cx2 > g_x_max - ox) cx2 = g_x_max - ox;
   if (cy1 < g_y_min - oy) cy1 = g_y_min - oy;
   if (cy2 > g_y_max - oy) cy2 = g_y_max - oy;

   if (cx1 <= cx2 && cy1 <= cy2) {
      int xstart = x_overlap ?      cx2 :      cx1;
      int ystart = y_overlap ?      cy2 :      cy1;
      int xend   = x_overlap ? cx1 - 1 : cx2 + 1;
      int yend   = y_overlap ? cy1 - 1 : cy2 + 1;
      // Copy/Move a pixel at a time (slow.......)
      int dy = ystart + oy;
      for (int sy = ystart; sy != yend; sy += ystep, dy += ystep) {
         int dx = xstart + ox;
         for (int sx = xstart; sx != xend; sx += xstep, dx += xstep) {
            // Default to a background colour pixel
            pixel_t px = g_bg_col;
            // Read source pixel, clipping if necessary
            if (sx >= g_x_min && sx <= g_x_max && sy >= g_y_min && sy <= g_y_max) {
               px = screen->get_pixel(screen, sx, sy);
               // If moving, set the source pixel back to the background colour
               if (move) {
                  set_pixel(screen, sx, sy, PC_BG);
               }
            }
            // Write destination pixel - inside the window by construction
            screen->set_pixel(screen, dx, dy, px);
         }
      }
   }

   if (move) {
      // Source pixels whose image falls outside the window were not walked
      // above, but a move still clears them where they lie inside the window.
      // Where the copy itself has just landed the pixel keeps the copied
      // value, exactly as it did when the walk cleared every source pixel
      // first and the copy then overwrote it.
      int sx1 = (x1 > g_x_min) ? x1 : g_x_min;
      int sx2 = (x2 < g_x_max) ? x2 : g_x_max;
      int sy1 = (y1 > g_y_min) ? y1 : g_y_min;
      int sy2 = (y2 < g_y_max) ? y2 : g_y_max;
      // The common case - source and destination both inside the window -
      // walked every in-window source pixel above; nothing to clear here.
      if (sx1 >= cx1 && sx2 <= cx2 && sy1 >= cy1 && sy2 <= cy2) {
         return;
      }
      for (int sy = sy1; sy <= sy2; sy++) {
         for (int sx = sx1; sx <= sx2; sx++) {
            if (sx >= cx1 && sx <= cx2 && sy >= cy1 && sy <= cy2) {
               continue;   // walked above
            }
            if (sx >= x1 + ox && sx <= x2 + ox && sy >= y1 + oy && sy <= y2 + oy) {
               continue;   // the copy landed here
            }
            set_pixel(screen, sx, sy, PC_BG);
         }
      }
   }
}

void prim_fill_rectangle(screen_mode_t *screen, int x1, int y1, int x2, int y2, plotcol_t colour) {
   // Ensure y1 < y2
   if (y1 > y2) {
      int tmp = y1;
      y1 = y2;
      y2 = tmp;
   }
   for (int y = y1; y <= y2; y++) {
      draw_hline(screen, x1, x2, y, colour);
   }
}

void prim_fill_parallelogram(screen_mode_t *screen, int x1, int y1, int x2, int y2, int x3, int y3, plotcol_t colour) {
   // The fourth corner completes the parallelogram; one span fill, so each
   // pixel is plotted exactly once whatever the plot mode
   int xs[4] = { x1, x2, x3, x3 - x2 + x1 };
   int ys[4] = { y1, y2, y3, y3 - y2 + y1 };
   span_fill(screen, 4, xs, ys, colour);
}


void prim_draw_circle(screen_mode_t *screen, int xc, int yc, int xr, int yr, plotcol_t colour) {
   master_walk(screen, WALK_CIRCLE, false, xc, yc, xr, yr, 0, 0, colour);
}

void prim_fill_circle(screen_mode_t *screen, int xc, int yc, int xr, int yr, plotcol_t colour) {
   master_walk(screen, WALK_CIRCLE, true, xc, yc, xr, yr, 0, 0, colour);
}

void prim_draw_ellipse(screen_mode_t *screen, int xc, int yc, int width, int height, int shear, plotcol_t colour) {
   gxr_ellipse(screen, xc, yc, width, height, shear, 0, colour);
}

void prim_fill_ellipse(screen_mode_t *screen, int xc, int yc, int width, int height, int shear, plotcol_t colour) {
   gxr_ellipse(screen, xc, yc, width, height, shear, 1, colour);
}

void prim_draw_character(screen_mode_t *screen, int c, int x_pos, int y_pos, plotcol_t colour) {
   // Draw the character
   font_t *font = screen->font;
   int x = x_pos;
   int y = y_pos;
   int scale_w = font->get_scale_w(font);
   int scale_h = font->get_scale_h(font);
   int width   = font->width << font->get_rounding(font);
   int height  = font->height << font->get_rounding(font);
   int p       = c * height;
   int mask    = 1 << (width - 1);
   for (int i = 0; i < height; i++) {
      int data = font->buffer[p++];
      for (int j = 0; j < width; j++) {
         if (data & mask) {
            // As in default_write_char: y is the top scanline of the cell and
            // the row loop walks downwards, so the scale expands downwards too
            for (int sx = 0; sx < scale_w; sx++) {
               for (int sy = 0; sy < scale_h; sy++) {
                  set_pixel(screen, x + sx, y - sy, colour);
               }
            }
         }
         x += scale_w;
         data <<= 1;
      }
      x = x_pos;
      y -= scale_h;
   }
}

void prim_reset_sprites(screen_mode_t *screen) {
   for (int i = 0; i < NUM_SPRITES; i++) {
      sprites[i].width = 0;
      sprites[i].height = 0;
      if (sprites[i].data) {
         free(sprites[i].data);
      }
      sprites[i].data = 0;
   }
}

void prim_define_sprite(screen_mode_t *screen, int n, int x1, int y1, int x2, int y2) {
   if (n >= NUM_SPRITES) {
      return;
   }
   sprite_t *sprite = sprites + n;

   // Make x1, y1 the bottom left, and x2, y2 the top right
   if (x1 > x2) {
      int tmp = x1;
      x1 = x2;
      x2 = tmp;
   }
   if (y1 > y2) {
      int tmp = y1;
      y1 = y2;
      y2 = tmp;
   }

#ifdef DEBUG_VDU
   printf("defining sprite %d (%d,%d to %d,%d)\r\n", n, x1, y1, x2, y2);
#endif

   if  (sprite->data != NULL)
         free(sprite->data);
   sprite->data = NULL;
   sprite->width = 0;
   sprite->height = 0;

   // A sprite is a capture of the screen, so one wider or taller than the
   // screen is refused rather than allocated. Beyond bounding the read loop
   // (this runs in IRQ context), at 32 bpp a 32768 x 32768 request made the
   // size wrap to zero, malloc(0) passed the NULL test and the loop wrote
   // 2^30 words through it.
   if (x2 - x1 >= screen->width || y2 - y1 >= screen->height) {
      return;
   }

   // Memory allocation
   sprite->width = (uint16_t)(x2 - x1 + 1);
   sprite->height = (uint16_t)(y2 - y1 + 1);
   size_t size = ((size_t)sprite->width * (size_t)sprite->height) << (screen->log2bpp - 3);
   sprite->data = malloc(size);

   if  (sprite->data == NULL) {
      sprite->width = 0;
      sprite->height = 0;
      return;
   }

   // Read the sprite
   if (screen->log2bpp == 4) {
      uint16_t *data = sprite->data;
      for (int yp = 0; yp < sprite->height; yp++) {
         for (int xp = 0; xp < sprite->width; xp++) {
            *data++ = (uint16_t)get_pixel(screen, x1 + xp, y1 + yp);
         }
      }
   } else if (screen->log2bpp == 5)  {
      uint32_t *data = sprite->data;
      for (int yp = 0; yp < sprite->height; yp++) {
         for (int xp = 0; xp < sprite->width; xp++) {
            *data++ = get_pixel(screen, x1 + xp, y1 + yp);
         }
      }
   } else {
      uint8_t *data = sprite->data;
      for (int yp = 0; yp < sprite->height; yp++) {
         for (int xp = 0; xp < sprite->width; xp++) {
            *data++ = (uint8_t)get_pixel(screen, x1 + xp, y1 + yp);
         }
      }
   }
}

void prim_draw_sprite(screen_mode_t *screen, int n, int x, int y) {
   if (n >= NUM_SPRITES) {
      return;
   }
   const sprite_t *sprite = sprites + n;
   // Return if sprite is not properly defined
   if (sprite->width == 0) {
#ifdef DEBUG_VDU
      printf("prim_draw_sprite: %d: width zero\n", n);
#endif
      return;
   }
   if (sprite->height == 0) {
#ifdef DEBUG_VDU
      printf("prim_draw_sprite: %d: height zero\n", n);
#endif
      return;
   }
   if (sprite->data == NULL) {
#ifdef DEBUG_VDU
      printf("prim_draw_sprite: %d: data null\n", n);
#endif
      return;
   }
#ifdef DEBUG_VDU
   printf("drawing sprite %d at %d,%d\r\n", n, x, y);
#endif

   // Write the sprite, allowing clipping to take care of off-screen pixels
   if (screen->log2bpp == 4) {
      const uint16_t *datap = sprite->data;
      for (int yp = 0; yp < sprite->height; yp++) {
         int yy = y + yp;
         for (int xp = 0; xp < sprite->width; xp++) {
            int xx = x + xp;
            uint16_t data = *datap++;
            if (xx >= g_x_min && xx <= g_x_max && yy >= g_y_min && yy <= g_y_max) {
               screen->set_pixel(screen, xx, yy, data);
            }
         }
      }
   } else if (screen->log2bpp == 5)  {
      const uint32_t *datap = sprite->data;
      for (int yp = 0; yp < sprite->height; yp++) {
         int yy = y + yp;
         for (int xp = 0; xp < sprite->width; xp++) {
            int xx = x + xp;
            uint32_t data = *datap++;
            if (xx >= g_x_min && xx <= g_x_max && yy >= g_y_min && yy <= g_y_max) {
               screen->set_pixel(screen, xx, yy, data);
            }
         }
      }
   } else {
      const uint8_t *datap = sprite->data;
      for (int yp = 0; yp < sprite->height; yp++) {
         int yy = y + yp;
         for (int xp = 0; xp < sprite->width; xp++) {
            int xx = x + xp;
            uint8_t data = *datap++;
            if (xx >= g_x_min && xx <= g_x_max && yy >= g_y_min && yy <= g_y_max) {
               screen->set_pixel(screen, xx, yy, data);
            }
         }
      }
   }
}
