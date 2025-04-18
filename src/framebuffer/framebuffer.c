#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>

#include "../Pi1MHz.h"

#include "../scripts/gitversion.h"
#include "../rpi/info.h"
#include "../rpi/armtimer.h"
#include "../rpi/auxuart.h"
#include "../rpi/base.h"
#include "../rpi/interrupts.h"
#include "../rpi/asm-helpers.h"
#include "../rpi/screen.h"
//#include "../tube.h"
//#include "../copro-defs.h"
//#include "../tube-defs.h"
#include "screen_modes.h"
#include "framebuffer.h"
#include "primitives.h"
#include "fonts.h"

// Current screen mode
static screen_mode_t *screen = NULL;

// Current font
static font_t font_normal;
static font_t font_teletext;
static int font_width;
static int font_height;
static int text_height; // of whole screen
static int text_width;  // of whole screen
static int cursor_start;
static int cursor_end;
static int cursor_off;  // controlled by VDU 23,0,10

// Text area clip window
static t_clip_window_t t_window;

#define COLOUR_MASK 0x3f
#define TINT_MASK   0xc0

// Character colour
static pixel_t c_bg_col;
static pixel_t c_fg_col;

static uint8_t c_bg_gcol;
static uint8_t c_bg_tint;
static uint8_t c_fg_gcol;
static uint8_t c_fg_tint;

// Character cursor status and position
static int16_t c_enabled;  // controlled by VDU 23,1
static int16_t c_x_pos;
static int16_t c_y_pos;

// Edit cursor status and position
static int16_t e_enabled;  // controlled by use of the cursor keys
static int16_t e_x_pos;
static int16_t e_y_pos;

// Block cursor position and current visibility
static int16_t b_x_pos;
static int16_t b_y_pos;
static int16_t b_visible;

// Flashing cursor position and current visibility
static int16_t f_x_pos;
static int16_t f_y_pos;
static int16_t f_visible;

// Graphics colour / cursor position
static pixel_t white_col;

static uint8_t g_bg_gcol;
static uint8_t g_bg_tint;
static uint8_t g_fg_gcol;
static uint8_t g_fg_tint;

static int16_t g_x_pos;
static int16_t g_x_pos_last1;
static int16_t g_x_pos_last2;
static int16_t g_y_pos;
static int16_t g_y_pos_last1;
static int16_t g_y_pos_last2;
static int16_t g_x_origin;
static int16_t g_y_origin;

// Sprites
static uint8_t current_sprite;

// Graphics area clip window
static g_clip_window_t g_window;

// Text or graphical cursor for printing characters
static int8_t text_at_g_cursor;

// Vsync flag
static volatile int vsync_flag = 0;

// Colour Flash Rate (in 20ms fields)
static volatile uint8_t flash_mark_time  = 25;
static volatile uint8_t flash_space_time = 25;

// VDU Queue
#define VDU_QSIZE 8192
static volatile unsigned int vdu_wp = 0;
static volatile unsigned int vdu_rp = 0;

NOINIT_SECTION static uint8_t vdu_queue[VDU_QSIZE];

#define VDU_BUF_LEN 16

typedef struct {
   int len;
   void (*handler)(const uint8_t *buf);
} vdu_operation_t;

static void vdu_4(const uint8_t *buf);
static void vdu_5(const uint8_t *buf);
static void vdu_16(const uint8_t *buf);
static void vdu_17(const uint8_t *buf);
static void vdu_18(const uint8_t *buf);
static void vdu_19(const uint8_t *buf);
static void vdu_20(const uint8_t *buf);
static void vdu_22(const uint8_t *buf);
static void vdu_23(const uint8_t *buf);
static void vdu_24(const uint8_t *buf);
static void vdu_25(const uint8_t *buf);
static void vdu_26(const uint8_t *buf);
static void vdu_27(const uint8_t *buf);
static void vdu_28(const uint8_t *buf);
static void vdu_29(const uint8_t *buf);
static void vdu_nop(const uint8_t *buf);
static void vdu_default(const uint8_t *buf);

static vdu_operation_t vdu_operation_table[256] = {
   // Entries 8-13,30,31,127 are filled in by VDU 4/5
   // remaining entries >=32 are filled in by fb_initialize
   { 0, vdu_nop }, // 0 -  Does nothing
   { 1, vdu_nop }, // 1 -  Send next character to printer only (do nothing)
   { 0, vdu_nop }, // 2 -  Enable printer (do nothing)
   { 0, vdu_nop }, // 3 -  Disable printer (do nothing)
   { 0, vdu_4   }, // 4 -  Write text at text cursor
   { 0, vdu_5   }, // 5 -  Write text at graphics cursor
   { 0, vdu_nop }, // 6 -  TODO: Enable VDU drivers
   { 0, vdu_nop }, // 7 -  Make a short beep (do nothing)
   { 0, vdu_nop }, // 8 -  Backspace cursor one character
   { 0, vdu_nop }, // 9 -  Forward space cursor one character
   { 0, vdu_nop }, // 10 - Move cursor down one line
   { 0, vdu_nop }, // 11 - Move cursor up one line
   { 0, vdu_nop }, // 12 - Clear text area
   { 0, vdu_nop }, // 13 - Move cursor to start of current line
   { 0, vdu_nop }, // 14 - Page mode on (do nothing)
   { 0, vdu_nop }, // 15 - Page mode off (do nothing)
   { 0, vdu_16  }, // 16 - Clear graphics area
   { 1, vdu_17  }, // 17 - Define text colour
   { 2, vdu_18  }, // 18 - Define graphics colour
   { 5, vdu_19  }, // 19 - Define logical colour
   { 0, vdu_20  }, // 20 - Reset logical colours to defaults
   { 0, vdu_nop }, // 21 - TODO: Disable VDU drivers or delete current line
   { 1, vdu_22  }, // 22 - Select screen mode
   { 9, vdu_23  }, // 23 - Re-program display character (+many other things)
   { 8, vdu_24  }, // 24 - Define graphics window
   { 5, vdu_25  }, // 25 - PLOT mode,x,y
   { 0, vdu_26  }, // 26 - Restore default windows
   { 1, vdu_27  }, // 27 - Escape next character
   { 4, vdu_28  }, // 28 - Define text window
   { 4, vdu_29  }, // 29 - Define graphics origin
   { 0, vdu_nop }, // 30 - Home text cursor to top left
   { 2, vdu_nop }  // 31 - Move text cursor to x,y
};

// ==========================================================================
// Static methods
// ==========================================================================

static void update_font_size(void);
static void update_text_area(void);
static void init_variables(void);
static void reset_areas(void);
static void set_text_area(const t_clip_window_t *window);
static void invert_cursor(int x_pos, int y_pos, int start, int end);
static void enable_edit_cursor(void);
static void disable_edit_cursor(void);
static void update_cursors(void);
static void cursor_interrupt(void);
static void edit_cursor_up(void);
static void edit_cursor_down(void);
static void edit_cursor_left(void);
static void edit_cursor_right(void);
static void text_area_scroll(scroll_dir_t dir);
static void update_g_cursors(int16_t x, int16_t y);
static void change_mode(screen_mode_t *new_screen);
static void set_graphics_area(const screen_mode_t *scr, const g_clip_window_t *window);
static int read_character(int x_pos, int y_pos);

// These are used in VDU 4 mode
static void text_cursor_left(const uint8_t *buf);
static void text_cursor_right(const uint8_t *buf);
static void text_cursor_up(const uint8_t *buf);
static void text_cursor_down(const uint8_t *buf);
static void text_cursor_col0(const uint8_t *buf);
static void text_cursor_home(const uint8_t *buf);
static void text_cursor_tab(const uint8_t *buf);
static void text_area_clear(const uint8_t *buf);
static void text_delete(const uint8_t *buf);

// These are used in VDU 5 mode
static void graphics_cursor_left(const uint8_t *buf);
static void graphics_cursor_right(const uint8_t *buf);
static void graphics_cursor_up(const uint8_t *buf);
static void graphics_cursor_down(const uint8_t *buf);
static void graphics_cursor_col0(const uint8_t *buf);
static void graphics_cursor_home(const uint8_t *buf);
static void graphics_cursor_tab(const uint8_t *buf);
static void graphics_area_clear(const uint8_t *buf);
static void graphics_delete(const uint8_t *buf);

static void update_font_size(void) {
   // get the current font from the screen
   font_t *font = screen->font;
   // Calculate the font size, taking account of scale and spacing
   font_width  = font->get_overall_w(font);
   font_height = font->get_overall_h(font);
   // Calculate the start/end of the flashing cursor
   // - in MODES 0,1,2,4,5 the cursor is 1px high (rows 7)
   // - in MODES 3,6 the cursor is 3px high (rows 7,8,9)
   int cursor_height = font_height >> 3;
   if (cursor_height < 1) {
      cursor_height = 1;
   }
   if (screen->mode_flags & (F_BBC_GAP | F_GAP)) {
      cursor_height += 2;
   }
   cursor_start = font_height - cursor_height;
   cursor_end = font_height - 1;
   // Calc screen text size
   text_width = screen->width / font_width;
   text_height = screen->height / font_height;
}

static void update_text_area(void) {
   // Make sure font size hasn't changed
   update_font_size();
   // Make sure text area is on the screen
   if (t_window.right >= text_width) {
      t_window.right = (uint8_t)(text_width - 1);
      if (t_window.left > t_window.right) {
         t_window.left = t_window.right;
      }
   }
   if (t_window.bottom >= text_height) {
      t_window.bottom = (uint8_t)(text_height - 1);
      if (t_window.top > t_window.bottom) {
         t_window.top = t_window.bottom;
      }
   }
   // Make sure cursor is in text area
   int16_t tmp_x = c_x_pos;
   int16_t tmp_y = c_y_pos;
   if (tmp_x < t_window.left) {
      tmp_x = t_window.left;
   } else if (tmp_x > t_window.right) {
      tmp_x = t_window.right;
   }
   if (tmp_y < t_window.top) {
      tmp_y = t_window.top;
   } else if (tmp_y > t_window.bottom) {
      tmp_y = t_window.bottom;
   }
   if (c_x_pos != tmp_x || c_y_pos != tmp_y) {
      c_x_pos = tmp_x;
      c_y_pos = tmp_y;
      if (!text_at_g_cursor) {
         update_cursors();
      }
   }
}

