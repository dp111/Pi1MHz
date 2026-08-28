/* Host-test stub: fat_service.c asks the webserver for the SD card's
   cached free-space figures (see its case 13). The tests provide their
   own webserver_sd_space_now(). */
#ifndef WIFI_WEBSERVER_H
#define WIFI_WEBSERVER_H

#include <stdbool.h>
#include <stdint.h>

bool webserver_sd_space_now(uint64_t *total, uint64_t *free_bytes);

#endif
