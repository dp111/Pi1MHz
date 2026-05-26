#ifndef WIFI_WEBSERVER_H
#define WIFI_WEBSERVER_H

#include <stdbool.h>

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
bool webserver_is_ready(void);
const char *webserver_last_error(void);

#endif
