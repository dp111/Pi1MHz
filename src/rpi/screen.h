#ifndef SCREEN_H
#define SCREEN_H

#include <stdint.h>
#include <stdbool.h>

// Function declarations
uint32_t screen_allocate_buffer( uint32_t buffer_size, uint32_t * handle );
void screen_release_buffer( uint32_t handle );
void screen_create_YUV_plane( uint32_t planeno, uint32_t width, uint32_t height, uint32_t buffer );
void screen_create_RGB_plane( uint32_t planeno, uint32_t width , uint32_t height, float par, uint32_t scale_height, uint32_t colour_depth, uint32_t buffer );
void screen_release_plane( uint32_t planeno );
void screen_set_plane_position( uint32_t * plane, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t buffer );
void screen_plane_enable(uint32_t planeno, bool enable);
void screen_update_palette_entry(uint32_t entry, uint32_t r, uint32_t g, uint32_t b);
void screen_set_palette(uint32_t planeno, uint32_t palette, uint32_t flags);
uint32_t screen_get_palette_entry( uint32_t entry );

#endif // SCREEN_H