// This is used by VDU 17, VDU 18 and VDU 23,17 to calculate a new pixel_t colour
// when ever the VDU colour or tint is altered.
static pixel_t calculate_colour(uint8_t col, uint8_t tint) {
   // Colour masking here is a precaution, it is also done in VDU 17/18
   col &= COLOUR_MASK;
   if (screen->ncolour < 255) {
      // Tint is ignored in 1,2,4 and 16 colour modes
      // Colour masking here is a precaution, it is also done in VDU 17/18
      return col & ((uint8_t)screen->ncolour);
   } else {
      // This should be the only place TINT_MASK is used,
      tint &= TINT_MASK;
      return screen->get_colour(screen, (uint8_t)((col << 2) | (tint >> 6)));
   }
}

static void set_default_colours(void) {

   // Lookup the pixel_t value for white...
   uint8_t white_gcol = screen->white;
   white_col = screen->get_colour(screen, white_gcol);

   // Character colour
   c_bg_col  = 0;                          // Black is always 0
   c_bg_gcol = 0;
   c_bg_tint = 0;
   c_fg_col  = white_col;                  // White depends on the screen bit depth
   c_fg_gcol = white_gcol;
   c_fg_tint = 0xFF;

   // Graphics colour
   g_bg_gcol = 0;
   g_bg_tint = 0;
   prim_set_bg_col(screen, 0);
   g_fg_gcol = white_gcol;
   g_fg_tint = 0xFF;
   prim_set_fg_col(screen, white_col);
}

static void init_variables(void) {

   // Separate global cursor disabled flag
   cursor_off = 0;

   // Text cursor
   c_enabled = 1;

   // Edit cursor
   e_x_pos   = 0;
   e_y_pos   = 0;
   e_enabled = 0;

   // Default colours and plot mode
   set_default_colours();
   prim_set_bg_plotmode(screen, PM_NORMAL);
   prim_set_fg_plotmode(screen, PM_NORMAL);
   // Reset dot pattern and pattern length to defaults
   prim_set_dot_pattern_len(screen, 0);

   // Reset ECF patterns to defaults
   prim_set_ecf_default(screen);

   // Sprites
   current_sprite = 0;

   // Cursor mode
   vdu_4(NULL);
   // Reset text/graphics areas and home cursors (VDU 26 actions)
   reset_areas();

}

static void reset_areas(void) {
   // Calculate the size of the text area
   update_font_size();
   // Initialize the text area to the full screen
   // (left, bottom, right, top)
   t_clip_window_t default_t_window = {0, (uint8_t)(text_height - 1), (uint8_t)(text_width - 1), 0};
   set_text_area(&default_t_window);
   // Set the graphics origin to 0,0
   g_x_origin = 0;
   g_y_origin = 0;
   // Initialize the graphics area to the full screen
   // (left, bottom, right, top)
   g_clip_window_t default_graphics_window = {0, 0, (int16_t)((screen->width << screen->xeigfactor) - 1), (int16_t)((screen->height << screen->yeigfactor) - 1)};
   set_graphics_area(screen, &default_graphics_window);

   // Home the text cursor
   c_x_pos = t_window.left;
   c_y_pos = t_window.top;
   // Home the graphics cursor
   g_x_pos       = 0;
   g_x_pos_last1 = 0;
   g_x_pos_last2 = 0;
   g_y_pos       = 0;
   g_y_pos_last1 = 0;
   g_y_pos_last2 = 0;
}

// 0,0 is the top left
static void set_text_area(const t_clip_window_t *window) {
   if (window->left > window->right ||
       (unsigned int )window->right > (unsigned int ) text_width - 1 ||
       window->top > window->bottom ||
       (unsigned int ) window->bottom > (unsigned int ) text_height - 1) {
      return;
   }
   // Shallow copy of the struct
   t_window = *window;
   // Update any dependent variables
   update_text_area();
}

static void invert_cursor(int x_pos, int y_pos, int start, int end) {
   int x = x_pos * font_width;
   int y = screen->height - y_pos * font_height - 1;
   for (int i = start; i <= end; i++) {
      for (int j = 0; j < font_width; j++) {
         pixel_t col = screen->get_pixel(screen, x + j, y - i);
         col ^= white_col;
         screen->set_pixel(screen, x + j, y - i, col);
      }
   }
}

static void update_cursors(void) {
   // Update the flashing cursor
   if (f_visible) {
      invert_cursor(f_x_pos, f_y_pos, cursor_start, cursor_end);
      f_visible = 0;
   }
   if (e_enabled || c_enabled) {
      f_x_pos = e_enabled ? e_x_pos : c_x_pos;
      f_y_pos = e_enabled ? e_y_pos : c_y_pos;
      invert_cursor(f_x_pos, f_y_pos, cursor_start, cursor_end);
      f_visible = 1;
   }
   // Update the block cursor
   if (b_visible) {
      invert_cursor(b_x_pos, b_y_pos, 0, font_height - 1);
      b_visible = 0;
   }
   if (e_enabled) {
      b_x_pos = c_x_pos;
      b_y_pos = c_y_pos;
      invert_cursor(b_x_pos, b_y_pos, 0, font_height - 1);
      b_visible = 1;
   }
}

static void enable_cursors(void) {
   c_enabled = !cursor_off;
}

static int disable_cursors(void) {
   int ret = c_enabled;
   c_enabled = 0;
   if (f_visible) {
      invert_cursor(f_x_pos, f_y_pos, cursor_start, cursor_end);
   }
   f_visible = 0;
   if (b_visible) {
      invert_cursor(b_x_pos, b_y_pos, 0, font_height - 1);
   }
   b_visible = 0;
   return ret;

}

static void cursor_interrupt(void) {
   if (c_enabled || e_enabled) {
      f_visible = !f_visible;
      invert_cursor(f_x_pos, f_y_pos, cursor_start, cursor_end);
   }
}

static void enable_edit_cursor(void) {
   if (!e_enabled && !text_at_g_cursor) {
      e_enabled = 1;
      e_x_pos = c_x_pos;
      e_y_pos = c_y_pos;
      update_cursors();
   }
}

static void disable_edit_cursor(void) {
   if (e_enabled) {
      e_enabled = 0;
      e_x_pos = 0;
      e_y_pos = 0;
      update_cursors();
   }
}

static void edit_cursor_up(void) {
   enable_edit_cursor();
   if (e_y_pos > t_window.top) {
      e_y_pos--;
   } else {
      e_y_pos = t_window.bottom;
   }
   update_cursors();
}

static void edit_cursor_down(void) {
   enable_edit_cursor();
   if (e_y_pos < t_window.bottom) {
      e_y_pos++;
   } else {
      e_y_pos = t_window.top;
   }
   update_cursors();
}

static void edit_cursor_left(void) {
   enable_edit_cursor();
   if (e_x_pos > t_window.left) {
      e_x_pos--;
   } else {
      e_x_pos = t_window.right;
      if (e_y_pos > t_window.top) {
         e_y_pos--;
      } else {
         e_y_pos = t_window.bottom;
      }
   }
   update_cursors();
}

static void edit_cursor_right(void) {
   enable_edit_cursor();
   if (e_x_pos < t_window.right) {
      e_x_pos++;
   } else {
      e_x_pos = t_window.left;
      if (e_y_pos < t_window.bottom) {
         e_y_pos++;
      } else {
         e_y_pos = t_window.top;
      }
   }
   update_cursors();
}

static void text_area_scroll(scroll_dir_t dir) {
   int tmp = disable_cursors();
   screen->scroll(screen, &t_window, c_bg_col, dir);
   if (tmp) {
      enable_cursors();
   }
   if (e_enabled) {
      if (dir == SCROLL_UP && e_y_pos > t_window.top) {
         e_y_pos--;
      } else if (dir == SCROLL_DOWN && e_y_pos < t_window.bottom) {
         e_y_pos++;
      }
   }
   update_cursors();
}


static void update_g_cursors(int16_t x, int16_t y) {
   g_x_pos_last2 = g_x_pos_last1;
   g_x_pos_last1 = g_x_pos;
   g_x_pos       = x;
   g_y_pos_last2 = g_y_pos_last1;
   g_y_pos_last1 = g_y_pos;
   g_y_pos       = y;
}

static void change_mode(screen_mode_t *new_screen) {
   // This stops the cursor interrupt having any effect
   disable_cursors();
   // Possibly re-initialize the screen
   if (new_screen && (new_screen != screen || new_screen->mode_num >= CUSTOM_8BPP_SCREEN_MODE)) {
      screen = new_screen;
      if (screen->mode_flags & F_TELETEXT) {
         screen->init(screen, &font_teletext);
      } else {
         screen->init(screen, &font_normal);
      }
   }
   // reset the screen to it's default state
   screen->reset(screen);
   // update the colour flash rate
   if (screen->mode_flags & F_TELETEXT) {
      flash_mark_time  = 16;
      flash_space_time = 48;
   } else {
      flash_mark_time  = 25;
      flash_space_time = 25;
   }
   // initialize the primitives
   prim_init(screen);
   // initialise VDU variable
   init_variables();
   // clear screen
   text_area_clear(NULL);
   // reset all sprite definitions
   prim_reset_sprites(screen);
}

