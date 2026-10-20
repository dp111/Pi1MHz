#ifndef TELNETD_H
#define TELNETD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Start listening on telnetd_port (Pi1MHz.cfg).  Called once lwIP is up, next
   to webserver_init; does nothing when the key is absent or 0. */
void telnetd_init(void);

/* Main loop, from the serial redirect's poll.  beeb_attached is true while
   helper 20 has the Beeb's keyboard and RS423 output on the Pi: then this
   owns the redirect's byte stream, and the modem does not. */
void telnetd_poll(uint32_t now_us, bool beeb_attached);

void telnetd_status(char *buf, size_t len);   /* for /status */
bool telnetd_enabled(void);   /* telnetd_port set and not Telnetd_addr=-1: the auto-start ROM may hook the Beeb in */
void telnetd_tcp_status(char *buf, size_t len);   /* ... and its TCP side (telnetd_debug=1) */
bool telnetd_debug(void);   /* telnetd_debug=1 in Pi1MHz.cfg: the diagnostics are shown */

#endif
