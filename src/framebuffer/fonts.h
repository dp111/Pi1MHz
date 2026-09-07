#ifndef _FONTS_H
#define _FONTS_H

#include <stddef.h>
#include "screen_modes.h"

#define DEFAULT_FONT 0

#define MAX_FONT_HEIGHT 32

typedef struct font_cat {
   // The raw font data itself
   const char *name;   // (max) 8 character ASCII name of the font
   const uint8_t *data;      // pointer to the raw font data
   int bytes_per_char; // Number of bytes of raw data per character
   int num_chars;      // Number of characters in the character set
   char offset;         // Offset (in bytes) to the first row of the character
   char shift;          // Offset (in bits) to the first column of the character
   char width;          // Width (in pixels) of the character
   char height;         // Height(in pixels) of the character

   // These control the way the font is rendered
   char spacing_w;
   char spacing_h;
   char scale_w;
   char scale_h;
} font_catalog_t;

typedef struct font {
   // The raw font data itself
   const char *name;   // (max) 8 character ASCII name of the font
   const uint8_t *data;      // pointer to the raw font data
   int bytes_per_char; // Number of bytes of raw data per character
   int num_chars;      // Number of characters in the character set
   char offset;         // Offset (in bytes) to the first row of the character
   char shift;          // Offset (in bits) to the first column of the character
   char width;          // Width (in pixels) of the character
   char height;         // Height(in pixels) of the character

   // These control the way the font is rendered
   char spacing_w;
   char spacing_h;
   char scale_w;
   char scale_h;
   char rounding;

   // The font number
   uint32_t number;

   // The working copy of the font data
   uint16_t *buffer;

   void  (*set_spacing_w)(struct font *font, char spacing_w);
   void  (*set_spacing_h)(struct font *font, char spacing_h);
   void    (*set_scale_w)(struct font *font, char scale_w);
   void    (*set_scale_h)(struct font *font, char scale_h);
   void   (*set_rounding)(struct font *font, char rounding);

   const char * (*get_name)(const struct font *font);
   uint32_t  (*get_number)(const struct font *font);
   char    (*get_spacing_w)(const struct font *font);
   char    (*get_spacing_h)(const struct font *font);
   char      (*get_scale_w)(const struct font *font);
   char      (*get_scale_h)(const struct font *font);
   char     (*get_rounding)(const struct font *font);
   int     (*get_overall_w)(const struct font *font);
   int     (*get_overall_h)(const struct font *font);

   void     (*write_char)(struct font *font, screen_mode_t *screen, int c, int x, int y, pixel_t fg_col, pixel_t bg_col);
   int       ( *read_char)(const struct font *font, screen_mode_t *screen, int x, int y,                        pixel_t bg_col);

} font_t;

/* initialize_font() copies a font_catalog_t over the start of a font_t with
   one memcpy, so font_t must begin with the catalog's members in the same
   order and the first member after them must lie beyond the copy.  Reorder
   or insert on either side and this fails to compile instead of silently
   corrupting every font. */
_Static_assert(offsetof(font_t, name)      == offsetof(font_catalog_t, name) &&
               offsetof(font_t, data)      == offsetof(font_catalog_t, data) &&
               offsetof(font_t, height)    == offsetof(font_catalog_t, height) &&
               offsetof(font_t, scale_h)   == offsetof(font_catalog_t, scale_h) &&
               offsetof(font_t, rounding)  >= sizeof(font_catalog_t),
               "font_t must start with font_catalog_t's members (initialize_font memcpy)");

const char * get_font_name(uint32_t num);

void initialize_font_by_number(uint32_t num, font_t *font);

void initialize_font_by_name(const char *name, font_t *font);

void define_character(const font_t *font, uint8_t c, const uint8_t *data);

#endif
