#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <math.h>
#include "rpi/mailbox.h"
#include "framebuffer.h"
#include "BBCFont.h"
#include "Mode7Font.h"
#include "Pi1MHz.h"

#define DEBUG_FB

#define BBC_X_RESOLUTION 1280
#define BBC_Y_RESOLUTION 1024

#define SCREEN_WIDTH    640
#define SCREEN_HEIGHT   512

#define CURSOR (95-32)
#define CLEAR_CHAR 255

//define how many bits per pixel

#define BPP8

#ifdef BPP32
#define SCREEN_DEPTH    32
#define PIXEL_T uint32_t
#define BYTES_PER_PIXEL (SCREEN_DEPTH/8)
#define ALPHA 0xFF000000
#define RED   0xFFFF0000
#define GREEN 0xFF00FF00
#define BLUE  0xFF0000FF

#endif

#ifdef BPP16
#define SCREEN_DEPTH    16
#define PIXEL_T uint16_t
#define BYTES_PER_PIXEL (SCREEN_DEPTH/8)
// R4 R3 R2 R1 R0 G5 G4 G3 G2 G1 G0 B4 B3 B2 B1 B0
#define ALPHA 0x0000
#define RED   0xF800
#define GREEN 0x07E0
#define BLUE  0x001F

#endif

#ifdef BPP8
#define SCREEN_DEPTH    8
#define PIXEL_T uint8_t
#define BYTES_PER_PIXEL (SCREEN_DEPTH/8)
#define ALPHA 0x0000
#define RED   0xE0
#define GREEN 0x1C
#define BLUE  0x03

#endif

#ifdef BPP4
#define SCREEN_DEPTH    4
#define PIXEL_T uint8_t
#define BYTES_PER_PIXEL (1)
#define ALPHA 0x0000
#define RED   0x8
#define GREEN 0x6
#define BLUE  0x1

#endif

#define NUM_COLOURS 256

NOINIT_SECTION static uint32_t colour_table[NUM_COLOURS];

#if defined(BPP32)


static inline PIXEL_T get_colour(unsigned int index) {
   return colour_table[index];
}

static inline void set_colour(unsigned int index, int r, int g, int b) {
   colour_table[index] = 0xFF000000 | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
};

static void update_palette(int offset, int num_colours) {
}


#elif defined (BPP16)

static inline PIXEL_T get_colour(unsigned int index) {
   return colour_table[index];
}

// 15 14 13 12 11 10  9  8  7  6  5  4  3  2  1  0
// R4 R3 R2 R1 R0 G5 G4 G3 G2 G1 G0 B4 B3 B2 B1 B0

static inline void set_colour(unsigned int index, int r, int g, int b) {
   colour_table[index] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3);
}

static void update_palette(int offset, int num_colours) {
}


#else

#define SCREEN_DEPTH    8

static inline PIXEL_T get_colour(unsigned int index) {
   return (PIXEL_T)index;
}

static inline void set_colour(unsigned int index, int r, int g, int b) {
   colour_table[index] = 0xFF000000 | ((b & 0xFF) << 16) | ((g & 0xFF) << 8) | (r & 0xFF);
}

static void update_palette(int offset, int num_colours) {
   RPI_PropertyStart(TAG_SET_PALETTE, 2 + num_colours);
   RPI_PropertyAddTwoWords(offset , num_colours);
   for(uint32_t i = 0 ; i < num_colours ; i++)
      RPI_PropertyAdd(colour_table[offset + i]);

   //RPI_PropertyInit();
   //RPI_PropertyAddTag(TAG_SET_PALETTE, offset, num_colours, colour_table);
   // Call the Check version as doorbell and mailboxes are separate
   //LOG_INFO("Calling TAG_SET_PALETTE\r\n");
   //RPI_PropertyProcess();
   //rpi_mailbox_property_t *buf = RPI_PropertyGet(TAG_SET_PALETTE);
   //if (buf) {
   //   LOG_INFO("TAG_SET_PALETTE returned %08x\r\n", buf->data.buffer_32[0]);
   //} else {
   //   LOG_INFO("TAG_SET_PALETTE returned ?\r\n");
   //}
}

#endif

#define BYTES_PER_PIXEL (SCREEN_DEPTH/8)

// Fill modes:
#define HL_LR_NB 1 // Horizontal line fill (left & right) to non-background
#define HL_RO_BG 2 // Horizontal line fill (right only) to background
#define HL_LR_FG 3 // Horizontal line fill (left & right) to foreground
#define HL_RO_NF 4 // Horizontal line fill (right only) to non-foreground
#define AF_NONBG 5 // Flood (area fill) to non-background
#define AF_TOFGD 6 // Flood (area fill) to foreground

// Character colour / cursor position
static uint32_t c_bg_col;
static uint32_t c_fg_col;
static uint32_t c_x_pos;
static uint32_t c_y_pos;
static uint32_t old_c_x_pos=0;
static uint32_t old_c_y_pos=0;
static uint32_t vdu45flag=0;
static uint32_t c_x_max;
static uint32_t c_y_max;
static uint32_t c_x_scale;
static uint32_t c_y_scale;
static uint32_t mode7flag;

// Graphics colour / cursor position
static uint8_t g_bg_col;
static uint8_t g_fg_col;
static int g_x_origin;
static int16_t g_x_pos;
static int g_x_pos_last1;
static int g_x_pos_last2;
static int g_y_origin;
static int16_t g_y_pos;
static int g_y_pos_last1;
static int g_y_pos_last2;

static uint32_t cheight;

static unsigned char* fb = NULL;
static unsigned char* fbcache = NULL;
static uint32_t width, height;

static int bpp, pitch;

static void mode_init(){
   c_x_max = 39;
   c_y_max = 24;
   c_x_scale = 2;
   c_y_scale = 2;
   mode7flag = 1;
   cheight = 10;
}

static void fb_init_variables() {

    // Character colour / cursor position
   c_bg_col = 0;
   c_fg_col = 15;
   c_x_pos  = 0;
   c_y_pos  = 0;

   // Graphics colour / cursor position
   g_bg_col      = 0;
   g_fg_col      = 15;
   g_x_origin    = 0;
   g_x_pos       = 0;
   g_x_pos_last1 = 0;
   g_x_pos_last2 = 0;
   g_y_origin    = 0;
   g_y_pos       = 0;
   g_y_pos_last1 = 0;
   g_y_pos_last2 = 0;

}

