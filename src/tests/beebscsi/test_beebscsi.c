/* Host test: the BeebSCSI LUN layer.  Review 2026-10-06 S1 and S7.

   S7 - filesystemFormatLun() sized the new image from fsLunGeometry, which
   is only current for a started LUN.  FORMAT of a stopped LUN (a fresh
   boot, or after a jukebox) used zero - FA_CREATE_ALWAYS truncated the
   image, f_expand(0) failed, and the image was left at 0 bytes - or the
   geometry of another directory's image.  And when f_expand failed the
   FIL was never closed.

   S1 - scsiJukebox() refused while ANY of the 16 LUNs was started, so an
   ADFS *SCSIJUKE was refused while the VFS LaserDisc LUN 8 was mounted,
   and the reverse, although each host swaps only its own directory.

   Real scsi.c, filesystem.c, fileparser.c and FatFs on a RAM disk.
   f_open/f_close are wrapped (-Wl,--wrap) to see handles left open. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Pi1MHz.h"
#include "config.h"
#include "videoplayer.h"
#include "harddisc_emulator.h"
#include "rpi/systimer.h"
#include "BeebSCSI/fatfs/ff.h"
#include "BeebSCSI/fatfs/diskio.h"
#include "BeebSCSI/filesystem.h"
#include "BeebSCSI/hostadapter.h"
#include "BeebSCSI/fcode.h"
#include "BeebSCSI/scsi.h"

/* ---- platform stubs ---------------------------------------------------- */

static Pi1MHz_t pi_struct;
Pi1MHz_t * const Pi1MHz = &pi_struct;
void Pi1MHz_LED(int led) { (void)led; }

const char *config_get(const char *key) { (void)key; return NULL; }
bool config_beeb_write_protected(void) { return false; }
uint32_t RPI_GetSystemTime(void) { return 0; }
void videoplayer_media_changed(void) {}

/* The bus and the F-code layer: the tests call the LUN layer directly. */
uint8_t scsiFcodeBuffer[256];
uint8_t scsiFcodeBufferRX[256];
void fcodeWriteBuffer(uint8_t lunNumber) { (void)lunNumber; }
void fcodeReadBuffer(void) {}
void fcodePoll(void) {}
void fcodeClearBuffer(void) {}
void fcode_disc_flip(void) {}
void hd_audio_service(void) {}
void hd_juke_service(void) {}
void hd_card_service(void) {}
uint8_t hostadapterReadDatabus(void) { return 0; }
uint8_t hostadapterReadByte(void) { return 0; }
void hostadapterWriteByte(uint8_t v) { (void)v; }
uint32_t hostadapterPerformReadDMA(const uint8_t *b) { (void)b; return 256; }
uint32_t hostadapterPerformWriteDMA(uint8_t *b) { (void)b; return 256; }
void hostadapterWriteResetFlag(bool f) { (void)f; }
bool hostadapterReadResetFlag(void) { return false; }
void hostadapterWriteDataPhaseFlags(bool m, bool c, bool i) { (void)m; (void)c; (void)i; }
void hostadapterWriteBusyFlag(bool f) { (void)f; }
void hostadapterWriteRequestFlag(bool f) { (void)f; }
bool hostadapterReadSelectFlag(void) { return false; }

/* ---- RAM disk ---------------------------------------------------------- */

#define DISK_SECTORS (128u * 1024u * 2u)        /* 128 MB of 512-byte sectors */
static uint8_t *disk;

DSTATUS disk_initialize(BYTE pdrv) { (void)pdrv; return 0; }
DSTATUS disk_status(BYTE pdrv) { (void)pdrv; return 0; }
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
   (void)pdrv;
   if (sector + count > DISK_SECTORS) return RES_PARERR;
   memcpy(buff, disk + (size_t)sector * 512u, (size_t)count * 512u);
   return RES_OK;
}
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
   (void)pdrv;
   if (sector + count > DISK_SECTORS) return RES_PARERR;
   memcpy(disk + (size_t)sector * 512u, buff, (size_t)count * 512u);
   return RES_OK;
}
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
   (void)pdrv;
   switch (cmd) {
   case CTRL_SYNC:        return RES_OK;
   case GET_SECTOR_COUNT: *(LBA_t *)buff = DISK_SECTORS; return RES_OK;
   case GET_SECTOR_SIZE:  *(WORD *)buff = 512; return RES_OK;
   case GET_BLOCK_SIZE:   *(DWORD *)buff = 1; return RES_OK;
   default:               return RES_PARERR;
   }
}
void disk_forget(void) {}
unsigned char disk_type(void) { return 0; }

