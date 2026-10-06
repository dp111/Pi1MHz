/* Host test of the VDU 23,27 sprite store in framebuffer/primitives.c: the
 * REAL prim_define_sprite / prim_draw_sprite / prim_reset_sprites against a
 * fake screen.
 *
 * Sprite define and reset run in the VDU drain, which is IRQ context, so they
 * must never touch the newlib heap (the main loop also uses it and nothing
 * locks it).  run_tests.sh links with --wrap=malloc/free/realloc/calloc, which
 * counts every heap call the framebuffer code makes: it must stay at zero.
 *
 * The rest checks behaviour: what is captured is what is drawn, redefining a
 * sprite larger or smaller leaves the others intact, reset forgets every
 * sprite and gives the whole store back, and running out of store makes the
 * sprite undefined (as a failed malloc did) instead of corrupting anything. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "primitives.h"

#ifndef SPRITE_POOL_BYTES
#define SPRITE_POOL_BYTES (1024u * 1024u)    /* old code: no pool, any size will do */
#endif

/* ---- heap accounting (see --wrap in run_tests.sh) ---- */
static unsigned heap_calls;
void *__real_malloc(size_t);
void  __real_free(void *);
void *__real_realloc(void *, size_t);
void *__real_calloc(size_t, size_t);
void *__wrap_malloc(size_t n)            { heap_calls++; return __real_malloc(n); }
void  __wrap_free(void *p)               { if (p) heap_calls++; __real_free(p); }
void *__wrap_realloc(void *p, size_t n)  { heap_calls++; return __real_realloc(p, n); }
void *__wrap_calloc(size_t a, size_t b)  { heap_calls++; return __real_calloc(a, b); }

/* ---- checks ---- */
static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
   printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- fake screen: pixel (x,y) of "picture" salt is a pure function ---- */
#define SW 1024
#define SH 1024
static int      salt, bpp_bits;
static uint32_t pix(int x, int y, int s) {
   uint32_t v = (uint32_t)(x * 31 + y * 17 + s * 101 + (x ^ y) * 7);
   return bpp_bits == 32 ? v * 2654435761u : v & ((1u << bpp_bits) - 1u);
}
static pixel_t fake_get(const screen_mode_t *s, int x, int y) { (void)s; return pix(x, y, salt); }

/* set_pixel verifies against the expectation instead of storing a frame */
static int ex_x1, ex_y1, ex_salt, ex_calls, ex_bad;
static void fake_set(const screen_mode_t *s, int x, int y, pixel_t v) {
   (void)s;
   ex_calls++;
   if (v != pix(ex_x1 + x, ex_y1 + y, ex_salt)) ex_bad++;
}

static screen_mode_t scr;
static void set_bpp(int log2bpp) {
   scr.log2bpp = log2bpp;
   bpp_bits = 1 << log2bpp;
   scr.ncolour = (log2bpp == 3) ? 255u : 0xffffu;
   prim_init(&scr);
}

/* define a w x h sprite whose lower-left capture corner is (x1,y1) */
static void define(int n, int x1, int y1, int w, int h, int s) {
   salt = s;
   prim_define_sprite(&scr, n, x1, y1, x1 + w - 1, y1 + h - 1);
}
/* draw sprite n at (0,0); true if it painted exactly w*h correct pixels */
static int paints(int n, int x1, int y1, int w, int h, int s) {
   ex_x1 = x1; ex_y1 = y1; ex_salt = s; ex_calls = 0; ex_bad = 0;
   prim_draw_sprite(&scr, n, 0, 0);
   return ex_calls == w * h && ex_bad == 0;
}
static int undefined(int n) {
   ex_calls = 0;
   prim_draw_sprite(&scr, n, 0, 0);
   return ex_calls == 0;
}