static void fb_putpixel(int x, int y, unsigned int colour) {

   x = ((x + g_x_origin) * SCREEN_WIDTH)  / BBC_X_RESOLUTION;
   y = ((y + g_y_origin) * SCREEN_HEIGHT) / BBC_Y_RESOLUTION;

   if (x < 0  || x > SCREEN_WIDTH - 1) {
      return;
   }
   if (y < 0 || y > SCREEN_HEIGHT - 1) {
      return;
   }

#ifdef BPP4
   PIXEL_T *fbptr = (PIXEL_T *)(fb + (SCREEN_HEIGHT - y - 1) * pitch + x * BYTES_PER_PIXEL);

   if (~x&1)
      {*fbptr = ((*fbptr) &0x0F) | get_colour(colour)<<4;}
   else
      {*fbptr = ((*fbptr) &0xF0) | get_colour(colour);}
   return;
#else
    *(PIXEL_T *)(fb + (SCREEN_HEIGHT - y - 1) * pitch + x * BYTES_PER_PIXEL) = get_colour(colour);
#endif
}

PIXEL_T fb_getpixel(int x, int y) {
   x = ((x + g_x_origin) * SCREEN_WIDTH)  / BBC_X_RESOLUTION;
   y = ((y + g_y_origin) * SCREEN_HEIGHT) / BBC_Y_RESOLUTION;
   if (x < 0  || x > SCREEN_WIDTH - 1) {
      return g_bg_col;
   }
   if (y < 0 || y > SCREEN_HEIGHT - 1) {
      return g_bg_col;
   }

   PIXEL_T *fbptr = (PIXEL_T *)(fb + (SCREEN_HEIGHT - y - 1) * pitch + x * BYTES_PER_PIXEL);
   return *fbptr;
}

// NB these functions can over run the buffer
static void memmovequick( void* addr_dst, const void * addr_src, size_t length)
{
  uint32_t * dst = (uint32_t *) addr_dst;
  const uint32_t * src = (const uint32_t *) addr_src;
  uint32_t temp = length;
  length = (length >>2)>>2;
  if ((length<<4)<(temp)) length +=1;
  for ( ; length; length--) {
    uint32_t a = *src++;
    uint32_t b = *src++;
    uint32_t c = *src++;
    uint32_t d = *src++;
    *dst++ = a;
    *dst++ = b;
    *dst++ = c;
    *dst++ = d;
  }
}

// Value is treated as a 32 bit word which is wrong
static void memsetquick( void * ptr, int value, size_t num )
{
  uint32_t * dst = (uint32_t *) ptr;
  uint32_t a=value;
  uint32_t length = num;
  length = (length >>2);
  // not much point here of doing more than 4 bytes as the write buffer will improve
  // performance
  for ( ; length; length--) {
    *dst++ = a;
  }
}
static void _clean_invalidate_dcache()
{
   asm volatile ("mcr p15, 0, %0, c7, c14, 0" : : "r" (0));
}
/*
#include "rpi/audio.h"

struct bcm2708_dma_cb {
   uint32_t info;
   uint32_t src;
   uint32_t dst;
   uint32_t length;
   uint32_t stride;
   uint32_t next;
   uint32_t pad[2];
};
NOINIT_SECTION static __attribute__ ((aligned (0x20))) struct bcm2708_dma_cb dma_memcpy_data;

static void dma_memcpy( void* addr_dst, const void * addr_src, size_t length)
{
   dma_memcpy_data.info = BCM2708_DMA_S_INC | BCM2708_DMA_WAIT_RESP | BCM2708_DMA_S_WIDTH | BCM2708_DMA_D_WIDTH | BCM2708_DMA_D_INC | BCM2708_DMA_BURST(4);
   dma_memcpy_data.src = ((uint32_t)addr_src) | GPU_BASE ;
   dma_memcpy_data.dst = ((uint32_t)addr_dst) | GPU_BASE ;
   dma_memcpy_data.length = length;
   dma_memcpy_data.stride = 0;
   dma_memcpy_data.next = 0;
   dma_memcpy_data.pad[0] = 0;
   dma_memcpy_data.pad[1] = 0;

   _clean_cache_area(dma_memcpy_data, sizeof(dma_memcpy_data));

   RPI_DMABase->Enable = 1<<4; // enable DMA 4

   RPI_DMA4Base->CS = BCM2708_DMA_RESET;
   RPI_WaitMicroSeconds(2);
   RPI_DMA4Base->CS = BCM2708_DMA_INT | BCM2708_DMA_END;
   RPI_DMA4Base->ADDR = (uint32_t)&dma_memcpy_data.info | GPU_BASE;
   RPI_DMA4Base->Debug = 7; // clear debug error flags
   RPI_WaitMicroSeconds(2);
   RPI_DMA4Base->CS = 0x10880000 | BCM2708_DMA_ACTIVE;  // go, mid priority, wait for outstanding writes
}
*/
static void fb_scroll() {

//RPI_SetGpioHi(TEST_PIN);
#if 0
   static uint32_t viewport = 0;
   viewport++;
   fb = fb + cheight * pitch;
   if (viewport>40)
   {
      viewport = 0;
      fb = fbcache;
   }
   RPI_PropertySetWord( TAG_SET_VIRTUAL_OFFSET , 0 , cheight*viewport );
   memsetquick(fbcache + (height - cheight) * pitch, 0, cheight * pitch);
#else

  //_clean_invalidate_dcache();
   memmovequick(fbcache, fbcache + cheight * pitch * c_y_scale, (height - cheight)* c_y_scale * pitch);
  // dma_memcpy(fb, fb + cheight * pitch, (height - cheight) * pitch);
   memsetquick(fbcache + (height - cheight)* c_y_scale * pitch, 0, cheight * c_y_scale * pitch);
  _clean_invalidate_dcache();/*
       _clean_cache_area(fb, fb + height * pitch);
   memmovequick(fb, fb + cheight * pitch, (height - cheight) * pitch);
   memsetquick(fb + (height - cheight) * pitch, 0, cheight * pitch);*/
#endif

//RPI_SetGpioLo(TEST_PIN);
}

static void fb_clear() {
   // initialize the cursor positions, origin, colours, etc.
   // TODO: clearing all of these may not be strictly correct
   fb_init_variables();
   // clear frame buffer *** bug here memset value is a byte so 32bpp doesn't work
   memsetquick((void *)fb, get_colour(c_bg_col), height * pitch);
}

int calc_radius(int x1, int y1, int x2, int y2) {
   return (int)(sqrt((x2-x1)*(x2-x1)+(y2-y1)*(y2-y1)) + 0.5);
}

