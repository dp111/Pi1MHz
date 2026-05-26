/*

Mouse re-director

Takes the Beeb mouse coordinates and displays a mouse pointer on the screen

Takes 5 bytes of RAM

0 - X low
1 - X high
2 - Y low
3 - Y high
4 - pointer type
  - 255 - off

*/
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "Pi1MHz.h"
#include "framebuffer/framebuffer.h"
#include "../rpi/screen.h"

//fb_get_current_screen_mode().mode_num

// abcdefghijklmnopqrstuvwxyz
// ABCDEFGHIJKLMNOPQRSTUVWXYZ

static char mouse_pointer_data[] = {
#define o 0
#define B (255)
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

#undef o
#undef H
#undef i
};

// mode 0 par = 0.5 24x16
// mode 1 par = 1.0 16x16
// mode 2 par = 2.0 8x16

#define MOUSE_PLANE 2

static uint8_t fred_address;
static uint32_t mouse_x;
static uint32_t mouse_y;
static uint8_t mouse_pointer;

static void mouse_redirect_move_mouse_data(unsigned int gpio)
{
    Pi1MHz_MemoryWrite(GET_ADDR(gpio), GET_DATA(gpio));
}

static void mouse_redirect_move_mouse(unsigned int gpio)
{
    mouse_x = Pi1MHz_MemoryRead(fred_address + 0) | (Pi1MHz_MemoryRead(fred_address+1)<<8);
    mouse_y = Pi1MHz_MemoryRead(fred_address + 2) | (GET_DATA(gpio)<<8);
    LOG_DEBUG("Mouse x %"PRIu32" y %"PRIu32" \r\n", mouse_x, mouse_y);
}

static void mouse_redirect_change_pointer(unsigned int gpio)
{
    mouse_pointer = GET_DATA(gpio);
    LOG_DEBUG("Mouse pointer %u mode %u \r\n", mouse_pointer, fb_get_current_screen_mode()->mode_num);
}

void mouse_redirect_init(uint8_t instance, uint8_t address)
{
    // register call backs
    fred_address = address;
    Pi1MHz_Register_Memory(WRITE_FRED, address+0, mouse_redirect_move_mouse_data );
    Pi1MHz_Register_Memory(WRITE_FRED, address+1, mouse_redirect_move_mouse_data );
    Pi1MHz_Register_Memory(WRITE_FRED, address+2, mouse_redirect_move_mouse_data );
    Pi1MHz_Register_Memory(WRITE_FRED, address+3, mouse_redirect_move_mouse );
    Pi1MHz_Register_Memory(WRITE_FRED, address+4, mouse_redirect_change_pointer );

    screen_create_RGB_plane(MOUSE_PLANE,24, 16, 1.0, 0, 3, (uint32_t) mouse_pointer_data);
    screen_set_palette( MOUSE_PLANE, 0, 2 );
    screen_plane_enable(MOUSE_PLANE, true);
}