static void set_graphics_area(const screen_mode_t *scr, const g_clip_window_t *window) {
   // Sanity check illegal windows
   if (window->left   < 0 || window->left   >= scr->width  << scr->xeigfactor ||
       window->bottom < 0 || window->bottom >= scr->height << scr->yeigfactor) {
      return;
   }
   if (window->right  < 0 || window->right  >= scr->width  << scr->xeigfactor ||
       window->top    < 0 || window->top    >= scr->height << scr->yeigfactor) {
      return;
   }
   if (window->left >= window->right || window->bottom >= window->top) {
      return;
   }
   // Accept the window
   g_window = *window;
   // Transform to screen coordinates
   int16_t x1 = window->left   >> scr->xeigfactor;
   int16_t y1 = window->bottom >> scr->yeigfactor;
   int16_t x2 = window->right  >> scr->xeigfactor;
   int16_t y2 = window->top    >> scr->yeigfactor;
   // Set the clipping window
   prim_set_graphics_area(screen, x1, y1, x2, y2);
}

static int read_character(int x_pos, int y_pos) {
   int tmp = disable_cursors();
   int c = screen->read_character(screen, x_pos, y_pos, c_bg_col);
   if (tmp) {
      enable_cursors();
   }
   return c;
}

// ==========================================================================
// VDU 4 mode: cursor commands operate on text cursor
// ==========================================================================

static void text_cursor_left(const uint8_t *buf) {
   if (c_x_pos > t_window.left) {
      c_x_pos--;
   } else {
      c_x_pos = t_window.right;
      text_cursor_up(NULL);
   }
   update_cursors();
}

static void text_cursor_right(const uint8_t *buf) {
   if (c_x_pos < t_window.right) {
      c_x_pos++;
   } else {
      c_x_pos = t_window.left;
      text_cursor_down(NULL);
   }
   update_cursors();
}

static void text_cursor_up(const uint8_t *buf) {
   if (c_y_pos > t_window.top) {
      c_y_pos--;
   } else {
      text_area_scroll(SCROLL_DOWN);
   }
   update_cursors();
}

static void text_cursor_down(const uint8_t *buf) {
   if (c_y_pos < t_window.bottom) {
      c_y_pos++;
   } else {
      text_area_scroll(SCROLL_UP);
   }
   update_cursors();
}

static void text_cursor_col0(const uint8_t *buf) {
   disable_edit_cursor();
   c_x_pos = t_window.left;
   update_cursors();
}

static void text_cursor_home(const uint8_t *buf) {
   c_x_pos = t_window.left;
   c_y_pos = t_window.top;
   update_cursors();
}

static void text_cursor_tab(const uint8_t *buf) {
   uint8_t x = buf[1];
   uint8_t y = buf[2];
#ifdef DEBUG_VDU
   printf("cursor move to %d %d\r\n", x, y);
#endif
   // Take account of current text window
   x += t_window.left;
   y += t_window.top;
   if (x <= t_window.right && y <= t_window.bottom) {
      c_x_pos = x;
      c_y_pos = y;
      update_cursors();
   }
}

static void text_area_clear(const uint8_t *buf) {
   int tmp = disable_cursors();
   screen->clear(screen, &t_window, c_bg_col);
   if (tmp) {
      enable_cursors();
   }
   c_x_pos = t_window.left;
   c_y_pos = t_window.top;
   update_cursors();
}

static void text_delete(const uint8_t *buf) {
   text_cursor_left(NULL);
   int tmp = disable_cursors();
   screen->write_character(screen, ' ', c_x_pos, c_y_pos, c_fg_col, c_bg_col);
   if (tmp) {
      enable_cursors();
   }
   update_cursors();
}

// ==========================================================================
// VDU 5 mode: cursor commands operate on graphics cursor
// ==========================================================================

// Notes:
//    g_x_pos/g_y_pos are in absolute external coordinates
//    g_window.left/max are also in absolute external coordinates
//    font_width/height are in screen pixels

static void graphics_cursor_left(const uint8_t *buf) {
   g_x_pos -= (int16_t)(font_width << screen->xeigfactor);
   if (g_x_pos < g_window.left) {
      g_x_pos = (int16_t)(g_window.right + 1 - (font_width << screen->xeigfactor));
      graphics_cursor_up(NULL);
   }
}

static void graphics_cursor_right(const uint8_t *buf) {
   g_x_pos += (int16_t)(font_width << screen->xeigfactor);
   if (g_x_pos > g_window.right) {
      g_x_pos = g_window.left;
      graphics_cursor_down(NULL);
   }
}

static void graphics_cursor_up(const uint8_t *buf) {
   g_y_pos += (int16_t)(font_height << screen->yeigfactor);
   if (g_y_pos > g_window.top) {
      g_y_pos = (int16_t)((g_window.bottom - 1) + (font_height << screen->yeigfactor));
   }
}

static void graphics_cursor_down(const uint8_t *buf) {
   g_y_pos -= (int16_t)(font_height << screen->yeigfactor);
   if (g_y_pos < g_window.bottom) {
      g_y_pos = g_window.top;
   }
}

static void graphics_cursor_col0(const uint8_t *buf) {
   g_x_pos = g_window.left;
}

static void graphics_cursor_home(const uint8_t *buf) {
   g_x_pos = g_window.left;
   g_y_pos = g_window.top;
}

static void graphics_cursor_tab(const uint8_t *buf) {
   uint8_t x = buf[1];
   uint8_t y = buf[2];
#ifdef DEBUG_VDU
   printf("cursor move to %d %d\r\n", x, y);
#endif
   // Scale to absolute external coordinates
   x *= (uint8_t)(font_width << screen->xeigfactor);
   y *= (uint8_t)(font_height << screen->yeigfactor);
   // Take account of current text window
   x += (uint8_t)g_window.left;
   y += (uint8_t)g_window.bottom;
   // Deliberately don't range check here
   g_x_pos = g_window.left + x;
   g_y_pos = g_window.bottom + y;
}

static void graphics_area_clear(const uint8_t *buf) {
   g_x_pos = g_window.left;
   g_y_pos = g_window.top;
   prim_clear_graphics_area(screen);
}

static void graphics_delete(const uint8_t *buf) {
   graphics_cursor_left(NULL);
   int x = g_x_pos >> screen->xeigfactor;
   int y = g_y_pos >> screen->yeigfactor;
   prim_fill_rectangle(screen, x, y, x + (font_width - 1), y - (font_height - 1), PC_BG);
}

// ==========================================================================
// VDU 23 commands
// ==========================================================================

static void vdu23_0(const uint8_t *buf) {
   // VDU 23,0: Write CRTC Register
   int tmp;
   switch (buf[1]) {
   case 10:
   case 11:
      // Cursor Start or End
      tmp = disable_cursors();
      if (buf[1] & 1) {
         cursor_end = buf[2] & 0x1f;
      } else {
         cursor_start = buf[2] & 0x1f;
         // Writing x01x xxxx turns off the cursor
         cursor_off = (buf[2] & 0x60) == 0x20;
      }
      if (tmp) {
         enable_cursors();
      }
      break;
   }
}

static void vdu23_1(const uint8_t *buf) {
   // VDU 23,1: Enable/Disable cursor
   if (buf[1] & 1) {
      enable_cursors();
   } else {
      disable_cursors();
   }
   update_cursors();
}

static void vdu23_2(const uint8_t *buf) {
   // VDU 23,2: Set ECF1 Pattern
   prim_set_ecf_pattern(screen, 0, buf + 1);
}

static void vdu23_3(const uint8_t *buf) {
   // VDU 23,3: Set ECF2 Pattern
   prim_set_ecf_pattern(screen, 1, buf + 1);
}

static void vdu23_4(const uint8_t *buf) {
   // VDU 23,4: Set ECF3 Pattern
   prim_set_ecf_pattern(screen, 2, buf + 1);
}

static void vdu23_5(const uint8_t *buf) {
   // VDU 23,5: Set ECF4 Pattern
   prim_set_ecf_pattern(screen, 3, buf + 1);
}

static void vdu23_6(const uint8_t *buf) {
   // VDU 23,6: Set Dot Pattern
   prim_set_dot_pattern(screen, buf + 1);
}

static void vdu23_7(const uint8_t *buf) {
   // VDU 23,7,extent,direction,movement,0,0,0,0,0 (Scroll rectangle)
   // TODO
}

static void vdu23_8(const uint8_t *buf) {
   // VDU 23,8,t1,t2,x1,y1,x2,x2,0,0 (Clear Block)
   // TODO
}

static void vdu23_9(const uint8_t *buf) {
   // VDU 23,9: Set flash mark (on) time
   flash_mark_time = buf[1];
}

static void vdu23_10(const uint8_t *buf) {
   // VDU 23,10: Set flash space (off) time
   flash_space_time = buf[1];
}

static void vdu23_11(const uint8_t *buf) {
   // VDU 23,11: Set Default ECF Patterns
   prim_set_ecf_default(screen);
   // Also sets the ECF Mode back to legacy BBC/Master
   prim_set_ecf_mode(screen, 0);
}

static void vdu23_12(const uint8_t *buf) {
   // VDU 23,12: Set ECF1 Pattern to a simple 2x4 pattern
   prim_set_ecf_simple(screen, 0, buf + 1);
}

static void vdu23_13(const uint8_t *buf) {
   // VDU 23,13: Set ECF2 Pattern to a simple 2x4 pattern
   prim_set_ecf_simple(screen, 1, buf + 1);
}

static void vdu23_14(const uint8_t *buf) {
   // VDU 23,14: Set ECF3 Pattern to a simple 2x4 pattern
   prim_set_ecf_simple(screen, 2, buf + 1);
}

static void vdu23_15(const uint8_t *buf) {
   // VDU 23,15: Set ECF4 Pattern to a simple 2x4 pattern
   prim_set_ecf_simple(screen, 3, buf + 1);
}

