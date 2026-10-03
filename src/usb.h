// Include TinyUSB headers for interrupt handler.
// Short name so it resolves via the -isystem TinyUSB dir (third-party).
#include <tusb.h>


void usb_init(uint8_t instance , uint8_t address);
/* The port was started as a host (usb_mode=), not as the MTP device. */
bool usb_is_host(void);
