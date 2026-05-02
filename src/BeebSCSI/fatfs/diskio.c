/*-----------------------------------------------------------------------*/
/* Low level disk I/O module glue functions         (C)ChaN, 2016        */
/*-----------------------------------------------------------------------*/
/* If a working storage control module is available, it should be        */
/* attached to the FatFs via a glue function rather than modifying it.   */
/* This is an example of glue functions to attach various existing       */
/* storage control modules to the FatFs module with a defined API.       */
/*-----------------------------------------------------------------------*/

#include "ff.h"			/* Obtains integer types */
/* Definitions of physical drive number for each drive */
#define DRV_SD    0  /* Example: Map MMC/SD card to physical drive 0 (default) */

#include "diskio.h"		/* Declarations of disk functions */


#ifdef DRV_SD
#include "../../rpi/sdcard.h"
#endif

/*static unsigned int sd_status=STA_NOINIT;*/

static struct emmc_block_dev bd;

static int sd_drive_initialized(void)
{
   return bd.card_rca != 0;
}
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
   return sd_drive_initialized() ? 0 : STA_NOINIT;
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
   if (sd_drive_initialized())
      return 0;

   return sdhost_init_device((struct block_device **)&bd) == 0 ? 0 : STA_NOINIT;
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
      result = sd_read((struct block_device *)&bd,buff,512*count,sector)?RES_OK:RES_ERROR;
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
   void *buff     /* Buffer to send/receive control data */
)
{
   switch (pdrv) {

#ifdef DRV_MMC
   case DRV_MMC :
      return mmc_disk_ioctl(cmd, buff);
#endif
#ifdef DRV_SD
   case DRV_SD :
      switch (cmd) {
      case CTRL_SYNC:
         return RES_OK;
      case GET_SECTOR_SIZE:
         *(WORD *)buff = 512;
         return RES_OK;
      case MMC_GET_TYPE:
         *(BYTE *)buff = disk_type();
         return RES_OK;
      default:
         return RES_PARERR;
      }
#endif
   }
   return RES_PARERR;
}

unsigned char disk_type( void)
{
   return bd.card_supports_sdhc;
}