static void vdu23_17(const uint8_t *buf) {
   // vdu 23,17: Set subsidiary colour effects
   switch (buf[1]) {
   case 0:
      // VDU 23,17,0 - sets tint for text foreground colour
      c_fg_tint = buf[2];
      c_fg_col = calculate_colour(c_fg_gcol, c_fg_tint);
      break;
   case 1:
      // VDU 23,17,1 - sets tint for text background colour
      c_bg_tint = buf[2];
      c_bg_col = calculate_colour(c_bg_gcol, c_bg_tint);
      break;
   case 2:
      // VDU 23,17,2 - sets tint for graphics foreground colour
      g_fg_tint = buf[2];
      prim_set_fg_col(screen, calculate_colour(g_fg_gcol, g_fg_tint));
      break;
   case 3:
      // VDU 23,17,3 - sets tint for graphics background colour
      g_bg_tint = buf[2];
      prim_set_bg_col(screen, calculate_colour(g_bg_gcol, g_bg_tint));
      break;
   case 4:
      // VDU 23,17,4 - Select colour patterns mode
      prim_set_ecf_mode(screen, buf[2] & 1);
      break;
   case 5:
      // VDU 23,17,5 - Swap text colours
      {
         pixel_t tmp = c_fg_col;
         c_fg_col = c_bg_col;
         c_bg_col = tmp;
         break;
      }
   case 6:
      // VDU 23,17,6,x;y;0,0,0 - Set ECF origin
      {
         int16_t x = (int16_t)(buf[2] + (buf[3] << 8));
         int16_t y = (int16_t)(buf[4] + (buf[5] << 8));
         prim_set_ecf_origin(screen, x, y);
         break;
      }
   case 7:
      // TODO VDU 23,17,7 - Set character size and spacing
      // VDU 23,17,7,flags,xsize;ysize;0,0
      break;
   default:
      break;
   }
}

static void vdu23_19(const uint8_t *buf) {
   // Select Custom Font and/or Custom Font Metrics
   // VDU 23,19,0,<font number>,0,0,0,0,0,0
   // VDU 23,19,0,<font number>,<h scale>,<v_scale>,<h_spacing>,<v_spacing>,<rounding>,0
   // VDU 23,19,1,<h scale>,<v scale>,0,0,0,0,0
   // VDU 23,19,2,<h spacing>,<v spacing>,0,0,0,0,0
   // VDU 23,19,3,<rounding>
   // VDU 23,19,"FONTNAME" (max of 8 characters, with 0 terminator if less than 8)
   // VDU 23,19,128,<num>,0,0,0,0,0,0 (print the name of the font with number n)
   //
   // Notes:
   // - Any metric of value &FF is ignored
   // - A scale of value &00 is also ignored

   // On enter, buf points to 19, so increment
   buf++;

   font_t *font = &font_normal;

   if (buf[0] >= 'A' && buf[0] <= 'Z') {
      // Select the font by name (up to 8 upper case characters)
      initialize_font_by_name((const char *)buf, font);
   } else {
      switch (buf[0]) {
      case 0:
         // Select the font by number
         initialize_font_by_number(buf[1], font);
         if (buf[2] != 0 && buf[3] != 0) {
            // Parse the extended form
            if (buf[2] != 0xff) {
               font->set_scale_w(font, buf[2]);
            }
            if (buf[3] != 0xff) {
               font->set_scale_h(font, buf[3]);
            }
            if (buf[4] != 0xff) {
               font->set_spacing_w(font, buf[4]);
            }
            if (buf[5] != 0xff) {
               font->set_spacing_h(font, buf[5]);
            }
            if (buf[6] <= 2) {
               font->set_rounding(font, buf[6]);
            }
         }
         break;
      case 1:
         if (buf[1] != 0x00 && buf[1] != 0xff) {
            font->set_scale_w(font, buf[1]);
         }
         if (buf[2] != 0x00 && buf[2] != 0xff) {
            font->set_scale_h(font, buf[2]);
         }
         break;
      case 2:
         if (buf[1] != 0xff) {
            font->set_spacing_w(font, buf[1]);
         }
         if (buf[2] != 0xff) {
            font->set_spacing_h(font, buf[2]);
         }
         break;
      case 3:
         if (buf[1] <= 2) {
            font->set_rounding(font, buf[1]);
         }
         break;
      case 128:
         fb_writes(get_font_name(buf[1]));
         break;
      }
   }
#ifdef DEBUG_VDU
   if (font != NULL)
   {
   printf("    Font name: %s\r\n",    font->get_name(font));
   printf("  Font number: %"PRId32"\r\n",    font->get_number(font));
   printf("   Font scale: %d,%d\r\n", font->get_scale_w(font),   font->get_scale_h(font));
   printf(" Font spacing: %d,%d\r\n", font->get_spacing_w(font), font->get_spacing_h(font));
   printf("Font rounding: %d\r\n",    font->get_rounding(font));
   }
#endif
   // As the font metrics have changed, update text area
   update_text_area();
}

static void vdu23_22(const uint8_t *buf) {
   // VDU 23,22,xpixels;ypixels;xchars,ychars,colours,flags
   // User Defined Screen Mode
   int x_pixels = buf[1] | (buf[2] << 8);
   int y_pixels = buf[3] | (buf[4] << 8);
   unsigned int n_colours = buf[7] & 0xff;
   if (n_colours == 0) {
      n_colours = 256;
   }
   fb_custom_mode(x_pixels, y_pixels, n_colours);
}

static void vdu23_27(const uint8_t *buf) {
   // VDU 23,27,0,N,0,0,0,0,0,0 - select sprite to be plotted
   // VDU 23,27,1,N,0,0,0,0,0,0 - define sprite
   if (buf[1] == 0) {
      // Select sprite to be plotted
      current_sprite = buf[2];
   } else if (buf[1] == 1) {
      // Define sprite
      int x_pos       = g_x_pos       >> screen->xeigfactor;
      int y_pos       = g_y_pos       >> screen->yeigfactor;
      int x_pos_last1 = g_x_pos_last1 >> screen->xeigfactor;
      int y_pos_last1 = g_y_pos_last1 >> screen->yeigfactor;
      prim_define_sprite(screen, buf[2], x_pos, y_pos, x_pos_last1, y_pos_last1);
   }
}
// ==========================================================================
// VDU commands
// ==========================================================================

static void vdu_4(const uint8_t *buf) {
   text_at_g_cursor = 0;
   vdu_operation_table[  8].handler = text_cursor_left;
   vdu_operation_table[  9].handler = text_cursor_right;
   vdu_operation_table[ 10].handler = text_cursor_down;
   vdu_operation_table[ 11].handler = text_cursor_up;
   vdu_operation_table[ 12].handler = text_area_clear;
   vdu_operation_table[ 13].handler = text_cursor_col0;
   vdu_operation_table[ 30].handler = text_cursor_home;
   vdu_operation_table[ 31].handler = text_cursor_tab;
   vdu_operation_table[127].handler = text_delete;
   enable_cursors();
}

static void vdu_5(const uint8_t *buf) {
   disable_cursors();
   vdu_operation_table[  8].handler = graphics_cursor_left;
   vdu_operation_table[  9].handler = graphics_cursor_right;
   vdu_operation_table[ 10].handler = graphics_cursor_down;
   vdu_operation_table[ 11].handler = graphics_cursor_up;
   vdu_operation_table[ 12].handler = graphics_area_clear;
   vdu_operation_table[ 13].handler = graphics_cursor_col0;
   vdu_operation_table[ 30].handler = graphics_cursor_home;
   vdu_operation_table[ 31].handler = graphics_cursor_tab;
   vdu_operation_table[127].handler = graphics_delete;
   text_at_g_cursor = 1;
}

static void vdu_16(const uint8_t *buf) {
   prim_clear_graphics_area(screen);
}

static void vdu_17(const uint8_t *buf) {
   uint8_t col = (uint8_t)(buf[1] & COLOUR_MASK & screen->ncolour);
   if (buf[1] & 128) {
      c_bg_gcol = col;
      c_bg_col = calculate_colour(c_bg_gcol, c_bg_tint);
#ifdef DEBUG_VDU
      printf("bg = %"PRIx32"\r\n", c_bg_col);
#endif
   } else {
      c_fg_gcol = col;             ;
      c_fg_col = calculate_colour(c_fg_gcol, c_fg_tint);
#ifdef DEBUG_VDU
      printf("fg = %"PRIx32"\r\n", c_fg_col);
#endif
   }
}

static void vdu_18(const uint8_t *buf) {
   uint8_t mode = buf[1];
   uint8_t col = (uint8_t)(buf[2] & COLOUR_MASK & screen->ncolour);
   if (buf[2] & 128) {
      g_bg_gcol = col;
      prim_set_bg_plotmode(screen, mode);
      prim_set_bg_col(screen, calculate_colour(g_bg_gcol, g_bg_tint));
   } else {
      g_fg_gcol = col;
      prim_set_fg_plotmode(screen, mode);
      prim_set_fg_col(screen, calculate_colour(g_fg_gcol, g_fg_tint));
   }
}

static void vdu_19(const uint8_t *buf) {
   uint8_t l = buf[1];
   uint8_t p = buf[2];
   uint8_t r = buf[3];
   uint8_t g = buf[4];
   uint8_t b = buf[5];
   // Don't allow Palette Changes in MODE 7 (there is no technical
   // reason for this, but doing so breaks Acornsoft Basic Editor)
   if (screen->mode_flags & F_TELETEXT) {
      return;
   }
   // See http://beebwiki.mdfs.net/VDU_19
   if (p < 16) {
      // Set to Physical Colour
      b = (p & 4) ? 0xff : 0;
      g = (p & 2) ? 0xff : 0;
      r = (p & 1) ? 0xff : 0;
      screen->set_colour(screen, l, r, g, b);
      if (p & 8) {
         screen->set_colour(screen, l + 0x100, 0xff - r, 0xff - g, 0xff - b);
      } else {
         screen->set_colour(screen, l + 0x100, r, g, b);
      }
   } else {
      // Set to RGB Colour
      if (p == 16 || p == 17) {
         // First flashing physical colour
         screen->set_colour(screen, l, r, g, b);
      }
      if (p == 16 || p == 18) {
         // Second flashing physical colour
         screen->set_colour(screen, l + 0x100, r, g, b);
      }
   }
}

