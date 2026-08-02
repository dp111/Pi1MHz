#pragma once
/* Host-test stub of FatFs diskio.h. */
#include <stdint.h>

typedef enum { RES_OK = 0, RES_ERROR, RES_WRPRT, RES_NOTRDY, RES_PARERR } DRESULT;

DRESULT disk_read(uint8_t pdrv, uint8_t *buff, uint32_t sector, unsigned int count);
DRESULT disk_write(uint8_t pdrv, const uint8_t *buff, uint32_t sector, unsigned int count);
unsigned char disk_type(void);