static void fb_cursor_left() {
   if (c_x_pos != 0) {
      c_x_pos-= bpp * c_x_scale;
   } else {
      c_x_pos = c_x_max * bpp * c_x_scale;
   }
}

static void fb_cursor_right() {
   if (c_x_pos < c_x_max * bpp * c_x_scale) {
      c_x_pos+= bpp * c_x_scale;
   } else {
      c_x_pos = 0;
   }
}

static void fb_cursor_up() {
   if (c_y_pos != 0) {
      c_y_pos-= c_y_scale * cheight;
   } else {
      c_y_pos = c_y_max * c_y_scale * cheight;
   }
}

static void fb_cursor_down() {
   if (c_y_pos < c_y_max* c_y_scale * cheight) {
      c_y_pos+= c_y_scale * cheight;
   } else {
      fb_scroll();
   }
}

static void fb_cursor_col0() {
   c_x_pos = 0;
}

static void fb_cursor_home() {
   c_x_pos = 0;
   c_y_pos = 0;
}

static void fb_cursor_next() {
   if (c_x_pos < c_x_max * bpp * c_x_scale) {
      c_x_pos+= bpp * c_x_scale;
   } else {
      c_x_pos = 0;
      fb_cursor_down();
   }
}

// Implementation of Bresenham's line drawing algorithm from here:
// http://tech-algorithm.com/articles/drawing-line-using-bresenham-algorithm/
static void fb_draw_line(int x,int y,int x2, int y2, unsigned int color) {
   int i;
   int w = x2 - x;
   int h = y2 - y;
   int dx1 = 0, dy1 = 0, dx2 = 0, dy2 = 0;
   if (w < 0) dx1 = -1 ; else if (w > 0) dx1 = 1;
   if (h < 0) dy1 = -1 ; else if (h > 0) dy1 = 1;
   if (w < 0) dx2 = -1 ; else if (w > 0) dx2 = 1;
   int longest = abs(w);
   int shortest = abs(h);
   if (!(longest > shortest)) {
      longest = abs(h);
      shortest = abs(w);
      if (h < 0) dy2 = -1 ; else if (h > 0) dy2 = 1;
      dx2 = 0;
   }
   int numerator = longest >> 1 ;
   for (i = 0; i <= longest; i++) {
      fb_putpixel(x, y, color);
      numerator += shortest;
      if (!(numerator < longest)) {
         numerator -= longest;
         x += dx1;
         y += dy1;
      } else {
         x += dx2;
         y += dy2;
      }
   }
}

static void fb_fill_triangle(int x, int y, int x2, int y2, int x3, int y3, unsigned int colour) {
   /*
   x = ((x + g_x_origin) * SCREEN_WIDTH)  / BBC_X_RESOLUTION;
   y = ((y + g_y_origin) * SCREEN_HEIGHT) / BBC_Y_RESOLUTION;
   x2 = ((x2 + g_x_origin) * SCREEN_WIDTH)  / BBC_X_RESOLUTION;
   y2 = ((y2 + g_y_origin) * SCREEN_HEIGHT) / BBC_Y_RESOLUTION;
   x3 = ((x3 + g_x_origin) * SCREEN_WIDTH)  / BBC_X_RESOLUTION;
   y3 = ((y3 + g_y_origin) * SCREEN_HEIGHT) / BBC_Y_RESOLUTION;
   // Flip y axis
   y = SCREEN_HEIGHT - 1 - y;
   y2 = SCREEN_HEIGHT - 1 - y2;
   y3 = SCREEN_HEIGHT - 1 - y3;
   colour = get_colour(colour);
 //  v3d_draw_triangle(x, y, x2, y2, x3, y3, colour);
 */
}


void fb_draw_triangle(int x1, int y1, int x2, int y2, int x3, int y3, unsigned int colour) {
   fb_draw_line(x1, y1, x2, y2, colour);
   fb_draw_line(x2, y2, x3, y3, colour);
   fb_draw_line(x3, y3, x1, y1, colour);
}

void fb_draw_circle(int xc, int yc, int r, unsigned int colour) {
   int x=0;
   int y=r;
   int p=3-(2*r);

   fb_putpixel(xc+x,yc-y,colour);

   for(x=0;x<=y;x++)
      {
         if (p<0)
            {
               p+=4*x+6;
            }
         else
            {
               y--;
               p+=4*(x-y)+10;
            }

         fb_putpixel(xc+x,yc-y,colour);
         fb_putpixel(xc-x,yc-y,colour);
         fb_putpixel(xc+x,yc+y,colour);
         fb_putpixel(xc-x,yc+y,colour);
         fb_putpixel(xc+y,yc-x,colour);
         fb_putpixel(xc-y,yc-x,colour);
         fb_putpixel(xc+y,yc+x,colour);
         fb_putpixel(xc-y,yc+x,colour);
      }
}

void fb_fill_circle(int xc, int yc, int r, unsigned int colour) {
   int x=0;
   int y=r;
   int p=3-(2*r);

   fb_putpixel(xc+x,yc-y,colour);

   for(x=0;x<=y;x++)
      {
         if (p<0)
            {
               p+=4*x+6;
            }
         else
            {
               y--;
               p+=4*(x-y)+10;
            }

         fb_draw_line(xc+x,yc-y,xc-x,yc-y,colour);
         fb_draw_line(xc+x,yc+y,xc-x,yc+y,colour);
         fb_draw_line(xc+y,yc-x,xc-y,yc-x,colour);
         fb_draw_line(xc+y,yc+x,xc-y,yc+x,colour);
      }
}
/* Bad rectangle due to triangle bug
   void fb_fill_rectangle(int x1, int y1, int x2, int y2, unsigned int colour) {
   fb_fill_triangle(x1, y1, x1, y2, x2, y2, colour);
   fb_fill_triangle(x1, y1, x2, y1, x2, y2, colour);
   }
*/

void fb_fill_rectangle(int x1, int y1, int x2, int y2, unsigned int colour) {
   int y;
   for (y = y1; y <= y2; y++) {
      fb_draw_line(x1, y, x2, y, colour);
   }
}

void fb_draw_rectangle(int x1, int y1, int x2, int y2, unsigned int colour) {
   fb_draw_line(x1, y1, x2, y1, colour);
   fb_draw_line(x2, y1, x2, y2, colour);
   fb_draw_line(x2, y2, x1, y2, colour);
   fb_draw_line(x1, y2, x1, y1, colour);
}