/* ---- open-handle accounting ------------------------------------------- */

FRESULT __real_f_open(FIL *fp, const TCHAR *path, BYTE mode);
FRESULT __real_f_close(FIL *fp);

#define MAX_OPEN 64u
static struct { FIL *fp; char path[64]; } open_fil[MAX_OPEN];

FRESULT __wrap_f_open(FIL *fp, const TCHAR *path, BYTE mode)
{
   FRESULT r = __real_f_open(fp, path, mode);
   if (r == FR_OK) {
      unsigned int i, free_slot = MAX_OPEN;
      for (i = 0; i < MAX_OPEN; i++) {
         if (open_fil[i].fp == fp) break;
         if (!open_fil[i].fp && free_slot == MAX_OPEN) free_slot = i;
      }
      if (i == MAX_OPEN) i = free_slot;
      if (i < MAX_OPEN) {
         open_fil[i].fp = fp;
         snprintf(open_fil[i].path, sizeof open_fil[i].path, "%s", path);
      }
   }
   return r;
}

FRESULT __wrap_f_close(FIL *fp)
{
   for (unsigned int i = 0; i < MAX_OPEN; i++)
      if (open_fil[i].fp == fp)
         open_fil[i].fp = NULL;
   return __real_f_close(fp);
}

/* A handle still open on `path`.  Asked only while the LUN is stopped -
   FORMAT stops it - so the LUN layer's own per-LUN handle, open exactly
   while the LUN is started, is closed and any hit is a leak. */
static bool leaked_on(const char *path)
{
   for (unsigned int i = 0; i < MAX_OPEN; i++)
      if (open_fil[i].fp && strcmp(open_fil[i].path, path) == 0)
         return true;
   return false;
}

/* ---- harness ----------------------------------------------------------- */

static int failures, passes;

static void check(const char *test, bool ok, const char *why)
{
   if (ok) {
      passes++;
      printf("PASS %s\n", test);
   } else {
      failures++;
      printf("FAIL %s: %s\n", test, why);
   }
}

static void put_file(const char *path, const void *data, UINT len)
{
   FIL f;
   UINT done;
   if (__real_f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK ||
       f_write(&f, data, len, &done) != FR_OK || done != len) {
      printf("FAIL setup: could not write %s\n", path);
      exit(1);
   }
   __real_f_close(&f);
}

/* A LUN descriptor with only the drive parameter list (ModePage0): block
   size and sectors per track take their defaults (256, 33). */
static void put_cfg(const char *path, unsigned int cylinders, unsigned int heads)
{
   char cfg[96];
   int n = snprintf(cfg, sizeof cfg, "ModePage0=01%04X%02X008000800001\n", cylinders, heads);
   put_file(path, cfg, (UINT)n);
}

static uint32_t geometry_bytes(unsigned int cylinders, unsigned int heads)
{
   return (uint32_t)cylinders * heads * DEFAULT_SECTORS_PER_TRACK * DEFAULT_BLOCK_SIZE;
}

static long size_of(const char *path)
{
   FILINFO fi;
   return f_stat(path, &fi) == FR_OK ? (long)fi.fsize : -1L;
}

/* ---- S7: FORMAT ------------------------------------------------------- */

/* A fresh boot: the LUN has never been started, so its geometry was never
   read.  The new image must have the size the descriptor gives. */
static void test_format_never_started(void)
{
   static char why[160];
   const char *t = "S7 FORMAT of a never-started LUN sizes from its descriptor";
   f_mkdir("/BeebSCSI0");
   put_cfg("/BeebSCSI0/scsi0.cfg", 20, 4);
   put_file("/BeebSCSI0/scsi0.dat", "old image", 9);

   bool ok = filesystemFormatLun(0, 0x6C);
   long got = size_of("/BeebSCSI0/scsi0.dat");
   snprintf(why, sizeof why, "returned %d, image is %ld bytes, want %lu",
            ok, got, (unsigned long)geometry_bytes(20, 4));
   check(t, ok && got == (long)geometry_bytes(20, 4), why);

   check("S7 ... and leaves no handle open on the image",
         !filesystemReadLunStatus(0) && !leaked_on("/BeebSCSI0/scsi0.dat"),
         "a FIL is still open");
}

