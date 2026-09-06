#ifndef UEF_SERVICE_H
#define UEF_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

/* The UEF tape half of the ElkWiFi service: commands 93 (stream) and 86
 * (guard image).  Split out of wifi_service.c because it is a state machine
 * with its own lifetime - an open tape owns heap and a JIM window - and none
 * of that has anything to do with WiFi association.
 *
 * The numbers, the "IUEF" request layout and the guard's position in each
 * JIM page are the ROM's ABI and can only change in lockstep with it.
 *
 * Nothing here is allocated until the host opens a tape, and it is all
 * released again on CLOSE, so an idle Pi1MHz pays nothing for the feature.
 */

/* Handle command 93.  Returns a WIFI_SVC_* status. */
uint8_t uef_service_stream_command(uint32_t command_pointer);

/* Handle command 86 (publish or withdraw the host's filing-vector guard). */
uint8_t uef_service_guard_command(uint32_t command_pointer);

/* Drop any open tape and free everything it owns.  Called on host reset:
 * the Beeb that opened the tape is gone, and its JIM window with it. */
void uef_service_reset(void);

#endif