void fb_fill_parallelogram(int x1, int y1, int x2, int y2, int x3, int y3, unsigned int colour) {
   int x4 = x3 - x2 + x1;
   int y4 = y3 - y2 + y1;
   fb_fill_triangle(x1, y1, x2, y2, x3, y3, colour);
   fb_fill_triangle(x1, y1, x4, y4, x3, y3, colour);
}

void fb_draw_parallelogram(int x1, int y1, int x2, int y2, int x3, int y3, unsigned int colour) {
   int x4 = x3 - x2 + x1;
   int y4 = y3 - y2 + y1;
   fb_draw_line(x1, y1, x2, y2, colour);
   fb_draw_line(x2, y2, x3, y3, colour);
   fb_draw_line(x3, y3, x4, y4, colour);
   fb_draw_line(x4, y4, x1, y1, colour);
}

void fb_draw_ellipse(int xc, int yc, int width, int height, unsigned int colour) {
   int a2 = width * width;
   int b2 = height * height;
   int fa2 = 4 * a2, fb2 = 4 * b2;
   int x, y, sigma;

   /* first half */
   for (x = 0, y = height, sigma = 2*b2+a2*(1-2*height); b2*x <= a2*y; x++)
      {
         fb_putpixel(xc + x, yc + y, colour);
         fb_putpixel(xc - x, yc + y, colour);
         fb_putpixel(xc + x, yc - y, colour);
         fb_putpixel(xc - x, yc - y, colour);
         if (sigma >= 0)
            {
               sigma += fa2 * (1 - y);
               y--;
            }
         sigma += b2 * ((4 * x) + 6);
      }

   /* second half */
   for (x = width, y = 0, sigma = 2*a2+b2*(1-2*width); a2*y <= b2*x; y++)
      {
         fb_putpixel(xc + x, yc + y, colour);
         fb_putpixel(xc - x, yc + y, colour);
         fb_putpixel(xc + x, yc - y, colour);
         fb_putpixel(xc - x, yc - y, colour);
         if (sigma >= 0)
            {
               sigma += fb2 * (1 - x);
               x--;
            }
         sigma += a2 * ((4 * y) + 6);
      }
}

void fb_fill_ellipse(int xc, int yc, int width, int height, unsigned int colour) {
   int a2 = width * width;
   int b2 = height * height;
   int fa2 = 4 * a2, fb2 = 4 * b2;
   int x, y, sigma;

   /* first half */
   for (x = 0, y = height, sigma = 2*b2+a2*(1-2*height); b2*x <= a2*y; x++)
      {
         fb_draw_line(xc + x, yc + y, xc - x, yc + y, colour);
         fb_draw_line(xc + x, yc - y, xc - x, yc - y, colour);
         if (sigma >= 0)
            {
               sigma += fa2 * (1 - y);
               y--;
            }
         sigma += b2 * ((4 * x) + 6);
      }

   /* second half */
   for (x = width, y = 0, sigma = 2*a2+b2*(1-2*width); a2*y <= b2*x; y++)
      {
         fb_draw_line(xc + x, yc + y, xc - x, yc + y, colour);
         fb_draw_line(xc + x, yc - y, xc - x, yc - y, colour);
         if (sigma >= 0)
            {
               sigma += fb2 * (1 - x);
               x--;
            }
         sigma += a2 * ((4 * y) + 6);
      }
}

