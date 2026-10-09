/* usb_storage.h - a USB flash drive on the Pi's port in host mode
   (docs/dev/usb-flash-storage-plan.md).  Stage 1: the drive is enumerated
   and reported; nothing reads it yet. */
#ifndef USB_STORAGE_H
#define USB_STORAGE_H

#include <stddef.h>

/* For /status: the drive plugged in, its size and block size. */
void usb_storage_status(char *buf, size_t len);

#endif
