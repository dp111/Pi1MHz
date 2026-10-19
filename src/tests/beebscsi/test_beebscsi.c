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

   Review 2026-10-06 S3, S5, S6, S8, S10, S11 and P8: whole commands run
   through scsiProcessEmulation() against a scripted host (run_cmd).

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

static const char *cfg_storage;        /* storage= in Pi1MHz.cfg */
const char *config_get(const char *key)
{
   return strcmp(key, "storage") == 0 ? cfg_storage : NULL;
}
bool config_beeb_write_protected(void) { return false; }
uint32_t RPI_GetSystemTime(void) { return 0; }
uint64_t RPI_GetSystemTime64(void) { return 0; }
void videoplayer_media_changed(void) {}

/* The USB drive (usb_storage.c): volume "1:" on the second RAM disk,
   mounted by the test as usb_storage_poll would.  The power-on wait is
   recorded; a drive can be made to turn up during it. */
static bool     usb_mounted;
static int      usb_waits;
static uint64_t usb_wait_until;
static bool     usb_arrives_in_wait;
bool usb_storage_mounted(void) { return usb_mounted; }
bool usb_storage_wait_for_drive(uint64_t until_us)
{
   usb_waits++;
   usb_wait_until = until_us;
   if (usb_arrives_in_wait)
      usb_mounted = true;
   return usb_mounted;
}

/* The bus and the F-code layer: the tests call the LUN layer directly. */
uint8_t scsiFcodeBuffer[256];
uint8_t scsiFcodeBufferRX[256];
static int fcode_writes;               /* F-codes handed to the player */
void fcodeWriteBuffer(uint8_t lunNumber) { (void)lunNumber; fcode_writes++; }
void fcodeReadBuffer(void) {}
void fcodePoll(void) {}
void fcodeClearBuffer(void) {}
void fcode_disc_flip(void) {}
void hd_audio_service(void) {}
void hd_juke_service(void) {}
void hd_card_service(void) {}

/* A scripted host for the bus tests (run_cmd): it selects, then supplies
   the CDB and any data-out bytes from bus_in[]; data-in bytes land in
   bus_out[] and the status byte in bus_status.  The reset flag behaves as
   the firmware's hostadapterReadResetFlag(): set by a host reset (here
   raised once reset_after_in host bytes have been read), cleared in BUS
   FREE by hostadapterWriteResetFlag(false). */
static bool     bus_sel;
static uint8_t  bus_host = 1;          /* 1 = ADFS drive 0, 16 = VFS */
static uint8_t  bus_in[32768];
static unsigned bus_in_len, bus_in_pos;
static uint8_t  bus_out[32768];
static unsigned bus_out_len;
static int      bus_status = -1;       /* -1: no status phase (BUS FREE) */
static bool     ph_msg, ph_cd, ph_io;
static bool     bus_reset;
static int      reset_after_in = -1;
static unsigned dma_short;             /* next write DMA delivers only this many bytes */
static int      fail_disk_after_dma = -1;  /* disk_read fails after this many read DMAs */
static unsigned read_dmas;
static bool     disk_fail;

static uint8_t bus_in_next(void)
{
   uint8_t b = bus_in_pos < bus_in_len ? bus_in[bus_in_pos] : 0;
   bus_in_pos++;
   if (reset_after_in >= 0 && bus_in_pos >= (unsigned)reset_after_in)
      bus_reset = true;
   return b;
}

uint8_t hostadapterReadDatabus(void) { return bus_host; }
uint8_t hostadapterReadByte(void) { return bus_in_next(); }
void hostadapterWriteByte(uint8_t v)
{
   if (ph_cd && ph_io && !ph_msg) bus_status = v;
   else if (!ph_cd && ph_io && bus_out_len < sizeof bus_out) bus_out[bus_out_len++] = v;
}
uint32_t hostadapterPerformReadDMALen(const uint8_t *b, uint32_t len)
{
   for (unsigned i = 0; i < len && bus_out_len < sizeof bus_out; i++)
      bus_out[bus_out_len++] = b[i];
   read_dmas++;
   if (fail_disk_after_dma >= 0 && read_dmas >= (unsigned)fail_disk_after_dma)
      disk_fail = true;
   return len;
}
uint32_t hostadapterPerformWriteDMALen(uint8_t *b, uint32_t len)
{
   unsigned n = dma_short ? dma_short : len;
   dma_short = 0;
   for (unsigned i = 0; i < n; i++)
      b[i] = bus_in_next();
   return n;
}
uint32_t hostadapterPerformReadDMA(const uint8_t *b) { return hostadapterPerformReadDMALen(b, 256); }
uint32_t hostadapterPerformWriteDMA(uint8_t *b) { return hostadapterPerformWriteDMALen(b, 256); }
void hostadapterWriteResetFlag(bool f) { bus_reset = f; }
bool hostadapterReadResetFlag(void) { return bus_reset; }
void hostadapterWriteDataPhaseFlags(bool m, bool c, bool i) { ph_msg = m; ph_cd = c; ph_io = i; }
void hostadapterWriteBusyFlag(bool f) { if (f) bus_sel = false; }
void hostadapterWriteRequestFlag(bool f) { (void)f; }
bool hostadapterReadSelectFlag(void) { return bus_sel; }

/* ---- RAM disk ---------------------------------------------------------- */

#define DISK_SECTORS (128u * 1024u * 2u)        /* 128 MB of 512-byte sectors */
#define USB_SECTORS  (16u * 1024u * 2u)         /* the USB drive: 16 MB */
static uint8_t *disk, *usbdisk;
static int usb_reads, usb_writes;               /* disk_read/write calls on drive 1 */