void fb_fill_area(int x, int y, unsigned int colour, unsigned int mode) {
   /*   Modes:
    * HL_LR_NB: horizontal line fill (left & right) to non-background - done
    * HL_RO_BG: Horizontal line fill (right only) to background - done
    * HL_LR_FG: Horizontal line fill (left & right) to foreground
    * HL_RO_NF: Horizontal line fill (right only) to non-foreground - done
    * AF_NONBG: Flood (area fill) to non-background
    * AF_TOFGD: Flood (area fill) to foreground
    */

   int save_x = x;
   int save_y = y;
   int x_left = x;
   int y_left = y;
   int x_right = x;
   int y_right = y;
   int real_y;
   unsigned int stop = 0;

   // printf("Plot (%d,%d), colour %d, mode %d\r\n", x, y, colour, mode);
   // printf("g_bg_col = %d, g_fg_col = %d\n\r", g_bg_col, g_fg_col);

   switch(mode) {
   case HL_LR_NB:
      while (! stop) {
         if (fb_getpixel(x_right,y) == get_colour(g_bg_col) && x_right <= BBC_X_RESOLUTION) {
            x_right += BBC_X_RESOLUTION/SCREEN_WIDTH;   // speeds up but might fail if not integer
         } else {
            stop = 1;
         }
      }
      stop = 0;
      //x = save_x - 1;
      while (! stop) {
         if (fb_getpixel(x_left,y) == get_colour(g_bg_col) && x_left >= 0) {
            x_left -= BBC_X_RESOLUTION/SCREEN_WIDTH;    // speeds up but might fail if not integer
         } else {
            stop = 1;
         }
      }
      break;

   case HL_RO_BG:
      while (! stop) {
         if (fb_getpixel(x_right,y) != get_colour(g_bg_col) && x_right <= BBC_X_RESOLUTION) {
            x_right += BBC_X_RESOLUTION/SCREEN_WIDTH;   // speeds up but might fail if not integer
         } else {
            stop = 1;
         }
      }
      break;

   case HL_LR_FG:
      while (! stop) {
         if (fb_getpixel(x_right,y) != get_colour(g_fg_col) && x_right <= BBC_X_RESOLUTION) {
            x_right += BBC_X_RESOLUTION/SCREEN_WIDTH;   // speeds up but might fail if not integer
         } else {
            stop = 1;
         }
      }
      stop = 0;
      //x = save_x - 1;
      while (! stop) {
         if (fb_getpixel(x_left,y) != get_colour(g_fg_col) && x_left >= 0) {
            x_left -= BBC_X_RESOLUTION/SCREEN_WIDTH;    // speeds up but might fail if not integer
         } else {
            stop = 1;
         }
      }
      break;


   case HL_RO_NF:
      while (! stop) {
         if (fb_getpixel(x_right,y) != get_colour(g_fg_col) && x_right <= BBC_X_RESOLUTION) {
            x_right += BBC_X_RESOLUTION/SCREEN_WIDTH;   // speeds up but might fail if not integer
         } else {
            stop = 1;
         }
      }
      break;

   case AF_NONBG:
      // going up
      while (! stop) {
         if (fb_getpixel(x,y) == get_colour(g_bg_col) && y <= BBC_Y_RESOLUTION) {
            fb_fill_area(x, y, colour, HL_LR_NB);
            // As the BBC_Y_RESOLUTION is not a multiple of SCREEN_HEIGHT we have to increment
            // y until the physical y-coordinate increases. If we don't do that and simply increment
            // y by 2 then at some point the physical y-coordinate is the same as the previous drawn
            // line and the floodings stops. This also speeds up drawing because each physical line
            // is only drawn once.
            real_y = ((y + g_y_origin) * SCREEN_HEIGHT) / BBC_Y_RESOLUTION;
            while(((y + g_y_origin) * SCREEN_HEIGHT) / BBC_Y_RESOLUTION == real_y) {
               y++;
            }
            x = (g_x_pos_last1 + g_x_pos_last2) / 2;
         } else {
            stop = 1;
         }
      }
      // going down
      stop = 0;
      x = save_x;
      y = save_y - BBC_Y_RESOLUTION/SCREEN_HEIGHT;
      while (! stop) {
         if (fb_getpixel(x,y) == get_colour(g_bg_col) && y >= 0) {
            fb_fill_area(x, y, colour, HL_LR_NB);
            real_y = ((y + g_y_origin) * SCREEN_HEIGHT) / BBC_Y_RESOLUTION;
            while(((y + g_y_origin) * SCREEN_HEIGHT) / BBC_Y_RESOLUTION == real_y) {
               y--;
            }
            x = (g_x_pos_last1 + g_x_pos_last2) / 2;
         } else {
            stop = 1;
         }
      }
      colour = -1;  // prevents any additional line drawing
      break;

   case AF_TOFGD:
      // going up
      while (! stop) {
         if (fb_getpixel(x,y) != get_colour(g_fg_col) && y <= BBC_Y_RESOLUTION) {
            fb_fill_area(x, y, colour, HL_LR_FG);
            real_y = ((y + g_y_origin) * SCREEN_HEIGHT) / BBC_Y_RESOLUTION;
            while(((y + g_y_origin) * SCREEN_HEIGHT) / BBC_Y_RESOLUTION == real_y) {
               y++;
            }
            x = (g_x_pos_last1 + g_x_pos_last2) / 2;
         } else {
            stop = 1;
         }
      }
      // going down
      stop = 0;
      x = save_x;
      y = save_y - BBC_Y_RESOLUTION/SCREEN_HEIGHT;
      while (! stop) {
         if (fb_getpixel(x,y) != get_colour(g_fg_col) && y >= 0) {
            fb_fill_area(x, y, colour, HL_LR_NB);
            real_y = ((y + g_y_origin) * SCREEN_HEIGHT) / BBC_Y_RESOLUTION;
            while(((y + g_y_origin) * SCREEN_HEIGHT) / BBC_Y_RESOLUTION == real_y) {
               y--;
            }
            x = (g_x_pos_last1 + g_x_pos_last2) / 2;
         } else {
            stop = 1;
         }
      }
      colour = -1;  // prevents any additional line drawing
      break;

   default:
      printf( "Unknown fill mode %u\r\n", mode);
      break;
   }

      fb_draw_line(x_left, y_left, x_right, y_right, colour);
      g_x_pos_last1 = x_left;
      g_y_pos_last1 = y_left;
      g_x_pos_last2 = x_right;
      g_y_pos_last2 = y_right;
}
static uint32_t fb_get_address() {
   return (uint32_t) fb;
}

static uint32_t fb_get_width() {
   return width;
}

static uint32_t fb_get_height() {
   return height;
}

static uint32_t fb_get_bpp() {
   return bpp;
}

// Fill modes:
#define HL_LR_NB 1 // Horizontal line fill (left & right) to non-background
#define HL_RO_BG 2 // Horizontal line fill (right only) to background
#define HL_LR_FG 3 // Horizontal line fill (left & right) to foreground
#define HL_RO_NF 4 // Horizontal line fill (right only) to non-foreground
#define AF_NONBG 5 // Flood (area fill) to non-background
#define AF_TOFGD 6 // Flood (area fill) to foreground

#define NORMAL    0
#define IN_VDU4   4
#define IN_VDU5   5
#define IN_VDU17  17
#define IN_VDU18  18
#define IN_VDU19  19
#define IN_VDU22  22
#define IN_VDU23  23
#define IN_VDU25  25
#define IN_VDU29  29
#define IN_VDU31  31

static void update_g_cursors(int16_t x, int16_t y) {
   g_x_pos_last2 = g_x_pos_last1;
   g_x_pos_last1 = g_x_pos;
   g_x_pos       = x;
   g_y_pos_last2 = g_y_pos_last1;
   g_y_pos_last1 = g_y_pos;
   g_y_pos       = y;
}

static void fb_draw_character(int c, int invert, int eor) {
#ifdef BPP4
   unsigned char temp=0;
#endif

   uint32_t ch = c * cheight;

   if(invert) invert = 0xFF ;
   PIXEL_T c_fgol = get_colour(c_fg_col);
   PIXEL_T c_bgol = get_colour(c_bg_col);

   // Copy the character into the frame buffer
   for (uint32_t i = 0; i < cheight; i++) {
      for (uint32_t scaley = c_y_scale; scaley != 0; scaley-- )
      {
      int data;
      if ( c == CLEAR_CHAR)
         data = 0;
      else
         if (mode7flag)
            data = MODE7Font[ch+i]<<2;
         else
            data = BBCFont[ch + i] ^ invert;

      PIXEL_T *fbptr = fb + c_x_pos + (c_y_pos + i*c_y_scale + scaley) * pitch;

      for (uint32_t j = 0; j < 8; j++) {
         PIXEL_T col = (data & 0x80) ? c_fgol : c_bgol;
         for(uint32_t scalex=c_x_scale; scalex !=0; scalex--)
         {
            if (eor) {
               *fbptr++ ^= col;
            } else {
               *fbptr++ = col;
            }
#ifdef BPP4
         if (eor) {
            *fbptr ^= col; // FIX 4 bit EOR
         } else {
           if (j & 1)
           {
             *(uint8_t *)(fb + c_x_pos +(0+(j>>1)) + (c_y_pos + i) * pitch) = (temp<<4) | col;
             fbptr += 1;
           } else
           {
             temp = col;
           }
         }
#endif
         }
         data <<= 1;
         }
      }
   }
}


