/* chainboot.h - restart the Pi into a kernel image held in RAM (kernel.now).

   Whoever receives an image (MTP, the webserver) checks it with
   chainboot_image_ok(), asks chainboot_refusal() whether a chain-boot is safe
   now, and hands it over with chainboot_request().  chainboot_poll() does the
   rest from the main loop. */
#ifndef CHAINBOOT_H
#define CHAINBOOT_H

#include <stdbool.h>
#include <stdint.h>

/* Room for an image: what MTP and the webserver allocate when the sender
   does not say how big it is, and the most either will accept. */
#define CHAINBOOT_MAX_IMAGE (4u * 1024u * 1024u)

/* Does this look like one of our kernels?  Every image this project builds
   begins with an ARM branch at offset 0. */
bool chainboot_image_ok(const uint8_t *image, uint32_t length);

/* NULL if a chain-boot may go ahead now, else why not, for the sender. */
const char *chainboot_refusal(void);

/* Restart into image, length bytes: a malloc'd buffer of capacity bytes,
   which chainboot now owns (it pads the length to 64 with zeros inside the
   capacity).  The jump happens from chainboot_poll, after a moment for the
   sender's answer to get out - unless chainboot_refusal() has changed its
   mind by then, when the image is dropped and the Pi carries on.  False if
   the image cannot be taken (no room for the padding): it is freed all the
   same, and the sender must answer with an error. */
bool chainboot_request(uint8_t *image, uint32_t length, uint32_t capacity);

/* Main-loop step; never returns once the jump is due.  Called from the
   polls that can make a request (USB, the webserver), so it needs no slot
   in the poll table. */
void chainboot_poll(void);

#endif
