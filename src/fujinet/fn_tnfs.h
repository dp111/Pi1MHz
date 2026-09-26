/* fn_tnfs.h - disk images and directories on TNFS servers, for fn_store.c.

   Random access to files by URI ("tnfs://host[:port]/path"): one session
   per server, mounted on first use and kept; one request in flight at a
   time (the Beeb serialises); the resend and backoff rules are net_tnfs.h's
   tnfs_xfer engine.  Nothing here waits: a call that needs the server
   starts the work and returns failure with fn_store's pending flag set (see
   fn_store.h), and the answer is remembered for the re-run.

   The network is reached only through the fn_tnfs_io_* functions below,
   which the platform provides - lwIP UDP on the Pi (src/fujibus_service.c),
   an in-memory server in the host tests. */
#ifndef FN_TNFS_H
#define FN_TNFS_H

#include <stdbool.h>
#include <stdint.h>

#include "fn_store.h"

#define FN_TNFS_HANDLE_BASE 64   /* handles 64.. are TNFS files */
#define FN_TNFS_HANDLES     8

/* ---- the storage backend (fn_store.c routes "tnfs" here) ---------------- */
fn_handle fn_tnfs_open(const char *uri, fn_open_mode mode);
bool fn_tnfs_read (fn_handle h, uint32_t offset, void *buf, uint32_t len);
bool fn_tnfs_write(fn_handle h, uint32_t offset, const void *buf, uint32_t len);
bool fn_tnfs_size (fn_handle h, uint32_t *size);
void fn_tnfs_close(fn_handle h);
bool fn_tnfs_is_dir(const char *uri);
bool fn_tnfs_dir_entry(const char *uri, uint32_t index, fn_dirent *out);

/* Request lifetime and time, from fn_store.c. */
void fn_tnfs_request_end(void);
void fn_tnfs_request_abort(void);
void fn_tnfs_poll(uint32_t now_ms);
bool fn_tnfs_take_pending(void);      /* was a call just told "not yet"? (clears) */

/* ---- the platform ---------------------------------------------------------- */
typedef enum { FN_IO_OK, FN_IO_PENDING, FN_IO_FAIL } fn_io;

/* Resolve a host name or dotted quad to an IPv4 address (network order is
   the platform's business: the value is only handed back to send). */
fn_io    fn_tnfs_io_resolve(const char *host, uint32_t *ip);
/* Send one datagram to ip:port.  False if it could not be queued. */
bool     fn_tnfs_io_send(uint32_t ip, uint16_t port, const uint8_t *pkt, uint16_t len);
uint32_t fn_tnfs_io_now_ms(void);

/* The platform calls this for every datagram arriving from a TNFS server. */
void fn_tnfs_input(uint32_t ip, uint16_t port, const uint8_t *pkt, uint16_t len);

#endif