static void test_basic(int log2bpp) {
   printf("basic define/draw/reset at %d bpp\n", 1 << log2bpp);
   set_bpp(log2bpp);
   prim_reset_sprites(&scr);
   CHECK(undefined(0) && undefined(255), "fresh store has sprites defined");
   define(3, 10, 20, 40, 30, 1);
   CHECK(paints(3, 10, 20, 40, 30, 1), "sprite 3 drew wrongly");
   CHECK(undefined(2) && undefined(4), "neighbours of 3 appeared");
   prim_reset_sprites(&scr);
   CHECK(undefined(3), "sprite 3 survived reset");
   /* swapped corners are normalised */
   salt = 2;
   prim_define_sprite(&scr, 5, 49, 59, 10, 20);
   CHECK(paints(5, 10, 20, 40, 40, 2), "swapped-corner sprite drew wrongly");
   /* too wide for the screen, and slot out of range, are refused */
   prim_define_sprite(&scr, 6, 0, 0, SW, 10);
   CHECK(undefined(6), "over-wide sprite accepted");
   prim_define_sprite(&scr, 256, 0, 0, 9, 9);
   prim_draw_sprite(&scr, 256, 0, 0);
   prim_reset_sprites(&scr);
}

static void test_redefine(void) {
   printf("redefine larger / smaller / same, neighbours intact\n");
   set_bpp(3);
   prim_reset_sprites(&scr);
   define(0, 0, 0, 100, 100, 10);
   define(1, 5, 5, 200, 50, 11);
   define(2, 7, 9, 64, 64, 12);
   /* grow the middle one well past its old space, then shrink it, then regrow */
   define(1, 3, 3, 500, 400, 21);
   CHECK(paints(1, 3, 3, 500, 400, 21), "grown sprite wrong");
   CHECK(paints(0, 0, 0, 100, 100, 10) && paints(2, 7, 9, 64, 64, 12), "neighbours damaged by grow");
   define(1, 1, 1, 8, 8, 22);
   CHECK(paints(1, 1, 1, 8, 8, 22), "shrunk sprite wrong");
   CHECK(paints(0, 0, 0, 100, 100, 10) && paints(2, 7, 9, 64, 64, 12), "neighbours damaged by shrink");
   define(1, 2, 2, 300, 300, 23);
   CHECK(paints(1, 2, 2, 300, 300, 23), "regrown sprite wrong");
   define(1, 2, 2, 300, 300, 24);                          /* same size */
   CHECK(paints(1, 2, 2, 300, 300, 24), "same-size redefine wrong");
   CHECK(paints(0, 0, 0, 100, 100, 10) && paints(2, 7, 9, 64, 64, 12), "neighbours damaged by regrow");
   /* a refused redefine leaves the slot undefined, as before */
   prim_define_sprite(&scr, 1, 0, 0, SW + 5, 5);
   CHECK(undefined(1), "refused redefine left old sprite");
   CHECK(paints(0, 0, 0, 100, 100, 10) && paints(2, 7, 9, 64, 64, 12), "neighbours damaged by refusal");
   prim_reset_sprites(&scr);
}

