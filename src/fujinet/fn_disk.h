/* fn_disk.h - configuration hooks for the FujiNet disk device (fn_disk.c). */
#ifndef FN_DISK_H
#define FN_DISK_H

#include <stdbool.h>

/* The image BeginHostSession and RestoreBoot mount, fujinet-nio's
   boot.config_uri; "" for none. */
void fn_disk_set_boot(const char *uri, bool read_only);

/* True if a mounted or pending SD-card image is host_path, or inside it
   (root-relative, case-insensitive): host-side writers must not touch it. */
bool fn_disk_uses_path(const char *host_path);

#endif
