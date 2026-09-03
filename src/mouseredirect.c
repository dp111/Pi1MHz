/*

Mouse re-director

Takes the Beeb mouse coordinates and plots a mouse pointer into the Beeb's
own display, the way the VFS ROM plots one into screen memory: save the
pixels under the sprite, draw it, and put them back before drawing it
somewhere else.  It is NOT a separate HVS plane - being part of the
computer's raster is what makes it land on the right pixel at any scale.

The ROM writes (screen AND mask) OR sprite through a 64-byte save-under in
its private workspace (ADFS-multi VFS_Mouse.asm, Move_Pointer_if_mouse_moved
and Restore_memory_under_mouse).  Our framebuffer is a pixel per byte or
more, not Beeb-packed bits, so the mask/sprite pair collapses to the three
states already encoded in mouse_pointer_data below: leave the screen alone,
paint the body in logical colour 1, or clear the halo to colour 0.  The
artwork and its 24x16 / 12x16 / 6x16 footprints are the ROM's own.

The coordinates the Beeb sends are the sprite's top-left corner - the ROM
has already subtracted the shape's hotspot - so nothing here needs a hotspot
table.  Like the ROM this erases and re-plots as a pair, driven by the Beeb writing
the last of the four bytes - the write that completes a position.

Takes 4 bytes of RAM

0 - X low
1 - X high
2 - Y low
3 - bits 0-3 Y high, bits 4-7 pointer type 0 1 2 3 (>=4 - off)

*/

#include <stdio.h>
#include <inttypes.h>
#include "Pi1MHz.h"
#include "framebuffer/framebuffer.h"
#include "framebuffer/screen_modes.h"

_Static_assert(sizeof(void*) == 4 ,"Must have 32bit pointers");

#define PTRMAX        4
#define PTRMODE0WIDTH 24
#define PTRMODE1WIDTH 12
#define PTRMODE2WIDTH 6
#define PTRMODEHEIGHT 16

