/* Pick the display mode from the monitor's EDID: its native resolution at
   the refresh rate Pi1MHz.cfg asks for (Display_refresh, default 50 Hz) -
   see display_mode.c. */
#ifndef DISPLAY_MODE_H
#define DISPLAY_MODE_H

/* Once, early in boot, before anything reads the display size. */
void display_mode_select(void);

/* One line for /status: what was found and what was done. */
const char *display_mode_report(void);

/* The EDID (block 0 and the first extension): 0, 128 or 256 bytes.  Read
   at boot if the mode decision needed it, otherwise on this call.  For the
   /edid diagnostic route (called from the webserver's poll slot). */
#include <stdint.h>
unsigned display_mode_edid(const uint8_t **bytes);

#endif
