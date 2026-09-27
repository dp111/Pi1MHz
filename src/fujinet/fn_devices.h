/* fn_devices.h - the FujiNet devices behind fujibus_answer().

   Each takes the command byte and the request's device payload and writes
   the reply's device payload, returning a FujiBus status (fujibus.h).  The
   layouts are fujinet-nio's, cited per command in each file. */
#ifndef FN_DEVICES_H
#define FN_DEVICES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fujibus.h"

uint8_t fn_disk_command    (uint8_t command, fb_in *in, fb_out *out);
uint8_t fn_slotcat_command (uint8_t command, fb_in *in, fb_out *out);
uint8_t fn_appstore_command(uint8_t command, fb_in *in, fb_out *out);
uint8_t fn_host_command    (uint8_t command, fb_in *in, fb_out *out);
uint8_t fn_file_command    (uint8_t command, fb_in *in, fb_out *out);

/* ---- shared services between the devices ------------------------------ */

/* App store, used directly by fn-rom and by the host and slot catalogue for
   their own state (in namespaces fn-rom never uses).  `exists` may be NULL.
   Reads return the bytes present from `offset`, up to `max`. */
bool fn_app_read  (const char *ns, const char *key, uint32_t offset,
                   uint8_t *buf, uint16_t max, uint16_t *got, bool *exists);
bool fn_app_write (const char *ns, const char *key, uint32_t offset,
                   const uint8_t *data, uint16_t len);
bool fn_app_delete(const char *ns, const char *key, bool *existed);

/* Host state.  Resolve `spec` - a URI, "fs:/path", or a path relative to
   the current host - into a canonical URI.  Returns false if it cannot be
   resolved (no current host for a relative spec, unknown filesystem). */
bool fn_host_resolve(const char *spec, char *uri, size_t cap);

/* Split a canonical URI into filesystem name and path, as fn_store takes
   them ("sd0:/a/b" -> "sd0", "/a/b").  False if the filesystem is not one
   this device has. */
bool fn_uri_split(const char *uri, char *fs, size_t fs_cap,
                  char *path, size_t path_cap);

/* Forget runtime state that must not outlive the Pi's own session (open
   images).  Persistent state - catalogue, app store, host - is on the card. */
void fn_disk_reset(void);

#endif