DSTATUS disk_initialize(BYTE pdrv) { (void)pdrv; return 0; }
DSTATUS disk_status(BYTE pdrv) { (void)pdrv; return 0; }
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
   uint8_t *d = pdrv ? usbdisk : disk;
   usb_reads += pdrv != 0;
   if (disk_fail) return RES_ERROR;
   if (sector + count > (pdrv ? USB_SECTORS : DISK_SECTORS)) return RES_PARERR;
   memcpy(buff, d + (size_t)sector * 512u, (size_t)count * 512u);
   return RES_OK;
}
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
   uint8_t *d = pdrv ? usbdisk : disk;
   usb_writes += pdrv != 0;
   if (sector + count > (pdrv ? USB_SECTORS : DISK_SECTORS)) return RES_PARERR;
   memcpy(d + (size_t)sector * 512u, buff, (size_t)count * 512u);
   return RES_OK;
}
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
   switch (cmd) {
   case CTRL_SYNC:        return RES_OK;
   case GET_SECTOR_COUNT: *(LBA_t *)buff = pdrv ? USB_SECTORS : DISK_SECTORS; return RES_OK;
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

/* A new disc, as SuperForm makes one: MODE SELECT on a LUN with no image
   (scsiCommandModeSelect6: the start fails, the descriptor is created from
   /Pi1MHz/defscsi.cfg, the drive parameter list written over it and saved),
   then FORMAT on the still-stopped LUN (scsiCommandFormat: create the
   image, format it, start it).  The image must have the selected size. */
static const char *defscsi_path;

static void test_format_new_disc(void)
{
   static char why[200];
   const char *t = "S7 new disc: MODE SELECT, FORMAT, START gives the selected size";
   FILE *h = fopen(defscsi_path, "rb");
   static char cfg[16384];
   size_t n = h ? fread(cfg, 1, sizeof cfg, h) : 0;
   if (h) fclose(h);
   if (!n) {
      check(t, false, "precondition: could not read the firmware's defscsi.cfg");
      return;
   }
   f_mkdir("/Pi1MHz");
   put_file("/Pi1MHz/defscsi.cfg", cfg, (UINT)n);
   filesystemSetLunDirectory(1, 4);           /* a directory that does not exist yet */

   bool st = filesystemSetLunStatus(1, true);  /* fails: no image */
   bool cd = filesystemCreateLunDescriptor(1);
   const uint8_t p0[10] = { 0x01, 0x00, 40, 5, 0x00, 0x80, 0x00, 0x80, 0x00, 0x01 };
   filesystemWriteModePageData(1, 0, 10, p0);
   filesystemCopyPage0toPage4(1);
   filesystemConfigToLunGeometry(1);
   bool wa = filesystemWriteAttributes(1);

   bool stopped = !filesystemReadLunStatus(1);
   bool ci = filesystemCreateLunImage(1);
   bool ok = filesystemFormatLun(1, 0x6C);
   bool st2 = filesystemSetLunStatus(1, true);
   long got = size_of("/BeebSCSI4/scsi1.dat");
   snprintf(why, sizeof why, "start %d (want 0), descriptor %d, saved %d, stopped %d, "
            "create %d, format %d, start %d, image %ld bytes, want %lu",
            st, cd, wa, stopped, ci, ok, st2, got, (unsigned long)geometry_bytes(40, 5));
   check(t, !st && cd && wa && stopped && ci && ok && st2 &&
            got == (long)geometry_bytes(40, 5), why);
   filesystemSetLunStatus(1, false);
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

/* S2: a jukebox stops only the swapped directory's LUNs and leaves the card
   mounted, so a file another subsystem holds open on it stays usable.  The
   control is the full filesystemReset() the jukebox used to do. */
static void test_stop_directory_luns(void)
{
   static char why[160];
   FIL other;
   UINT br = 0;
   char buf[4];

   stop_all();
   filesystemSetLunDirectory(1, 3);
   filesystemSetLunDirectory(16, 1);
   bool s0 = filesystemSetLunStatus(0, true);
   bool s8 = filesystemSetLunStatus(8, true);
   FRESULT fo = f_open(&other, "/BeebVFS0/scsi0.dat", FA_READ);

   filesystemStopDirectoryLuns(8);
   FRESULT fr = f_read(&other, buf, sizeof buf, &br);
   snprintf(why, sizeof why, "start 0 %d 8 %d, open %d, LUN0 %d LUN8 %d, read %d",
            s0, s8, fo, filesystemReadLunStatus(0), filesystemReadLunStatus(8), fr);
   check("S2 stopping the VFS directory's LUNs leaves ADFS LUN 0 started",
         s0 && s8 && filesystemReadLunStatus(0) && !filesystemReadLunStatus(8), why);
   check("S2 ... and another subsystem's open file stays valid",
         fo == FR_OK && fr == FR_OK && br == sizeof buf, why);

   filesystemReset();
   fr = f_read(&other, buf, sizeof buf, &br);
   snprintf(why, sizeof why, "read after reset %d (want an error)", fr);
   check("S2 control: a full filesystemReset invalidates that file", fr != FR_OK, why);
   stop_all();
}

/* ---- the bus: whole commands through scsiProcessEmulation() ----------- */

/* One command from selection to BUS FREE.  Returns the status byte, or -1
   if the target went BUS FREE without a status phase. */
static int run_cmd(uint8_t host, const uint8_t *cdb, unsigned cdblen,
                   const uint8_t *data, unsigned datalen)
{
   memcpy(bus_in, cdb, cdblen);
   if (datalen) memcpy(bus_in + cdblen, data, datalen);
   bus_in_len = cdblen + datalen;
   bus_in_pos = 0;
   bus_out_len = 0;
   bus_status = -1;
   bus_host = host;
   bus_sel = true;
   for (int i = 0; i < 64; i++) {
      scsiProcessEmulation();
      if (i > 0 && scsiDiagState() == SCSI_BUSFREE)
         break;
   }
   reset_after_in = -1;
   return bus_status;
}

/* REQUEST SENSE on `lun` (host 1): the error code byte - byte 0 of the
   Adaptec's 4-byte sense, or the additional sense code (byte 12) of a SCSI
   drive's extended sense, which the FileStore reads as its error number. */
static int sense_of(uint8_t lun)
{
   const uint8_t rs[6] = { 0x03, (uint8_t)(lun << 5), 0, 0, 18, 0 };
   int st = run_cmd(1, rs, 6, NULL, 0);
   if (st != 0 || bus_out_len < 4)
      return -1;
   if ((bus_out[0] & 0x7F) == 0x70)
      return bus_out_len >= 13 ? bus_out[12] : -1;
   return bus_out[0];
}

static void read_file(const char *path, void *buf, UINT len)
{
   FIL f;
   UINT done = 0;
   memset(buf, 0, len);
   if (__real_f_open(&f, path, FA_READ) == FR_OK) {
      (void)f_read(&f, buf, len, &done);
      __real_f_close(&f);
   }
}

/* S3: a FAT read error part-way through BSFATREAD. */
static void test_bsfatread_failure(void)
{
   static char why[160];
   static uint8_t data[2048];
   memset(data, 0x5A, sizeof data);
   f_mkdir("/Transfer");
   put_file("/Transfer/big.bin", data, sizeof data);
   stop_all();

   /* G6 0x14: block offset 0, 4 blocks, FAT file 0 (the only entry). */
   const uint8_t cdb[6] = { 0xD4, 0, 0, 0, 4, 0 };
   int ok = run_cmd(1, cdb, 6, NULL, 0);
   check("S3 BSFATREAD control: 4 blocks, status GOOD", ok == 0 && bus_out_len == 1024,
         "control transfer failed");

   /* The disc fails after the first block: block 3 needs a new sector. */
   read_dmas = 0;
   fail_disk_after_dma = 1;
   int st = run_cmd(1, cdb, 6, NULL, 0);
   fail_disk_after_dma = -1;
   disk_fail = false;
   int sense = sense_of(0);
   snprintf(why, sizeof why, "status %d (want 2, -1 = BUS FREE mid data-in), sense %d (want 4)",
            st, sense);
   check("S3 BSFATREAD read failure ends in CHECK CONDITION with sense", st == 2 && sense == 4, why);
}

/* S5: CHECK CONDITION always comes with sense data. */
static void test_sense_set(void)
{
   static char why[120];
   const uint8_t bad[6] = { 0x05, 0, 0, 0, 0, 0 };      /* group 0 opcode 5: none */
   int st = run_cmd(1, bad, 6, NULL, 0);
   int sense = sense_of(0);
   snprintf(why, sizeof why, "status %d, sense %d (want 2, 0x20)", st, sense);
   check("S5 unknown opcode gives CHECK CONDITION and INVALID COMMAND sense",
         st == 2 && sense == 0x20, why);

   const uint8_t inq[6] = { 0x12, 5 << 5, 0, 0, 36, 0 }; /* LUN 5: never started */
   st = run_cmd(1, inq, 6, NULL, 0);
   sense = sense_of(5);
   snprintf(why, sizeof why, "status %d, sense %d (want 2, 2)", st, sense);
   check("S5 INQUIRY of a never-started LUN gives UNIT NOT READY sense",
         st == 2 && sense == 0x02, why);

   /* A SCSI drive: not ready as extended sense, additional sense code 04.
      An Adaptec board refuses INQUIRY outright (see test_controller_gating). */
   scsiSetController(SCSI_CONTROLLER_SCSI);
   st = run_cmd(1, inq, 6, NULL, 0);
   sense = sense_of(5);
   scsiSetController(SCSI_CONTROLLER_MIXED);
   snprintf(why, sizeof why, "status %d, sense %d (want 2, 4)", st, sense);
   check("S5 ... and a SCSI drive's gives NOT READY, ASC 04",
         st == 2 && sense == 0x04, why);
}

/* S8: a short WRITE F-code transfer must not run. */
static void test_fcode_short(void)
{
   static char why[120];
   static uint8_t fc[256];
   memset(fc, 0, sizeof fc);
   memcpy(fc, "E0\r", 3);
   const uint8_t cdb[6] = { 0xCA, 0, 0, 0, 1, 0 };     /* G6 0x0A, LUN 0 (+8) */
   fcode_writes = 0;
   int st = run_cmd(16, cdb, 6, fc, sizeof fc);
   snprintf(why, sizeof why, "status %d, F-codes run %d", st, fcode_writes);
   check("S8 WRITE F-code control: a full block runs", st == 0 && fcode_writes == 1, why);

   fcode_writes = 0;
   dma_short = 100;
   st = run_cmd(16, cdb, 6, fc, 100);
   snprintf(why, sizeof why, "status %d, F-codes run %d (want 0)", st, fcode_writes);
   check("S8 WRITE F-code with a short transfer does not run", fcode_writes == 0, why);
}

/* A drive with its own controller keeps its sense across REQUEST SENSE:
   the FileStore MOS asks for it after a CHECK CONDITION, then a tool asks
   again and still sees sense key 5 (measured on a real drive).  The next
   other command clears it. */
static void test_sense_kept(void)
{
   static char why[200];
   scsiSetController(SCSI_CONTROLLER_SCSI);
   static const char cfg[] =
      "PageControl=CCS\n"
      "ModeParamHeader=10000000\n"
      "LBADescriptor=000123A800000100\n"
      "ModePage3=03130132013200060006003D010000080000000080\n"
      "ModePage4=040400013204\n";
   stop_all();
   filesystemSetLunDirectory(1, 0);
   put_file("/BeebSCSI0/scsi0.cfg", cfg, sizeof cfg - 1);
   put_file("/BeebSCSI0/scsi0.dat", "image", 5);
   scsiClearPendingPages(0);
   filesystemSetLunStatus(0, true);
   const uint8_t ms7[6] = { 0x1A, 0, 0x07, 0, 255, 0 };      /* a page it does not hold */
   int st = run_cmd(1, ms7, 6, NULL, 0);
   const uint8_t rs[6] = { 0x03, 0, 0, 0, 18, 0 };
   run_cmd(1, rs, 6, NULL, 0);
   int k1 = bus_out_len > 2 ? bus_out[2] : -1;
   run_cmd(1, rs, 6, NULL, 0);
   int k2 = bus_out_len > 2 ? bus_out[2] : -1, a2 = bus_out_len > 12 ? bus_out[12] : -1;
   const uint8_t tur[6] = { 0x00, 0, 0, 0, 0, 0 };
   run_cmd(1, tur, 6, NULL, 0);
   run_cmd(1, rs, 6, NULL, 0);
   int k3 = bus_out_len > 2 ? bus_out[2] : -1;
   snprintf(why, sizeof why, "status %d; keys %d, %d (ASC %02X), after TUR %d (want 5, 5, 24, 0)", st, k1, k2, a2, k3);
   check("SCSI drive: REQUEST SENSE keeps the sense; the next other command clears it",
         st == 2 && k1 == 5 && k2 == 5 && a2 == 0x24 && k3 == 0, why);
   f_unlink("/BeebSCSI0/scsi0.cfg");
   stop_all();
}

/* DriveGeometry=: a drive .cfg without its geometry page.  Auto deduces the
   geometry from the image size and publishes it as page 4 (with a warning in
   the log); CFG invents nothing, so MODE SENSE
   refuses page 4 as from a drive without it, and an Adaptec board without
   its parameter list answers 1C (not formatted by the board). */
static void test_drive_geometry_setting(void)
{
   static char why[200];
   static char image[33 * 256 * 12];             /* 396 blocks: 1 x 12 x 33 */
   static const char nopage4[] =
      "ModeParamHeader=10000008\n"
      "LBADescriptor=0000000000000100\n"
      "ModePage1=0103200405\n";
   const uint8_t ms4[6] = { 0x1A, 0, 0x04, 0, 255, 0 };
   bool okName = scsiSetGeometryName("auto") && !scsiGeometryFromCfgOnly() &&
                 scsiSetGeometryName("CFG") && scsiGeometryFromCfgOnly() &&
                 !scsiSetGeometryName("sometimes") && !scsiGeometryFromCfgOnly() &&
                 scsiSetGeometryName(NULL) && !scsiGeometryFromCfgOnly();
   check("DriveGeometry= names: Auto, CFG (any case), absent = Auto, unknown = Auto and reported", okName,
         "name parsing wrong");

   /* Auto, a SCSI drive without page 4: page 4 deduced and published */
   scsiSetController(SCSI_CONTROLLER_SCSI);
   scsiSetGeometryFromCfgOnly(false);
   stop_all();
   filesystemSetLunDirectory(1, 0);
   put_file("/BeebSCSI0/scsi0.cfg", nopage4, sizeof nopage4 - 1);
   put_file("/BeebSCSI0/scsi0.dat", image, sizeof image);
   scsiClearPendingPages(0);
   filesystemSetLunStatus(0, true);
   int st = run_cmd(1, ms4, 6, NULL, 0);
   snprintf(why, sizeof why, "status %d, %u bytes, page %02X, heads %d (want 12)", st, bus_out_len,
            bus_out_len > 12 ? bus_out[12] : 0, bus_out_len > 17 ? bus_out[17] : -1);
   check("DriveGeometry=Auto: a SCSI drive without page 4 gets one deduced from the image (1 x 12 x 33)",
         st == 0 && bus_out_len >= 18 && bus_out[12] == 0x04 && bus_out[17] == 12, why);

   /* CFG: nothing invented; page 4 refused, the image still readable */
   scsiSetGeometryFromCfgOnly(true);
   stop_all();
   put_file("/BeebSCSI0/scsi0.cfg", nopage4, sizeof nopage4 - 1);
   filesystemSetLunStatus(0, true);
   st = run_cmd(1, ms4, 6, NULL, 0);
   int sense = sense_of(0);
   size_t l4 = 0;
   const char *p4 = filesystemGetModePageData(0, 4, &l4);
   snprintf(why, sizeof why, "status %d sense %02X (want 2, 24), page 4 %s, capacity %lu blocks (want 396)",
            st, sense, p4 ? "INVENTED" : "absent", (unsigned long)filesystemGetLunTotalSectors(0));
   check("DriveGeometry=CFG: no page 4 is invented; MODE SENSE refuses it; capacity from the image",
         st == 2 && sense == 0x24 && p4 == NULL && filesystemGetLunTotalSectors(0) == 396, why);

   /* CFG on an Adaptec board without its parameter list: 1C, as a disc the board never formatted */
   scsiSetController(SCSI_CONTROLLER_ACB4000);
   stop_all();
   put_file("/BeebSCSI0/scsi0.cfg", nopage4, sizeof nopage4 - 1);
   filesystemSetLunStatus(0, true);
   const uint8_t sf[6] = { 0x1A, 0, 0, 0, 0x16, 0 };
   st = run_cmd(1, sf, 6, NULL, 0);
   sense = sense_of(0);
   snprintf(why, sizeof why, "status %d sense %02X (want 2, 1C)", st, sense);
   check("DriveGeometry=CFG: an Adaptec board with no list in the .cfg answers MODE SENSE with 1C", st == 2 && sense == 0x1C, why);

   scsiSetGeometryFromCfgOnly(false);
   scsiSetController(SCSI_CONTROLLER_MIXED);
   f_unlink("/BeebSCSI0/scsi0.cfg");
   stop_all();
}

/* A drive that holds page 0 (an RO3085S, as a real one answers): page 0 is
   held (80 02 00 00, the Unit Attention page), the geometry is page 4, every
   reply has a block descriptor, and the PS bit stays set after the FileStore
   formatter writes (C)Acorn with page codes 25/26 (without PS). */
static void test_scsi_page0_ps(void)
{
   static char why[200];
   scsiSetController(SCSI_CONTROLLER_SCSI);
   static const char cfg[] =
      "ModeParamHeader=10000008\n"
      "LBADescriptor=0003D86000000100\n"
      "ModePage0=80020000\n"
      "ModePage3=031302EE02EE000600060030010000080000000080\n"
      "ModePage4=04040002EE07\n"
      "ModePage37=A506000000000000\n"
      "ModePage38=A606000000000000\n";
   stop_all();
   filesystemSetLunDirectory(1, 0);
   put_file("/BeebSCSI0/scsi0.cfg", cfg, sizeof cfg - 1);
   put_file("/BeebSCSI0/scsi0.dat", "image", 5);
   scsiClearPendingPages(0);
   filesystemSetLunStatus(0, true);

   const uint8_t ms0[6] = { 0x1A, 0, 0x00, 0, 255, 0 };
   int st = run_cmd(1, ms0, 6, NULL, 0);
   snprintf(why, sizeof why, "status %d, %u bytes, BD length %d, page %02X %02X %02X %02X; cylinders %u heads %u",
            st, bus_out_len, bus_out_len > 3 ? bus_out[3] : -1,
            bus_out_len > 12 ? bus_out[12] : 0, bus_out_len > 13 ? bus_out[13] : 0,
            bus_out_len > 14 ? bus_out[14] : 0, bus_out_len > 15 ? bus_out[15] : 0,
            (unsigned)filesystemGetLunCylinders(0), (unsigned)filesystemGetheadspercylinder(0));
   check("SCSI drive: page 0 is an ordinary page (80 02 00 00), the geometry is page 4 (750 x 7)",
         st == 0 && bus_out_len == 16 && bus_out[3] == 8 && bus_out[12] == 0x80 && bus_out[13] == 0x02 &&
         filesystemGetLunCylinders(0) == 750 && filesystemGetheadspercylinder(0) == 7, why);

   /* FServFmt's RO3000 path: both user pages in one MODE SELECT, SP set, codes without PS */
   const uint8_t sel[6] = { 0x15, 0x01, 0, 0, 20, 0 };
   const uint8_t d[20] = { 0, 0, 0, 0,  0x25, 6, '(', 'C', ')', 'A', 0x0D, 0,  0x26, 6, 'c', 'o', 'r', 'n', 0x0D, 0 };
   st = run_cmd(1, sel, 6, d, sizeof d);
   size_t l37 = 0, l38 = 0;
   const char *p37 = filesystemGetModePageData(0, 37, &l37);
   const char *p38 = filesystemGetModePageData(0, 38, &l38);
   snprintf(why, sizeof why, "status %d (want 0), page 37 code %02X (want A5) text %s, page 38 code %02X (want A6)", st,
            p37 ? (uint8_t)p37[0] : 0, (p37 && l37 >= 6 && memcmp(&p37[2], "(C)A", 4) == 0) ? "ok" : "WRONG",
            p38 ? (uint8_t)p38[0] : 0);
   check("SCSI drive: the PS bit stays after a MODE SELECT of the (C)Acorn pages",
         st == 0 && p37 && p38 && (uint8_t)p37[0] == 0xA5 && (uint8_t)p38[0] == 0xA6 && memcmp(&p37[2], "(C)A", 4) == 0, why);

   /* an RO652-style drive (no ModePage0): page 0 refused, none invented */
   static const char cfg652[] =
      "PageControl=CCS\n"
      "ModeParamHeader=10000000\n"
      "LBADescriptor=000123A800000100\n"
      "ModePage3=03130132013200060006003D010000080000000080\n"
      "ModePage4=040400013204\n";
   stop_all();
   put_file("/BeebSCSI0/scsi0.cfg", cfg652, sizeof cfg652 - 1);
   filesystemSetLunStatus(0, true);
   st = run_cmd(1, ms0, 6, NULL, 0);
   int sense = sense_of(0);
   size_t l0 = 0;
   const char *p0 = filesystemGetModePageData(0, 0, &l0);
   snprintf(why, sizeof why, "status %d sense %02X (want 2, 24), page 0 %s, cylinders %u", st, sense,
            p0 ? "INVENTED" : "absent", (unsigned)filesystemGetLunCylinders(0));
   check("SCSI drive without page 0: refused (24h), none invented, geometry from page 4 (306)",
         st == 2 && sense == 0x24 && p0 == NULL && filesystemGetLunCylinders(0) == 306, why);

   f_unlink("/BeebSCSI0/scsi0.cfg");
   stop_all();
}

/* The FileStore formatter's page 3 on a drive with its real page 3 mask (an
   RO652's MaskPage3, as measured on one): the page is read, bytes 2-11 and 14-17
   zeroed, bytes per sector set to 256, and sent with len+4 bytes, two short.
   On the drive the mask decides: the zeros in read-only fields (sectors per
   track, interleave) are ignored, a zero in a changeable one (track skew)
   is taken, and the two missing bytes keep their values (RO652 ROM 5.02). */
static void test_fservfmt_page3_masked(void)
{
   static char why[200];
   scsiSetController(SCSI_CONTROLLER_SCSI);
   static const char cfg[] =
      "PageControl=CCS\n"
      "ModeParamHeader=10000000\n"
      "LBADescriptor=000123A800000100\n"
      "ModePage3=03130132013200060006003D010000080005000080\n"
      "MaskPage3=03130000000000000000000007FF0000007F007F00\n"
      "ModePage4=040400013204\n";
   stop_all();
   filesystemSetLunDirectory(1, 0);
   put_file("/BeebSCSI0/scsi0.cfg", cfg, sizeof cfg - 1);
   put_file("/BeebSCSI0/scsi0.dat", "image", 5);
   scsiClearPendingPages(0);     /* the test before left a pending page 3 on LUN 0 */
   filesystemSetLunStatus(0, true);

   /* header all zero (the RO652 takes no block descriptor), then 19 of page 3's 21 bytes */
   const uint8_t ms23[6] = { 0x15, 0x00, 0, 0, 23, 0 };
   const uint8_t d3[23] = { 0, 0, 0, 0,  3, 0x13,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  1, 0,  0, 0, 0, 0,  0 };
   int st = run_cmd(1, ms23, 6, d3, sizeof d3);
   static const uint8_t want[] = { 0x03, 0x13, 0x01, 0x32, 0x01, 0x32, 0x00, 0x06, 0x00, 0x06,
                                   0x00, 0x3D, 0x01, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x80 };
   size_t l3 = 0;
   const char *g3 = scsiGetPendingModePageData(0, 3, &l3);
   snprintf(why, sizeof why, "status %d (want 0), pending sectors/track %02X%02X (want 003D), interleave %02X (want 08), "
            "track skew %02X (want 00), drive type %02X (want 80)", st,
            g3 ? (uint8_t)g3[10] : 0, g3 ? (uint8_t)g3[11] : 0, g3 ? (uint8_t)g3[15] : 0,
            g3 ? (uint8_t)g3[17] : 0, g3 ? (uint8_t)g3[20] : 0);
   check("FServFmt page 3 on an RO652 with its mask: read-only zeros ignored, track skew 0 taken, tail kept",
         st == 0 && g3 != NULL && l3 == sizeof want && memcmp(g3, want, sizeof want) == 0, why);

   f_unlink("/BeebSCSI0/scsi0.cfg");
   stop_all();
}

/* The Acorn FileStore formatter (FServFmt / FSFormat) on a 3000-series drive
   sends its MODE SELECT pages two bytes short (length+4, not length+6) and
   zeroes most of page 3 first.  A real drive takes the bytes that arrive and
   keeps the rest; with no mask in the .cfg (as here) zero means "the drive's
   own value".  The payloads are
   the ones a FileStore sent in a serial log of the RO3057S. */
static int page_is(uint8_t lun, uint8_t page, const uint8_t *want, size_t n)
{
   size_t len = 0;
   const char *got = filesystemGetModePageData(lun, page, &len);
   return got != NULL && len == n && memcmp(got, want, n) == 0;
}

static void test_filestore_mode_select(void)
{
   static char why[200];
   scsiSetController(SCSI_CONTROLLER_SCSI);   /* a drive with its own controller */
   static const char cfg[] =
      "ModeParamHeader=10000008\n"
      "LBADescriptor=00027D8000000100\n"
      "ModePage1=0103200405\n"
      "ModePage3=031302A802A8000600060030010000080000000080\n"
      "ModePage4=04040002A805\n"
      "ModePage37=2506000000000000\n"
      "ModePage38=2606000000000000\n";
   static const uint8_t p1[]  = { 0x01, 0x03, 0x20, 0x04, 0x05 };
   static const uint8_t p3[]  = { 0x03, 0x13, 0x02, 0xA8, 0x02, 0xA8, 0x00, 0x06, 0x00, 0x06,
                                  0x00, 0x30, 0x01, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x80 };
   static const uint8_t p4[]  = { 0x04, 0x04, 0x00, 0x02, 0xA8, 0x05 };

   stop_all();
   filesystemSetLunDirectory(1, 0);
   put_file("/BeebSCSI0/scsi0.cfg", cfg, sizeof cfg - 1);
   put_file("/BeebSCSI0/scsi0.dat", "image", 5);
   bool started = filesystemSetLunStatus(0, true);
   check("FS MODE SELECT: the test drive starts", started, "LUN 0 did not start");

   /* FSFormat PROCromodesel, page 3: 23 bytes where 25 are needed */
   const uint8_t ms23[6] = { 0x15, 0x01, 0, 0, 23, 0 };
   const uint8_t d3[23] = { 0, 0, 0, 0,  3, 19,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  1, 0, 0, 0, 0, 0, 0 };
   int st = run_cmd(1, ms23, 6, d3, sizeof d3);
   snprintf(why, sizeof why, "status %d (want 0), page 3 %s", st,
            page_is(0, 3, p3, sizeof p3) ? "kept" : "CHANGED");
   check("FS MODE SELECT: page 3 two bytes short is accepted, sectors per track kept",
         st == 0 && page_is(0, 3, p3, sizeof p3), why);

   /* page 4: 8 bytes where 10 are needed.  Cylinders and heads must not change. */
   const uint8_t ms8[6] = { 0x15, 0x01, 0, 0, 8, 0 };
   const uint8_t d4[8] = { 0, 0, 0, 0,  4, 4, 0, 2 };
   st = run_cmd(1, ms8, 6, d4, sizeof d4);
   snprintf(why, sizeof why, "status %d (want 0), heads %u (want 5), page 4 %s", st,
            (unsigned)filesystemGetheadspercylinder(0), page_is(0, 4, p4, sizeof p4) ? "kept" : "CHANGED");
   check("FS MODE SELECT: page 4 two bytes short is accepted, the rest of the page kept",
         st == 0 && page_is(0, 4, p4, sizeof p4) && filesystemGetheadspercylinder(0) == 5, why);

   /* page 1: 7 bytes where 9 are needed.  The byte that arrived is applied, the rest kept. */
   const uint8_t ms7[6] = { 0x15, 0x01, 0, 0, 7, 0 };
   const uint8_t d1[7] = { 0, 0, 0, 0,  1, 3, 0x21 };
   static const uint8_t p1new[] = { 0x01, 0x03, 0x21, 0x04, 0x05 };
   st = run_cmd(1, ms7, 6, d1, sizeof d1);
   snprintf(why, sizeof why, "status %d (want 0), page 1 %s", st, page_is(0, 1, p1new, sizeof p1new) ? "as wanted" : "WRONG");
   check("FS MODE SELECT: a cut-short page changes only the bytes it carries",
         st == 0 && page_is(0, 1, p1new, sizeof p1new), why);
   (void)p1;

   /* a whole page 4 is still honoured: the heads change - at FORMAT (a SCSI
      drive's pages 3 and 4 wait for it), so for now only in the pending page */
   const uint8_t ms10[6] = { 0x15, 0x01, 0, 0, 10, 0 };
   const uint8_t d4full[10] = { 0, 0, 0, 0,  4, 4, 0, 2, 0xA8, 6 };
   st = run_cmd(1, ms10, 6, d4full, sizeof d4full);
   size_t l4 = 0;
   const char *g4 = scsiGetPendingModePageData(0, 4, &l4);
   snprintf(why, sizeof why, "status %d (want 0), heads %u (want 5 until FORMAT), pending heads %d (want 6)", st,
            (unsigned)filesystemGetheadspercylinder(0), g4 && l4 > 5 ? g4[5] : -1);
   check("FS MODE SELECT: a whole page 4 still sets the heads (pending until FORMAT)",
         st == 0 && filesystemGetheadspercylinder(0) == 5 && g4 != NULL && l4 > 5 && g4[5] == 6, why);

   /* a page 3 that does carry a new sectors per track sets it (pending too) */
   const uint8_t ms25[6] = { 0x15, 0x01, 0, 0, 25, 0 };
   const uint8_t d3full[25] = { 0, 0, 0, 0,  3, 19,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0x31,  1, 0, 0, 8, 0, 0, 0, 0, 0 };
   st = run_cmd(1, ms25, 6, d3full, sizeof d3full);
   size_t l3 = 0;
   const char *g3 = scsiGetPendingModePageData(0, 3, &l3);
   snprintf(why, sizeof why, "status %d (want 0), pending sectors per track byte %02X (want 31), current %s", st,
            g3 ? (unsigned)(uint8_t)g3[11] : 0u, page_is(0, 3, p3, sizeof p3) ? "kept" : "CHANGED");
   check("FS MODE SELECT: a page 3 with a non-zero sectors per track sets it (pending until FORMAT)",
         st == 0 && g3 != NULL && (uint8_t)g3[11] == 0x31 && page_is(0, 3, p3, sizeof p3), why);

   /* the (C)Acorn pages: both in one MODE SELECT, as a 3000-series drive gets them */
   const uint8_t ms20[6] = { 0x15, 0x01, 0, 0, 20, 0 };
   const uint8_t dc[20] = { 0, 0, 0, 0,  0x25, 6, '(', 'C', ')', 'A', 13, 0,  0x26, 6, 'c', 'o', 'r', 'n', 13, 0 };
   static const uint8_t p37[] = { 0x25, 0x06, '(', 'C', ')', 'A', 13, 0 };
   static const uint8_t p38[] = { 0x26, 0x06, 'c', 'o', 'r', 'n', 13, 0 };
   st = run_cmd(1, ms20, 6, dc, sizeof dc);
   snprintf(why, sizeof why, "status %d (want 0), page 37 %s, page 38 %s", st,
            page_is(0, 37, p37, sizeof p37) ? "ok" : "WRONG", page_is(0, 38, p38, sizeof p38) ? "ok" : "WRONG");
   check("FS MODE SELECT: the (C)Acorn pages are written",
         st == 0 && page_is(0, 37, p37, sizeof p37) && page_is(0, 38, p38, sizeof p38), why);

   /* a page header with no data at all changes nothing */
   size_t before_len = 0;
   uint8_t before[64];
   const char *g = filesystemGetModePageData(0, 3, &before_len);
   if (g == NULL || before_len > sizeof before) before_len = 0;
   if (before_len) memcpy(before, g, before_len);
   const uint8_t ms6[6] = { 0x15, 0x01, 0, 0, 6, 0 };
   const uint8_t dhdr[6] = { 0, 0, 0, 0,  3, 19 };
   st = run_cmd(1, ms6, 6, dhdr, sizeof dhdr);
   snprintf(why, sizeof why, "status %d (want 0), page 3 %s", st,
            before_len && page_is(0, 3, before, before_len) ? "unchanged" : "CHANGED");
   check("FS MODE SELECT: a page header with no data changes nothing",
         st == 0 && before_len && page_is(0, 3, before, before_len), why);
   stop_all();
   scsiSetController(SCSI_CONTROLLER_MIXED);
}

/* The Acorn FileStore formatter writes page 1 back with its last byte zero (a
   650-series drive gets all 9 bytes).  A WritPage1 mask of zeros keeps the
   page as it is; without one the page changes. */
static void test_write_mask_page1(void)
{
   static char why[200];
   scsiSetController(SCSI_CONTROLLER_SCSI);   /* a drive with its own controller */
   static const uint8_t p1[]     = { 0x01, 0x03, 0x20, 0x04, 0x05 };
   static const uint8_t p1zero[] = { 0x01, 0x03, 0x20, 0x04, 0x00 };
   const uint8_t ms9[6] = { 0x15, 0x00, 0, 0, 9, 0 };
   const uint8_t d1[9] = { 0, 0, 0, 0,  1, 3, 0x20, 4, 0 };
   static const char plain[] = "ModePage1=0103200405\n";
   static const char masked[] = "ModePage1=0103200405\nWritPage1=0000000000\n";

   stop_all();
   filesystemSetLunDirectory(1, 0);
   put_file("/BeebSCSI0/scsi0.dat", "image", 5);
   put_file("/BeebSCSI0/scsi0.cfg", plain, sizeof plain - 1);
   bool started = filesystemSetLunStatus(0, true);
   int st = run_cmd(1, ms9, 6, d1, sizeof d1);
   snprintf(why, sizeof why, "start %d, status %d (want 0), page 1 %s", started, st,
            page_is(0, 1, p1zero, sizeof p1zero) ? "changed as the tool writes it" : "NOT as expected");
   check("FS WritPage1: without a mask the formatter's page 1 write changes the page",
         started && st == 0 && page_is(0, 1, p1zero, sizeof p1zero), why);

   stop_all();
   put_file("/BeebSCSI0/scsi0.cfg", masked, sizeof masked - 1);
   started = filesystemSetLunStatus(0, true);
   st = run_cmd(1, ms9, 6, d1, sizeof d1);
   snprintf(why, sizeof why, "start %d, status %d (want 0), page 1 %s", started, st,
            page_is(0, 1, p1, sizeof p1) ? "kept" : "CHANGED");
   check("FS WritPage1: with WritPage1=0000000000 the page is kept and the command is GOOD",
         started && st == 0 && page_is(0, 1, p1, sizeof p1), why);
   stop_all();
   scsiSetController(SCSI_CONTROLLER_MIXED);
}

/* P8 (scsi.c side): a host reset or reselection part-way through a
   data-out phase abandons the command before anything is saved. */
static void test_reset_abandons(void)
{
   static char why[160];
   static char before[256], after[256];
   stop_all();
   filesystemSetLunDirectory(1, 0);
   put_cfg("/BeebSCSI0/scsi0.cfg", 10, 2);
   bool s = filesystemSetLunStatus(0, true);

   /* MODE SELECT: header, LBA descriptor, drive parameter list 50 x 6 */
   const uint8_t ms[6] = { 0x15, 0, 0, 0, 22, 0 };
   const uint8_t msd[22] = { 0, 0, 0, 8,  0, 0, 0, 0, 0, 0, 1, 0,
                             1, 0, 50, 6, 0, 0x80, 0, 0x80, 0, 1 };
   read_file("/BeebSCSI0/scsi0.cfg", before, sizeof before - 1);
   reset_after_in = 6 + 10;
   int st = run_cmd(1, ms, 6, msd, sizeof msd);
   read_file("/BeebSCSI0/scsi0.cfg", after, sizeof after - 1);
   snprintf(why, sizeof why, "start %d, status %d (want -1), heads %u (want 2), cfg %s",
            s, st, (unsigned)filesystemGetheadspercylinder(0),
            strcmp(before, after) ? "CHANGED" : "unchanged");
   check("P8 MODE SELECT cut by a host reset saves nothing",
         s && st == -1 && filesystemGetheadspercylinder(0) == 2 && !strcmp(before, after), why);

   st = run_cmd(1, ms, 6, msd, sizeof msd);
   snprintf(why, sizeof why, "status %d, heads %u (want 0, 6)",
            st, (unsigned)filesystemGetheadspercylinder(0));
   check("P8 MODE SELECT control: uncut, it is saved",
         st == 0 && filesystemGetheadspercylinder(0) == 6, why);
   stop_all();
   put_cfg("/BeebSCSI0/scsi0.cfg", 10, 2);

   /* BSSELECT (jukebox) cut part-way through its 8 bytes */
   const uint8_t bs[6] = { 0xD1, 0, 0, 0, 8, 0 };
   const uint8_t bsd[8] = { 3, 0, 0, 0, 0, 0, 0, 0 };
   reset_after_in = 6 + 4;
   st = run_cmd(1, bs, 6, bsd, sizeof bsd);
   snprintf(why, sizeof why, "status %d (want -1), directory %u (want 0)",
            st, filesystemGetLunDirectory());
   check("P8 BSSELECT cut by a host reset does not jukebox",
         st == -1 && filesystemGetLunDirectory() == 0, why);

   /* A CDB cut part-way is never run as a command */
   const uint8_t tur[6] = { 0x00, 0, 0, 0, 0, 0 };
   s = filesystemSetLunStatus(0, true);
   reset_after_in = 3;
   st = run_cmd(1, tur, 6, NULL, 0);
   int st2 = run_cmd(1, tur, 6, NULL, 0);
   snprintf(why, sizeof why, "status %d (want -1), then uncut %d (want 0)", st, st2);
   check("P8 a CDB cut by a host reset is not run", s && st == -1 && st2 == 0, why);
   stop_all();
}

/* S9: FORMAT takes the sectors per track from the interleave, as the
   ACB-4000 does (issue #2): 32 for 1:1, otherwise 33, kept in the LUN's
   .cfg.  A LUN set to any other value keeps it. */
static void put_cfg_spt(const char *path, unsigned int cylinders, unsigned int heads,
                        unsigned int spt)
{
   char cfg[160];
   int n = snprintf(cfg, sizeof cfg,
                    "ModePage0=01%04X%02X008000800001\n"
                    "ModePage3=03150132013200060006%04X0100000800000000800000\n",
                    cylinders, heads, spt);
   put_file(path, cfg, (UINT)n);
}

static void format_with(unsigned interleave, int *status, long *bytes, uint32_t *spt)
{
   const uint8_t fm[6] = { 0x04, 0, 0x6C, (uint8_t)(interleave >> 8), (uint8_t)interleave, 0 };
   *status = run_cmd(1, fm, 6, NULL, 0);
   *bytes = size_of("/BeebSCSI6/scsi0.dat");
   stop_all();                          /* the next start reads the saved .cfg */
   *spt = filesystemSetLunStatus(0, true) ? filesystemGetLunSPTSize(0) : 0u;
   stop_all();
}

static void test_format_interleave(void)
{
   static char why[200];
   int st; long got; uint32_t spt;
   f_mkdir("/BeebSCSI6");
   put_cfg_spt("/BeebSCSI6/scsi0.cfg", 10, 2, 33);
   put_file("/BeebSCSI6/scsi0.dat", "old image", 9);
   stop_all();
   filesystemSetLunDirectory(1, 6);

   format_with(1, &st, &got, &spt);
   snprintf(why, sizeof why, "status %d, image %ld bytes (want %u), now %lu per track",
            st, got, 10u * 2u * 32u * 256u, (unsigned long)spt);
   check("S9 FORMAT with interleave 1 gives 32 sectors per track, kept in the .cfg",
         st == 0 && got == 10L * 2 * 32 * 256 && spt == 32u, why);

   format_with(2, &st, &got, &spt);
   snprintf(why, sizeof why, "status %d, image %ld bytes (want %u), now %lu per track",
            st, got, 10u * 2u * 33u * 256u, (unsigned long)spt);
   check("S9 ... and with interleave 2, 33 again",
         st == 0 && got == 10L * 2 * 33 * 256 && spt == 33u, why);

   format_with(0, &st, &got, &spt);
   check("S9 ... interleave 0 is not 1: 33", st == 0 && spt == 33u, "changed");

   put_cfg_spt("/BeebSCSI6/scsi0.cfg", 10, 2, 17);
   format_with(1, &st, &got, &spt);
   snprintf(why, sizeof why, "status %d, image %ld bytes (want %u), now %lu per track",
            st, got, 10u * 2u * 17u * 256u, (unsigned long)spt);
   check("S9 ... a LUN set to another value (17) keeps it",
         st == 0 && got == 10L * 2 * 17 * 256 && spt == 17u, why);

   /* The ACB-4000 lays down its table only where the board chose the value
      (32/33 at 256 bytes): a LUN set to 17 or 63 in its .cfg keeps it */
   scsiSetController(SCSI_CONTROLLER_ACB4000);
   const unsigned kept[] = { 17, 63 };
   for (size_t i = 0; i < sizeof kept / sizeof kept[0]; i++) {
      put_cfg_spt("/BeebSCSI6/scsi0.cfg", 10, 2, kept[i]);
      format_with(2, &st, &got, &spt);
      snprintf(why, sizeof why, "%u: status %d, image %ld bytes (want %u), now %lu per track", kept[i],
               st, got, 10u * 2u * kept[i] * 256u, (unsigned long)spt);
      check("S9 ... an ACB-4000 LUN set to 17 or 63 keeps it",
            st == 0 && got == 10L * 2 * (long)kept[i] * 256 && spt == kept[i], why);
   }

   /* SASI keeps the .cfg's value */
   scsiSetController(SCSI_CONTROLLER_SASI);
   put_cfg_spt("/BeebSCSI6/scsi0.cfg", 10, 2, 17);
   format_with(1, &st, &got, &spt);
   scsiSetController(SCSI_CONTROLLER_MIXED);
   snprintf(why, sizeof why, "status %d, image %ld bytes (want %u), now %lu per track",
            st, got, 10u * 2u * 17u * 256u, (unsigned long)spt);
   check("S9 ... a SASI LUN set to another value (17) keeps it",
         st == 0 && got == 10L * 2 * 17 * 256 && spt == 17u, why);

   filesystemSetLunDirectory(1, 0);
}

/* The Adaptec boards' MODE SENSE and SELECT list, and their FORMAT table. */
static void test_acb_mode_and_format(void)
{
   static char why[240];
   int st; long got; uint32_t spt;
   stop_all();
   scsiSetController(SCSI_CONTROLLER_ACB4000);
   f_mkdir("/BeebSCSI6");
   {
      static const char cfg[] =
         "ModeParamHeader=10000008\nLBADescriptor=0000000000000100\n"
         "ModePage0=01000A02008000800001\n"
         "ModePage3=0315013201320006000600210100000800000000800000\n"
         "ModePage4=0406000A02040000\n";
      put_file("/BeebSCSI6/scsi0.cfg", cfg, sizeof cfg - 1);
   }
   put_file("/BeebSCSI6/scsi0.dat", "old image", 9);
   filesystemSetLunDirectory(1, 6);

   /* SuperForm's MODE SENSE: 22 bytes.  The board has no page codes: CDB
      byte 2 is reserved, so a page asked for is a bad argument (24). */
   static const uint8_t flat[22] = { 0, 0, 0, 8,  0, 0, 0, 0, 0, 0, 1, 0,
                                     1, 0, 10, 2, 0, 0x80, 0, 0x80, 0, 1 };
   const uint8_t sf[6] = { 0x1A, 0, 0, 0, 0x16, 0 };
   st = run_cmd(1, sf, 6, NULL, 0);
   bool a = st == 0 && bus_out_len == 22 && memcmp(bus_out, flat, 22) == 0;
   const uint8_t p3[6] = { 0x1A, 0, 0x03, 0, 0xFF, 0 };
   st = run_cmd(1, p3, 6, NULL, 0);
   bool b = st == 2 && sense_of(0) == 0x24;
   const uint8_t cut[6] = { 0x1A, 0, 0x00, 0, 12, 0 };
   st = run_cmd(1, cut, 6, NULL, 0);
   bool c = st == 0 && bus_out_len == 12 && memcmp(bus_out, flat, 12) == 0;
   snprintf(why, sizeof why, "list %d, page 3 refused %d, allocation 12 %d", a, b, c);
   check("ACB MODE SENSE: the flat 22-byte list, cut to the allocation length; a page code is refused (24)",
         a && b && c, why);

   /* MODE SELECT: refused lists (each 24) */
   static const uint8_t good[22] = { 0, 0, 0, 8,  0, 0, 0, 0, 0, 0, 2, 0,
                                     1, 0, 10, 2, 0, 0x80, 0, 0x80, 0, 1 };
   uint8_t bad[24];
   const uint8_t ms22[6] = { 0x15, 0, 0, 0, 22, 0 };
   struct { int at; uint8_t value; const char *what; } cases[] = {
      { 4, 1, "density 1" }, { 10, 3, "block size 768" }, { 12, 2, "list format 02 in 22 bytes" },
      { 13, 0x09, "2304 cylinders" }, { 15, 0, "0 heads" }, { 15, 17, "17 heads" }, { 3, 0, "no extent" },
   };
   bool refused = true;
   for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
      memcpy(bad, good, 22);
      bad[cases[i].at] = cases[i].value;
      st = run_cmd(1, ms22, 6, bad, 22);
      int sense = sense_of(0);
      if (st != 2 || sense != 0x24) {
         refused = false;
         snprintf(why, sizeof why, "%s: status %d sense %02X", cases[i].what, st, sense);
      }
   }
   const uint8_t ms20[6] = { 0x15, 0, 0, 0, 20, 0 };
   st = run_cmd(1, ms20, 6, good, 20);
   refused = refused && st == 2 && sense_of(0) == 0x24;
   check("ACB MODE SELECT: density, block size, list format, cylinders, heads and length checked (24)",
         refused, why);

   /* a 12-byte list (no drive parameters) is legal; its block size (512)
      waits for FORMAT, so a format abandoned after it (SuperForm stopped)
      leaves the 256-byte image read in 256-byte blocks */
   const uint8_t ms12[6] = { 0x15, 0, 0, 0, 12, 0 };
   st = run_cmd(1, ms12, 6, good, 12);
   stop_all();
   bool s = filesystemSetLunStatus(0, true);
   const uint8_t rc[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
   int rcst = run_cmd(1, rc, 10, NULL, 0);
   uint32_t rcbs = ((uint32_t)bus_out[4] << 24) | ((uint32_t)bus_out[5] << 16) | ((uint32_t)bus_out[6] << 8) | bus_out[7];
   snprintf(why, sizeof why, "status %d, start %d, block size %lu (want 256 until FORMAT), data path %lu, "
            "READ CAPACITY %d %lu, heads %lu (want 2)", st, s, (unsigned long)filesystemGetLunBlockSize(0),
            (unsigned long)filesystemGetLunDataBlockSize(0), rcst, (unsigned long)rcbs,
            (unsigned long)filesystemGetheadspercylinder(0));
   check("ACB MODE SELECT: a 12-byte list is legal; its block size waits for FORMAT",
         st == 0 && s && filesystemGetLunBlockSize(0) == 256 && filesystemGetLunDataBlockSize(0) == 256 &&
         rcst == 0 && rcbs == 256 && filesystemGetheadspercylinder(0) == 2, why);
   stop_all();

   /* FORMAT at 512 bytes: 17 for interleave 1, 18 otherwise */
   format_with(1, &st, &got, &spt);
   snprintf(why, sizeof why, "status %d, image %ld (want %u), %lu per track", st, got, 10u * 2u * 17u * 512u,
            (unsigned long)spt);
   check("ACB FORMAT: 512-byte blocks, interleave 1: 17 per track", st == 0 && spt == 17 &&
         got == 10L * 2 * 17 * 512, why);
   format_with(5, &st, &got, &spt);
   check("ACB FORMAT: ... interleave 5: 18", st == 0 && spt == 18 && got == 10L * 2 * 18 * 512, "wrong");

   /* back to 256 with the whole list: interleave 0 is 2 (33), 1 is 32 */
   memcpy(bad, good, 22);
   bad[10] = 1;
   st = run_cmd(1, ms22, 6, bad, 22);
   format_with(0, &st, &got, &spt);
   bool i0 = st == 0 && spt == 33;
   format_with(1, &st, &got, &spt);
   snprintf(why, sizeof why, "interleave 0: %d, interleave 1: status %d spt %lu", i0, st, (unsigned long)spt);
   check("ACB FORMAT: 256 bytes, interleave 0 (= 2) gives 33, 1 gives 32", i0 && st == 0 && spt == 32, why);

   /* interleave limit: 32 at 256 (33 per track) is fine, 33 is error 24;
      a high byte is an interleave over the limit too */
   format_with(32, &st, &got, &spt);
   int ok32 = st;
   format_with(33, &st, &got, &spt);
   int sense = sense_of(0);
   format_with(0x100, &st, &got, &spt);
   int sense2 = sense_of(0);
   snprintf(why, sizeof why, "interleave 32 %d, 33 %d sense %02X, 256 sense %02X", ok32, st, sense, sense2);
   check("ACB-4000 FORMAT: an interleave above sectors per track - 1 is error 24",
         ok32 == 0 && st == 2 && sense == 0x24 && sense2 == 0x24, why);

   /* ACB-4070: 46 / 47, and its interleave error is 1A */
   scsiSetController(SCSI_CONTROLLER_ACB4070);
   format_with(1, &st, &got, &spt);
   bool r1 = st == 0 && spt == 46;
   format_with(2, &st, &got, &spt);
   bool r2 = st == 0 && spt == 47 && got == 10L * 2 * 47 * 256;
   format_with(47, &st, &got, &spt);
   sense = sense_of(0);
   snprintf(why, sizeof why, "interleave 1: %d, 2: %d, 47: status %d sense %02X (want 2, 1A)", r1, r2, st, sense);
   check("ACB-4070 FORMAT: 46 / 47 per track at 256 bytes, interleave error 1A", r1 && r2 && st == 2 && sense == 0x1A, why);
   scsiSetController(SCSI_CONTROLLER_ACB4000);

   /* FORMAT byte 1: 00 02 1C 1E only */
   const uint8_t f1[6] = { 0x04, 0x08, 0x6C, 0, 2, 0 };
   st = run_cmd(1, f1, 6, NULL, 0);
   sense = sense_of(0);
   const uint8_t f1e[6] = { 0x04, 0x1E, 0x6C, 0, 2, 0 };
   const uint8_t emptylist[4] = { 0, 0, 0, 0 };
   int st2 = run_cmd(1, f1e, 6, emptylist, 4);
   snprintf(why, sizeof why, "byte 1 08: status %d sense %02X (want 2, 24); 1E with an empty list: %d", st, sense, st2);
   check("ACB FORMAT: byte 1 other than 00 02 1C 1E is error 24; 1E with an empty defect list is GOOD",
         st == 2 && sense == 0x24 && st2 == 0, why);

   scsiSetController(SCSI_CONTROLLER_MIXED);
   stop_all();
   filesystemSetLunDirectory(1, 0);
}

/* S6: an ADFS image marked read-only starts, and writes to it are
   swallowed and reported as done. */
static void test_readonly_image(void)
{
   static char why[200];
   static uint8_t img[4096], back[4096], blk[256];
   memset(img, 0x11, sizeof img);
   f_mkdir("/BeebSCSI5");
   put_cfg("/BeebSCSI5/scsi0.cfg", 10, 2);
   put_file("/BeebSCSI5/scsi0.dat", img, sizeof img);
   FRESULT cr = f_chmod("/BeebSCSI5/scsi0.dat", AM_RDO, AM_RDO);
   stop_all();
   filesystemSetLunDirectory(1, 5);

   bool s = filesystemSetLunStatus(0, true);
   snprintf(why, sizeof why, "chmod %d, start %d", cr, s);
   check("S6 a read-only ADFS image starts", cr == FR_OK && s, why);

   const uint8_t rd[6] = { 0x08, 0, 0, 1, 1, 0 };
   int st = run_cmd(1, rd, 6, NULL, 0);
   check("S6 ... READ6 works", st == 0 && bus_out_len == 256 && bus_out[0] == 0x11,
         "read failed");

   memset(blk, 0xEE, sizeof blk);
   const uint8_t wr[6] = { 0x0A, 0, 0, 1, 1, 0 };
   st = run_cmd(1, wr, 6, blk, sizeof blk);
   read_file("/BeebSCSI5/scsi0.dat", back, sizeof back);
   snprintf(why, sizeof why, "status %d (want 0), image %s", st,
            memcmp(back, img, sizeof img) ? "CHANGED" : "unchanged");
   check("S6 ... WRITE6 reports GOOD and writes nothing", st == 0 && !memcmp(back, img, sizeof img), why);

   const uint8_t fm[6] = { 0x04, 0, 0x6C, 0, 0, 0 };
   st = run_cmd(1, fm, 6, NULL, 0);
   long sz = size_of("/BeebSCSI5/scsi0.dat");
   snprintf(why, sizeof why, "status %d (want 0), image %ld bytes (want %u)", st, sz,
            (unsigned)sizeof img);
   check("S6 ... FORMAT reports GOOD and leaves the image alone",
         st == 0 && sz == (long)sizeof img && filesystemReadLunStatus(0), why);

   stop_all();
   (void)f_chmod("/BeebSCSI5/scsi0.dat", 0, AM_RDO);
   s = filesystemSetLunStatus(0, true);
   st = run_cmd(1, wr, 6, blk, sizeof blk);
   read_file("/BeebSCSI5/scsi0.dat", back, sizeof back);
   check("S6 control: the same image read/write takes the write",
         s && st == 0 && back[256] == 0xEE, "write did not land");
   stop_all();
   filesystemSetLunDirectory(1, 0);
}

/* S11: geometry from a stopped LUN's image size. */
static void test_stopped_geometry(void)
{
   static char why[160];
   static uint8_t img[40u * 33u * 256u];          /* 40 tracks: 4 cyl x 10 heads */
   memset(img, 0, sizeof img);
   f_mkdir("/BeebSCSI6");
   f_mkdir("/BeebSCSI7");
   put_file("/BeebSCSI6/scsi2.dat", img, 4096);   /* the previous directory's image */
   put_file("/BeebSCSI7/scsi2.dat", img, sizeof img);
   stop_all();
   filesystemSetLunDirectory(1, 6);
   bool s = filesystemSetLunStatus(2, true);
   filesystemSetLunStatus(2, false);
   filesystemSetLunDirectory(1, 7);
   filesytemdattoconfigGeometry(2);
   snprintf(why, sizeof why, "start %d, %lu bytes, want %lu", s,
            (unsigned long)filesystemGetLunTotalBytes(2), (unsigned long)sizeof img);
   check("S11 a stopped LUN's geometry comes from its own image",
         s && filesystemGetLunTotalBytes(2) == sizeof img, why);
   filesystemSetLunDirectory(1, 0);
}

/* MODE SENSE: a page the descriptor does not hold is refused at once with
   CHECK CONDITION and BAD ARGUMENT (a real drive's ILLEGAL REQUEST), not
   DRIVE NOT READY - a FileStore waited and retried on "not ready" until the
   command seemed hung; and page 0x3F returns every page the descriptor
   holds, in page order (measured against a real RO652). */
static void test_mode_sense(void)
{
   static char why[200];
   scsiSetController(SCSI_CONTROLLER_SCSI);   /* a drive with its own controller */
   static const char cfg[] =
      "ModeParamHeader=00000008\n"
      "LBADescriptor=0000000000000100\n"
      "ModePage1=0103200405\n"
      "ModePage4=040400013204\n"
      "ModePage37=250428432941\n";
   static const uint8_t p1[]  = { 0x01, 0x03, 0x20, 0x04, 0x05 };
   static const uint8_t all[] = { 0x01, 0x03, 0x20, 0x04, 0x05,
                                  0x04, 0x04, 0x00, 0x01, 0x32, 0x04,
                                  0x25, 0x04, 0x28, 0x43, 0x29, 0x41 };
   stop_all();
   filesystemSetLunDirectory(1, 0);
   put_file("/BeebSCSI0/scsi3.cfg", cfg, sizeof cfg - 1);

   const uint8_t ms1[6] = { 0x1A, 3 << 5, 0x01, 0, 255, 0 };
   int st = run_cmd(1, ms1, 6, NULL, 0);
   snprintf(why, sizeof why, "status %d, %u bytes, length byte %d, page at 12: %02X %02X",
            st, bus_out_len, bus_out_len ? bus_out[0] : -1, bus_out[12], bus_out[13]);
   check("MS1 MODE SENSE of a page the descriptor holds",
         st == 0 && bus_out_len >= 17 && bus_out[0] == 4 + 8 + 5 - 1 && bus_out[3] == 8 &&
         memcmp(&bus_out[12], p1, sizeof p1) == 0, why);

   const uint8_t ms2[6] = { 0x1A, 3 << 5, 0x02, 0, 255, 0 };
   st = run_cmd(1, ms2, 6, NULL, 0);
   int sense = sense_of(3);
   snprintf(why, sizeof why, "status %d, sense 0x%02X (want 2, 0x24)", st, sense);
   check("MS2 a page it does not hold: CHECK CONDITION, BAD ARGUMENT (not NOT READY)",
         st == 2 && sense == 0x24, why);

   const uint8_t ms3f[6] = { 0x1A, 3 << 5, 0x3F, 0, 255, 0 };
   st = run_cmd(1, ms3f, 6, NULL, 0);
   snprintf(why, sizeof why, "status %d, length byte %d (want %d)", st,
            bus_out_len ? bus_out[0] : -1, (int)(4 + 8 + sizeof all - 1));
   check("MS3 page 0x3F returns every page held, in page order",
         st == 0 && bus_out_len >= 12 + sizeof all && bus_out[0] == 4 + 8 + sizeof all - 1 &&
         memcmp(&bus_out[12], all, sizeof all) == 0, why);

   const uint8_t ms3f_short[6] = { 0x1A, 3 << 5, 0x3F, 0, 14, 0 };
   st = run_cmd(1, ms3f_short, 6, NULL, 0);
   snprintf(why, sizeof why, "status %d, %u bytes (want 14)", st, bus_out_len);
   check("MS4 page 0x3F keeps to the allocation length", st == 0 && bus_out_len == 14, why);

   f_unlink("/BeebSCSI0/scsi3.cfg");
   stop_all();
   scsiSetController(SCSI_CONTROLLER_MIXED);
}

/* CCS-era mode pages, as a real Rodime RO652 (firmware 5.46) answers them
   (measured): PageControl=CCS refuses page control 1-3 and gives
   every page's factory values for page 3D and changeable masks for 3E; a
   SCSI drive gives them for page control 2 and 1.  A page with no MaskPage
   line has a full-length zero mask, one with no FDefPage line its current
   values.  A header with descriptor length 0 sends no block descriptor, and
   INQUIRY sends only the data held, without padding. */
static void test_ccs_pages(void)
{
   static char why[200];
   scsiSetController(SCSI_CONTROLLER_SCSI);   /* a drive with its own controller */
   static const char ccs[] =
      "PageControl=CCS\n"
      "Inquiry=000000003141424344\n"
      "ModeParamHeader=00000000\n"
      "LBADescriptor=0000000000000100\n"
      "ModePage1=0103200405\n"
      "MaskPage1=010327FF0F\n"
      "FDefPage1=0103200100\n"
      "ModePage4=040400013204\n"
      "MaskPage4=04040007FF0F\n"
      "FDefPage4=040400013204\n"
      "ModePage35=230100\n"
      "ModePage37=250428432941\n"
      "MaskPage37=2504FFFFFFFF\n"
      "FDefPage37=250400000000\n";
   static const uint8_t masks[]   = { 0x01, 0x03, 0x27, 0xFF, 0x0F,  0x04, 0x04, 0x00, 0x07, 0xFF, 0x0F,
                                      0x23, 0x01, 0x00,
                                      0x25, 0x04, 0xFF, 0xFF, 0xFF, 0xFF };
   static const uint8_t factory[] = { 0x01, 0x03, 0x20, 0x01, 0x00,  0x04, 0x04, 0x00, 0x01, 0x32, 0x04,
                                      0x23, 0x01, 0x00,
                                      0x25, 0x04, 0x00, 0x00, 0x00, 0x00 };
   stop_all();
   filesystemSetLunDirectory(1, 0);
   put_file("/BeebSCSI0/scsi4.cfg", ccs, sizeof ccs - 1);

   const uint8_t pc1[6] = { 0x1A, 4 << 5, 0x41, 0, 255, 0 };     /* page 1, page control 1 */
   int st = run_cmd(1, pc1, 6, NULL, 0);
   int sense = sense_of(4);
   snprintf(why, sizeof why, "status %d, sense 0x%02X (want 2, 0x24)", st, sense);
   check("MS5 CCS drive: page control 1 is refused", st == 2 && sense == 0x24, why);

   const uint8_t p3e[6] = { 0x1A, 4 << 5, 0x3E, 0, 255, 0 };
   st = run_cmd(1, p3e, 6, NULL, 0);
   snprintf(why, sizeof why, "status %d, %u bytes, length byte %d, BD length %d", st, bus_out_len,
            bus_out_len ? bus_out[0] : -1, bus_out_len > 3 ? bus_out[3] : -1);
   check("MS6 CCS drive: page 3E gives every page's mask (a page with no MaskPage: zeros)",
         st == 0 && bus_out_len == 4 + sizeof masks && bus_out[0] == 3 + sizeof masks &&
         bus_out[3] == 0 && memcmp(&bus_out[4], masks, sizeof masks) == 0, why);

   const uint8_t p3d[6] = { 0x1A, 4 << 5, 0x3D, 0, 255, 0 };
   st = run_cmd(1, p3d, 6, NULL, 0);
   snprintf(why, sizeof why, "status %d, %u bytes", st, bus_out_len);
   check("MS7 CCS drive: page 3D gives every page's factory values (none given: current)",
         st == 0 && bus_out_len == 4 + sizeof factory && memcmp(&bus_out[4], factory, sizeof factory) == 0, why);

   const uint8_t inq[6] = { 0x12, 4 << 5, 0, 0, 255, 0 };
   st = run_cmd(1, inq, 6, NULL, 0);
   snprintf(why, sizeof why, "status %d, %u bytes (want 9)", st, bus_out_len);
   check("MS10 INQUIRY sends only the data held, not padded to the allocation length",
         st == 0 && bus_out_len == 9 && bus_out[5] == 0x41, why);

   /* the same pages on a SCSI drive (no PageControl line), with a block descriptor */
   static const char scsi[] =
      "ModeParamHeader=00000008\n"
      "LBADescriptor=0000000000000100\n"
      "ModePage1=0103200405\n"
      "WritPage1=010327FF0F\n"
      "FDefPage1=0103200100\n"
      "ModePage35=230100\n"
      "MaskPage35=0000FF\n";
   stop_all();
   put_file("/BeebSCSI0/scsi4.cfg", scsi, sizeof scsi - 1);
   const uint8_t s1[6] = { 0x1A, 4 << 5, 0x41, 0, 255, 0 };
   const uint8_t s2[6] = { 0x1A, 4 << 5, 0x81, 0, 255, 0 };
   const uint8_t s3[6] = { 0x1A, 4 << 5, 0xC1, 0, 255, 0 };
   const uint8_t s35[6] = { 0x1A, 4 << 5, 0x63, 0, 255, 0 };    /* page 35, page control 1 */
   int a = run_cmd(1, s1, 6, NULL, 0);
   bool m1 = a == 0 && bus_out_len == 17 && bus_out[3] == 8 && memcmp(&bus_out[12], masks, 5) == 0;
   int b = run_cmd(1, s2, 6, NULL, 0);
   bool m2 = b == 0 && bus_out_len == 17 && memcmp(&bus_out[12], factory, 5) == 0;
   int c = run_cmd(1, s3, 6, NULL, 0);
   static const uint8_t cur1[] = { 0x01, 0x03, 0x20, 0x04, 0x05 };
   bool m3 = c == 0 && bus_out_len == 17 && memcmp(&bus_out[12], cur1, 5) == 0;
   int d = run_cmd(1, s35, 6, NULL, 0);
   static const uint8_t zero35[] = { 0x23, 0x01, 0xFF };   /* MaskPage35 holds 00 00 for code and length */
   bool m4 = d == 0 && bus_out_len == 15 && memcmp(&bus_out[12], zero35, 3) == 0;
   snprintf(why, sizeof why, "mask %d, factory %d, saved %d, no-mask page %d", m1, m2, m3, m4);
   check("MS8 SCSI drive: page control 1 mask (code and length the page's own), 2 factory, 3 saved",
         m1 && m2 && m3 && m4, why);
   const uint8_t s3d[6] = { 0x1A, 4 << 5, 0x3D, 0, 255, 0 };
   st = run_cmd(1, s3d, 6, NULL, 0);
   sense = sense_of(4);
   snprintf(why, sizeof why, "status %d, sense 0x%02X (want 2, 0x24)", st, sense);
   check("MS9 SCSI drive: page 3D is an ordinary page it does not hold", st == 2 && sense == 0x24, why);

   f_unlink("/BeebSCSI0/scsi4.cfg");
   stop_all();
   scsiSetController(SCSI_CONTROLLER_MIXED);
}

/* ---- DriveController= ----------------------------------------------------- */

/* The raw REQUEST SENSE reply on `lun` with allocation length `alloc`. */
static int sense_raw(uint8_t lun, uint8_t alloc)
{
   const uint8_t rs[6] = { 0x03, (uint8_t)(lun << 5), 0, 0, alloc, 0 };
   return run_cmd(1, rs, 6, NULL, 0);
}

/* A LUN image of `bytes` bytes in /BeebSCSI8, its .cfg `cfg`. */
static void put_lun8(uint8_t lun, const char *cfg, uint32_t bytes)
{
   static uint8_t zero[4096];
   char path[40];
   FIL f;
   UINT done;
   f_mkdir("/BeebSCSI8");
   snprintf(path, sizeof path, "/BeebSCSI8/scsi%u.cfg", lun);
   put_file(path, cfg, (UINT)strlen(cfg));
   snprintf(path, sizeof path, "/BeebSCSI8/scsi%u.dat", lun);
   if (__real_f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
      printf("FAIL setup: could not write %s\n", path);
      exit(1);
   }
   for (uint32_t n = 0; n < bytes; n += sizeof zero)
      f_write(&f, zero, (bytes - n) < sizeof zero ? (UINT)(bytes - n) : (UINT)sizeof zero, &done);
   __real_f_close(&f);
}

static void test_controller_name(void)
{
   static char why[160];
   bool a = scsiSetControllerName(NULL) && scsiGetController() == SCSI_CONTROLLER_MIXED;
   bool b = scsiSetControllerName("acb-4070") && scsiGetController() == SCSI_CONTROLLER_ACB4070;
   bool c = scsiSetControllerName("Sasi") && scsiGetController() == SCSI_CONTROLLER_SASI;
   bool d = scsiSetControllerName("SCSI") && scsiGetController() == SCSI_CONTROLLER_SCSI;
   bool e = !scsiSetControllerName("ACB4000") && scsiGetController() == SCSI_CONTROLLER_MIXED;
   bool f = scsiSetControllerName("ACB-4000") && scsiGetController() == SCSI_CONTROLLER_ACB4000;
   bool g = scsiSetControllerName("mixed") && scsiGetController() == SCSI_CONTROLLER_MIXED;
   snprintf(why, sizeof why, "absent %d, acb-4070 %d, Sasi %d, SCSI %d, unknown %d, ACB-4000 %d, mixed %d",
            a, b, c, d, e, f, g);
   check("DriveController=: names in any case, absent and unknown are Mixed", a && b && c && d && e && f && g, why);
   scsiSetController(SCSI_CONTROLLER_MIXED);
}

/* ACB-4000 (opt-in): only the Adaptec's commands, its 4-byte sense
   and its "drive not ready" 04; Pi1MHz's own commands still work. */
static void test_controller_gating(void)
{
   static char why[200];
   static const char cfg[] = "ModePage0=01001402008000800001\n"
                             "ModeParamHeader=00000008\nLBADescriptor=0000000000000100\n";
   stop_all();
   scsiSetController(SCSI_CONTROLLER_ACB4000);
   put_lun8(3, cfg, 20u * 2u * 33u * 256u);
   filesystemSetLunDirectory(1, 8);

   const uint8_t inq[6] = { 0x12, 3 << 5, 0, 0, 36, 0 };
   int st = run_cmd(1, inq, 6, NULL, 0);
   unsigned sent = bus_out_len;
   int rs = sense_raw(3, 18);
   snprintf(why, sizeof why, "status %d, %u bytes sent, sense status %d, %u bytes, code %02X (want 2, 0, 0, 4, 20)",
            st, sent, rs, bus_out_len, bus_out[0]);
   check("ACB-4000: INQUIRY is refused, 4-byte sense 20 whatever the allocation length",
         st == 2 && sent == 0 && rs == 0 && bus_out_len == 4 && bus_out[0] == 0x20, why);

   const uint8_t others[][6] = {
      { 0x07, 3 << 5, 0, 0, 0, 0 }, { 0xE2, 3 << 5, 0, 1, 0, 0 },
      { 0x37, 3 << 5, 0, 0, 0, 0 },
   };
   bool refused = true;
   for (size_t i = 0; i < sizeof others / sizeof others[0]; i++) {
      unsigned len = (others[i][0] >> 5) == 1 ? 10u : 6u;
      uint8_t cdb[10] = {0};
      memcpy(cdb, others[i], 6);
      st = run_cmd(1, cdb, len, NULL, 0);
      refused = refused && st == 2 && sense_of(3) == 0x20;
   }
   check("ACB-4000: REASSIGN BLOCKS, CERTIFY and READ DEFECT DATA are refused (20)", refused,
         "one was answered");

   const uint8_t tur7[6] = { 0x00, 7 << 5, 0, 0, 0, 0 };     /* LUN 7: no image */
   st = run_cmd(1, tur7, 6, NULL, 0);
   int sense = sense_of(7);
   snprintf(why, sizeof why, "status %d, sense %02X (want 2, 04)", st, sense);
   check("ACB-4000: a drive that is not there is not ready, 04 (02 is no seek complete)",
         st == 2 && sense == 0x04, why);

   /* Pi1MHz's own commands: BSSENSE and BSSELECT on LUN 3 */
   const uint8_t bss[6] = { 0xD0, 3 << 5, 0, 0, 8, 0 };
   st = run_cmd(1, bss, 6, NULL, 0);
   bool sensed = st == 0 && bus_out_len == 8 && bus_out[1] == 8;
   const uint8_t bsel[6] = { 0xD1, 3 << 5, 0, 0, 8, 0 };
   const uint8_t bseld[8] = { 9, 0, 0, 0, 0, 0, 0, 0 };
   stop_all();
   int st2 = run_cmd(1, bsel, 6, bseld, sizeof bseld);
   snprintf(why, sizeof why, "BSSENSE %d (dir %d), BSSELECT %d, directory %u (want 9)",
            st, bus_out_len > 1 ? bus_out[1] : -1, st2, filesystemGetLunDirectory());
   check("ACB-4000: BSSENSE and BSSELECT (Pi1MHz's own) still work on LUN 3",
         sensed && st2 == 0 && filesystemGetLunDirectory() == 9, why);
   filesystemSetLunDirectory(1, 8);

   /* VERIFY from the end of the disc: 21 with the address valid (A1), the
      address in bytes 1-3 - SuperForm's verify loop ends on it */
   uint32_t end = 20u * 2u * 33u;
   const uint8_t vfy[10] = { 0x2F, 3 << 5, 0, (uint8_t)(end >> 16), (uint8_t)(end >> 8), (uint8_t)end, 0, 0, 0, 0 };
   filesystemSetLunStatus(3, true);
   st = run_cmd(1, vfy, 10, NULL, 0);
   rs = sense_raw(3, 4);
   snprintf(why, sizeof why, "status %d, sense %02X %02X %02X %02X (want 2, A1 00 05 28)",
            st, bus_out[0], bus_out[1], bus_out[2], bus_out[3]);
   check("ACB-4000: VERIFY length 0 at the end of the disc is A1 with the address",
         st == 2 && rs == 0 && bus_out[0] == 0xA1 && bus_out[1] == 0x00 &&
         bus_out[2] == (uint8_t)(end >> 8) && bus_out[3] == (uint8_t)end, why);
   const uint8_t vfy0[10] = { 0x2F, 3 << 5, 0, 0, 0, 0, 0, 0, 0, 0 };
   st = run_cmd(1, vfy0, 10, NULL, 0);
   check("ACB-4000: ... and VERIFY inside the disc is GOOD", st == 0, "not GOOD");
   stop_all();

   /* SCSI: TRANSLATE is the Adaptec's; STOP UNIT stays (a Pi1MHz extension) */
   scsiSetController(SCSI_CONTROLLER_SCSI);
   const uint8_t tr[6] = { 0x0F, 3 << 5, 0, 0, 0, 0 };
   st = run_cmd(1, tr, 6, NULL, 0);
   sense = sense_of(3);
   const uint8_t stop[6] = { 0x1B, 3 << 5, 0, 0, 0, 0 };
   st2 = run_cmd(1, stop, 6, NULL, 0);
   snprintf(why, sizeof why, "TRANSLATE %d sense %02X (want 2, 20), STOP UNIT %d (want 0)", st, sense, st2);
   check("SCSI: TRANSLATE refused, STOP UNIT kept", st == 2 && sense == 0x20 && st2 == 0, why);
   scsiSetController(SCSI_CONTROLLER_MIXED);
   stop_all();
}

/* SCSI: extended sense, as a Rodime RO652 sent it after a MODE SENSE of a
   page it does not hold (measured): 70 00 05 00 ... 0A ... 24. */
static void test_extended_sense(void)
{
   static char why[200];
   static const char cfg[] = "PageControl=CCS\nModeParamHeader=00000000\n"
                             "LBADescriptor=0000000000000100\nModePage1=0103200405\n";
   static const uint8_t want[18] = { 0x70, 0, 0x05, 0, 0, 0, 0, 0x0A, 0, 0, 0, 0, 0x24, 0, 0, 0, 0, 0 };
   stop_all();
   scsiSetController(SCSI_CONTROLLER_SCSI);
   put_lun8(3, cfg, 20u * 2u * 33u * 256u);
   filesystemSetLunDirectory(1, 8);

   const uint8_t ms[6] = { 0x1A, 3 << 5, 0x02, 0, 255, 0 };
   int st = run_cmd(1, ms, 6, NULL, 0);
   int rs = sense_raw(3, 18);
   snprintf(why, sizeof why, "status %d, sense %d, %u bytes: %02X %02X %02X .. %02X %02X", st, rs, bus_out_len,
            bus_out[0], bus_out[1], bus_out[2], bus_out[7], bus_out[12]);
   check("SCSI: a refused MODE SENSE page gives extended sense 70 00 05 .. 0A .. 24 (RO652, measured)",
         st == 2 && rs == 0 && bus_out_len == 18 && memcmp(bus_out, want, 18) == 0, why);

   run_cmd(1, ms, 6, NULL, 0);
   rs = sense_raw(3, 0);
   unsigned n0 = bus_out_len;
   bool head0 = bus_out[0] == 0x70 && bus_out[2] == 0x05;
   run_cmd(1, ms, 6, NULL, 0);
   rs = sense_raw(3, 8);
   snprintf(why, sizeof why, "allocation 0: %u bytes (want 4), allocation 8: %u bytes", n0, bus_out_len);
   check("SCSI: extended sense keeps to the allocation length, 0 meaning 4 bytes",
         n0 == 4 && head0 && bus_out_len == 8, why);

   /* VERIFY past the end: key 5, ASC 21, the address valid and in bytes 3-6 */
   uint32_t end = 20u * 2u * 33u;
   const uint8_t vfy[10] = { 0x2F, 3 << 5, 0, (uint8_t)(end >> 16), (uint8_t)(end >> 8), (uint8_t)end, 0, 0, 0, 0 };
   filesystemSetLunStatus(3, true);
   st = run_cmd(1, vfy, 10, NULL, 0);
   rs = sense_raw(3, 18);
   snprintf(why, sizeof why, "status %d, %02X key %02X info %02X%02X%02X%02X ASC %02X", st, bus_out[0], bus_out[2],
            bus_out[3], bus_out[4], bus_out[5], bus_out[6], bus_out[12]);
   check("SCSI: VERIFY past the end is F0, key 5, the LBA in bytes 3-6, ASC 21",
         st == 2 && bus_out[0] == 0xF0 && bus_out[2] == 5 && bus_out[3] == 0 && bus_out[4] == 0 &&
         bus_out[5] == (uint8_t)(end >> 8) && bus_out[6] == (uint8_t)end && bus_out[12] == 0x21, why);
   scsiSetController(SCSI_CONTROLLER_MIXED);
   stop_all();
}

/* The data path in the LUN's block size: READ6 and WRITE6 move whole blocks
   of it, at the right offsets in the image, and a 256-byte LUN is as before. */
static void test_block_sizes(void)
{
   static char why[200];
   static uint8_t data[10 * 2047], back[16 * 2047];
   /* 2047 (the RO652's largest) over 10 blocks: more than the 16 KB
      sector buffer holds */
   const uint32_t sizes[] = { 128, 512, 1024, 2047, 256 };
   stop_all();
   filesystemSetLunDirectory(1, 8);
   for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
      uint32_t bs = sizes[i];
      uint8_t n = (bs == 2047) ? 10 : 3;
      char cfg[200];
      snprintf(cfg, sizeof cfg, "ModeParamHeader=00000008\nLBADescriptor=0000000000%06X\n"
               "ModePage0=01000A02008000800001\n", (unsigned)bs);
      put_lun8(2, cfg, 16u * bs);
      for (uint32_t b = 0; b < n * bs; b++)
         data[b] = (uint8_t)(b / bs * 0x40 + b % 251);
      /* WRITE6 3 blocks at LBA 2, then READ6 them back */
      const uint8_t wr[6] = { 0x0A, 2 << 5, 0, 2, n, 0 };
      int st = run_cmd(1, wr, 6, data, n * bs);
      read_file("/BeebSCSI8/scsi2.dat", back, 16u * bs);
      bool placed = memcmp(back + 2u * bs, data, n * bs) == 0 && back[2u * bs - 1u] == 0 &&
                    back[(2u + n) * bs] == 0;
      const uint8_t rd[6] = { 0x08, 2 << 5, 0, 2, n, 0 };
      int st2 = run_cmd(1, rd, 6, NULL, 0);
      bool readback = bus_out_len == n * bs && memcmp(bus_out, data, n * bs) == 0;
      /* READ CAPACITY gives the block size */
      const uint8_t rc[10] = { 0x25, 2 << 5, 0, 0, 0, 0, 0, 0, 0, 0 };
      int st3 = run_cmd(1, rc, 10, NULL, 0);
      uint32_t rcbs = ((uint32_t)bus_out[4] << 24) | ((uint32_t)bus_out[5] << 16) | ((uint32_t)bus_out[6] << 8) | bus_out[7];
      snprintf(why, sizeof why, "%lu bytes: write %d placed %d, read %d %u bytes back %d, capacity %d block %lu",
               (unsigned long)bs, st, placed, st2, bus_out_len, readback, st3, (unsigned long)rcbs);
      char t[96];
      snprintf(t, sizeof t, "data path: %lu-byte blocks round-trip at offset LBA x %lu",
               (unsigned long)bs, (unsigned long)bs);
      check(t, st == 0 && placed && st2 == 0 && readback && st3 == 0 && rcbs == bs, why);
      stop_all();
   }

   /* A short write DMA (the host stopped) at 512 bytes: BUS FREE, not run */
   put_lun8(2, "ModeParamHeader=00000008\nLBADescriptor=0000000000000200\n"
            "ModePage0=01000A02008000800001\n", 8u * 512u);
   memset(data, 0xAB, 512);
   const uint8_t wr1[6] = { 0x0A, 2 << 5, 0, 1, 1, 0 };
   dma_short = 256;
   int st = run_cmd(1, wr1, 6, data, 512);
   read_file("/BeebSCSI8/scsi2.dat", back, 8u * 512u);
   snprintf(why, sizeof why, "status %d (want -1), block 1 %s", st, back[512] == 0xAB ? "WRITTEN" : "untouched");
   check("data path: a 512-byte block cut at 256 bytes is a short DMA", st == -1 && back[512] != 0xAB, why);
   stop_all();
   filesystemSetLunDirectory(1, 0);
}

/* SCSI: a drive with its own controller, as the Rodime RO652's ROM 5.02
   takes MODE SELECT and FORMAT.  The drive's .cfg (no block descriptor,
   CCS pages, page 3 mask 07FF on bytes per sector, page 4 mask 0007FF / 0F)
   and TrackFormat=20384,70,1:
     physical sectors per track = track bytes / (bytes per sector +
     overhead), rounded down; sectors per track = physical - spare sectors.
     Track bytes (20384): usable bytes on one track - the drive has 20,484
     raw bytes per revolution; the firmware keeps the gap at the index pulse
     and a margin for speed variation.  Overhead (70): bytes each sector
     takes on the disc besides its data - sync field, address mark, ID field
     (cylinder, head, sector, check bytes), gap, data sync and address mark,
     the 4-byte ECC, closing gap.  Spare sectors (1): kept in reserve on
     every track so a bad sector can be remapped on the same track;
     formatted but never given a block number.
     128/256/512/1024 -> 101/61/34/17; 256: 20384/326 = 62 physical, 61 used. */
static const char ro652_cfg[] =
   "# TrackFormat=track bytes,overhead,spare sectors (see the comment above)\n"
   "TrackFormat=20384,70,1\n"
   "PageControl=CCS\n"
   "ModeParamHeader=00000000\n"
   "LBADescriptor=0000000000000100\n"
   "FDefPage1=0103200100\n"
   "ModePage1=0103200405\n"
   "FDefPage3=0315000100000000000000220100000100000000400000\n"
   "MaskPage3=03150000000000000000000007FF0000007F007F000000\n"
   "ModePage3=0315000100000000000000220100000800000000400000\n"
   "FDefPage4=040400001402\n"
   "MaskPage4=04040007FF0F\n"
   "ModePage4=040400001402\n";

static int scsi_format(uint16_t interleave)
{
   const uint8_t fm[6] = { 0x04, 0, 0, (uint8_t)(interleave >> 8), (uint8_t)interleave, 0 };
   int st = run_cmd(1, fm, 6, NULL, 0);
   stop_all();
   return st;
}

/* Page 3 with only bytes per sector (the rest zero: the drive's own) */
static int scsi_select_block_size(uint32_t bs)
{
   const uint8_t ms[6] = { 0x15, 0, 0, 0, 4 + 23, 0 };
   uint8_t d[27] = { 0, 0, 0, 0,  3, 0x15 };
   d[4 + 12] = (uint8_t)(bs >> 8);
   d[4 + 13] = (uint8_t)bs;
   return run_cmd(1, ms, 6, d, sizeof d);
}

/* MODE SENSE page 3 (current) and READ CAPACITY: block size, sectors per
   track, interleave, capacity */
static void scsi_sense3(uint32_t *bs, uint32_t *spt, uint32_t *il, uint32_t *rcbs, uint32_t *blocks)
{
   const uint8_t ms[6] = { 0x1A, 0, 0x03, 0, 0xFF, 0 };
   run_cmd(1, ms, 6, NULL, 0);
   const uint8_t *p = &bus_out[4];          /* no block descriptor */
   *spt = ((uint32_t)p[10] << 8) | p[11];
   *bs = ((uint32_t)p[12] << 8) | p[13];
   *il = ((uint32_t)p[14] << 8) | p[15];
   filesystemSetLunStatus(0, true);
   const uint8_t rc[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
   run_cmd(1, rc, 10, NULL, 0);
   *blocks = (((uint32_t)bus_out[0] << 24) | ((uint32_t)bus_out[1] << 16) | ((uint32_t)bus_out[2] << 8) | bus_out[3]) + 1u;
   *rcbs = ((uint32_t)bus_out[4] << 24) | ((uint32_t)bus_out[5] << 16) | ((uint32_t)bus_out[6] << 8) | bus_out[7];
   stop_all();
}

static void test_scsi_drive(void)
{
   static char why[240];
   int st, sense;
   stop_all();
   scsiSetController(SCSI_CONTROLLER_SCSI);
   f_mkdir("/BeebSCSI9");
   put_file("/BeebSCSI9/scsi0.cfg", ro652_cfg, sizeof ro652_cfg - 1);
   put_file("/BeebSCSI9/scsi0.dat", "image", 5);
   filesystemSetLunDirectory(1, 9);

   /* a block descriptor (header byte 3 = 8) to a drive with none: ASC 22 */
   const uint8_t ms12[6] = { 0x15, 0, 0, 0, 12, 0 };
   const uint8_t bd[12] = { 0, 0, 0, 8,  0, 0, 0, 0, 0, 0, 2, 0 };
   st = run_cmd(1, ms12, 6, bd, sizeof bd);
   sense = sense_of(0);
   snprintf(why, sizeof why, "status %d sense %02X (want 2, 22)", st, sense);
   check("SCSI MODE SELECT: a block descriptor to a drive without one is refused (ASC 22)",
         st == 2 && sense == 0x22, why);

   /* page code bit 6, a page sent longer than the drive's, a page it does not hold: ASC 26 */
   const uint8_t ms9[6] = { 0x15, 0, 0, 0, 9, 0 };
   const uint8_t b6[9] = { 0, 0, 0, 0,  0x41, 3, 0x20, 4, 5 };
   const uint8_t lng[9] = { 0, 0, 0, 0,  0x01, 5, 0x20, 4, 5 };
   const uint8_t p2[9] = { 0, 0, 0, 0,  0x02, 3, 0, 0, 0 };
   int s1 = run_cmd(1, ms9, 6, b6, 9), e1 = sense_of(0);
   int s2 = run_cmd(1, ms9, 6, lng, 9), e2 = sense_of(0);
   int s3 = run_cmd(1, ms9, 6, p2, 9), e3 = sense_of(0);
   snprintf(why, sizeof why, "bit 6 %d/%02X, too long %d/%02X, not held %d/%02X (want 2/26 each)", s1, e1, s2, e2, s3, e3);
   check("SCSI MODE SELECT: page bit 6, a page longer than stored, a page not held: ASC 26",
         s1 == 2 && e1 == 0x26 && s2 == 2 && e2 == 0x26 && s3 == 2 && e3 == 0x26, why);

   /* page bit 7 with length 0: back to the factory values */
   const uint8_t chg[9] = { 0, 0, 0, 0,  0x01, 3, 0x27, 9, 9 };
   static const uint8_t p1chg[] = { 0x01, 0x03, 0x27, 0x09, 0x09 };
   static const uint8_t p1fdef[] = { 0x01, 0x03, 0x20, 0x01, 0x00 };
   st = run_cmd(1, ms9, 6, chg, 9);
   bool changed = st == 0 && page_is(0, 1, p1chg, sizeof p1chg);
   const uint8_t ms6[6] = { 0x15, 0, 0, 0, 6, 0 };
   const uint8_t rst[6] = { 0, 0, 0, 0,  0x81, 0 };
   st = run_cmd(1, ms6, 6, rst, 6);
   snprintf(why, sizeof why, "changed %d, reset status %d, page 1 %s", changed, st,
            page_is(0, 1, p1fdef, sizeof p1fdef) ? "factory" : "NOT factory");
   check("SCSI MODE SELECT: page code bit 7 with length 0 resets the page to FDefPage",
         changed && st == 0 && page_is(0, 1, p1fdef, sizeof p1fdef), why);

   /* bytes per sector 512: pending until FORMAT */
   uint32_t bs, spt, il, rcbs, blocks;
   st = scsi_select_block_size(512);
   scsi_sense3(&bs, &spt, &il, &rcbs, &blocks);
   snprintf(why, sizeof why, "status %d; before FORMAT page 3 %lu bytes, READ CAPACITY %lu bytes (want 256, 256)",
            st, (unsigned long)bs, (unsigned long)rcbs);
   check("SCSI: a page 3 block size change waits for FORMAT (MODE SENSE, READ CAPACITY unchanged)",
         st == 0 && bs == 256 && rcbs == 256, why);
   st = scsi_format(0);
   scsi_sense3(&bs, &spt, &il, &rcbs, &blocks);
   snprintf(why, sizeof why, "FORMAT %d; page 3 %lu bytes %lu per track interleave %lu, READ CAPACITY %lu x %lu bytes",
            st, (unsigned long)bs, (unsigned long)spt, (unsigned long)il, (unsigned long)blocks, (unsigned long)rcbs);
   check("SCSI: ... FORMAT makes it current: 512 bytes, 34 per track (TrackFormat 20384,70,1), interleave 0 kept as 2",
         st == 0 && bs == 512 && spt == 34 && il == 2 && rcbs == 512 && blocks == 20u * 2u * 34u &&
         size_of("/BeebSCSI9/scsi0.dat") == 20L * 2 * 34 * 512, why);

   const struct { uint32_t bs, spt; } tf[] = { { 128, 101 }, { 1024, 17 }, { 256, 61 } };
   for (size_t i = 0; i < sizeof tf / sizeof tf[0]; i++) {
      st = scsi_select_block_size(tf[i].bs);
      int fs = scsi_format(5);
      scsi_sense3(&bs, &spt, &il, &rcbs, &blocks);
      snprintf(why, sizeof why, "select %d FORMAT %d: %lu bytes, %lu per track (want %lu), interleave %lu, capacity %lu",
               st, fs, (unsigned long)bs, (unsigned long)spt, (unsigned long)tf[i].spt, (unsigned long)il,
               (unsigned long)blocks);
      char t[96];
      snprintf(t, sizeof t, "SCSI: TrackFormat 20384,70,1 at %lu bytes gives %lu per track",
               (unsigned long)tf[i].bs, (unsigned long)tf[i].spt);
      check(t, st == 0 && fs == 0 && bs == tf[i].bs && spt == tf[i].spt && il == 5 && rcbs == tf[i].bs &&
            blocks == 20u * 2u * tf[i].spt, why);
   }

   /* 2048 bytes per sector: outside the mask 07FF, refused (ASC 22) */
   st = scsi_select_block_size(2048);
   sense = sense_of(0);
   snprintf(why, sizeof why, "status %d sense %02X (want 2, 22)", st, sense);
   check("SCSI MODE SELECT: 2048 bytes per sector is refused", st == 2 && sense == 0x22, why);

   /* interleave: must be below physical - 1 (62 - 1 = 61 at 256 bytes) */
   st = scsi_format(61);
   sense = sense_of(0);
   int ok60 = scsi_format(60);
   const uint8_t fm5[6] = { 0x04, 0, 0, 0, 2, 1 };
   int bad5 = run_cmd(1, fm5, 6, NULL, 0);
   int sense5 = sense_of(0);
   snprintf(why, sizeof why, "interleave 61: %d %02X (want 2, 24); 60: %d; CDB byte 5 set: %d %02X",
            st, sense, ok60, bad5, sense5);
   check("SCSI FORMAT: interleave 61 at 256 bytes refused (ASC 24), 60 taken; CDB byte 5 must be 0",
         st == 2 && sense == 0x24 && ok60 == 0 && bad5 == 2 && sense5 == 0x24, why);

   /* page 0 is the Adaptec's: a SCSI drive refuses it */
   const uint8_t ms0[6] = { 0x1A, 0, 0, 0, 0xFF, 0 };
   st = run_cmd(1, ms0, 6, NULL, 0);
   sense = sense_of(0);
   check("SCSI MODE SENSE: page 0 refused (ASC 24)", st == 2 && sense == 0x24, "answered");

   stop_all();
   scsiSetController(SCSI_CONTROLLER_MIXED);
   filesystemSetLunDirectory(1, 0);
}

/* A SCSI drive's pending pages (MODE SELECT before FORMAT) last while the
   drives are the same ones: a STOP/START UNIT keeps them; a jukebox to
   another ADFS directory and back, or a remount (a BREAK), drops them, so a
   FORMAT after it lays down the block size the .cfg has. */
static void test_pending_pages_lifetime(void)
{
   static char why[240];
   uint32_t bs, spt, il, rcbs, blocks;
   stop_all();
   scsiSetController(SCSI_CONTROLLER_SCSI);
   put_file("/BeebSCSI9/scsi0.cfg", ro652_cfg, sizeof ro652_cfg - 1);
   put_file("/BeebSCSI9/scsi0.dat", "image", 5);
   filesystemSetLunDirectory(1, 9);
   scsiClearPendingPages(0);

   const uint8_t stop[6] = { 0x1B, 0, 0, 0, 0, 0 }, start[6] = { 0x1B, 0, 0, 0, 1, 0 };
   int sel = scsi_select_block_size(512);
   int s1 = run_cmd(1, stop, 6, NULL, 0), s2 = run_cmd(1, start, 6, NULL, 0);
   int fs = scsi_format(0);
   scsi_sense3(&bs, &spt, &il, &rcbs, &blocks);
   snprintf(why, sizeof why, "select %d, STOP %d, START %d, FORMAT %d: %lu bytes (want 512)", sel, s1, s2, fs,
            (unsigned long)rcbs);
   check("SCSI: pending pages survive STOP/START UNIT; FORMAT lays them down",
         sel == 0 && s1 == 0 && s2 == 0 && fs == 0 && rcbs == 512 && bs == 512, why);

   sel = scsi_select_block_size(1024);
   filesystemSetLunDirectory(1, 8);
   filesystemSetLunDirectory(1, 9);
   fs = scsi_format(0);
   scsi_sense3(&bs, &spt, &il, &rcbs, &blocks);
   snprintf(why, sizeof why, "select %d, FORMAT %d: %lu bytes (want 512)", sel, fs, (unsigned long)rcbs);
   check("SCSI: a jukebox to another ADFS directory drops the pending pages",
         sel == 0 && fs == 0 && rcbs == 512 && bs == 512, why);

   sel = scsi_select_block_size(128);
   filesystemReset();
   fs = scsi_format(0);
   scsi_sense3(&bs, &spt, &il, &rcbs, &blocks);
   snprintf(why, sizeof why, "select %d, FORMAT %d: %lu bytes (want 512)", sel, fs, (unsigned long)rcbs);
   check("SCSI: a remount drops the pending pages", sel == 0 && fs == 0 && rcbs == 512 && bs == 512, why);

   stop_all();
   scsiSetController(SCSI_CONTROLLER_MIXED);
   filesystemSetLunDirectory(1, 0);
}

/* An existing user's .cfg from before the mode pages had their true
   lengths: the shipped default of then, pages 1, 3 and 4 two bytes longer
   than their length byte says and WritPage3 as page 3's mask.  The Acorn
   FileStore formatter's MODE SELECT of page 3 (bytes per sector set, the
   rest zero, sent two bytes short), then FORMAT with interleave 8: under
   Mixed exactly as before; under SCSI the page waits for FORMAT, which
   lays the new block size down. */
static const char legacy_cfg[] =
   "ModeParamHeader=00000000\n"
   "LBADescriptor=0000000000000100\n"
   "ModePage0=01001404008000800001\n"
   "ModePage1=01052004050000\n"
   "ModePage3=0315013201320006000600210100000800000000800000\n"
   "WritPage3=000000000000000000000000FFFF000000000000000000\n"
   "ModePage4=0406000014040000\n";

/* MODE SELECT page 3 with bytes per sector `bs`, as FServFmt sends it:
   its own length byte, then only len bytes of the page (len+4 with the
   header, where the whole page needs len+6) */
static int select_page3_short(uint8_t len, uint32_t bs)
{
   uint8_t d[4 + 256] = { 0, 0, 0, 0,  3, len };
   uint8_t n = (uint8_t)(4u + len);
   d[4 + 12] = (uint8_t)(bs >> 8);
   d[4 + 13] = (uint8_t)bs;
   const uint8_t ms[6] = { 0x15, 0, 0, 0, n, 0 };
   return run_cmd(1, ms, 6, d, n);
}

static uint32_t capacity_block_size(void)
{
   const uint8_t rc[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
   if (run_cmd(1, rc, 10, NULL, 0) != 0 || bus_out_len < 8)
      return 0;
   return ((uint32_t)bus_out[4] << 24) | ((uint32_t)bus_out[5] << 16) | ((uint32_t)bus_out[6] << 8) | bus_out[7];
}

static void test_upgrade_legacy_cfg(void)
{
   static char why[240];
   const uint8_t fm8[6] = { 0x04, 0, 0, 0, 8, 0 };
   size_t l3 = 0;
   const char *g3;
   stop_all();
   filesystemSetLunDirectory(1, 8);

   /* Mixed: page 3 keeps its 23 bytes, the image its 256-byte blocks */
   scsiSetController(SCSI_CONTROLLER_MIXED);
   put_lun8(0, legacy_cfg, 4096);
   int st = select_page3_short(0x15, 256);
   int fs = run_cmd(1, fm8, 6, NULL, 0);
   long sz = size_of("/BeebSCSI8/scsi0.dat");
   uint32_t rcbs = capacity_block_size();
   g3 = filesystemGetModePageData(0, 3, &l3);
   snprintf(why, sizeof why, "select %d, FORMAT %d, image %ld (want %u), page 3 %u bytes, %02X per track, block %lu",
            st, fs, sz, 20u * 4u * 33u * 256u, (unsigned)l3, g3 && l3 > 11 ? (uint8_t)g3[11] : 0,
            (unsigned long)rcbs);
   check("legacy .cfg (23-byte pages, WritPage3), Mixed: MODE SELECT then FORMAT as before",
         st == 0 && fs == 0 && sz == 20L * 4 * 33 * 256 && g3 != NULL && l3 == 23 && (uint8_t)g3[1] == 0x15 &&
         (uint8_t)g3[11] == 0x21 && rcbs == 256, why);
   stop_all();

   /* SCSI: 512 bytes per sector, pending until FORMAT */
   scsiSetController(SCSI_CONTROLLER_SCSI);
   put_lun8(0, legacy_cfg, 4096);
   scsiClearPendingPages(0);
   st = select_page3_short(0x15, 512);
   filesystemSetLunStatus(0, true);
   uint32_t before = capacity_block_size();
   fs = run_cmd(1, fm8, 6, NULL, 0);
   sz = size_of("/BeebSCSI8/scsi0.dat");
   stop_all();                          /* the next start reads the saved .cfg */
   filesystemSetLunStatus(0, true);
   rcbs = capacity_block_size();
   g3 = filesystemGetModePageData(0, 3, &l3);
   snprintf(why, sizeof why, "select %d, block before FORMAT %lu, FORMAT %d, image %ld (want %u), block %lu, "
            "page 3 %u bytes, bytes per sector %02X%02X, interleave %02X",
            st, (unsigned long)before, fs, sz, 20u * 4u * 33u * 512u, (unsigned long)rcbs, (unsigned)l3,
            g3 && l3 > 13 ? (uint8_t)g3[12] : 0, g3 && l3 > 13 ? (uint8_t)g3[13] : 0,
            g3 && l3 > 15 ? (uint8_t)g3[15] : 0);
   check("legacy .cfg (23-byte pages, WritPage3), SCSI: MODE SELECT waits, FORMAT lays down 512-byte blocks",
         st == 0 && before == 256 && fs == 0 && sz == 20L * 4 * 33 * 512 && rcbs == 512 && g3 != NULL &&
         l3 == 23 && (uint8_t)g3[12] == 0x02 && (uint8_t)g3[13] == 0x00 && (uint8_t)g3[15] == 8, why);
   stop_all();

   /* SCSI: page 3 back to its factory values, a 21-byte FDefPage3 over the
      23-byte page: after FORMAT the page is 21 bytes, no old tail kept */
   char cfg[sizeof legacy_cfg + 64];
   snprintf(cfg, sizeof cfg, "%sFDefPage3=031301320132000600060021010000080000000080\n", legacy_cfg);
   put_lun8(0, cfg, 4096);
   scsiClearPendingPages(0);
   const uint8_t ms6[6] = { 0x15, 0, 0, 0, 6, 0 };
   const uint8_t rst[6] = { 0, 0, 0, 0,  0x83, 0 };
   st = run_cmd(1, ms6, 6, rst, sizeof rst);
   fs = run_cmd(1, fm8, 6, NULL, 0);
   g3 = filesystemGetModePageData(0, 3, &l3);
   size_t now = l3;
   stop_all();
   filesystemSetLunStatus(0, true);
   const uint8_t ms3[6] = { 0x1A, 0, 0x03, 0, 0xFF, 0 };
   int ss = run_cmd(1, ms3, 6, NULL, 0);
   g3 = filesystemGetModePageData(0, 3, &l3);
   snprintf(why, sizeof why, "reset %d, FORMAT %d, page 3 %u bytes, %u after a restart (want 21), "
            "MODE SENSE %d %u bytes (want 25), length byte %02X",
            st, fs, (unsigned)now, (unsigned)l3, ss, bus_out_len, bus_out_len > 5 ? bus_out[5] : 0);
   check("SCSI: a factory reset to a shorter FDefPage3 leaves page 3 at its length (21), no old tail",
         st == 0 && fs == 0 && now == 21 && l3 == 21 && g3 != NULL && (uint8_t)g3[1] == 0x13 &&
         ss == 0 && bus_out_len == 4 + 21 && bus_out[5] == 0x13, why);

   stop_all();
   scsiSetController(SCSI_CONTROLLER_MIXED);
   filesystemSetLunDirectory(1, 0);
}

/* 2048 bytes, MAX_BLOCK_SIZE, on every path of a SCSI drive: the block
   descriptor and page 3 of MODE SELECT, FORMAT, READ CAPACITY and the data
   path; 2049 is refused (ASC 22). */
static void test_block_size_2048(void)
{
   static char why[240];
   static uint8_t data[2 * 2048];
   static const char cfg[] =
      "ModeParamHeader=00000008\n"
      "LBADescriptor=0000000000000100\n"
      "ModePage3=031300020002000000000011010000080000000080\n"
      "MaskPage3=0313000000000000000000000FFF00000000000000\n"
      "ModePage4=040400001402\n";
   const uint8_t ms12[6] = { 0x15, 0, 0, 0, 12, 0 };
   uint8_t bd[12] = { 0, 0, 0, 8,  0, 0, 0, 0, 0, 0x00, 0x08, 0x01 };
   stop_all();
   scsiSetController(SCSI_CONTROLLER_SCSI);
   put_lun8(0, cfg, 4096);
   filesystemSetLunDirectory(1, 8);
   scsiClearPendingPages(0);

   int s1 = run_cmd(1, ms12, 6, bd, sizeof bd), e1 = sense_of(0);
   int s2 = select_page3_short(0x13, 2049), e2 = sense_of(0);
   bd[11] = 0x00;                       /* 2048 */
   int s3 = run_cmd(1, ms12, 6, bd, sizeof bd);
   int s4 = select_page3_short(0x13, 2048);
   const uint8_t fm[6] = { 0x04, 0, 0, 0, 2, 0 };
   int fs = run_cmd(1, fm, 6, NULL, 0);
   long sz = size_of("/BeebSCSI8/scsi0.dat");
   filesystemSetLunStatus(0, true);
   uint32_t rcbs = capacity_block_size();
   snprintf(why, sizeof why, "2049: descriptor %d/%02X, page 3 %d/%02X (want 2/22); 2048: descriptor %d, page 3 %d, "
            "FORMAT %d, image %ld (want %u), READ CAPACITY %lu", s1, e1, s2, e2, s3, s4, fs, sz,
            20u * 2u * 17u * 2048u, (unsigned long)rcbs);
   check("2048-byte blocks: MODE SELECT and FORMAT take 2048 (MAX_BLOCK_SIZE), refuse 2049",
         s1 == 2 && e1 == 0x22 && s2 == 2 && e2 == 0x22 && s3 == 0 && s4 == 0 && fs == 0 &&
         sz == 20L * 2 * 17 * 2048 && rcbs == 2048, why);

   for (uint32_t b = 0; b < sizeof data; b++)
      data[b] = (uint8_t)(b / 2048u * 0x40 + b % 251);
   const uint8_t wr[6] = { 0x0A, 0, 0, 3, 2, 0 };
   int sw = run_cmd(1, wr, 6, data, sizeof data);
   const uint8_t rd[6] = { 0x08, 0, 0, 3, 2, 0 };
   int sr = run_cmd(1, rd, 6, NULL, 0);
   bool back = bus_out_len == sizeof data && memcmp(bus_out, data, sizeof data) == 0;
   static uint8_t img[6 * 2048];
   read_file("/BeebSCSI8/scsi0.dat", img, sizeof img);
   bool placed = memcmp(img + 3u * 2048u, data, sizeof data) == 0;
   snprintf(why, sizeof why, "WRITE6 %d, placed %d, READ6 %d %u bytes, same %d", sw, placed, sr, bus_out_len, back);
   check("2048-byte blocks: WRITE6 and READ6 of two blocks round-trip at LBA x 2048",
         sw == 0 && placed && sr == 0 && back, why);

   stop_all();
   scsiSetController(SCSI_CONTROLLER_MIXED);
   filesystemSetLunDirectory(1, 0);
}

/* S10: the VFS title cache does not survive a card change. */
static void test_title_cache(void)
{
   static char why[160];
   char t[64];
   static const char a[] = "Title=Alpha\n", b[] = "Title=Bravo\n";
   f_mkdir("/BeebVFS7");
   put_file("/BeebVFS7/scsi0.cfg", a, sizeof a - 1);
   bool r1 = filesystemReadVFSCfgTextDir(7, TITLE, t, sizeof t) && !strcmp(t, "Alpha");
   put_file("/BeebVFS7/scsi0.cfg", b, sizeof b - 1);   /* another card */
   filesystemReset();                                  /* the card change */
   bool r2 = filesystemReadVFSCfgTextDir(7, TITLE, t, sizeof t);
   snprintf(why, sizeof why, "first read %d, after the change '%s' (want Bravo)", r1, r2 ? t : "");
   check("S10 the VFS title cache is dropped on a card change", r1 && r2 && !strcmp(t, "Bravo"), why);
}

/* ---- storage=usb: where the Beeb's storage lives ------------------------ */

static void put_lun0(const char *root, const char *tag)
{
   static uint8_t img[10u * 2u * DEFAULT_SECTORS_PER_TRACK * DEFAULT_BLOCK_SIZE];
   char path[48];
   memset(img, 0, sizeof img);
   memcpy(img, tag, strlen(tag));
   snprintf(path, sizeof path, "%s/BeebSCSI0", root);
   f_mkdir(path);
   snprintf(path, sizeof path, "%s/BeebSCSI0/scsi0.cfg", root);
   put_cfg(path, 10, 2);
   snprintf(path, sizeof path, "%s/BeebSCSI0/scsi0.dat", root);
   put_file(path, img, (UINT)sizeof img);
}

/* LUN 0 started: which image did it open? */
static const char *lun0_volume(void)
{
   stop_all();
   filesystemSetLunDirectory(1, 0);
   if (!filesystemSetLunStatus(0, true))
      return "none";
   const char *v = leaked_on("1:/BeebSCSI0/scsi0.dat") ? "usb"
                 : leaked_on("/BeebSCSI0/scsi0.dat") ? "sd" : "?";
   return v;
}

static void test_storage(void)
{
   static char why[200];
   put_lun0("", "sd");
   put_lun0("1:", "usb");

   /* storage unset: the card, nothing waited for - even with a drive. */
   cfg_storage = NULL;
   usb_mounted = true;
   filesystemReset();
   const char *v = lun0_volume();
   bool busy = filesystemHostPathBusy("/BeebSCSI0/scsi0.dat");
   snprintf(why, sizeof why, "LUN 0 on %s (want sd), waits %d, card image busy %d", v, usb_waits, busy);
   check("USB storage unset: the card", !strcmp(v, "sd") && usb_waits == 0 && busy &&
         !filesystemStorageOnUsb(), why);

   /* storage=usb, the drive there at the BREAK: decided at the first use,
      without waiting; the card's image is free for WebDAV/MTP. */
   cfg_storage = "usb";
   stop_all();
   filesystemReset();
   bool undecided = !filesystemStorageOnUsb();
   int reads = usb_reads;
   v = lun0_volume();
   busy = filesystemHostPathBusy("/BeebSCSI0/scsi0.dat");
   int8_t lun = filesystemLunFromHostPath("/BeebSCSI0/scsi0.dat");
   snprintf(why, sizeof why, "undecided after reset %d, LUN 0 on %s (want usb), drive reads %d, "
            "waits %d, card image busy %d, LUN from host path %d",
            undecided, v, usb_reads - reads, usb_waits, busy, lun);
   check("USB storage: the drive's ADFS image, the card's left to the host",
         undecided && !strcmp(v, "usb") && usb_reads > reads && usb_waits == 0 && !busy &&
         lun == -1 && filesystemStorageOnUsb() && !strcmp(filesystemStorageRoot(), "1:"), why);

   /* Names: the Beeb's on the drive; the Pi's own folder, and a name with
      its own volume, left alone (the helpers load "Pi1MHz/<n>.rom"). */
   {
      char a[64], b[64], c[64], d[64];
      bool fits = filesystemStoragePath("/Games/x.ssd", a, sizeof a) &&
                  filesystemStoragePath("Pi1MHz/SWMMFS.rom", b, sizeof b) &&
                  filesystemStoragePath("/pi1mhz/Pi1MHz.cfg", c, sizeof c) &&
                  filesystemStoragePath("0:/x", d, sizeof d);
      snprintf(why, sizeof why, "'%s' '%s' '%s' '%s'", a, b, c, d);
      check("USB storage: names on the drive, the Pi's folder on the card",
            fits && !strcmp(a, "1:/Games/x.ssd") && !strcmp(b, "Pi1MHz/SWMMFS.rom") &&
            !strcmp(c, "/pi1mhz/Pi1MHz.cfg") && !strcmp(d, "0:/x"), why);
   }

   /* VFS sides and the FAT transfer directory follow it. */
   f_mkdir("1:/BeebVFS3");
   put_file("1:/BeebVFS3/scsi0.dat", "vfs", 3);
   f_mkdir("1:/Transfer");
   put_file("1:/Transfer/USB.TXT", "usb", 3);
   uint8_t info[256] = {0};
   filesystemSetFatDirectory((const uint8_t *)"/Transfer");
   bool fi = filesystemGetFatFileInfo(0, info);
   snprintf(why, sizeof why, "VFS side type %u (want 2), transfer entry %d '%s'",
            filesystemVFSDirType(3), fi, (const char *)info + 127);
   check("USB storage: VFS sides and the FAT transfer directory are the drive's",
         filesystemVFSDirType(3) == 2u && fi && !strcmp((const char *)info + 127, "USB.TXT"), why);

   /* A BREAK puts the drive's current directory back at its root, as the
      card's remount does for the card (the FAT service's record of it
      starts at "/" again). */
   {
      char cwd[64] = "";
      f_chdir("1:/Transfer");
      stop_all();
      filesystemReset();
      f_chdrive("1:");
      FRESULT r = f_getcwd(cwd, sizeof cwd);
      f_chdrive("0:");
      snprintf(why, sizeof why, "drive's directory after BREAK '%s' (%d)", cwd, r);
      check("USB storage: a BREAK resets the drive's current directory",
            r == FR_OK && !strcmp(cwd, "1:/"), why);
   }

   /* No drive at the BREAK, after the power-on wait: the card - and a
      drive that turns up later is not swapped in until the next BREAK. */
   usb_mounted = false;
   stop_all();
   filesystemReset();
   v = lun0_volume();
   int waits = usb_waits;
   snprintf(why, sizeof why, "LUN 0 on %s (want sd), waits %d (want 1), until %lu",
            v, waits, (unsigned long)usb_wait_until);
   check("USB storage, no drive: the card", !strcmp(v, "sd") && waits == 1 &&
         usb_wait_until == 5000000u && filesystemVFSDirType(3) == 0u, why);
   usb_mounted = true;
   v = lun0_volume();
   snprintf(why, sizeof why, "LUN 0 on %s (want sd), waits %d (want 1)", v, usb_waits);
   check("USB storage: a drive arriving mid-session waits for the BREAK",
         !strcmp(v, "sd") && usb_waits == 1 && !filesystemStorageOnUsb(), why);
   stop_all();
   filesystemReset();
   v = lun0_volume();
   snprintf(why, sizeof why, "LUN 0 on %s (want usb)", v);
   check("USB storage: ... and is used from it", !strcmp(v, "usb"), why);

   /* Power-on: the drive turns up during the wait. */
   usb_mounted = false;
   usb_arrives_in_wait = true;
   stop_all();
   filesystemReset();
   v = lun0_volume();
   snprintf(why, sizeof why, "LUN 0 on %s (want usb), waits %d (want 2)", v, usb_waits);
   check("USB storage: a drive found by the power-on wait is used",
         !strcmp(v, "usb") && usb_waits == 2, why);
   usb_arrives_in_wait = false;

   stop_all();
   cfg_storage = NULL;
   filesystemReset();
}

/* *FX147,203,4/5: the Beeb chooses the storage itself, over storage=, and
   a BREAK keeps the choice.  Last: the choice lasts until the Pi restarts. */
static void test_storage_choice(void)
{
   static char why[200];
   const char *v;
   int waits = usb_waits;

   /* storage unset, the drive chosen: used at once, never waited for */
   cfg_storage = NULL;
   usb_mounted = true;
   stop_all();
   bool ok = filesystemChooseStorage(STORAGE_USB);
   filesystemReset();                        /* the insert after the eject */
   v = lun0_volume();
   snprintf(why, sizeof why, "chosen %d, LUN 0 on %s (want usb), waits %d (want %d)",
            ok, v, usb_waits, waits);
   check("*FX147,203,5: the drive, over storage=sd, without the power-on wait",
         ok && !strcmp(v, "usb") && usb_waits == waits, why);

   stop_all();
   filesystemReset();                        /* BREAK */
   v = lun0_volume();
   snprintf(why, sizeof why, "LUN 0 on %s (want usb)", v);
   check("*FX147,203,5: a BREAK keeps the choice", !strcmp(v, "usb"), why);

   /* the card chosen, over storage=usb with the drive there */
   cfg_storage = "usb";
   stop_all();
   ok = filesystemChooseStorage(STORAGE_SD);
   filesystemReset();
   v = lun0_volume();
   snprintf(why, sizeof why, "chosen %d, LUN 0 on %s (want sd), waits %d (want %d)",
            ok, v, usb_waits, waits);
   check("*FX147,203,4: the card, over storage=usb", ok && !strcmp(v, "sd") &&
         usb_waits == waits, why);

   /* the drive asked for, none there: refused, the card choice kept - a
      later BREAK with a drive does not swap to it */
   usb_mounted = false;
   ok = filesystemChooseStorage(STORAGE_USB);
   usb_mounted = true;
   stop_all();
   filesystemReset();
   v = lun0_volume();
   snprintf(why, sizeof why, "chosen %d (want 0), LUN 0 after BREAK on %s (want sd)", ok, v);
   check("*FX147,203,5 with no drive: refused, the choice unchanged", !ok && !strcmp(v, "sd"), why);

   /* the drive chosen, then gone at a BREAK: the card; back at the next:
      the drive */
   ok = filesystemChooseStorage(STORAGE_USB);
   usb_mounted = false;
   stop_all();
   filesystemReset();
   const char *gone = lun0_volume();
   usb_mounted = true;
   stop_all();
   filesystemReset();
   v = lun0_volume();
   snprintf(why, sizeof why, "drive gone: %s (want sd), back: %s (want usb)", gone, v);
   check("*FX147,203,5: the drive gone at a BREAK - the card, then the drive again",
         ok && !strcmp(gone, "sd") && !strcmp(v, "usb"), why);
   stop_all();
}

/* *FX147,203,6 and storage=smb's decision (filesystem.c), the share played
   by the fsr_net_* hooks: set up or not, reachable or not, and whether the
   caller may wait (not from inside the network poll). */
static bool net_configured, net_reachable, net_can_wait = true;
static int  net_connects;
bool fsr_net_configured(void) { return net_configured; }
bool fsr_net_can_wait(void) { return net_can_wait; }
bool fsr_net_connect(void) { net_connects++; return net_reachable; }

static void test_storage_choice_smb(void)
{
   static char why[200];

   /* no share set up: refused, the choice left as it was */
   stop_all();
   filesystemReset();
   char before[4];
   snprintf(before, sizeof before, "%s", filesystemStorageRoot());
   net_configured = false;
   bool ok = filesystemChooseStorage(STORAGE_SMB);
   stop_all();
   filesystemReset();
   const char *r = filesystemStorageRoot();
   snprintf(why, sizeof why, "chosen %d (want 0), root \"%s\" (want \"%s\")", ok, r, before);
   check("*FX147,203,6 with no share set up: refused, the choice unchanged",
         !ok && !strcmp(r, before) && net_connects == 0, why);

   /* set up, not reachable: chosen all the same, the card meanwhile */
   net_configured = true;
   net_reachable = false;
   ok = filesystemChooseStorage(STORAGE_SMB);
   stop_all();
   filesystemReset();
   r = filesystemStorageRoot();
   r = filesystemStorageRoot();              /* decided: not asked again */
   snprintf(why, sizeof why, "chosen %d, root \"%s\" (want \"\"), on share %d, logons %d (want 1)",
            ok, r, filesystemStorageOnSmb(), net_connects);
   check("*FX147,203,6, share unreachable: chosen, the card until the next BREAK",
         ok && !strcmp(r, "") && !filesystemStorageOnSmb() && net_connects == 1, why);

   /* the next BREAK tries again - and now it answers */
   net_reachable = true;
   stop_all();
   filesystemReset();
   r = filesystemStorageRoot();
   snprintf(why, sizeof why, "root \"%s\" (want \"2:\"), on share %d, logons %d (want 2)",
            r, filesystemStorageOnSmb(), net_connects);
   check("*FX147,203,6: a BREAK logs on again, and the share is used",
         !strcmp(r, "2:") && filesystemStorageOnSmb() && filesystemStorageOffCard() &&
         net_connects == 2, why);

   /* first use from inside the network poll: left undecided, never the card */
   stop_all();
   filesystemReset();
   net_can_wait = false;
   r = filesystemStorageRoot();
   bool undecided = !filesystemStorageOnSmb() && !filesystemStorageOffCard();
   int logons = net_connects;
   net_can_wait = true;
   const char *r2 = filesystemStorageRoot();
   snprintf(why, sizeof why, "in the poll: \"%s\" undecided %d logons %d (want 2); after: \"%s\" logons %d (want 3)",
            r, undecided, logons, r2, net_connects);
   check("storage=smb first asked inside the poll: undecided (the call fails on the share), "
         "decided at the next use", !strcmp(r, "2:") && undecided && logons == 2 &&
         !strcmp(r2, "2:") && filesystemStorageOnSmb() && net_connects == 3, why);

   /* the card chosen back */
   ok = filesystemChooseStorage(STORAGE_SD);
   stop_all();
   filesystemReset();
   r = filesystemStorageRoot();
   check("*FX147,203,4 after 6: the card", ok && !strcmp(r, "") && !filesystemStorageOnSmb(),
         r);
   stop_all();
}

/* Vendor mode pages longer than the parser's first limits (3, 4, 6 and 6
   bytes for pages 35 to 38).  The Acorn FileStore sends MODE SENSE for pages
   37 and 38, where it looks for "(C)Acorn", and these are a Rodime RO3000S's
   own: 8 bytes each, with a length byte that matches.  Cut short, a page
   went out with a length of 6 and only 4 bytes after it. */
static void test_mode_pages_whole(void)
{
   static char why[160];
   static const char cfg[] =
      "ModeParamHeader=10000008\n"
      "LBADescriptor=0003D86000000100\n"
      "ModePage0=00020000\n"
      "ModePage35=23020000\n"
      "ModePage36=24020000\n"
      "ModePage37=2506284329410D00\n"
      "ModePage38=2606636F726E0D00\n";
   static const uint8_t p35[] = { 0x23, 0x02, 0x00, 0x00 };
   static const uint8_t p36[] = { 0x24, 0x02, 0x00, 0x00 };
   static const uint8_t p37[] = { 0x25, 0x06, 0x28, 0x43, 0x29, 0x41, 0x0D, 0x00 };
   static const uint8_t p38[] = { 0x26, 0x06, 0x63, 0x6F, 0x72, 0x6E, 0x0D, 0x00 };
   const struct { uint8_t page; const uint8_t *want; size_t n; } pages[] = {
      { 35, p35, sizeof p35 }, { 36, p36, sizeof p36 },
      { 37, p37, sizeof p37 }, { 38, p38, sizeof p38 },
   };

   f_mkdir("/BeebSCSI0");
   filesystemSetLunDirectory(1, 0);             /* earlier tests leave another directory selected */
   put_file("/BeebSCSI0/scsi6.cfg", cfg, sizeof cfg - 1);
   (void)filesystemReadLunDescriptor(6);        /* LUN 6: no other test uses it */
   for (size_t i = 0; i < sizeof pages / sizeof pages[0]; i++) {
      size_t len = 0;
      const char *got = filesystemGetModePageData(6, pages[i].page, &len);
      bool ok = got != NULL && len == pages[i].n && memcmp(got, pages[i].want, len) == 0;
      snprintf(why, sizeof why, "page %u comes back %zu bytes long, want %zu",
               (unsigned)pages[i].page, len, pages[i].n);
      check("mode pages: a page of the length its own length byte says is kept whole", ok, why);
   }
}

/* A MODE SELECT saves the whole .cfg again (filesystemWriteAttributes), so a
   page the parser cut short on reading was cut short in the file for good.
   The Acorn formatter's MODE SELECT on a Rodime RO3057S (an E40) did exactly
   that to its pages 35, 37 and 38 (8 bytes each): the file came back with
   230600, 250628432941 and 2606636F726E. */
static void test_mode_select_keeps_vendor_pages(void)
{
   static char why[200];
   scsiSetController(SCSI_CONTROLLER_SCSI);   /* a drive with its own controller */
   static char text[8192];
   static const char cfg[] =
      "ModeParamHeader=10000008\n"
      "LBADescriptor=00027D8000000100\n"
      "ModePage1=0103200405\n"
      "ModePage32=20083338313435323736\n"
      "ModePage33=2106590B1B020000\n"
      "ModePage35=2306000000000000\n"
      "ModePage36=24020000\n"
      "ModePage37=2506284329410D00\n"
      "ModePage38=2606636F726E0D00\n";
   static const char *const lines[] = {
      "ModePage32=20083338313435323736", "ModePage33=2106590B1B020000", "ModePage35=2306000000000000",
      "ModePage36=24020000", "ModePage37=2506284329410D00", "ModePage38=2606636F726E0D00",
   };

   stop_all();
   f_mkdir("/BeebSCSI0");
   filesystemSetLunDirectory(1, 0);
   put_file("/BeebSCSI0/scsi6.cfg", cfg, sizeof cfg - 1);
   put_file("/BeebSCSI0/scsi6.dat", "image", 5);

   /* FSFormat writes page 1 back whole; any MODE SELECT saves the file */
   const uint8_t ms[6] = { 0x15, 0xC1, 0, 0, 9, 0 };      /* LUN 6, SP */
   const uint8_t d1[9] = { 0, 0, 0, 0,  1, 3, 0x20, 4, 5 };
   int st = run_cmd(1, ms, 6, d1, sizeof d1);
   read_file("/BeebSCSI0/scsi6.cfg", text, sizeof text - 1);
   check("saved cfg: the MODE SELECT is accepted", st == 0, "status not GOOD");
   for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++) {
      snprintf(why, sizeof why, "the saved file lacks the line %s", lines[i]);
      check("saved cfg: a vendor page the drive has is written back whole",
            strstr(text, lines[i]) != NULL, why);
   }
   stop_all();
   scsiSetController(SCSI_CONTROLLER_MIXED);
}

int main(int argc, char **argv)
{
   defscsi_path = argc > 1 ? argv[1] : "defscsi.cfg";
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
   static FATFS usb_fs;
   const MKFS_PARM usb_opt = { FM_ANY, 0, 0, 0, 0 };
   usbdisk = calloc(USB_SECTORS, 512u);
   f_mount(&usb_fs, "1:", 0);           /* as usb_storage.c does: its own FATFS */
   if (!usbdisk || f_mkfs("1:", &usb_opt, work, sizeof work) != FR_OK) {
      printf("FAIL: could not format the USB RAM disk\n");
      return 1;
   }

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
   test_format_new_disc();
   test_jukebox();
   test_stop_directory_luns();
   test_mode_pages_whole();
   test_mode_select_keeps_vendor_pages();

   scsiReset(0);
   test_bsfatread_failure();
   test_sense_set();
   test_fcode_short();                  /* after test_jukebox: a VFS side with data */
   test_reset_abandons();
   test_filestore_mode_select();
   test_fservfmt_page3_masked();
   test_scsi_page0_ps();
   test_drive_geometry_setting();
   test_sense_kept();
   test_write_mask_page1();
   test_readonly_image();
   test_format_interleave();
   test_acb_mode_and_format();
   test_stopped_geometry();
   test_mode_sense();
   test_ccs_pages();
   test_controller_name();
   test_controller_gating();
   test_extended_sense();
   test_block_sizes();
   test_scsi_drive();
   test_pending_pages_lifetime();
   test_upgrade_legacy_cfg();
   test_block_size_2048();
   test_title_cache();                  /* it remounts the card */
   test_storage();                      /* it moves the storage */
   test_storage_choice();               /* last: the choice lasts till restart */
   test_storage_choice_smb();

   printf("%d passed, %d failed\n", passes, failures);
   return failures ? 1 : 0;
}
