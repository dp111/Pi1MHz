/* fn_network.h - NetworkDevice ($FD): fn-rom's network channels, on the
   Pi's own network stack (net_service.h's net_capi_* sessions).

   See fn_network.c for the protocol and the one divergence in how it waits. */
#ifndef FN_NETWORK_H
#define FN_NETWORK_H

#include <stdbool.h>
#include <stdint.h>

#include "fujibus.h"

uint8_t fn_network_command(uint8_t command, fb_in *in, fb_out *out);

/* Did the last command stop to wait (a Write the network has not taken
   yet)?  fujibus_answer then reports the request pending and runs it again. */
bool fn_network_waiting(void);

/* Every main loop pass: advance the connections that are still opening, so
   they are ready by the time the Beeb reads. */
void fn_network_poll(void);

/* BBC reset: the network service drops every connection on a reset, so the
   sessions go with them.  Safe from any context - the closes happen on the
   next fn_network_poll. */
void fn_network_reset(void);

#endif
