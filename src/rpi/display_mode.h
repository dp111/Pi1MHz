/* Pick the display mode from the monitor's EDID: its native resolution at
   the refresh rate Pi1MHz.cfg asks for (Display_refresh, default 50 Hz) -
   see display_mode.c. */
#ifndef DISPLAY_MODE_H
#define DISPLAY_MODE_H

/* Once, early in boot, before anything reads the display size. */
void display_mode_select(void);

/* One line for /status: what was found and what was done. */
const char *display_mode_report(void);

#endif
