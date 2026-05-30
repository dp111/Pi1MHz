// Include TinyUSB headers for interrupt handler.
// Short name so it resolves via the -isystem TinyUSB dir (third-party).
#include <tusb.h>


void usb_init(uint8_t instance , uint8_t address);
