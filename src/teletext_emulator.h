/* teletext_emulator.h - Acorn Teletext Adapter (ATS) emulation for Pi1MHz.
 *
 * Emulates the BBC teletext adapter at FRED &FC10-&FC13, sourcing the
 * broadcast teletext stream from a TCP server carrying raw "t42" data
 * (16 lines x 42 bytes per field, ~50 fields/s), the same wire format
 * BeebEm uses. Ported from BeebEm's Teletext.cpp.
 *
 * Pi1MHz.cfg options (no spaces in values):
 *   teletext_server1=a.b.c.d:port   CEEFAX channel 0 source
 *   teletext_server2=a.b.c.d:port   channel 1
 *   teletext_server3=a.b.c.d:port   channel 2
 *   teletext_server4=a.b.c.d:port   channel 3
 *   teletext_debug=1                terse log on the wifi debug channel
 * The port is optional and defaults to TELETEXT_DEFAULT_PORT.
 */

#ifndef TELETEXT_EMULATOR_H
#define TELETEXT_EMULATOR_H

#include <stdint.h>
#include <stddef.h>

/* 'instance' is the emulator-table slot (its nIRQ source bit); 'address'
 * is the FRED base (the adapter occupies address..address+3). */
void teletext_emulator_init(uint8_t instance, uint8_t address);

/* Plain-text status block for diagnostics (e.g. a web page). */
void teletext_status_text(char *buf, size_t size);

#endif
