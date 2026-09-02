#ifndef PI1MHZ_SECURE_SERVICE_H
#define PI1MHZ_SECURE_SERVICE_H

#include <stdint.h>

void secure_service_init(uint8_t instance, uint8_t address);
void secure_service_command(uint32_t command_pointer, uint32_t address,
                            uint8_t data);

#endif
