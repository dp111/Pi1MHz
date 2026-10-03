/* usb_mouse.h - a USB mouse on the Pi's port in host mode (usb_mode=host),
   read by the Beeb through the mouse redirect's registers (mouseredirect.c). */
#ifndef USB_MOUSE_H
#define USB_MOUSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* FIQ-safe: the movement since the last latch, clamped to 14-bit two's
   complement, as the four bytes the Beeb reads at &FCAC-&FCAF:
     out[0] X bits 0-7
     out[1] bits 0-5 X bits 8-13, bit 6 left button, bit 7 right button
     out[2] Y bits 0-7                      (Y up is positive, as on the Beeb)
     out[3] bits 0-5 Y bits 8-13, bit 6 middle button, bit 7 a mouse is there
   Reading takes everything built up since the last read; movement beyond
   what one read can carry is dropped as it arrives. */
void usb_mouse_latch(uint8_t out[4]);

/* For /status: what is plugged in, and what it has done. */
void usb_mouse_status(char *buf, size_t len);

#endif
