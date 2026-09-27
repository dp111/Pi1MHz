#ifndef FUJIBUS_SERVICE_H
#define FUJIBUS_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

/* The FujiNet device on the services port (commands 114-119). */
void fujibus_service_init(uint8_t instance, uint8_t address);

/* True while a FujiNet-mounted SD image is host_path or inside it: part of
   beeb_path_busy(). */
bool fujibus_service_path_busy(const char *host_path);

#endif