_Alignas(16) static char mouse_pointer_data[] = {
#define B (1)
#define i 16

0,0,0,0,0,0,i,i,i,i,i,i,i,i,i,i,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,i,i,B,B,B,B,B,B,i,i,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,i,i,B,B,B,B,B,B,i,i,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,i,i,B,B,B,B,B,B,i,i,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,i,i,B,B,B,B,B,B,i,i,0,0,0,0,0,0,0,0,
i,i,i,i,i,i,i,i,B,B,B,B,B,B,i,i,i,i,i,i,i,i,0,0,
i,i,B,B,B,B,B,B,B,B,B,B,B,B,B,B,B,B,B,B,i,i,0,0,
i,i,B,B,B,B,B,B,B,B,B,B,B,B,B,B,B,B,B,B,i,i,0,0,
i,i,B,B,B,B,B,B,B,B,B,B,B,B,B,B,B,B,B,B,i,i,0,0,
i,i,i,i,i,i,i,i,B,B,B,B,B,B,i,i,i,i,i,i,i,i,0,0,
0,0,0,0,0,0,i,i,B,B,B,B,B,B,i,i,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,i,i,B,B,B,B,B,B,i,i,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,i,i,B,B,B,B,B,B,i,i,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,i,i,B,B,B,B,B,B,i,i,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,i,i,i,i,i,i,i,i,i,i,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,

0,0,0,0,0,0,0,0,0,0,i,i,i,i,i,i,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,i,i,i,i,B,B,B,B,B,B,i,i,i,i,0,0,0,0,
0,0,0,0,i,i,B,B,B,B,B,B,B,B,B,B,B,B,B,B,i,i,0,0,
0,0,0,0,i,i,B,B,B,B,i,i,i,i,i,i,B,B,B,B,i,i,0,0,
0,0,i,i,B,B,B,B,i,i,0,0,0,0,0,0,i,i,B,B,B,B,i,i,
0,0,i,i,B,B,B,B,i,i,0,0,0,0,0,0,i,i,B,B,B,B,i,i,
0,0,i,i,B,B,B,B,i,i,0,0,0,0,0,0,i,i,B,B,B,B,i,i,
0,0,0,0,i,i,B,B,B,B,i,i,i,i,i,i,B,B,B,B,i,i,0,0,
0,0,0,0,i,i,B,B,B,B,B,B,B,B,B,B,B,B,B,B,i,i,0,0,
0,0,0,0,0,0,i,i,i,i,B,B,B,B,B,B,B,B,i,i,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,i,i,i,i,B,B,B,B,i,i,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,i,i,i,i,B,B,B,B,i,i,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,i,i,B,B,B,B,i,i,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,i,i,B,B,B,B,i,i,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,i,i,B,B,B,B,i,i,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,i,i,i,i,0,0,

i,i,i,i,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // mode 0, pointer 2
i,i,B,B,i,i,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
i,i,B,B,B,B,i,i,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
i,i,B,B,B,B,B,B,i,i,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
i,i,B,B,B,B,B,B,B,B,i,i,0,0,0,0,0,0,0,0,0,0,0,0,
i,i,B,B,B,B,B,B,B,B,B,B,i,i,0,0,0,0,0,0,0,0,0,0,
i,i,B,B,B,B,B,B,B,B,B,B,B,B,i,i,0,0,0,0,0,0,0,0,
i,i,B,B,B,B,B,B,B,B,i,i,i,i,0,0,0,0,0,0,0,0,0,0,
i,i,B,B,i,i,B,B,B,B,i,i,0,0,0,0,0,0,0,0,0,0,0,0,
i,i,i,i,i,i,i,i,B,B,B,B,i,i,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,i,i,B,B,B,B,i,i,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,i,i,B,B,B,B,i,i,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,i,i,B,B,B,B,i,i,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,i,i,B,B,B,B,i,i,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,i,i,B,B,B,B,i,i,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,i,i,i,i,0,0,0,0,0,0,0,0,

0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,i,i,i,i,  // mode 0, pointer 3
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,i,i,B,B,i,i,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,i,i,B,B,B,B,i,i,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,i,i,B,B,B,B,B,B,i,i,
0,0,0,0,0,0,0,0,0,0,0,0,i,i,B,B,B,B,B,B,B,B,i,i,
0,0,0,0,0,0,0,0,0,0,i,i,B,B,B,B,B,B,B,B,B,B,i,i,
0,0,0,0,0,0,0,0,i,i,B,B,B,B,B,B,B,B,B,B,B,B,i,i,
0,0,0,0,0,0,0,0,0,0,i,i,i,i,B,B,B,B,B,B,B,B,i,i,
0,0,0,0,0,0,0,0,0,0,0,0,i,i,B,B,B,B,i,i,B,B,i,i,
0,0,0,0,0,0,0,0,0,0,i,i,B,B,B,B,i,i,i,i,i,i,i,i,
0,0,0,0,0,0,0,0,0,0,i,i,B,B,B,B,i,i,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,i,i,B,B,B,B,i,i,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,i,i,B,B,B,B,i,i,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,i,i,B,B,B,B,i,i,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,i,i,B,B,B,B,i,i,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,i,i,i,i,0,0,0,0,0,0,0,0,0,0,0,0,

0,0,0,i,i,i,i,i,0,0,0,0,     // mode 1, pointer 0,
0,0,0,i,B,B,B,i,0,0,0,0,
0,0,0,i,B,B,B,i,0,0,0,0,
0,0,0,i,B,B,B,i,0,0,0,0,
0,0,0,i,B,B,B,i,0,0,0,0,
i,i,i,i,B,B,B,i,i,i,i,0,
i,B,B,B,B,B,B,B,B,B,i,0,
i,B,B,B,B,B,B,B,B,B,i,0,
i,B,B,B,B,B,B,B,B,B,i,0,
i,i,i,i,B,B,B,i,i,i,i,0,
0,0,0,i,B,B,B,i,0,0,0,0,
0,0,0,i,B,B,B,i,0,0,0,0,
0,0,0,i,B,B,B,i,0,0,0,0,
0,0,0,i,B,B,B,i,0,0,0,0,
0,0,0,i,i,i,i,i,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,

0,0,0,0,0,i,i,i,0,0,0,0,  // mode 1, pointer 1,
0,0,0,i,i,B,B,B,i,i,0,0,
0,0,i,B,B,B,B,B,B,B,i,0,
0,0,i,B,B,i,i,i,B,B,i,0,
0,i,B,B,i,0,0,0,i,B,B,i,
0,i,B,B,i,0,0,0,i,B,B,i,
0,i,B,B,i,0,0,0,i,B,B,i,
0,0,i,B,B,i,i,i,B,B,i,0,
0,0,i,B,B,B,B,B,B,B,i,0,
0,0,0,i,i,B,B,B,B,i,0,0,
0,0,0,0,i,i,i,B,B,i,0,0,
0,0,0,0,0,0,0,i,B,B,i,0,
0,0,0,0,0,0,0,i,B,B,i,0,
0,0,0,0,0,0,0,0,i,B,B,i,
0,0,0,0,0,0,0,0,i,B,B,i,
0,0,0,0,0,0,0,0,0,i,i,0,

i,i,0,0,0,0,0,0,0,0,0,0,   // mode 1, pointer 2
i,B,i,0,0,0,0,0,0,0,0,0,
i,B,B,i,0,0,0,0,0,0,0,0,
i,B,B,B,i,0,0,0,0,0,0,0,
i,B,B,B,B,i,0,0,0,0,0,0,
i,B,B,B,B,B,i,0,0,0,0,0,
i,B,B,B,B,B,B,i,0,0,0,0,
i,B,B,B,B,i,i,0,0,0,0,0,
i,B,i,B,B,i,0,0,0,0,0,0,
i,i,i,i,B,B,i,0,0,0,0,0,
0,0,0,i,B,B,i,0,0,0,0,0,
0,0,0,0,i,B,B,i,0,0,0,0,
0,0,0,0,i,B,B,i,0,0,0,0,
0,0,0,0,0,i,B,B,i,0,0,0,
0,0,0,0,0,i,B,B,i,0,0,0,
0,0,0,0,0,0,i,i,0,0,0,0,

0,0,0,0,0,0,0,0,0,0,i,i,    // mode 1, pointer 3
0,0,0,0,0,0,0,0,0,i,B,i,
0,0,0,0,0,0,0,0,i,B,B,i,
0,0,0,0,0,0,0,i,B,B,B,i,
0,0,0,0,0,0,i,B,B,B,B,i,
0,0,0,0,0,i,B,B,B,B,B,i,
0,0,0,0,i,B,B,B,B,B,B,i,
0,0,0,0,0,0,i,B,B,B,B,i,
0,0,0,0,0,0,i,B,B,i,B,i,
0,0,0,0,0,i,B,B,i,i,i,i,
0,0,0,0,0,i,B,B,i,0,0,0,
0,0,0,0,i,B,B,i,0,0,0,0,
0,0,0,0,i,B,B,i,0,0,0,0,
0,0,0,i,B,B,i,0,0,0,0,0,
0,0,0,i,B,B,i,0,0,0,0,0,
0,0,0,0,i,i,0,0,0,0,0,0,

0,0,i,i,i,0,    // mode 2 pointer 0,
0,0,i,B,i,0,
0,0,i,B,i,0,
0,0,i,B,i,0,
0,0,i,B,i,0,
0,0,i,B,i,0,
0,i,B,B,B,i,
0,i,B,B,B,i,
0,i,B,B,B,i,
0,0,i,B,i,0,
0,0,i,B,i,0,
0,0,i,B,i,0,
0,0,i,B,i,0,
0,0,i,B,i,0,
0,0,i,i,i,0,
0,0,0,0,0,0,

0,0,i,i,0,0,    // mode 2 pointer 1
0,i,B,B,i,0,
0,i,B,B,i,0,
i,B,i,i,B,i,
i,B,i,i,B,i,
i,B,i,i,B,i,
i,B,i,i,B,i,
i,B,i,i,B,i,
0,i,B,B,i,0,
0,i,B,B,i,0,
0,0,i,B,i,0,
0,0,0,i,B,i,
0,0,0,i,B,i,
0,0,0,i,B,i,
0,0,0,i,B,i,
0,0,0,0,i,0,

i,i,0,0,0,0,     // mode 2 pointer 2
i,B,i,0,0,0,
i,B,i,0,0,0,
i,B,B,i,0,0,
i,B,B,i,0,0,
i,B,B,B,i,0,
i,B,B,B,i,0,
i,B,B,i,0,0,
i,B,B,i,0,0,
i,i,i,B,i,0,
0,0,i,B,i,0,
0,0,i,B,i,0,
0,0,i,B,i,0,
0,0,0,i,B,i,
0,0,0,i,B,i,
0,0,0,0,i,0,

0,0,0,0,i,i,     // mode 2 pointer 3
0,0,0,i,B,i,
0,0,0,i,B,i,
0,0,i,B,B,i,
0,0,i,B,B,i,
0,i,B,B,B,i,
0,i,B,B,B,i,
0,0,i,B,B,i,
0,0,i,B,B,i,
0,i,B,i,i,i,
0,i,B,i,0,0,
0,i,B,i,0,0,
0,i,B,i,0,0,
i,B,i,0,0,0,
i,B,i,0,0,0,
0,i,0,0,0,0

#undef B
#undef i
};

