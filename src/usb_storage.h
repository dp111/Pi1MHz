/* usb_storage.h - a USB flash drive on the Pi's port in host mode
   (docs/dev/usb-flash-storage-plan.md): FatFs volume "1:".  With
   storage=usb in Pi1MHz.cfg the Beeb's storage comes from it - see
   filesystemStorageRoot. */
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

/* For diskio.c.  Usable: a drive is there with 512-byte blocks, and no
   command to it has timed out.  A transfer waits for the drive, servicing
   the USB host meanwhile; false if it fails, times out or the drive goes. */
bool usb_storage_usable(void);
bool usb_storage_read(uint8_t *buf, uint32_t lba, uint32_t count);
bool usb_storage_write(const uint8_t *buf, uint32_t lba, uint32_t count);

/* Volume "1:" is mounted and usable. */
bool usb_storage_mounted(void);

/* Run USB (usb_service) until volume "1:" is mounted or the 64-bit system
   timer reaches until_us; true if it is.  For the power-on decision only
   (filesystemStorageRoot): main loop, outside FatFs and tuh_task. */
bool usb_storage_wait_for_drive(uint64_t until_us);

#endif
