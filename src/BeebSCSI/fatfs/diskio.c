/*-----------------------------------------------------------------------*/
/* Low level disk I/O module glue functions         (C)ChaN, 2016        */
/*-----------------------------------------------------------------------*/
/* If a working storage control module is available, it should be        */
/* attached to the FatFs via a glue function rather than modifying it.   */
/* This is an example of glue functions to attach various existing       */
/* storage control modules to the FatFs module with a defined API.       */
/*-----------------------------------------------------------------------*/

#include "ff.h"			/* Obtains integer types */
#include <string.h>		/* memcpy for the read-ahead below */
/* Definitions of physical drive number for each drive */
#define DRV_SD    0  /* Example: Map MMC/SD card to physical drive 0 (default) */

#include "diskio.h"		/* Declarations of disk functions */


#ifdef DRV_SD
#include "../../rpi/sdcard.h"
#include "../../rpi/block.h"
#endif

/*static unsigned int sd_status=STA_NOINIT;*/

static struct emmc_block_dev sd_dev_storage;
static struct block_device *sd_dev = (struct block_device *)&sd_dev_storage.bd;

static int sd_drive_initialized(void)
{
   return sd_dev_storage.card_rca != 0;
}

/*-----------------------------------------------------------------------*/
/* Sequential read-ahead for single-sector reads                          */
/*-----------------------------------------------------------------------*/
/* FatFs asks move_window() for exactly one sector at a time, and both
   directory scans and FAT chain walks step through consecutive sectors.
   On this card a single-sector read costs ~300us of which only ~29us is
   data - the rest is the command round trip and the card's access time -
   so a 35-sector directory walk spends ~10.5ms almost entirely in
   latency, while a multi-block read of the same data streams at
   17.7MB/s.  Reading a short run ahead and serving the rest from RAM
   turns that walk into a handful of commands.
   Multi-sector reads bypass this entirely: they are already one command
   and are usually file data, which would only evict metadata.
   RA_SECTORS is a latency/waste trade: a run costs (300 + RA*29)us
   whether or not all of it is used.  Measured on this card, 8 vs 16
   sectors - 16 costs a further 233us on a shallow two-sector walk and
   saves 628us on a deep one and 15.4ms on a 2GB FAT seek, so 16 it is.
   Beyond that the deep cases flatten out and the 8KB doubles again. */
#define RA_SECTORS 16u

static BYTE     ra_buf[RA_SECTORS * 512u];
static LBA_t    ra_first;                  /* first sector held          */
static UINT     ra_count;                  /* 0 = empty                  */
static BYTE     ra_pdrv;

/* Only read ahead once access actually looks sequential: the sector a
   sequential reader would ask for next.  A lone random single-sector read
   - a directory entry for an already-known cluster, a one-off FAT probe -
   then costs exactly what it did before instead of dragging in seven
   neighbours nobody wants.  Without this the run-ahead added ~36% to the
   SD time of a bulk transfer for no gain. */
static LBA_t    ra_expect;
static int      ra_expect_valid;

static void ra_invalidate(void)
{
   ra_count = 0u;
}

/* Drop the cache if a write touches any sector it holds.  disk_write is
   the only writer - sd_write is called from nowhere else - so this is
   the single place coherency has to be maintained. */
static void ra_note_write(BYTE pdrv, LBA_t sector, UINT count)
{
   if (ra_count == 0u || pdrv != ra_pdrv)
      return;
   if (sector + count > ra_first && sector < ra_first + ra_count)
      ra_invalidate();
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

   /* A re-init means a different card may be in the slot. */
   ra_invalidate();
   return sdhost_init_device(&sd_dev) == 0 ? 0 : STA_NOINIT;
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
      if (count == 1u) {
         /* Hit: the run already holds it. */
         if (ra_count != 0u && pdrv == ra_pdrv
             && sector >= ra_first && sector < ra_first + ra_count) {
            memcpy(buff, ra_buf + (sector - ra_first) * 512u, 512u);
            return RES_OK;
         }
         /* Miss.  Pull a run in only if this continues a sequential walk;
            otherwise read the one sector and just remember where we were,
            so a run starts on the second consecutive sector.  A run near
            the end of the card would read past it, so a failure is not an
            error - fall through to the plain read and leave it empty. */
         ra_invalidate();
         if (ra_expect_valid && sector == ra_expect
             && sd_read(sd_dev, ra_buf, 512u * RA_SECTORS, sector)) {
            ra_first = sector;
            ra_count = RA_SECTORS;
            ra_pdrv  = pdrv;
            ra_expect = sector + RA_SECTORS;
            memcpy(buff, ra_buf, 512u);
            return RES_OK;
         }
         ra_expect = sector + 1u;
         ra_expect_valid = 1;
      }
      result = sd_read(sd_dev,buff,512*count,sector)?RES_OK:RES_ERROR;
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
   ra_note_write(pdrv, sector, count);
   return sd_write(sd_dev,buff,512*count,sector)?RES_OK:RES_ERROR;
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
   return sd_dev_storage.card_supports_sdhc;
}