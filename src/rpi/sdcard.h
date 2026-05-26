#ifndef RPI_SDCARD_H
#define RPI_SDCARD_H

#include "block.h"

int sdhost_init_device(struct block_device **dev);
size_t sd_read(struct block_device *dev, uint8_t *buf, size_t buf_size, uint32_t block_no);
size_t sd_write(struct block_device *dev, const uint8_t *buf, size_t buf_size, uint32_t block_no);

#endif