void init_colour_table() {
   // Colour  0 = Black
   // Colour  1 = Dark Red
   // Colour  2 = Dark Green
   // Colour  3 = Dark Yellow
   // Colour  4 = Dark Blue
   // Colour  5 = Dark Magenta
   // Colour  6 = Dark Cyan
   // Colour  7 = Dark White
   // Colour  8 = Dark Black
   // Colour  9 = Red
   // Colour 10 = Green
   // Colour 11 = Yellow
   // Colour 12 = Blue
   // Colour 13 = Magenta
   // Colour 14 = Cyan
   // Colour 15 = White
   // Colour 16-255 Black
   for (int i = 0; i < 16; i++) {
      int intensity = (i & 8) ? 255 : 127;
      int b = (i & 4) ? intensity : 0;
      int g = (i & 2) ? intensity : 0;
      int r = (i & 1) ? intensity : 0;
      if (i == 8) {
         r = g = b = 63;
      }
      set_colour(i, r, g, b);
   }
   for (int i = 16; i < 256; i++) {
      set_colour(i, 0, 0, 0);
   }
}

int max(int a, int b)
{
   if (a>b)
      return a;
   else
      return b;
}
static void fb_writec(uint8_t c) {

//   static uint8_t g_plotmode; // not currently used
   static int state = NORMAL;
   static int count = 0;
   static int16_t x_tmp;
   static int16_t y_tmp;

   static int l; // logical colour
   static int p; // physical colour
   static int r;
   static int g;
   static int b;
   static uint32_t temp;

   switch (state)
   {
   case IN_VDU17 :
   {
      state = NORMAL;
      if (c & 128) {
         c_bg_col = c & 15;
#ifdef DEBUG_VDU
         printf("bg = %u\r\n", c_bg_col);
#endif
      } else {
         c_fg_col = c & 15;
#ifdef DEBUG_VDU
         printf("fg = %u\r\n", c_fg_col);
#endif
      }
      return;

   }
   case IN_VDU18 :
   {
      switch (count) {
      case 0:
//         g_plotmode = c;
         break;
      case 1:
         if (c & 128) {
            g_bg_col = c & 15;
         } else {
            g_fg_col = c & 15;
         }
         break;
      }
      count++;
      if (count == 2) {
         state = NORMAL;
      }
      return;

   }
   case IN_VDU19:
   {
      switch (count) {
      case 0:
         l = c;
         break;
      case 1:
         p = c;
         break;
      case 2:
         r = c;
         break;
      case 3:
         g = c;
         break;
      case 4:
         b = c;
         if (p == 255) {
            init_colour_table();
            update_palette(l, NUM_COLOURS);
         } else {
            // See http://beebwiki.mdfs.net/VDU_19
            if ((p < 16) && (r==0) && (g==0) && (b==0 )) {
               int i = (p & 8) ? 255 : 127;
               b = (p & 4) ? i : 0;
               g = (p & 2) ? i : 0;
               r = (p & 1) ? i : 0;
            }
            set_colour(l, r, g, b);
            update_palette(l, 1);
         }
         state = NORMAL;
         break;
      }
      count++;
      return;
   }

   case IN_VDU22:
      { // mode change
         fb_clear();
         vdu45flag = 0;
         init_colour_table();
         update_palette(l, NUM_COLOURS);
         mode7flag = 0;
         cheight = 8;
         switch (c)
         {
         case 0: c_x_max = 79; c_x_scale = 1; c_y_max = 31; break;
         case 1: c_x_max = 39; c_x_scale = 2; c_y_max = 31; break;
         case 2: c_x_max = 19; c_x_scale = 4; c_y_max = 31; break;
         case 3: c_x_max = 79; c_x_scale = 1; c_y_max = 24; break;
         case 4: c_x_max = 39; c_x_scale = 2; c_y_max = 31; break;
         case 5: c_x_max = 19; c_x_scale = 4; c_y_max = 31; break;
         case 6: c_x_max = 39; c_x_scale = 2; c_y_max = 24; break;
         case 7: c_x_max = 39; c_x_scale = 2; c_y_max = 24;
               mode7flag = 1; cheight=10;
         break;
         }
         state = NORMAL;
         return;
      }

    case IN_VDU23:
    { // redefine character
      if (count == 0 )
         temp = c -32;
      else
         BBCFont[temp*cheight+(count -1)] = c;
      count++;
      if (count==9)
      {
         state = NORMAL;
      }
      return;
    }


   case IN_VDU25:
   {
      static uint8_t g_mode;
      switch (count) {
      case 0:
         g_mode = c;
         break;
      case 1:
         x_tmp = c;
         break;
      case 2:
         x_tmp |= c << 8;
         break;
      case 3:
         y_tmp = c;
         break;
      case 4:
         y_tmp |= c << 8;
#ifdef DEBUG_VDU
         printf("plot %d %d %d\r\n", g_mode, x_tmp, y_tmp);
#endif

         if (g_mode & 4) {
            // Absolute position X, Y.
            update_g_cursors(x_tmp, y_tmp);
         } else {
            // Relative to the last point.
            update_g_cursors(g_x_pos + x_tmp, g_y_pos + y_tmp);
         }

         int col;
         switch (g_mode & 3) {
         case 0:
            col = -1;
            break;
         case 1:
            col = g_fg_col;
            break;
         case 2:
            col = 15 - g_fg_col;
            break;
         case 3:
            col = g_bg_col;
            break;
         }

         if (col >= 0) {
            if (g_mode < 32) {
               // Draw a line
               fb_draw_line(g_x_pos_last1, g_y_pos_last1, g_x_pos, g_y_pos, col);
            } else if (g_mode >= 64 && g_mode < 72) {
               // Plot a single pixel
               fb_putpixel(g_x_pos, g_y_pos, g_fg_col);
            } else if (g_mode >= 72 && g_mode < 80) {
               // Horizontal line fill (left and right) to non background
               fb_fill_area(g_x_pos, g_y_pos, col, HL_LR_NB);
            } else if (g_mode >= 80 && g_mode < 88) {
               // Fill a triangle
               fb_fill_triangle(g_x_pos_last2, g_y_pos_last2, g_x_pos_last1, g_y_pos_last1, g_x_pos, g_y_pos, col);
            } else if (g_mode >= 88 && g_mode < 96) {
               // Horizontal line fill (right only) to background
               fb_fill_area(g_x_pos, g_y_pos, col, HL_RO_BG);
            } else if (g_mode >= 96 && g_mode < 104) {
               // Fill a rectangle
               fb_fill_rectangle(g_x_pos_last1, g_y_pos_last1, g_x_pos, g_y_pos, col);
            } else if (g_mode >= 104 && g_mode < 112) {
               // Horizontal line fill (left and right) to foreground
               fb_fill_area(g_x_pos, g_y_pos, col, HL_LR_FG);
            } else if (g_mode >= 112 && g_mode < 120) {
               // Fill a parallelogram
               fb_fill_parallelogram(g_x_pos_last2, g_y_pos_last2, g_x_pos_last1, g_y_pos_last1, g_x_pos, g_y_pos, col);
            } else if (g_mode >= 120 && g_mode < 128) {
               // Horizontal line fill (right only) to non-foreground
               fb_fill_area(g_x_pos, g_y_pos, col, HL_RO_NF);
            } else if (g_mode >= 128 && g_mode < 136) {
               // Flood fill to non-background
               fb_fill_area(g_x_pos, g_y_pos, col, AF_NONBG);
            } else if (g_mode >= 136 && g_mode < 144) {
               // Flood fill to non-foreground
               fb_fill_area(g_x_pos, g_y_pos, col, AF_TOFGD);
            } else if (g_mode >= 144 && g_mode < 152) {
               // Draw a circle outline
               int radius = calc_radius(g_x_pos_last1, g_y_pos_last1, g_x_pos, g_y_pos);
               fb_draw_circle(g_x_pos_last1, g_y_pos_last1, radius, col);
            } else if (g_mode >= 152 && g_mode < 160) {
               // Fill a circle
               int radius = calc_radius(g_x_pos_last1, g_y_pos_last1, g_x_pos, g_y_pos);
               fb_fill_circle(g_x_pos_last1, g_y_pos_last1, radius, col);
            } else if (g_mode >= 160 && g_mode < 168) {
               // Draw a rectangle outline
               fb_draw_rectangle(g_x_pos, g_y_pos, g_x_pos_last1, g_y_pos_last1, col);
            } else if (g_mode >= 168 && g_mode < 176) {
               // Draw a parallelogram outline
               fb_draw_parallelogram(g_x_pos, g_y_pos, g_x_pos_last1, g_y_pos_last1, g_x_pos_last2, g_y_pos_last2, col);
            } else if (g_mode >= 176 && g_mode < 184) {
               // Draw a triangle outline
               fb_draw_triangle(g_x_pos, g_y_pos, g_x_pos_last1, g_y_pos_last1, g_x_pos_last2, g_y_pos_last2, col);
            } else if (g_mode >= 192 && g_mode < 200) {
               // Draw an ellipse
               fb_draw_ellipse(g_x_pos_last2, g_y_pos_last2, max(/*abs(g_x_pos_last1 - g_x_pos_last2)*/0, abs(g_x_pos - g_x_pos_last2) ), max(abs(g_y_pos - g_y_pos_last2),abs(g_y_pos_last1 - g_y_pos_last2)), col);
            } else if (g_mode >= 200 && g_mode < 208) {
               // Fill a n ellipse
               fb_fill_ellipse(g_x_pos_last2, g_y_pos_last2, abs(g_x_pos_last1 - g_x_pos_last2), abs(g_y_pos - g_y_pos_last2), col);
            }
         }
      }
      count++;
      if (count == 5) {
         state = NORMAL;
      }
      return;

   }
   case IN_VDU29:
   {
      switch (count) {
      case 0:
         x_tmp = c;
         break;
      case 1:
         x_tmp |= c << 8;
         break;
      case 2:
         y_tmp = c;
         break;
      case 3:
         y_tmp |= c << 8;
         g_x_origin = x_tmp;
         g_y_origin = y_tmp;
#ifdef DEBUG_VDU
         printf("graphics origin %d %d\r\n", g_x_origin, g_y_origin);
#endif
      }
      count++;
      if (count == 4) {
         state = NORMAL;
      }
      return;
   }

   case IN_VDU31 :
   {
      switch (count) {
      case 0:
         x_tmp = c;
         break;
      case 1:
         fb_draw_character(CURSOR, 0, 1);
         y_tmp = c;
         c_x_pos = x_tmp * bpp * c_x_scale;
         c_y_pos = y_tmp * c_y_scale * cheight;

#ifdef DEBUG_VDU
         printf("cursor move to %d %d\r\n", x_tmp, y_tmp);
#endif
      }
      count++;
      if (count == 2) {
         state = NORMAL;
      }
      return;
   }
   }
   switch(c) {

   case 4:
      if (vdu45flag)
      {
         c_x_pos = old_c_x_pos;
         c_y_pos = old_c_y_pos;
         vdu45flag = 0;
      }
      return;

   case 5:
      if (vdu45flag == 0 )
      {
         fb_draw_character(CURSOR, 0, 1);
         old_c_x_pos = c_x_pos;
         old_c_y_pos = c_y_pos;
      }
         c_x_pos = ((g_x_pos + g_x_origin)*SCREEN_WIDTH)/BBC_X_RESOLUTION;
         c_y_pos = ((BBC_Y_RESOLUTION - (g_y_pos + g_y_origin))*SCREEN_HEIGHT)/BBC_Y_RESOLUTION ;
         vdu45flag = 1;
         // **** Todo **** cursor should be off here
      return;

   case 8:
      fb_draw_character(CURSOR, 0, 1);
      fb_cursor_left();
      fb_draw_character(CURSOR, 0, 0);
      break;

   case 9:
      fb_draw_character(CURSOR, 0, 1);
      fb_cursor_right();
      fb_draw_character(CURSOR, 0, 1);
      break;

   case 10:
      fb_draw_character(CURSOR, 0, 1);
      fb_cursor_down();
      fb_draw_character(CURSOR, 0, 1);
      break;

   case 11:
      fb_draw_character(CURSOR, 0, 1);
      fb_cursor_up();
      fb_draw_character(CURSOR, 0, 1);
      break;

   case 12:
      fb_clear();
      fb_draw_character(CURSOR, 0, 1);
      break;

   case 13:
      fb_draw_character(CURSOR, 0, 1);
      fb_cursor_col0();
      fb_draw_character(CURSOR, 0, 1);
      break;

   case 16:
      fb_clear();
      break;

   case 17:
      state = IN_VDU17;
      count = 0;
      return;

   case 18:
      state = IN_VDU18;
      count = 0;
      return;

   case 19:
      state = IN_VDU19;
      count = 0;
      return;

   case 22:
      state = IN_VDU22;
      count = 0;
      return;

   case 23:
      state = IN_VDU23;
      count = 0;
      return;

   case 25:
      state = IN_VDU25;
      count = 0;
      return;

   case 29:
      state = IN_VDU29;
      count = 0;
      return;

   case 30:
      fb_draw_character(CURSOR, 0, 1);
      fb_cursor_home();
      fb_draw_character(CURSOR, 0, 1);
      break;
   case 31 :
      state = IN_VDU31;
      count = 0 ;
      return;

   case 127:
      fb_draw_character(CLEAR_CHAR, 0, 0);
      fb_cursor_left();
      fb_draw_character(CURSOR, 0, 0);
      break;

   default:

      if (c < 0x20)
         return;

      // Erase the cursor
      //fb_draw_character(CURSOR, 1, 1);

      // Draw the next character
      fb_draw_character(c-32, 0, 0);

      // Advance the drawing position
      fb_cursor_next();

      // Draw the cursor
      fb_draw_character(CURSOR, 0, 1);
   }
}