// mode 0 par = 0.5 24x16
// mode 1 par = 1.0 16x16
// mode 2 par = 2.0 8x16

static uint8_t fred_address;

/* Set by the write of the LAST of the four bytes: that write is what says a
   new position is complete.  FIQ only latches it - the plot happens in the
   poll loop, where the rest of the framebuffer is written. */
static volatile bool moved;

/* What is on the screen right now, and the pixels it is covering.  The
   restore has to reproduce the plot exactly, so it works from the shape and
   position that were used to draw, never from the incoming ones - the same
   reason the ROM keeps VFS_N914_MouseX2 alongside the live X. */
static const char *drawn_shape;         /* NULL = nothing is drawn */
static int         drawn_x, drawn_y;    /* top-left, framebuffer coords */
static int         drawn_width;
static pixel_t     under[PTRMODE0WIDTH * PTRMODEHEIGHT];

/* Walk the sprite once.  restore = put the saved pixels back, otherwise save
   what is there and paint.  set_pixel takes bottom-up coordinates, the same
   way up as the Beeb's, so a row of the shape counts DOWN the screen.

   Clipped to the screen, as the ROM clips: it abandons a byte-column whose
   X has left 0..1279 and skips a character cell whose address has left
   $3000-$7FFF (VFS_Mouse.asm @LB13D).  The pointer is up to 24 pixels wide
   while the Beeb only constrains its hotspot, so the sprite can overhang an
   edge.  Save and restore walk the same test, so what was saved is what is
   put back. */