static void vdu_20(const uint8_t *buf) {
   screen->reset(screen);
   set_default_colours();
}

static void vdu_22(const uint8_t *buf) {
   uint8_t mode = buf[1] & 0x7F; // Map MODE 128 to MODE 0, etc
   screen_mode_t *new_screen = get_screen_mode(mode);
   if (new_screen != NULL) {
      change_mode(new_screen);
   } else {
      fb_writes("Unsupported screen mode!\r\n");
   }
}

static void vdu_23(const uint8_t *buf) {
#ifdef DEBUG_VDU
      printf("VDU %X %X %X %X %X %X %X %X %X %X\n\r", buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], buf[8], buf[9]);
#endif
   // User defined characters
   if (buf[1] >= 32) {
      define_character(&font_normal, buf[1], buf + 2);
   } else {
      switch (buf[1]) {
      case  0: vdu23_0 (buf + 1); break;
      case  1: vdu23_1 (buf + 1); break;
      case  2: vdu23_2 (buf + 1); break;
      case  3: vdu23_3 (buf + 1); break;
      case  4: vdu23_4 (buf + 1); break;
      case  5: vdu23_5 (buf + 1); break;
      case  6: vdu23_6 (buf + 1); break;
      case  7: vdu23_7 (buf + 1); break;
      case  8: vdu23_8 (buf + 1); break;
      case  9: vdu23_9 (buf + 1); break;
      case 10: vdu23_10(buf + 1); break;
      case 11: vdu23_11(buf + 1); break;
      case 12: vdu23_12(buf + 1); break;
      case 13: vdu23_13(buf + 1); break;
      case 14: vdu23_14(buf + 1); break;
      case 15: vdu23_15(buf + 1); break;
      case 17: vdu23_17(buf + 1); break;
      case 19: vdu23_19(buf + 1); break;
      case 22: vdu23_22(buf + 1); break;
      case 27: vdu23_27(buf + 1); break;
      default: screen->unknown_vdu(screen, buf);
      }
   }
}

static void vdu_24(const uint8_t *buf) {
   g_clip_window_t window;
   window.left   = (int16_t)(buf[1] + (buf[2] << 8));
   window.bottom = (int16_t)(buf[3] + (buf[4] << 8));
   window.right  = (int16_t)(buf[5] + (buf[6] << 8));
   window.top    = (int16_t)(buf[7] + (buf[8] << 8));
#ifdef DEBUG_VDU
   printf("graphics area left:%d bottom:%d right:%d top:%d\r\n",
          window.left, window.bottom, window.right, window.top);
#endif
   // Transform to absolute external coordinates
   window.left   += g_x_origin;
   window.bottom += g_y_origin;
   window.right  += g_x_origin;
   window.top    += g_y_origin;
   // Set the window
   set_graphics_area(screen, &window);
}


static void vdu_25(const uint8_t *buf) {
   int skew;
   uint8_t g_mode = buf[1];
   int16_t x = (int16_t)(buf[2] + (buf[3] << 8));
   int16_t y = (int16_t)(buf[4] + (buf[5] << 8));

#ifdef DEBUG_VDU
   printf("plot %d %d %d\r\n", g_mode, x, y);
#endif

   if (g_mode & 4) {
      // Relative to the graphics origin
      update_g_cursors(g_x_origin + x, g_y_origin + y);
   } else {
      // Relative to the last point.
      update_g_cursors(g_x_pos + x, g_y_pos + y);
   }

   // Transform plotting coordinates to screen coordinates
   int x_pos       = g_x_pos       >> screen->xeigfactor;
   int y_pos       = g_y_pos       >> screen->yeigfactor;
   int x_pos_last1 = g_x_pos_last1 >> screen->xeigfactor;
   int y_pos_last1 = g_y_pos_last1 >> screen->yeigfactor;
   int x_pos_last2 = g_x_pos_last2 >> screen->xeigfactor;
   int y_pos_last2 = g_y_pos_last2 >> screen->yeigfactor;

   if (g_mode & 0x03) {

      plotcol_t colour;

      if ((g_mode & 0x03) == 1) {
         colour = PC_FG;
      } else if ((g_mode & 3) == 3) {
         colour = PC_BG;
      } else {
         colour = PC_INV;
      }

      switch (g_mode & 0xF8) {

      case 0:
         // Plot solid line (both endpoints included)
         prim_draw_line(screen, x_pos_last1, y_pos_last1, x_pos, y_pos, colour, g_mode);
         break;
      case 8:
         // Plot solid line (final endpoint omitted)
         prim_draw_line(screen, x_pos_last1, y_pos_last1, x_pos, y_pos, colour, g_mode);
         break;
      case 16:
         // Plot dotted line (both endpoints included)
         prim_draw_line(screen, x_pos_last1, y_pos_last1, x_pos, y_pos, colour, g_mode);
         break;
      case 24:
         // Plot dotted line (final endpoint omitted)
         prim_draw_line(screen, x_pos_last1, y_pos_last1, x_pos, y_pos, colour, g_mode);
         break;
      case 32:
         // Plot solid line (initial endpoint omitted)
         prim_draw_line(screen, x_pos_last1, y_pos_last1, x_pos, y_pos, colour, g_mode);
         break;
      case 40:
         // Plot solid line (final endpoint omitted)
         prim_draw_line(screen, x_pos_last1, y_pos_last1, x_pos, y_pos, colour, g_mode);
         break;
      case 48:
         // Plot dotted line (initial endpoint omitted)
         prim_draw_line(screen, x_pos_last1, y_pos_last1, x_pos, y_pos, colour, g_mode);
         break;
      case 56:
         // Plot dotted line (final endpoint omitted)
         prim_draw_line(screen, x_pos_last1, y_pos_last1, x_pos, y_pos, colour, g_mode);
         break;
      case 64:
         // Plot point
         prim_set_pixel(screen, x_pos, y_pos, colour);
         break;
      case 72:
         // Horizontal line fill (left and right) to non-background
         prim_fill_area(screen, x_pos, y_pos, colour, HL_LR_NB);
         break;
      case 80:
         // Fill a triangle
         prim_fill_triangle(screen, x_pos_last2, y_pos_last2, x_pos_last1, y_pos_last1, x_pos, y_pos, colour);
         break;
      case 88:
         // Horizontal line fill (right only) to background
         prim_fill_area(screen, x_pos, y_pos, colour, HL_RO_BG);
         break;
      case 96:
         // Fill a rectangle
         prim_fill_rectangle(screen, x_pos_last1, y_pos_last1, x_pos, y_pos, colour);
        break;
      case 104:
         // Horizontal line fill (left and right) to foreground
         prim_fill_area(screen, x_pos, y_pos, colour, HL_LR_FG);
         break;
      case 112:
         // Fill a parallelogram
         prim_fill_parallelogram(screen, x_pos_last2, y_pos_last2, x_pos_last1, y_pos_last1, x_pos, y_pos, colour);
         break;
      case 120:
         // Horizontal line fill (right only) to non-foreground
         prim_fill_area(screen, x_pos, y_pos, colour, HL_RO_NF);
         break;
      case 128:
         // Flood fill to non-background
         prim_fill_area(screen, x_pos, y_pos, colour, AF_NONBG);
         break;
      case 136:
         // Flood fill to foreground
         prim_fill_area(screen, x_pos, y_pos, colour, AF_TOFGD);
         break;
      case 144:
         // Draw a circle outline
         prim_draw_circle(screen, x_pos_last1, y_pos_last1, x_pos, y_pos, colour);
         break;
      case 152:
         // Fill a circle
         prim_fill_circle(screen, x_pos_last1, y_pos_last1, x_pos, y_pos, colour);
         break;
      case 160:
         // Plot a circular arc
         prim_draw_arc(screen, x_pos_last2, y_pos_last2, x_pos_last1, y_pos_last1, x_pos, y_pos, colour);
         break;
      case 168:
         // Plot a filled chord segment
         prim_fill_chord(screen, x_pos_last2, y_pos_last2, x_pos_last1, y_pos_last1, x_pos, y_pos, colour);
         break;
      case 176:
         // Plot a filled sector
         prim_fill_sector(screen, x_pos_last2, y_pos_last2, x_pos_last1, y_pos_last1, x_pos, y_pos, colour);
         break;
      case 184:
         // Move/Copy rectangle
         prim_move_copy_rectangle(screen, x_pos_last2, y_pos_last2, x_pos_last1, y_pos_last1, x_pos, y_pos, ((g_mode & 2) == 0));
         break;
      case 192:
         // Plot ellipse outline
         skew = (y_pos > y_pos_last2) ? x_pos - x_pos_last2 : x_pos_last2 - x_pos;
         prim_draw_ellipse(screen, x_pos_last2, y_pos_last2, abs(x_pos_last1 - x_pos_last2), abs(y_pos - y_pos_last2), skew, colour);
         break;
      case 200:
         // Plot solid ellipse
         skew = (y_pos > y_pos_last2) ? x_pos - x_pos_last2 : x_pos_last2 - x_pos;
         prim_fill_ellipse(screen, x_pos_last2, y_pos_last2, abs(x_pos_last1 - x_pos_last2), abs(y_pos - y_pos_last2), skew, colour);
         break;
      case 232:
         // Draw Sprite
         prim_draw_sprite(screen, current_sprite, x_pos, y_pos);
         break;
      default:
         printf("Unsupported plot code: %d\r\n", g_mode);
         break;
      }
   }
}

static void vdu_26(const uint8_t *buf) {
   reset_areas();
}

static void vdu_27(const uint8_t *buf) {
   uint8_t c = buf[1];
   switch (c) {
   case 136:
      edit_cursor_left();
      break;
   case 137:
      edit_cursor_right();
      break;
   case 138:
      edit_cursor_down();
      break;
   case 139:
      edit_cursor_up();
      break;
   default:
      vdu_default(&c);
   }
}

