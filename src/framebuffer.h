// framebuffer.h

#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

enum {
   PALETTE_DEFAULT,
   PALETTE_INVERSE,
   PALETTE_MONO1,
   PALETTE_MONO2,
   PALETTE_RED,
   PALETTE_GREEN,
   PALETTE_BLUE,
   PALETTE_NOT_RED,
   PALETTE_NOT_GREEN,
   PALETTE_NOT_BLUE,
   PALETTE_ATOM_COLOUR_NORMAL,
   PALETTE_ATOM_COLOUR_EXTENDED,
   PALETTE_ATOM_COLOUR_ACORN,
   PALETTE_ATOM_MONO,
   NUM_PALETTES
};

void fb_emulator_init(uint8_t instance, int address);
/*
void fb_initialize();

void fb_writec(int c);

void fb_writes(char *string);

void fb_putpixel(int x, int y, unsigned int colour);

void fb_draw_line(int x, int y, int x2, int y2, unsigned int colour);

void fb_fill_triangle(int x, int y, int x2, int y2, int x3, int y3, unsigned int color);

uint32_t fb_get_address();

uint32_t fb_get_width();

uint32_t fb_get_height();

uint32_t fb_get_bpp32();
*/
#endif