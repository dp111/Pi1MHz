// vduhost - run Pi1MHz's real VDU driver (src/framebuffer/*) on a PC.
//
// Reads a test script on stdin and writes the plotted pixels to stdout in the
// same "L .../P x y" record format the Beeb-side BASIC program emits, so the
// two can be diffed directly.  See docs/dev/vdu-rom-conformance.md.
//
//   V <byte>...          feed these bytes to fb_writec (one VDU command)
//   L <text>             start a new record: echoed verbatim as "L <text>"
//   DUMP <x0> <y0> <x1> <y1>   emit "P x y" for every set pixel in the box,
//                              in pixel coordinates, x-major (as BASIC scans)
//   CLG                  clear the graphics area (VDU 16)
//
// Coordinates in DUMP are *pixels*; the VDU bytes use OS units, exactly as on
// the Beeb.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "framebuffer.h"
#include "primitives.h"

int main(void)
{
   char line[4096];

   fb_emulator_init(0, 0xd0);

   while (fgets(line, sizeof line, stdin)) {
      char *p = line;
      while (*p == ' ' || *p == '\t') p++;

      if (!strncmp(p, "L ", 2)) {
         fputs("L ", stdout);
         fputs(p + 2, stdout);
         continue;
      }
      if (!strncmp(p, "V ", 2)) {
         for (char *t = strtok(p + 2, " \t\r\n"); t; t = strtok(NULL, " \t\r\n"))
            fb_writec((char)(uint8_t)strtol(t, NULL, 0));
         fb_process_vdu_queue();
         continue;
      }
      if (!strncmp(p, "CLG", 3)) {
         fb_writec(16);
         fb_process_vdu_queue();
         continue;
      }
      if (!strncmp(p, "DUMP", 4)) {
         int x0, y0, x1, y1;
         if (sscanf(p + 4, "%d %d %d %d", &x0, &y0, &x1, &y1) != 4) continue;
         screen_mode_t *screen = fb_get_current_screen_mode();
         // Match the BASIC scan order: x outer, y inner, both ascending
         for (int x = x0; x <= x1; x++)
            for (int y = y0; y <= y1; y++)
               if (prim_get_pixel(screen, x, y))
                  printf("P %d %d\n", x, y);
         continue;
      }
   }
   fputs("ENDOFTEST\n", stdout);
   return 0;
}
