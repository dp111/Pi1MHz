#ifndef RPI_SDCARD_H
#define RPI_SDCARD_H

#include "block.h"

int sdhost_init_device(struct block_device **dev);
size_t sd_read(struct block_device *dev, uint8_t *buf, size_t buf_size, uint32_t block_no);
size_t sd_write(struct block_device *dev, const uint8_t *buf, size_t buf_size, uint32_t block_no);

/* Called, if set, while the driver only waits on the card (FIFO full or empty,
   the data state machine busy): work that must not stop for a multi-
   millisecond write.  usb.c sets it while wired Ethernet is the link - the
   LAN7515's FIFO lasts ~100 us at 1 Gbit/s, and a WebDAV PUT writes the card
   from inside lwIP's receive callback.  It must not touch the SD card. */
extern void (*sdcard_wait_hook)(void);

#endif