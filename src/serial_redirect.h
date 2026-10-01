#ifndef SERIAL_REDIRECT_H
#define SERIAL_REDIRECT_H

#include <stddef.h>
#include <stdint.h>

void serial_redirect_init(uint8_t instance, uint8_t address);
uint8_t serial_redirect_address(void);      /* stub's FRED offset, 0 = disabled */
void serial_redirect_status(char *buf, size_t len);

/* Main loop only: the bytes the Beeb has sent, and room for / bytes to it. */
size_t serial_redirect_read(uint8_t *dst, size_t max);
size_t serial_redirect_room(void);
size_t serial_redirect_write(const uint8_t *src, size_t len);

#endif
