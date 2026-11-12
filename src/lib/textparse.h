/* textparse.h - the small text parsers several services share: a TCP/UDP
 * port number and a dotted-quad IPv4 address, as they appear in Pi1MHz.cfg
 * values and in the URLs the Beeb hands the network services, and one hex
 * digit.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Why not lwIP's ip4addr_aton: it accepts the BSD inet_aton forms - "10" is
 * 0.0.0.10, "10.1" is 10.0.0.1, "0x0a.1.2.3" is hex and "010.1.2.3" is
 * OCTAL (8.1.2.3).  A user typing 192.168.001.010 into Pi1MHz.cfg means
 * .1.10, not .1.8, and a host name made of digits must not be mistaken for
 * an address and skip DNS.  So the parse here is strict decimal: exactly
 * four octets of one to three digits each, 0-255.
 *
 * The same goes for plain numbers: strtol(s, NULL, 0) reads "010" as octal 8
 * and atoi() reads "off" or "12abc" as 0 or 12 without a word, so a typo in
 * Pi1MHz.cfg became a real setting.  text_parse_u32/s32 take the whole span
 * or nothing.  The query and URL helpers are the web server's, here so the
 * other HTTP handlers (and their host tests) share them.
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

/* The value of one hex digit (either case), or -1 if c is not one. */
int text_hex_nibble(int c);

/* A number spelled as s[0..len): decimal digits, or hex digits after "0x",
   "0X" or "&" (the BBC's way).  Never octal: "010" is ten.  Nothing else in
   the span (no sign, spaces or trailing text), at least one digit, and no
   more than max.  On failure *value is untouched. */
bool text_parse_u32(const char *s, size_t len, uint32_t max, uint32_t *value);
bool text_parse_u32_str(const char *s, uint32_t max, uint32_t *value);

/* The same with an optional leading '-', in min..max. */
bool text_parse_s32(const char *s, size_t len, int32_t min, int32_t max, int32_t *value);
bool text_parse_s32_str(const char *s, int32_t min, int32_t max, int32_t *value);

/* In a raw query string ("k=v&k2=v2", the text after '?', percent-encoding
   left as-is): the value of key, matched case-insensitively and only at a
   parameter's start (so "xoffset=1" never answers "offset"), and its length
   in *len; NULL if key isn't there.  The value isn't copied or terminated. */
const char *text_query_find(const char *query, const char *key, size_t *len);

/* The same, copied to out and NUL-terminated, silently cut to osz-1 bytes;
   false if key isn't there.  A caller that must not act on a cut value uses
   text_query_find and checks the length itself. */
bool text_query_param(const char *query, const char *key, char *out, size_t osz);

/* Percent-decode src into dst (dstsz bytes, always NUL-terminated).  Any
   control byte, literal or decoded (0x00-0x1F, 0x7F), becomes '_', so a
   decoded name can't smuggle CR/LF into a header or a NUL into a path.  A
   '%' not followed by two hex digits is kept as it is.  False if dst was
   too small for the whole of src. */
bool text_url_decode(const char *src, char *dst, size_t dstsz);

#endif /* PI1MHZ_TEXTPARSE_H */
