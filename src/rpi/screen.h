#ifndef SCREEN_H
#define SCREEN_H

#include <stdint.h>
#include <stdbool.h>

// Function declarations
uint32_t screen_allocate_buffer( uint32_t buffer_size, uint32_t * handle );
void screen_release_buffer( uint32_t handle );
void screen_create_YUV420_plane( uint32_t planeno, uint32_t width, uint32_t height, uint32_t buffer );
void screen_set_YUV_pointers( uint32_t planeno, uint32_t y, uint32_t cb, uint32_t cr );
void screen_create_RGB_plane( uint32_t planeno, uint32_t width , uint32_t height, float par, uint32_t scale_height, uint32_t colour_depth, uint32_t buffer );
void screen_release_plane( uint32_t planeno );
void screen_set_plane_position( uint32_t planeno, int x, int y );
void screen_plane_enable(uint32_t planeno, bool enable);
void screen_plane_alpha(uint32_t planeno, uint32_t alpha);
void screen_update_palette_entry(uint32_t entry, uint32_t r, uint32_t g, uint32_t b);
void screen_set_highlight(bool on);   /* VP5 *VOHIGHLIGHT keyed-palette inverse */
void screen_plane_gate(uint32_t planeno, bool gated);   /* mixer-level hide: wanted && !gated shows */
void screen_mixer_reset(void);   /* Beeb reset: clear gates + highlight */
void screen_dim_frame(bool on);  /* VP5: dim the band outside the computer's raster */
void screen_geometry_report(uint32_t planeno, uint32_t *disp_w, uint32_t *disp_h, uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h, uint32_t *src_w, uint32_t *src_h);
void screen_set_palette(uint32_t planeno, uint32_t palette, uint32_t flags);
uint32_t screen_get_palette_entry( uint32_t entry );
void screen_set_vsync(bool enable);
bool screen_check_vsync(void);
uint32_t screen_refresh_mhz(void);   /* measured refresh, millihertz (0 = not yet) */
uint32_t screen_vsync_count(void);   /* end-of-frame counter, for tear-free flips */

#endif // SCREEN_H