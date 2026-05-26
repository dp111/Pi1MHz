/*

Mouse re-director

Takes the Beeb mouse coordinates and displays a mouse pointer on the screen

Takes 5 bytes of RAM

0 - X low
1 - X high
2 - Y low
3 - Y high
4 - pointer type 0 1 2 3
  - 255 - off

*/

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "Pi1MHz.h"
#include "framebuffer/framebuffer.h"
#include "rpi/screen.h"

// abcdefghijklmnopqrstuvwxyz
// ABCDEFGHIJKLMNOPQRSTUVWXYZ

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

#undef H
#undef i
};

// mode 0 par = 0.5 24x16
// mode 1 par = 1.0 16x16
// mode 2 par = 2.0 8x16

#define MOUSE_PLANE 2

static bool change;

static uint8_t fred_address;
static uint8_t lastmouse_pointer=255;

static void mouse_redirect_move_mouse_data(unsigned int gpio)
{
    Pi1MHz_MemoryWrite(GET_ADDR(gpio), GET_DATA(gpio));
}

static void mouse_redirect_mouse_type(unsigned int gpio)
{
    Pi1MHz_MemoryWrite(GET_ADDR(gpio), GET_DATA(gpio));
    change = true;
}


void mouse_redirect_move_mouse()
{
    int32_t mouse_x;
    int32_t mouse_y;
    uint8_t mouse_pointer;

    if (!change)
        return;

    change = false;
    mouse_x = (int32_t)((int16_t)(Pi1MHz_MemoryRead(fred_address + 0) | (Pi1MHz_MemoryRead(fred_address + 1)<<8)));
    mouse_y = (int32_t)((int16_t)(Pi1MHz_MemoryRead(fred_address + 2) | (Pi1MHz_MemoryRead(fred_address + 3)<<8)));
    mouse_pointer = Pi1MHz_MemoryRead(fred_address + 4);
    LOG_DEBUG("Mouse x %"PRIi32" y %"PRIi32" Pointer %u Last pointer %u\r\n", mouse_x, mouse_y, mouse_pointer, lastmouse_pointer);

    if (lastmouse_pointer != mouse_pointer)
    {
        lastmouse_pointer = mouse_pointer;
        screen_plane_enable(MOUSE_PLANE, false);
        if (mouse_pointer == 255u)
            return;
        LOG_DEBUG("*** Mouse pointer %u\r\n", mouse_pointer);
        switch (fb_get_current_screen_mode()->mode_num)
        {
        case 0: screen_create_RGB_plane(MOUSE_PLANE,PTRMODE0WIDTH, PTRMODEHEIGHT , 0.5, 256, 3, (uint32_t) &mouse_pointer_data[PTRMODE0WIDTH*PTRMODEHEIGHT*mouse_pointer]);
                screen_set_palette( MOUSE_PLANE, 2, 0 );
                break;
        case 1: screen_create_RGB_plane(MOUSE_PLANE,PTRMODE1WIDTH, PTRMODEHEIGHT , 1.0, 256, 3, (uint32_t) &mouse_pointer_data[(PTRMODE0WIDTH*PTRMODEHEIGHT*PTRMAX) + PTRMODE1WIDTH*PTRMODEHEIGHT*mouse_pointer]);
                screen_set_palette( MOUSE_PLANE, 2, 0 );
                break;
        case 2: screen_create_RGB_plane(MOUSE_PLANE,PTRMODE2WIDTH, PTRMODEHEIGHT , 2.0, 256, 3, (uint32_t) &mouse_pointer_data[(PTRMODE0WIDTH*PTRMODEHEIGHT*PTRMAX) + (PTRMODE1WIDTH*PTRMODEHEIGHT*PTRMAX) + PTRMODE2WIDTH*PTRMODEHEIGHT*mouse_pointer]);
                screen_set_palette( MOUSE_PLANE, 2, 0 );
                break;
        default:
            return;
            break;
        }
    }

// BBC coordinates are 0-1279, 0-1023 origin bottom left
// Screen coordinates are 0-319, 0-255 origin top left

    switch (fb_get_current_screen_mode()->mode_num)
    {
    case 0: mouse_y = 255 - (mouse_y / 2);
            mouse_x = mouse_x / 2;
            break;
    case 1: mouse_y = 255 - (mouse_y / 4);
            mouse_x = mouse_x / 4;
            break;
    case 2: mouse_y = 255 - (mouse_y / 4);
            mouse_x = mouse_x / 4;
            break;
    default : break;
    }

  //  LOG_DEBUG("calc Mouse x %"PRIi32" y %"PRIi32" \r\n", mouse_x, mouse_y);

    screen_set_plane_position( MOUSE_PLANE, mouse_x, mouse_y );
    screen_plane_enable(MOUSE_PLANE, true);
}

void mouse_redirect_mouseoff(void)
{
    screen_plane_enable(MOUSE_PLANE, false);
    lastmouse_pointer = 255;
}

void mouse_redirect_init(uint8_t instance, uint8_t address)
{
    // register call backs
    fred_address = address;
    Pi1MHz_Register_Memory(WRITE_FRED, address+0, mouse_redirect_move_mouse_data );
    Pi1MHz_Register_Memory(WRITE_FRED, address+1, mouse_redirect_move_mouse_data );
    Pi1MHz_Register_Memory(WRITE_FRED, address+2, mouse_redirect_move_mouse_data );
    Pi1MHz_Register_Memory(WRITE_FRED, address+3, mouse_redirect_move_mouse_data );
    Pi1MHz_Register_Memory(WRITE_FRED, address+4, mouse_redirect_mouse_type );
}