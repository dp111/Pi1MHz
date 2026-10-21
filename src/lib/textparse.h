/* textparse.h - the small text parsers several services share: a TCP/UDP
 * port number and a dotted-quad IPv4 address, as they appear in Pi1MHz.cfg
 * values and in the URLs the Beeb hands the network services.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Why not lwIP's ip4addr_aton: it accepts the BSD inet_aton forms - "10" is
 * 0.0.0.10, "10.1" is 10.0.0.1, "0x0a.1.2.3" is hex and "010.1.2.3" is
 * OCTAL (8.1.2.3).  A user typing 192.168.001.010 into Pi1MHz.cfg means
 * .1.10, not .1.8, and a host name made of digits must not be mistaken for
 * an address and skip DNS.  So the parse here is strict decimal: exactly
 * four octets of one to three digits each, 0-255.
 */
#ifndef PI1MHZ_TEXTPARSE_H
#define PI1MHZ_TEXTPARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A port number spelled as s[0..len): one or more decimal digits and nothing
   else, 1-65535.  0 is refused - no service listens or connects there, so a
   0 is a mistake, and a caller that means "off" by 0 gets false, which every
   caller treats as unset.  The value is checked as it accumulates, so a long
   run of digits cannot wrap round into range. */
bool text_parse_port(const char *s, size_t len, uint16_t *port);

/* A whole NUL-terminated string as a port (see text_parse_port). */
bool text_parse_port_str(const char *s, uint16_t *port);

/* Strict dotted quad at the start of s: four decimal octets of one to three
   digits, 0-255, joined by '.'.  On success fills oct[] (first octet in
   oct[0]) and returns a pointer just past the last digit, so the caller
   decides what may follow (end of string, ':port', ...); NULL if s does not
   start with one. */
const char *text_parse_ipv4(const char *s, uint8_t oct[4]);

/* The whole NUL-terminated string is a dotted quad (oct may be NULL). */
bool text_parse_ipv4_str(const char *s, uint8_t oct[4]);

#endif /* PI1MHZ_TEXTPARSE_H */
