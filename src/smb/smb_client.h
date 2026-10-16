/* smb_client.h - the one SMB connection behind storage=smb (smb_client.c). */
#ifndef SMB_CLIENT_H
#define SMB_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct smb2_context;

/* storage=smb's share is configured (smb_server and smb_share set). */
bool smb_client_configured(void);

/* Connected and logged on to the share, connecting first if need be.  Main
   loop only; waits (running the network) up to the power-on window for the
   WiFi to get an address, then up to the command limit to log on.  False
   when that fails, or after a link loss until smb_client_reset(). */
bool smb_client_connect(void);

/* The context while connected, else NULL. */
struct smb2_context *smb_client_context(void);

/* Wait for *done, running the network and libsmb2 meanwhile, up to the
   command limit.  False on a timeout, a dropped connection or a BBC reset
   not yet handled: the share is then not used again until the next
   smb_client_reset(). */
bool smb_client_wait(volatile bool *done);

/* One step of libsmb2, never waiting: puts a request sent without waiting
   for its answer (a close) on the wire.  Nothing from inside the poll. */
void smb_client_kick(void);

/* The logon in force, never 0; 0 when not connected.  A handle opened in
   one session is dead in any other: the connection it lived on is gone. */
uint32_t smb_client_session(void);

/* Can this caller wait here?  Not from inside the network's own poll (an
   lwIP callback): the wait would re-enter it.  Such callers are refused at
   once (docs/dev/smb-network-storage-plan.md, "Re-entrancy"). */
bool smb_client_can_wait(void);

/* A BBC reset: after a link loss, drop the connection so the next use
   reconnects.  Until this has run for an nRST edge, every wait ends at
   once (smb_client_wait): the poll loop must get back to its re-init while
   the Beeb is still held in reset. */
void smb_client_reset(void);

/* For /status: one line. */
void smb_client_status(char *out, size_t size);

#endif