static void fb_writes(char *string) {
   while (*string) {
      fb_writec(*string++);
   }
}

static void fb_initialize() {

    rpi_mailbox_property_t *mp;

    /* Initialise a framebuffer... */
    RPI_PropertyStart(TAG_ALLOCATE_BUFFER, 2); RPI_PropertyAddTwoWords(64,0);
    RPI_PropertyNewTag(TAG_SET_PHYSICAL_SIZE,2); RPI_PropertyAddTwoWords(SCREEN_WIDTH, SCREEN_HEIGHT );
    RPI_PropertyNewTag(TAG_SET_VIRTUAL_SIZE,2); RPI_PropertyAddTwoWords(SCREEN_WIDTH, SCREEN_HEIGHT *2 );
    RPI_PropertyNewTag(TAG_SET_DEPTH,1); RPI_PropertyAdd(SCREEN_DEPTH);
    RPI_PropertyProcess(true);

    if( ( mp = RPI_PropertyGet( TAG_ALLOCATE_BUFFER  ) ) )
    {
        fb = (unsigned char*)(mp->data.buffer_32[0] &0x1FFFFFFF);
        fbcache = fb + 0x80000000;
#ifdef DEBUG_FB
        printf( "Framebuffer address: %8.8X\r\n", (unsigned int)fb );
#endif
    }

    if( ( mp = RPI_PropertyGetWord( TAG_GET_PHYSICAL_SIZE , 0) ) )
    {
        width = mp->data.buffer_32[0];
        height = mp->data.buffer_32[1];
#ifdef DEBUG_FB
        printf( "Initialised Framebuffer: %lux%lu ", width, height );
#endif
    }

    if( ( mp = RPI_PropertyGetWord( TAG_GET_DEPTH , 0) ) )
    {
        bpp = mp->data.buffer_32[0];
#ifdef DEBUG_FB
        printf( "%dbpp\r\n", bpp );
#endif
    }

    if( ( mp = RPI_PropertyGetWord( TAG_GET_PITCH , 0) ) )
    {
        pitch = mp->data.buffer_32[0];
#ifdef DEBUG_FB
        printf( "Pitch: %d bytes\r\n", pitch );
#endif
    }

    // On the Pi 2/3 the mailbox returns the address with bits 31..30 set, which is wrong
  //  fb = (unsigned char *)(((unsigned int) fb) & 0x3fffffff);

    //update_palette();

    // Change bpp from bits to bytes
    //bpp >>= 3;

    /* Copy default colour table */
    init_colour_table();

    /* Update the palette (only in 8-bpp modes) */
    update_palette(0, NUM_COLOURS);

    mode_init();

    fb_clear();
    fb_draw_character(CURSOR, 0, 1);

    fb_writes("\r\n\r\nAcorn MOS\r\n\r\nPi1MHz\r\n\r\nBASIC\r\n\r\n>");

#ifdef BPP32
       for (int y = 0; y < 16; y++) {
          PIXEL_T *fbptr = (PIXEL_T *) (fb + pitch * y);
          for (PIXEL_T col = 23; col >= 0; col--) {
             for (int x = 0; x < 8; x++) {
                *fbptr++ = col;
             }
          }
       }
#endif
#ifdef BPP16
       for (int y = 0; y < 16; y++) {
          PIXEL_T *fbptr = (PIXEL_T *) (fb + pitch * y);
          for (PIXEL_T col = 15; col >= 0; col--) {
             for (int x = 0; x < 8; x++) {
                *fbptr++ = col;
             }
          }
       }
#endif
#ifdef BPP8
       for (int y = 0; y < 16; y++) {
          PIXEL_T *fbptr = (PIXEL_T *) (fb + pitch * y);
          for (PIXEL_T col = 0; col <= 15; col++) {
             for (int x = 0; x < 8; x++) {
                *fbptr++ = col;
             }
          }
       }
#endif

}

