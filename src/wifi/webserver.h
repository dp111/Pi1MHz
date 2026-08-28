#ifndef WIFI_WEBSERVER_H
#define WIFI_WEBSERVER_H

#include <stdbool.h>
#include <stdint.h>

/* Minimal HTTP file-browser webserver.
 *
 * Serves a handful of pages over the lwIP raw-TCP API:
 *   /          - home page
 *   /status    - WiFi / network status
 *   /files/... - browse the SD card, download files, upload files
 *
 * The server is wired up by wifi.c once the network comes up; only the
 * three entry points below are part of the public interface. */

void webserver_init(void);
/* Per-tick housekeeping (deferred reboot, etc).  Called from the
   shared WiFi dispatcher in wifi.c; safe to call before
   webserver_init - it just no-ops until something is pending. */
void webserver_poll(void);
/* Cached SD capacity/free from the background FAT sweep (queues a refresh;
   returns false until the first sweep completes). Never blocks. */
bool webserver_sd_space(uint64_t* total, uint64_t* free_bytes);
/* As above, but pays ONE blocking FAT scan if no sweep has completed yet
   (cached afterwards). For callers that must answer immediately. */
bool webserver_sd_space_now(uint64_t* total, uint64_t* free_bytes);
bool webserver_is_ready(void);
const char *webserver_last_error(void);

#endif