static void mouse_pointer_walk(bool restore)
{
    const screen_mode_t *screen = fb_get_current_screen_mode();
    if (screen == NULL || drawn_shape == NULL)
        return;

    const pixel_t  body = screen->get_colour(screen, 1);
    const pixel_t  halo = screen->get_colour(screen, 0);
    const int      w    = (int)screen->width;
    const int      h    = (int)screen->height;

    for (int row = 0; row < PTRMODEHEIGHT; row++)
    {
        const int y = drawn_y - row;
        if ((y < 0) || (y >= h))
            continue;
        for (int col = 0; col < drawn_width; col++)
        {
            const int x = drawn_x + col;
            if ((x < 0) || (x >= w))
                continue;
            const size_t i = (size_t)row * (size_t)drawn_width + (size_t)col;
            const char v = drawn_shape[i];
            if (v == 0)
                continue;                    /* mask FF, sprite 00: untouched */
            if (restore)
                screen->set_pixel(screen, x, y, under[i]);
            else
            {
                under[i] = screen->get_pixel(screen, x, y);
                screen->set_pixel(screen, x, y, (v == 1) ? body : halo);
            }
        }
    }
}

static void mouse_pointer_erase(void)
{
    if (drawn_shape == NULL)
        return;
    mouse_pointer_walk(true);
    drawn_shape = NULL;
}