static void test_exhaustion(void) {
   printf("exhaustion: refused, nothing corrupted, store recovers\n");
   set_bpp(3);
   prim_reset_sprites(&scr);
   const int rows = (int)(SPRITE_POOL_BYTES / SW) * 2 / 5;          /* ~40% of the store each */
   define(0, 0, 0, SW, rows, 31);
   define(1, 0, 0, SW, rows, 32);
   CHECK(paints(0, 0, 0, SW, rows, 31) && paints(1, 0, 0, SW, rows, 32), "two big sprites wrong");
   define(2, 0, 0, SW, rows, 33);                                   /* a third cannot fit */
   CHECK(undefined(2), "third big sprite fitted (store is %u bytes)", (unsigned)SPRITE_POOL_BYTES);
   CHECK(paints(0, 0, 0, SW, rows, 31) && paints(1, 0, 0, SW, rows, 32), "exhaustion damaged earlier sprites");
   /* the whole screen is 1 MiB at 8bpp: more than the store holds, refused cleanly */
   define(3, 0, 0, SW, SH, 34);
   CHECK(undefined(3), "screen-sized sprite fitted");
   /* freeing space (a smaller redefine) lets a later one in */
   define(0, 0, 0, 16, 16, 35);
   define(2, 0, 0, SW, rows / 2, 36);
   CHECK(paints(2, 0, 0, SW, rows / 2, 36), "no room after freeing space");
   CHECK(paints(0, 0, 0, 16, 16, 35) && paints(1, 0, 0, SW, rows, 32), "damage after reuse");
   /* reset gives it all back: the same fill succeeds again */
   prim_reset_sprites(&scr);
   define(0, 0, 0, SW, rows, 37);
   define(1, 0, 0, SW, rows, 38);
   CHECK(paints(0, 0, 0, SW, rows, 37) && paints(1, 0, 0, SW, rows, 38), "store not whole after reset");
   prim_reset_sprites(&scr);
   /* a full set of 256 small sprites fits */
   for (int n = 0; n < 256; n++) define(n, n, n, 8 + n % 9, 8 + n % 7, n);
   int ok = 1;
   for (int n = 0; n < 256; n++) ok &= paints(n, n, n, 8 + n % 9, 8 + n % 7, n);
   CHECK(ok, "256 small sprites did not all survive");
   prim_reset_sprites(&scr);
}

/* Random define/redefine/reset against a shadow of what each slot should hold.
   Sprites are small enough that the live set can never come near the store,
   so a define must always succeed. */
static void test_churn(int log2bpp, int maxdim, int ops) {
   printf("churn at %d bpp, %d ops\n", 1 << log2bpp, ops);
   set_bpp(log2bpp);
   prim_reset_sprites(&scr);
   struct { int def, x, y, w, h, s; } sh[256];
   memset(sh, 0, sizeof sh);
   srand(12345u + (unsigned)log2bpp);
   for (int i = 0; i < ops; i++) {
      int n = rand() % 256, r = rand() % 100;
      if (r < 2) {
         prim_reset_sprites(&scr);
         memset(sh, 0, sizeof sh);
      } else {
         int w = 1 + rand() % maxdim, h = 1 + rand() % maxdim;
         int x = rand() % (SW - w), y = rand() % (SH - h);
         define(n, x, y, w, h, i);
         sh[n].def = 1; sh[n].x = x; sh[n].y = y; sh[n].w = w; sh[n].h = h; sh[n].s = i;
         CHECK(paints(n, x, y, w, h, i), "op %d: new sprite %d wrong", i, n);
      }
      if (i % 50 == 0) {
         for (int k = 0; k < 256; k++) {
            if (sh[k].def) CHECK(paints(k, sh[k].x, sh[k].y, sh[k].w, sh[k].h, sh[k].s), "op %d: sprite %d corrupted", i, k);
            else           CHECK(undefined(k), "op %d: sprite %d should be undefined", i, k);
         }
      }
   }
   prim_reset_sprites(&scr);
}

int main(void) {
   scr.width = SW; scr.height = SH;
   scr.get_pixel = fake_get; scr.set_pixel = fake_set;
   set_bpp(3);
   prim_set_graphics_area(&scr, 0, 0, SW - 1, SH - 1);

   heap_calls = 0;                         /* count from here: only the code under test runs */
   test_basic(3); test_basic(4); test_basic(5);
   test_redefine();
   test_exhaustion();
   test_churn(3, 24, 3000);
   test_churn(4, 24, 3000);
   test_churn(5, 24, 400);      /* 256 * 24*24*4 = 590 KB worst case: still fits */
   CHECK(heap_calls == 0, "sprite code made %u heap calls (malloc/free in IRQ context)", heap_calls);

   printf("%d checks, %d failed\n", checks, fails);
   return fails ? 1 : 0;
}