/* The ordinary case, a started LUN: unchanged. */
static void test_format_started(void)
{
   static char why[160];
   const char *t = "S7 FORMAT of a started LUN (control)";
   put_cfg("/BeebSCSI0/scsi0.cfg", 10, 2);
   filesystemSetLunStatus(0, false);
   bool started = filesystemSetLunStatus(0, true);
   bool ok = filesystemFormatLun(0, 0x6C);
   long got = size_of("/BeebSCSI0/scsi0.dat");
   snprintf(why, sizeof why, "start %d, returned %d, image is %ld bytes, want %lu",
            started, ok, got, (unsigned long)geometry_bytes(10, 2));
   check(t, started && ok && got == (long)geometry_bytes(10, 2), why);
}

/* After a jukebox the stopped LUN's cached geometry is the OLD directory's
   image: it must not decide the size of the new one. */
static void test_format_after_jukebox(void)
{
   static char why[160];
   const char *t = "S7 FORMAT after a jukebox sizes from the new directory's descriptor";
   f_mkdir("/BeebSCSI1");
   put_cfg("/BeebSCSI1/scsi0.cfg", 30, 3);
   put_file("/BeebSCSI1/scsi0.dat", "other image", 11);

   filesystemSetLunStatus(0, true);           /* geometry of /BeebSCSI0 (10x2) */
   filesystemSetLunStatus(0, false);
   filesystemSetLunDirectory(1, 1);           /* an ADFS host's *SCSIJUKE 1 */

   bool ok = filesystemFormatLun(0, 0x6C);
   long got = size_of("/BeebSCSI1/scsi0.dat");
   snprintf(why, sizeof why, "returned %d, image is %ld bytes, want %lu (old dir's: %lu)",
            ok, got, (unsigned long)geometry_bytes(30, 3), (unsigned long)geometry_bytes(10, 2));
   check(t, ok && got == (long)geometry_bytes(30, 3), why);
   filesystemSetLunDirectory(1, 0);
}

/* f_expand fails (the image would not fit on the card): the FIL must be
   closed on that path too.  The LUN is started first, so the old code
   reaches f_expand with this geometry as well. */
static void test_format_expand_fails(void)
{
   static char why[160];
   const char *t = "S7 a failed f_expand closes the image";
   f_mkdir("/BeebSCSI2");
   put_cfg("/BeebSCSI2/scsi0.cfg", 1000, 16);  /* 135 MB on a 128 MB card */
   put_file("/BeebSCSI2/scsi0.dat", "x", 1);
   filesystemSetLunDirectory(1, 2);

   bool started = filesystemSetLunStatus(0, true);
   bool ok = filesystemFormatLun(0, 0x6C);
   bool leak = filesystemReadLunStatus(0) || leaked_on("/BeebSCSI2/scsi0.dat");
   snprintf(why, sizeof why, "start %d, returned %d (want 0), handle left open: %s",
            started, ok, leak ? "yes" : "no");
   check(t, started && !ok && !leak, why);
   filesystemSetLunDirectory(1, 0);
}

/* ---- S1: the jukebox guard -------------------------------------------- */

static void stop_all(void)
{
   for (uint8_t l = 0; l < MAX_LUNS; l++)
      filesystemSetLunStatus(l, false);
}

