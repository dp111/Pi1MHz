/* Host stub of the TinyUSB HID host API that usb_mouse.c uses.  The
   interface table mirrors hid_host.c: an instance index is a slot in one
   table shared by every device, valid for a device only while that slot
   holds its address (get_hid_itf). */
#ifndef STUB_TUSB_H
#define STUB_TUSB_H

#include <stdbool.h>
#include <stdint.h>

#define HID_ITF_PROTOCOL_NONE     0
#define HID_ITF_PROTOCOL_KEYBOARD 1
#define HID_ITF_PROTOCOL_MOUSE    2

typedef struct {
   uint8_t daddr;          /* 0: slot free */
   uint8_t protocol;
   bool    mounted;
   int     receives;       /* tuh_hid_receive_report calls for this slot */
} stub_hid_itf_t;

extern stub_hid_itf_t stub_hid_itf[CFG_TUH_HID];

uint8_t tuh_hid_interface_protocol(uint8_t dev_addr, uint8_t idx);
bool    tuh_hid_mounted(uint8_t dev_addr, uint8_t idx);
bool    tuh_hid_receive_report(uint8_t dev_addr, uint8_t idx);
bool    tuh_vid_pid_get(uint8_t dev_addr, uint16_t *vid, uint16_t *pid);

#endif
