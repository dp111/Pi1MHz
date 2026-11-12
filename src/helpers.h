#ifndef HELPERS_H
#define HELPERS_H

#include <stddef.h>
#include <stdint.h>

size_t helpers_screen_setup( char * helpscreen, size_t helpscreen_size);
void helpers_init( uint8_t instance , uint8_t address);
/* The help screen's version, build date and kernel letter (D = DEBUG, R =
   release), for /status: here so that only helpers.c is rebuilt when the
   generated gitversion.h changes. */
const char *helpers_firmware_version(void);
/* A helper page's last byte, which pages that need it hold their own number in (the RAM
   probe's do): -1 for no such page.  A 6502code.bin that predates the page has 0x60 there. */
int helpers_page_number(uint8_t page);
/* A helper page's 256 bytes as loaded from 6502code.bin, or NULL for no such page. */
const uint8_t *helpers_page(uint8_t page);

#endif /* HELPERS_H */