#define QSIZE 4096

volatile int wp = 0;
static int rp = 0;

NOINIT_SECTION static unsigned char vdu_queue[QSIZE];

static void fb_emulator_vdu(unsigned int gpio)
{
   vdu_queue[wp] = GET_DATA(gpio);
   wp = (wp + 1) & (QSIZE - 1);
}

static void fb_emulator_poll()
{
   if (rp != wp) {
//      printf("char %x,\r\n",vdu_queue[rp]);
      fb_writec(vdu_queue[rp]);
      rp = (rp + 1) & (QSIZE - 1);
   }
}

void fb_emulator_ram(unsigned int gpio)
{
   uint8_t  data = GET_DATA(gpio);
   uint32_t addr = GET_ADDR(gpio);
   Pi1MHz_MemoryWrite(addr, data);
}

void fb_emulator_init(uint8_t instance, int address)
{
  fb_initialize();

  //v3d_initialize(fb_get_address(), fb_get_width(), fb_get_height(), fb_get_bpp());

  Pi1MHz_Register_Memory(WRITE_FRED, address, fb_emulator_vdu);
 // Create 6 bytes of RAM for vector code
  Pi1MHz_MemoryWrite(address+1, 0x8D);
  Pi1MHz_MemoryWrite(address+2, (uint8_t) address);
  Pi1MHz_MemoryWrite(address+3, 0XFC);
  Pi1MHz_MemoryWrite(address+4, 0x4c);

  Pi1MHz_Register_Memory(WRITE_FRED, address + 5, fb_emulator_ram);
  Pi1MHz_Register_Memory(WRITE_FRED, address + 6, fb_emulator_ram);

  Pi1MHz_Register_Poll(fb_emulator_poll);
}
