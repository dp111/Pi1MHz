#ifndef SERIAL_MODEM_H
#define SERIAL_MODEM_H

#include <stddef.h>
#include <stdint.h>

/* The Hayes-style modem behind the serial redirect: AT commands from the
   Beeb's RS423 stream, and ATD opens a TCP connection instead of dialling. */
void modem_reset(void);              /* defaults, and hang up */
void modem_poll(uint32_t now_us);    /* main loop, from the redirect's poll */
void modem_status(char *buf, size_t len);   /* for /status */

#endif