static void vdu_28(const uint8_t *buf) {
   // left, bottom, right, top
   t_clip_window_t window = {buf[1], buf[2], buf[3], buf[4]};
   set_text_area(&window);
#ifdef DEBUG_VDU
   printf("text area left:%d bottom:%d right:%d top:%d\r\n",
          window.left, window.bottom, window.right, window.top);
#endif
}

static void vdu_29(const uint8_t *buf) {
   g_x_origin = (int16_t)(buf[1] + (buf[2] << 8));
   g_y_origin = (int16_t)(buf[3] + (buf[4] << 8));
#ifdef DEBUG_VDU
   printf("graphics origin %d %d\r\n", g_x_origin, g_y_origin);
#endif
}


static void vdu_nop(const uint8_t *buf) {
}

static void vdu_default(const uint8_t *buf) {
   uint8_t c = buf[0];
   if (text_at_g_cursor) {
      // Draw the character at the graphics cursor (VDU 5 mode)
      int x = g_x_pos >> screen->xeigfactor;
      int y = g_y_pos >> screen->yeigfactor;
      // Only draw the foreground pixels
      prim_draw_character(screen, c, x, y, PC_FG);
      // Advance the drawing position
      graphics_cursor_right(NULL);
   } else {
      // Draw the character at the text cursor (VDU 4 mode)
      // - Pixel 0,0 is in the bottom left
      // - Character 0,0 is in the top left
      // - So the Y axis needs flipping
      // Draw the foreground and background pixels
      int tmp = disable_cursors();
      screen->write_character(screen, c, c_x_pos, c_y_pos, c_fg_col, c_bg_col);
      if (tmp) {
         enable_cursors();
      }
      // Advance the drawing position
      text_cursor_right(NULL);
   }
}

// ==========================================================================
// Public interface
// ==========================================================================

static void select_font(int n, int sh, int sv, int r) {
   fb_writec(23);
   fb_writec(19);
   fb_writec(0);
   fb_writec((char)n);
   fb_writec((char)sh);
   fb_writec((char)sv);
   fb_writec(0xff);
   fb_writec(0xff);
   fb_writec((char)r);
   fb_writec(0);
}

static void cursor(int n) {
   fb_writec(23);
   fb_writec(1);
   fb_writec((char)n);
   fb_writec(0);
   fb_writec(0);
   fb_writec(0);
   fb_writec(0);
   fb_writec(0);
   fb_writec(0);
   fb_writec(0);
}

static void plot(int n, int x, int y) {
   fb_writec(25);
   fb_writec((char)n);
   fb_writec((char)x);
   fb_writec((char)(x >> 8));
   fb_writec((char)y);
   fb_writec((char)(y >> 8));
}

static void owl(int x0, int y0, int r, int col) {
   const int data[] = {
      0, 2, 4, 6, 8, 10, 12, 14, 16, -1,
      1, 7, 9, 15, -1,
      0, 4, 8, 12, 16, -1,
      3, 5, 11, 13, -1,
      0, 4, 8, 12, 16, -1,
      1, 7, 9, 15, -1,
      0, 2, 8, 14, 16, -1,
      1, 3, 13, -1,
      0, 2, 4, 6, 8, 10, 12, 16, -1,
      1, 3, 5, 7, -1,
      0, 2, 4, 6, 8, 16, -1,
      1, 3, 5, 7, -1,
      2, 4, 6, 8, 16, -1,
      3, 5, 7, 9, -1,
      4, 6, 8, 10, 16, -1,
      5, 7, 9, 11, -1,
      6, 8, 10, 12, 16, -1,
      7, 11, 13, -1,
      6, 10, 14, 16, -1,
      1, 3, 5, 7, 9, 11, 15, -1,
      16
   };
   int y = 0;


   // 256-colour GCOL but numbering
   // Bits 0..1 = Tint (0x00, 0x11, 0x22, 0x33) added
   // Bits 3..2 = R    (0x00, 0x44, 0x88, 0xCC)
   // Bits 5..4 = G    (0x00, 0x44, 0x88, 0xCC)
   // Bits 7..6 = B    (0x00, 0x44, 0x88, 0xCC)

   uint8_t cols[] = {
             //         RRGGBBTT
      0x0F,  // Red     00001111
      0x1F,  // Orange  00011111
      0x3F,  // Yellow  00111111
      0x20,  // Green   00100000
      0xC3,  // Blue    11000011
      0x84,  // Indigo  10000100
      0x9A   // Violet  10011010
   };

   // Save graphics colour
   pixel_t old_col = prim_get_fg_col();
   plotmode_t old_mode = prim_get_fg_plotmode();
   for (unsigned int i = 0; i < sizeof(data) / sizeof(int); i++) {
      int x = data[i];
      if (x >= 0) {
         int xc = x0 + x * r;
         int yc = y0 - y * r;
         int d = y / 3;
         prim_set_fg_plotmode(screen, PM_NORMAL);
         prim_set_fg_col(screen, screen->get_colour(screen, cols[d]));
         plot(  4, xc, yc);
         plot(157, xc, yc + r * 5 / 7);
      } else {
         y++;
      }
   }
   // Restore graphics colour
   prim_set_fg_plotmode(screen, old_mode);
   prim_set_fg_col(screen, old_col);
}

static void fb_show_splash_screen(void) {
   char buffer[256];
   int address = 0x88; // TODO takes this value from the helper code
   // Select the default screen mode
   fb_writec(22);

#if 1
   fb_writec(DEFAULT_SCREEN_MODE);

   // Turn of the cursor (as there are some bugs when changing fonts)
   cursor(0);
   select_font(12, 2, 2, 0); // Computer Font
   fb_writes("Pi1MHz VDU Driver\r\n\n");

   fb_writec(17);
#ifdef DEBUG
   fb_writec(3); // Red in MODE 21
   fb_writes("      DEBUG Kernel\r\n");
#else
   fb_writec(12); // Green in MODE 21
   fb_writes("      NORMAL Kernel\r\n");
#endif
   fb_writec(17);
   fb_writec(63); // White in MODE 21

   // Draw a green owl
   owl(944, 1008, 20, 12);

   select_font(32, 1, 1, 1); // SAA5050
   fb_writec(26);
   fb_writec(31);
   fb_writec(0);
   fb_writec(4);
   fb_writes("Release: "RELEASENAME"\r\n");
   fb_writes("Commit : "GITVERSION"\r\n");
   fb_writes("Pi Info: ");
   fb_writes(get_info_string());
   fb_writes("\r\n");
   sprintf(buffer, "Temp   : %2.1fC\r\n", (double) get_temp() ); fb_writes(buffer);
    fb_writes("\r\n\n\n\n\n\n\n");

   fb_writes("Helper functions can be started in one  of three ways : \r\n\r\n");

   sprintf(buffer, "*FX147,%d,n <ret> *GO FD00 <ret>\r\n", address); fb_writes(buffer);
   sprintf(buffer, "*FX147,%d,n <ret> *GOIO FD00 <ret>\r\n", address); fb_writes(buffer);
   sprintf(buffer, "X%%=n:CALL&FC%x <ret>\r\n", address); fb_writes(buffer);
   fb_writes("\r\n where n is one of the following"
        "\r\n"
        "\r\n 0 # This help screen"
        "\r\n 1 # Status N/A"
        "\r\n 2 # Enable screen redirector"
        "\r\n 3 # Load ADFS into SWR"
        "\r\n 4 # Load MMFS into SWR"
        "\r\n 5 # Load MMFS2 into SWR"
        "\r\n 6 # Load BeebSCSI helper ROM into SWR");

   select_font(0, 1, 1, 0); // Default
   fb_writec(26);
   fb_writec(31);
   fb_writec(0);
   fb_writec(56);

  // cursor(1);
#else
   fb_writec(1);
   cursor(0);
#endif
}

static void fb_initialize(void) {

   // Initialize the VDU operation table
   for (unsigned int i = 32; i < sizeof(vdu_operation_table) / sizeof(vdu_operation_t); i++) {
      vdu_operation_table[i].len = 0;
      vdu_operation_table[i].handler = vdu_default;
   }

   // Fonts
   initialize_font_by_number(DEFAULT_FONT, &font_normal);
   initialize_font_by_name("SAA5050", &font_teletext);

   // Select the default screen mode
   fb_writec(22);
   fb_writec(DEFAULT_SCREEN_MODE);

   // Enable the timer interrupts (flashing colours, cursor, etc)
   RPI_ArmTimerInit();
   RPI_GetIrqController()->Enable_Basic_IRQs = RPI_BASIC_ARM_TIMER_IRQ;

   screen_set_vsync(true);
}

#if 0
void fb_destroy(void) {

   // Disable the VSync Interrupt
    screen_set_vsync(false);

   // Disable the timer interrupts (flashing colours, cursor, etc)
   RPI_GetIrqController()->Disable_Basic_IRQs = RPI_BASIC_ARM_TIMER_IRQ;

   // Disable the frame buffer
   screen_release_buffer ( handle);
}
#endif
void fb_custom_mode(int x_pixels, int y_pixels, unsigned int n_colours) {
   screen_mode_t *new_screen;
   if (n_colours > 0x10000) {
      new_screen = get_screen_mode(CUSTOM_32BPP_SCREEN_MODE);
   } else if (n_colours > 0x100) {
      new_screen = get_screen_mode(CUSTOM_16BPP_SCREEN_MODE);
   } else {
      new_screen = get_screen_mode(CUSTOM_8BPP_SCREEN_MODE);
   }
   new_screen->width = x_pixels;
   new_screen->height = y_pixels;
   // Calculate xeigfactor so minimum X dimension in OS Units is 1280
   // and the minimum xeigfactor is 1
   new_screen->xeigfactor = 0;
   do {
      new_screen->xeigfactor++;
      x_pixels <<= 1;
   } while (x_pixels < 1280);
   // Calculate yeigfactor so minimum Y dimension in OS Units is 1024
   // and the minimum yeigfactor is 1
   new_screen->yeigfactor = 0;
   do {
      new_screen->yeigfactor++;
      y_pixels <<= 1;
   } while (y_pixels < 1024);
   new_screen->ncolour = n_colours - 1;
   new_screen->par = ((float) (1 << new_screen->xeigfactor)) / ((float) (1 << new_screen->yeigfactor));
   change_mode(new_screen);
}

