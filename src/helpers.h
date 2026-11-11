#ifndef HELPERS_H
#define HELPERS_H

size_t helpers_screen_setup( char * helpscreen, size_t helpscreen_size);
void helpers_init( uint8_t instance , uint8_t address);
/* The help screen's version, build date and kernel letter (D = DEBUG, R =
   release), for /status: here so that only helpers.c is rebuilt when the
   generated gitversion.h changes. */
const char *helpers_firmware_version(void);

#endif /* HELPERS_H */