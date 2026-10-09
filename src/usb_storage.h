/* usb_storage.h - a USB flash drive on the Pi's port in host mode
   (docs/dev/usb-flash-storage-plan.md).  Stage 2: the drive is FatFs volume
   "1:", read-only; nothing uses it yet but /status. */
#ifndef USB_STORAGE_H
#define USB_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* For /status: the drive plugged in, its size, and its volume. */
void usb_storage_status(char *buf, size_t len);

/* Main loop, from the USB host poll and never from inside tuh_task: mounts
   "1:" when a drive has arrived, and forgets it when the drive has gone. */
void usb_storage_poll(void);

/* For diskio.c.  Usable: a drive is there with 512-byte blocks, and no read
   of it has timed out.  A read waits for the drive, servicing the USB host
   meanwhile; false if it fails, times out or the drive goes. */
bool usb_storage_usable(void);
bool usb_storage_read(uint8_t *buf, uint32_t lba, uint32_t count);

#endif