static void writec(char ch) {

   static int vdu_index = 0;
   static vdu_operation_t *vdu_op = NULL;
   NOINIT_SECTION  static uint8_t vdu_buf[VDU_BUF_LEN];

   uint8_t c = (uint8_t) ch;

   // Buffer the character
   vdu_buf[vdu_index] = c;

   // Start of a VDU command
   if (vdu_index == 0) {
      vdu_op = vdu_operation_table + c;
   }

   // End of a VDU command
   if (vdu_index == vdu_op->len) {
      vdu_index = 0;
      vdu_op->handler(vdu_buf);
   } else {
      vdu_index++;
   }
}

void fb_process_flash(void)
{
   static uint8_t cursor_count = 0;

   // Note the vsync interrupt
   vsync_flag = 1;
   // Handle the flashing cursor (toggles every 160ms / 320ms)
   cursor_count++;
   if (cursor_count >= (e_enabled ? 8 : 16)) {
      cursor_interrupt();
      cursor_count = 0;
   }

   // Handle the flashing colours
   // - non-teletext mode, 500ms on, 500ms off
   // - teletext mode, 320ms on 960ms off
   // -
   if (screen->flash) {
      static uint8_t flash_count = 0;
      static uint8_t flash_state = 0;
      if (flash_mark_time == 0 || flash_space_time == 0) {
         // An on/off time of zero is infinite and flashing stops
         uint8_t tmp = (flash_mark_time == 0) ? 1 : 0;
         if (tmp != flash_state) {
            flash_state = tmp;
            screen->flash(screen, tmp);
            flash_count = 0;
         }
      } else {
         flash_count++;
         if (flash_count >= (flash_state ? flash_mark_time : flash_space_time)) {
            flash_state = !flash_state;
            screen->flash(screen, flash_state);
            flash_count = 0;
         }
      }
   }
}

void fb_process_vdu_queue(void) {
   if (RPI_GetIrqController()->IRQ_basic_pending & RPI_BASIC_ARM_TIMER_IRQ) {

      // Clear the ARM Timer interrupt
      RPI_GetArmTimer()->IRQClear = 0;

      // Service the VDU Queue
      while (vdu_rp != vdu_wp) {
         uint8_t ch = vdu_queue[vdu_rp];
         writec(ch);
         vdu_rp = (vdu_rp + 1) & (VDU_QSIZE - 1);
      }
   }
}

void fb_writec_buffered(char c) {
   // TODO: Deal with overflow
   unsigned int irq = _disable_interrupts_cspr();
   vdu_queue[vdu_wp] = c;
   vdu_wp = (vdu_wp + 1) & (VDU_QSIZE - 1);
  _set_interrupts(irq);
}

void fb_writec(char c) {
 // *** TODO Disable interrupts to avoid race condition
   // Avoid re-ordering parasite and host characters
   if (vdu_rp != vdu_wp) {
      // Some characters are already queued, so append to the end of the queue
      fb_writec_buffered(c);
   } else {
      // Otherwise, it's safe to print directly
      writec(c);
   }
}

void fb_writes(const char *string) {
   while (*string) {
      fb_writec(*string++);
   }
}

int fb_get_cursor_x(void) {
   if (e_enabled) {
      return e_x_pos - t_window.left;
   } else {
      return c_x_pos - t_window.left;
   }
}

int fb_get_cursor_y(void) {
   if (e_enabled) {
      return e_y_pos - t_window.top;
   } else {
      return c_y_pos - t_window.top;
   }
}
// cppcheck-suppress unusedFunction
int fb_get_cursor_char(void) {
   if (e_enabled) {
      return read_character(e_x_pos, e_y_pos);
   } else {
      return read_character(c_x_pos, c_y_pos);
   }
}
// cppcheck-suppress unusedFunction
void fb_wait_for_vsync(void) {

   // Wait for the VSYNC flag to be set by the IRQ handler
   while (!vsync_flag);

   // Clear the VSYNC flag
   vsync_flag = 0;
}

screen_mode_t *fb_get_current_screen_mode(void) {
   return screen;
}

int32_t fb_read_vdu_variable(vdu_variable_t v) {
   if (v < 0x80) {
      return fb_read_mode_variable( (mode_variable_t) v, screen);
   }

   font_t *font = screen->font;
   // (ic) means internal coordinates: the origin is always the bottom left of the screen.
   // One unit is one pixel wide and one pixel high.
   //
   // (ec) means external coordinates: a pixel is (1 « XEigFactor) units wide and (1 « YEigFactor) units high,
   // where XEigFactor and YEigFactor are VDU variables.

   // g_window is in (ex) but with the origin applied (so it's neither ic or ec)

   switch (v) {
   case V_GWLCOL:
      // Graphics Window – Lefthand Column (ic)
      return g_window.left >> screen->xeigfactor;
   case V_GWBROW:
      // Graphics Window – Bottom Row  (ic)
      return g_window.bottom >> screen->yeigfactor;
   case V_GWRCOL:
      // Graphics Window – Righthand Column  (ic)
      return g_window.right >> screen->xeigfactor;
   case V_GWTROW:
      // Graphics Window – Top Row (ic)
      return g_window.top >> screen->yeigfactor;
   case V_TWLCOL:
      // Text Window – Lefthand Column
      return t_window.left;
   case V_TWBROW:
      // Text Window – Bottom Row
      return t_window.bottom;
   case V_TWRCOL:
      // Text Window – Righthand Column
      return t_window.right;
   case V_TWTROW:
      // Text Window – Top Row
      return t_window.top;
   case V_ORGX:
      // X coord of graphics Origin  (ec)
      return g_x_origin;
   case V_ORGY:
      // Y coord of graphics Origin  (ec)
      return g_y_origin;
   case V_GCSX:
      // Graphics Cursor X coord (ec)
      return g_x_pos - g_x_origin;
   case V_GCSY:
      // Graphics Cursor Y coord (ec)
      return g_y_pos - g_y_origin;
   case V_OLDERCSX:
      // Oldest gr. Cursor X coord (ic)
      return g_x_pos_last2 >> screen->xeigfactor;
   case V_OLDERCSY:
      // Oldest gr. Cursor Y coord (ic)
      return g_y_pos_last2 >> screen->yeigfactor;
   case V_OLDCSX:
      // Previous gr. Cursor X coord (ic)
      return g_x_pos_last1 >> screen->xeigfactor;
   case V_OLDCSY:
      // Previous gr. Cursor Y coord (ic)
      return g_y_pos_last1 >> screen->yeigfactor;
   case V_GCSIX:
      // Graphics Cursor X coord (ic)
      return g_x_pos >> screen->xeigfactor;
   case V_GCSIY:
      // Graphics Cursor Y coord (ic)
      return g_y_pos >> screen->yeigfactor;
   case V_NEWPTX:
      // New point X coord (ic) - TODO: no idea what this is
      return 0;
   case V_NEWPTY:
      // New point Y coord (ic) - TODO: no idea what this is
      return 0;
   case V_SCREENSTART:
      // As used by VDU drivers
      return (int)(get_fb_address());
   case V_DISPLAYSTART:
      // As used by display hardware
      return (int)(get_fb_address());
   case V_TOTALSCREENSIZE:
      return screen->height * screen->pitch;
   case V_GPLFMD:
      // GCOL action for foreground col
      return prim_get_fg_plotmode();
   case V_GPLBMD:
      // GCOL action for background col
      return prim_get_bg_plotmode();
   case V_GFCOL:
      // Graphics foreground col
      return g_fg_gcol;
   case V_GBCOL:
      // Graphics background col
      return g_bg_gcol;
   case V_TFORECOL:
      // Text foreground col
      return c_fg_gcol;
   case V_TBACKCOL:
      // Text background col
      return c_bg_gcol;
   case V_GFTINT:
      // Graphics foreground tint
      return g_fg_tint;
   case V_GBTINT:
      // Graphics background tint
      return g_bg_tint;
   case V_TFTINT:
      // Text foreground tint
      return c_fg_tint;
   case V_TBTINT:
      // Text background tint
      return c_bg_tint;
   case V_MAXMODE:
      // Highest built-in numbered mode known to kernel - TODO
      return 95;
   case V_GCHARSIZEX:
      // X size of VDU5 chars (pixels)
      return font->get_overall_w(font);
   case V_GCHARSIZEY:
      // Y size of VDU5 chars (pixels)
      return font->get_overall_h(font);
   case V_GCHARSPACEX:
      // X spacing of VDU5 chars (pixels)
      return 0;
   case V_GCHARSPACEY:
      // Y spacing of VDU5 chars (pixels)
      return 0;
   case V_HLINEADDR:
      // Address of horizontal line-draw routine - NOT IMPLEMENTED
      return 0;
   case V_TCHARSIZEX:
      // X size of VDU4 chars (pixels)
      return font->get_overall_w(font);
   case V_TCHARSIZEY:
      // Y size of VDU4 chars (pixels)
      return font->get_overall_h(font);
   case V_TCHARSPACEX:
      // X spacing of VDU4 chars (pixels)
      return 0;
   case V_TCHARSPACEY:
      // Y spacing of VDU4 chars (pixels)
      return 0;
   case V_GCOLORAEORADDR:
      // Addr of colour blocks for current GCOLs - NOT IMPLEMENTED
      return 0;
   case V_VIDCCLOCKSPEED:
      // VIDC clock speed in kHz3 - NOT IMPLEMENTED
      return 0;
   case V_LEFT:
      // border size
      return 0;
   case V_BOTTOM:
      // border size
      return 0;
   case V_RIGHT:
      // border size
      return 0;
   case V_TOP:
      // border size
      return 0;
   case V_CURRENT:
      // GraphicsV driver number - NOT IMPLEMENTED
      return 0;
   case V_WINDOWWIDTH:
      //  Width of text window in chars
      return text_width;
   case V_WINDOWHEIGHT:
      //  Height of text window in chars
      return text_height;
   }
   return 0;
}

