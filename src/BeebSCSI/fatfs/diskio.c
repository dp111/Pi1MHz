/*-----------------------------------------------------------------------*/
/* Low level disk I/O module glue functions         (C)ChaN, 2016        */
/*-----------------------------------------------------------------------*/
/* If a working storage control module is available, it should be        */
/* attached to the FatFs via a glue function rather than modifying it.   */
/* This is an example of glue functions to attach various existing       */
/* storage control modules to the FatFs module with a defined API.       */
/*-----------------------------------------------------------------------*/

#include "ff.h"			/* Obtains integer types */
#include <stdio.h>
/* Definitions of physical drive number for each drive */
#define DRV_SD    0  /* Example: Map MMC/SD card to physical drive 0 (default) */

#include "diskio.h"		/* Declarations of disk functions */


#ifdef DRV_SD
#include "../../rpi/block.h"
size_t sd_read(struct block_device *dev, uint8_t *buf, size_t buf_size, uint32_t block_no);
size_t sd_write(struct block_device *dev, const uint8_t *buf, size_t buf_size, uint32_t block_no);
#endif

/*static unsigned int sd_status=STA_NOINIT;*/

static struct emmc_block_dev bd;
/*-----------------------------------------------------------------------*/
/* Get Drive Status                                                      */
/*-----------------------------------------------------------------------*/

DSTATUS disk_status (
	BYTE pdrv		/* Physical drive number to identify the drive */
)
{
   switch (pdrv) {

#ifdef DRV_MMC
   case DRV_MMC :
      return mmc_disk_status();
#endif

#ifdef DRV_SD
   case DRV_SD :
      return 0; /*(sdInitCard())? STA_NOINIT:0;//mmc_disk_status();*/
#endif

   }
   return STA_NOINIT;
}



/*-----------------------------------------------------------------------*/
/* Inidialize a Drive                                                    */
/*-----------------------------------------------------------------------*/

DSTATUS disk_initialize (
	BYTE pdrv				/* Physical drive number to identify the drive */
)
{
   switch (pdrv) {

#ifdef DRV_MMC
   case DRV_MMC :
      return mmc_disk_initialize();
#endif
#ifdef DRV_SD
   case DRV_SD :
      /*sd_status = (sdInitCard())? STA_NOINIT:RES_OK;*/
      return RES_OK;/*sd_status;//mmc_disk_initialize();*/
#endif
   }
   return STA_NOINIT;
}

/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                        */
/*-----------------------------------------------------------------------*/

DRESULT disk_read (
	BYTE pdrv,		/* Physical drive number to identify the drive */
	BYTE *buff,		/* Data buffer to store read data */
	LBA_t sector,	/* Start sector in LBA */
	UINT count		/* Number of sectors to read */
)
{
   DRESULT result;
   switch (pdrv) {

#ifdef DRV_MMC
   case DRV_MMC :
      return mmc_disk_read(buff, sector, count);
#endif
#ifdef DRV_SD
   case DRV_SD :
     // printf("sd_read buf %p, sector %lu, count %d\r\n",buff,sector,count);
      result = sd_read((struct block_device *)&bd,buff,512*count,sector)?RES_OK:RES_ERROR;
    /*  for(int i=0;i<512;i++)
         {
            printf("%x ",buff[i] );
            if ((i%16)==15) printf("\r\n");
         }*/
      return result;

#endif
   }
   return RES_PARERR;
}


/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/
#if FF_FS_READONLY == 0
DRESULT disk_write (
	BYTE pdrv,			/* Physical drive number to identify the drive */
	const BYTE *buff,	/* Data to be written */
	LBA_t sector,		/* Start sector in LBA */
	UINT count			/* Number of sectors to write */
)
{
   switch (pdrv) {

#ifdef DRV_MMC
   case DRV_MMC :
      return mmc_disk_write(buff, sector, count);
#endif
#ifdef DRV_SD
   case DRV_SD :
  // printf("sd_write buf %p, sector %lu, count %d\r\n",buff,sector,count);
      return sd_write((struct block_device *)&bd,buff,512*count,sector)?RES_OK:RES_ERROR;
#endif
   }
   return RES_PARERR;
}
#endif


/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/

DRESULT disk_ioctl (
   BYTE pdrv,     /* Physical drive number (0..) */
   BYTE cmd,      /* Control code */
   const void *buff     /* Buffer to send/receive control data */
)
{
   switch (pdrv) {

#ifdef DRV_MMC
   case DRV_MMC :
      return mmc_disk_ioctl(cmd, buff);
#endif
   }
#ifdef DRV_SD
   return RES_OK; /* sync is the only case used and writes always complete before returning so always synced */
#else
   return RES_PARERR;
#endif
}

unsigned char disk_type()
{
   return bd.card_supports_sdhc;
}