static void test_jukebox(void)
{
   static char why[160];
   /* A VFS side with a data image in /BeebVFS0 and /BeebVFS1, ADFS images
      in /BeebSCSI0 (from above) and /BeebSCSI3. */
   f_mkdir("/BeebVFS0");
   f_mkdir("/BeebVFS1");
   f_mkdir("/BeebSCSI3");
   put_cfg("/BeebVFS0/scsi0.cfg", 10, 2);
   put_file("/BeebVFS0/scsi0.dat", "vfs side 0", 10);
   put_cfg("/BeebVFS1/scsi0.cfg", 10, 2);
   put_file("/BeebVFS1/scsi0.dat", "vfs side 1", 10);
   put_cfg("/BeebSCSI3/scsi0.cfg", 10, 2);
   put_file("/BeebSCSI3/scsi0.dat", "adfs dir 3", 10);
   put_cfg("/BeebSCSI0/scsi0.cfg", 10, 2);
   stop_all();
   filesystemSetLunDirectory(1, 0);
   filesystemSetLunDirectory(16, 0);

   /* Control: an ADFS host's jukebox with its own LUN 0 started. */
   scsiHostID = 1;
   bool s = filesystemSetLunStatus(0, true);
   bool r = scsiJukebox(3);
   snprintf(why, sizeof why, "start %d, jukebox %d (want 0), dir %u",
            s, r, filesystemGetLunDirectory());
   check("S1 ADFS jukebox refused while an ADFS LUN is started (control)",
         s && !r && filesystemGetLunDirectory() == 0, why);
   stop_all();

   /* ADFS jukebox while only the LaserDisc (LUN 8) is mounted. */
   s = filesystemSetLunStatus(8, true);
   scsiHostID = 1;
   r = scsiJukebox(3);
   snprintf(why, sizeof why, "start 8 %d, jukebox %d (want 1), dir %u (want 3)",
            s, r, filesystemGetLunDirectory());
   check("S1 ADFS jukebox allowed while only VFS LUN 8 is started",
         s && r && filesystemGetLunDirectory() == 3, why);
   check("S1 ... and leaves the VFS side and LUN 8 alone",
         filesystemGetLunDirectoryVFS() == 0 && filesystemReadLunStatus(8),
         "VFS directory or LUN 8 changed");
   stop_all();

   /* VFS jukebox (the disc flip) while an ADFS LUN is mounted. */
   filesystemSetLunDirectory(1, 3);           /* whatever the case above did */
   s = filesystemSetLunStatus(0, true);
   scsiHostID = 16;
   r = scsiJukebox(1);
   snprintf(why, sizeof why, "start 0 %d, jukebox %d (want 1), VFS dir %u (want 1)",
            s, r, filesystemGetLunDirectoryVFS());
   check("S1 VFS jukebox allowed while only ADFS LUN 0 is started",
         s && r && filesystemGetLunDirectoryVFS() == 1, why);
   check("S1 ... and leaves the ADFS directory and LUN 0 alone",
         filesystemGetLunDirectory() == 3 && filesystemReadLunStatus(0),
         "ADFS directory or LUN 0 changed");
   stop_all();

   /* Control: the VFS host's jukebox with its own LUN 8 started. */
   filesystemSetLunDirectory(16, 1);
   s = filesystemSetLunStatus(8, true);
   scsiHostID = 16;
   r = scsiJukebox(0);
   snprintf(why, sizeof why, "start 8 %d, jukebox %d (want 0), VFS dir %u (want 1)",
            s, r, filesystemGetLunDirectoryVFS());
   check("S1 VFS jukebox refused while VFS LUN 8 is started (control)",
         s && !r && filesystemGetLunDirectoryVFS() == 1, why);
   stop_all();

   /* The top VFS LUN guards too: the range is 8-15, not just 8.  LUN 15 is
      /BeebVFS<n>/scsi7 (fsLunFilePath masks to lun & 7). */
   put_file("/BeebVFS1/scsi7.dat", "vfs lun 15", 10);
   s = filesystemSetLunStatus(15, true);
   r = scsiJukebox(0);
   snprintf(why, sizeof why, "start 15 %d, jukebox %d (want 0), VFS dir %u (want 1)",
            s, r, filesystemGetLunDirectoryVFS());
   check("S1 VFS jukebox refused while VFS LUN 15 is started",
         s && !r && filesystemGetLunDirectoryVFS() == 1, why);
   stop_all();
}

int main(void)
{
   disk = calloc(DISK_SECTORS, 512u);
   static uint8_t work[FF_MAX_SS * 4];
   static FATFS mkfs_fs;
   const MKFS_PARM opt = { FM_FAT32, 0, 0, 0, 0 };
   f_mount(&mkfs_fs, "", 0);
   if (!disk || f_mkfs("", &opt, work, sizeof work) != FR_OK) {
      printf("FAIL: could not format the RAM disk\n");
      return 1;
   }
   f_mount(NULL, "", 0);

   filesystemInitialise(0);
   filesystemInitialiseVFS(0);
   filesystemReset();                   /* power on: mount */
   if (!filesystemMounted()) {
      printf("FAIL: could not mount the RAM disk\n");
      return 1;
   }

   test_format_never_started();         /* first: nothing has read LUN 0 yet */
   test_format_started();
   test_format_after_jukebox();
   test_format_expand_fails();
   test_jukebox();

   printf("%d passed, %d failed\n", passes, failures);
   return failures ? 1 : 0;
}