static uint8_t read_legacy_vdu_variable_helper(vdu_variable_t v, uint8_t offset) {
   int value = fb_read_vdu_variable(v);
   if (offset & 1) {
      return (uint8_t)((value >> 8) & 0xff);
   } else {
      return (uint8_t)(value & 0xff);
   }
}
// cppcheck-suppress unusedFunction
uint8_t fb_read_legacy_vdu_variable(uint8_t v) {
   // VDU Variables are unfortunately platform specific, so try to do some mapping
   //
   // Where these correspond to RISCOS VDU variables, we use them directly.
   switch(v) {
      // &00/1 Current graphics window left column in pixels
      // &02/3 Current graphics window bottom row in pixels
      // &04/5 Current graphics window right column in pixels
      // &06/7 Current graphics window top row in pixels
   case 0x00:
   case 0x01:
      return read_legacy_vdu_variable_helper(V_GWLCOL, v);
   case 0x02:
   case 0x03:
      return read_legacy_vdu_variable_helper(V_GWBROW, v);
   case 0x04:
   case 0x05:
      return read_legacy_vdu_variable_helper(V_GWRCOL, v);
   case 0x06:
   case 0x07:
      return read_legacy_vdu_variable_helper(V_GWTROW, v);
      // &08   Current text window left hand column
      // &09   Current text window bottom row
      // &0A   Current text window right hand column
      // &0B   Current text window top column
   case 0x08:
      return read_legacy_vdu_variable_helper(V_TWLCOL, 0);
   case 0x09:
      return read_legacy_vdu_variable_helper(V_TWBROW, 0);
   case 0x0A:
      return read_legacy_vdu_variable_helper(V_TWRCOL, 0);
   case 0x0B:
      return read_legacy_vdu_variable_helper(V_TWTROW, 0);
      // &0C/D Current graphics X origin in external coordinates
      // &0E/F Current graphics Y origin in external coordinates
   case 0x0C:
   case 0x0D:
      return read_legacy_vdu_variable_helper(V_ORGX, v);
   case 0x0E:
   case 0x0F:
      return read_legacy_vdu_variable_helper(V_ORGY, v);
      // &10/1 Current graphics X position in external coordinates
      // &12/3 Current graphics Y position in external coordinates
   case 0x10:
   case 0x11:
      return read_legacy_vdu_variable_helper(V_GCSX, v);
   case 0x12:
   case 0x13:
      return read_legacy_vdu_variable_helper(V_GCSY, v);
      // &14/5 Old graphics X position in internal coordinates
      // &16/7 Old graphics Y position in internal coordinates
   case 0x14:
   case 0x15:
      return read_legacy_vdu_variable_helper(V_OLDCSX, v);
   case 0x16:
   case 0x17:
      return read_legacy_vdu_variable_helper(V_OLDCSY, v);
      // &18 Current absolute text X position (=POS+vduvar &08)
      // &19 Current absolute text Y position (=VPOS+vduvar &0B)
   case 0x18:
      return (uint8_t)((fb_get_cursor_x() + fb_read_vdu_variable(V_TWLCOL)) & 0xff);
   case 0x19:
      return (uint8_t)((fb_get_cursor_y() + fb_read_vdu_variable(V_TWTROW)) & 0xff);
      // &24/5 Current graphics X position in internal coordinates
      // &26/7 Current graphics Y position in internal coordinates
   case 0x24:
   case 0x25:
      return read_legacy_vdu_variable_helper(V_GCSIX, v);
   case 0x26:
   case 0x27:
      return read_legacy_vdu_variable_helper(V_GCSIY, v);
      // &4F Bytes per character for current mode
   case 0x4f:
      if (screen->mode_flags & F_TELETEXT) {
         return 1;
      } else {
         // Note: the values returned are the same as on the Beeb
         // and don't actually reflect the real frame buffer bit depths
         // in some cases. Also assumes an 8x8 font.
         switch (screen->ncolour) {
         case 0x01:
            return (uint8_t)8;   // actually 64
         case 0x03:
            return (uint8_t)16;  // actually 64
         case 0x0f:
            return (uint8_t)32;  // actually 64
         case 0xff:
            return (uint8_t)64;
         case 0xffff:
            return (uint8_t)128;
         case 0xffffff:
            return (uint8_t)0;   // actually 256
         default:
            return (uint8_t)0;   // should never hit this case
         }
      }
      // &55 Current screen mode read by OSBYTE &87
   case 0x55:
      return (uint8_t)(screen->mode_num & 0xff);
      // &60 Number logical colours -1
   case 0x60:
      return (uint8_t)(screen->ncolour & 0xff);
      // &61 Pixels per byte -1 (zero if text only mode)
   case 0x61:
      if (screen->mode_flags & (F_TELETEXT | F_BBC_GAP)) {
         return (uint8_t)0;
      } else {
         switch (screen->ncolour) {
         case 0x01:
            return (uint8_t)7;
         case 0x03:
            return (uint8_t)3;
         case 0x0f:
            return (uint8_t)1;
         default:
            return (uint8_t)0;
         }
      }
   }
   return (uint8_t) 0x00;
}
// cppcheck-suppress unusedFunction
void fb_set_flash_mark_time(uint8_t time) {
   flash_mark_time = time;
}
// cppcheck-suppress unusedFunction
void fb_set_flash_space_time(uint8_t time) {
   flash_space_time = time;
}
// cppcheck-suppress unusedFunction
uint8_t fb_get_flash_mark_time(void) {
   return flash_mark_time;
}
// cppcheck-suppress unusedFunction
uint8_t fb_get_flash_space_time(void) {
   return flash_space_time;
}
// cppcheck-suppress unusedFunction
int fb_point(int16_t x, int16_t y, pixel_t *colour) {
   // convert to absolute external coordinates
   x += g_x_origin;
   y += g_y_origin;
   if (x < g_window.left || x > g_window.right || y < g_window.bottom || y > g_window.top) {
      // -1 indicates pixel off screen
      return -1;
   } else {
      // convert to absolute pixel coordinates
      x >>= screen->xeigfactor;
      y >>= screen->yeigfactor;
      // read the pixel
      *colour = prim_get_pixel(screen, x, y);
      // 0 indicates pixel on screen
      return 0;
   }
}

// Set the foreground graphics colour directly, bypassing the colour/tint VDU variables
// cppcheck-suppress unusedFunction
void fb_set_g_fg_col(uint8_t action, pixel_t colour) {
   prim_set_fg_plotmode(screen, action);
   prim_set_fg_col(screen, colour);
}

// Set the background graphics colour directly, bypassing the colour/tint VDU variables
// cppcheck-suppress unusedFunction
void fb_set_g_bg_col(uint8_t action, pixel_t colour) {
   prim_set_bg_plotmode(screen, action);
   prim_set_bg_col(screen, colour);
}

// Set the foreground text colour directly, bypassing the colour/tint VDU variables
// cppcheck-suppress unusedFunction
void fb_set_c_fg_col(pixel_t colour) {
   c_fg_col = colour;
}

// Set the background text colour directly, bypassing the colour/tint VDU variables
// cppcheck-suppress unusedFunction
void fb_set_c_bg_col(pixel_t colour) {
   c_bg_col = colour;
}

// Extracts the VDU gcol number (0..255) from the 8-bit colour number
// It is an error to use this function in high colour modes
// cppcheck-suppress unusedFunction
uint8_t fb_get_gcol_from_colnum(uint8_t colnum) {
   if (screen->ncolour < 255) {
      return (uint8_t)(colnum & screen->ncolour);
   } else if (screen->ncolour == 255) {
      //                                     7  6  5  4  3  2  1  0
      // The  8-bit colour number format is B3 G3 G2 R3 B2 R2 T1 T0
      // The      VDU gcol number format is B3 B2 G3 G2 R3 R2 T1 T0
      return (uint8_t)((colnum & 0x87) | ((colnum & 0x70) >> 1) | ((colnum & 0x08) << 3));
   } else {
      printf("Illegal use of get_gcol_from_colnum()\n\r");
      return 0;
   }
}

static void fb_emulator_vdu(unsigned int gpio)
{
   vdu_queue[vdu_wp] = GET_DATA(gpio);
   vdu_wp = (vdu_wp + 1) & (VDU_QSIZE - 1);
}

void fb_emulator_ram(unsigned int gpio)
{
   uint8_t  data = GET_DATA(gpio);
   uint32_t addr = GET_ADDR(gpio);
   Pi1MHz_MemoryWrite(addr, data);
}

void fb_emulator_init(uint8_t instance, uint8_t address)
{

  fb_initialize();
  fb_show_splash_screen();

  Pi1MHz_Register_Memory(WRITE_FRED, address, fb_emulator_vdu);
 // Create 6 bytes of RAM for vector code
  Pi1MHz_MemoryWrite(address+0, 0x8D);
  Pi1MHz_MemoryWrite(address+1, (uint8_t) address);
  Pi1MHz_MemoryWrite(address+2, 0XFC);
  Pi1MHz_MemoryWrite(address+3, 0x4c);

  Pi1MHz_Register_Memory(WRITE_FRED, address + 4, fb_emulator_ram);
  Pi1MHz_Register_Memory(WRITE_FRED, address + 5, fb_emulator_ram);

 // Pi1MHz_Register_Poll(fb_emulator_poll);
}