/* Erase and re-plot, once per completed position from the Beeb. */
void mouse_redirect_move_mouse(void)
{
    if (!moved)
        return;
    moved = false;

    const screen_mode_t *screen = fb_get_current_screen_mode();
    if (screen == NULL)
        return;

    int32_t mouse_x = (int32_t)((int16_t)(Pi1MHz_MemoryRead((uint32_t)(fred_address + 0)) | (Pi1MHz_MemoryRead((uint32_t)(fred_address + 1))<<8)));
    int32_t mouse_y = (int32_t)((Pi1MHz_MemoryRead((uint32_t)(fred_address + 2)) | (Pi1MHz_MemoryRead((uint32_t)(fred_address + 3))<<8)) & 0x0FFF);
    uint8_t mouse_pointer = Pi1MHz_MemoryRead((uint32_t)(fred_address + 3))>>4;
    LOG_DEBUG("Mouse x %"PRIi32" y %"PRIi32" Pointer %u\r\n", mouse_x, mouse_y, mouse_pointer);

    /* Lift the old one before anything else: the pixels under it are only
       valid for where it was drawn. */
    mouse_pointer_erase();
    if (mouse_pointer >= PTRMAX)
        return;                            /* type >= 4 is "off" */

    /* One shape per mode, the ROM's own artwork.  Anything past MODE 2 has
       none - Setup_mouse_pointer_workspace refuses those with Bad MODE. */
    const char *shape;
    int width;
    switch (screen->mode_num)
    {
    case 0: width = PTRMODE0WIDTH;
            shape = &mouse_pointer_data[PTRMODE0WIDTH*PTRMODEHEIGHT*mouse_pointer];
            break;
    case 1: width = PTRMODE1WIDTH;
            shape = &mouse_pointer_data[(PTRMODE0WIDTH*PTRMODEHEIGHT*PTRMAX)
                                        + PTRMODE1WIDTH*PTRMODEHEIGHT*mouse_pointer];
            break;
    case 2: width = PTRMODE2WIDTH;
            shape = &mouse_pointer_data[(PTRMODE0WIDTH*PTRMODEHEIGHT*PTRMAX)
                                        + (PTRMODE1WIDTH*PTRMODEHEIGHT*PTRMAX)
                                        + PTRMODE2WIDTH*PTRMODEHEIGHT*mouse_pointer];
            break;
    default:
        return;
    }

    /* BBC mouse coordinates are 0-1279 by 0-1023 from the bottom left, which
       is the way up set_pixel already works - so this scales, and does not
       flip.  The scale comes from the mode table because the three modes put
       different pixel counts across the same 40 us of raster. */
    drawn_x     = (mouse_x * (int32_t)screen->width) / 1280;
    drawn_y     = (mouse_y * (int32_t)screen->height) / 1024;
    drawn_width = width;
    drawn_shape = shape;
    mouse_pointer_walk(false);
}

/* Called when the screen mode changes (screen_modes.c).  Forget the pointer
   WITHOUT restoring it: the buffer it was drawn in is about to be released
   and the geometry under our feet is already the new mode's, so putting the
   old pixels back would write the wrong place in a doomed buffer.  The ROM
   loses its pointer across a MODE too - it does not even rebuild its shifted
   sprites until the next VFS command. */
void mouse_redirect_mouseoff(void)
{
    drawn_shape = NULL;
}

static void mouse_redirect_position_complete(unsigned int gpio)
{
    Pi1MHz_MemoryWrite(GET_ADDR(gpio), GET_DATA(gpio));
    moved = true;
}

void mouse_redirect_init(uint8_t instance, uint8_t address)
{
    // register call backs
    fred_address = address;
    Pi1MHz_Register_Memory(WRITE_FRED, (address+0u), Pi1MHz_EmulatedMemoryByte );
    Pi1MHz_Register_Memory(WRITE_FRED, (address+1u), Pi1MHz_EmulatedMemoryByte );
    Pi1MHz_Register_Memory(WRITE_FRED, (address+2u), Pi1MHz_EmulatedMemoryByte );
    Pi1MHz_Register_Memory(WRITE_FRED, (address+3u), mouse_redirect_position_complete );
    Pi1MHz_Register_Poll(mouse_redirect_move_mouse, "mouse");
}