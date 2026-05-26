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

//fb_get_current_screen_mode().mode_num

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
}