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
static uint8_t  bus_in[1024];
static unsigned bus_in_len, bus_in_pos;
static uint8_t  bus_out[4096];
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
uint32_t hostadapterPerformReadDMA(const uint8_t *b)
{
   for (unsigned i = 0; i < 256u && bus_out_len < sizeof bus_out; i++)
      bus_out[bus_out_len++] = b[i];
   read_dmas++;
   if (fail_disk_after_dma >= 0 && read_dmas >= (unsigned)fail_disk_after_dma)
      disk_fail = true;
   return 256;
}
uint32_t hostadapterPerformWriteDMA(uint8_t *b)
{
   unsigned n = dma_short ? dma_short : 256u;
   dma_short = 0;
   for (unsigned i = 0; i < n; i++)
      b[i] = bus_in_next();
   return n;
}
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

/* REQUEST SENSE on `lun` (host 1): the error code byte. */
static int sense_of(uint8_t lun)
{
   const uint8_t rs[6] = { 0x03, (uint8_t)(lun << 5), 0, 0, 4, 0 };
   int st = run_cmd(1, rs, 6, NULL, 0);
   return (st == 0 && bus_out_len >= 4) ? bus_out[0] : -1;
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
   bool ok = filesystemChooseStorage(true);
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
   ok = filesystemChooseStorage(false);
   filesystemReset();
   v = lun0_volume();
   snprintf(why, sizeof why, "chosen %d, LUN 0 on %s (want sd), waits %d (want %d)",
            ok, v, usb_waits, waits);
   check("*FX147,203,4: the card, over storage=usb", ok && !strcmp(v, "sd") &&
         usb_waits == waits, why);

   /* the drive asked for, none there: refused, the card choice kept - a
      later BREAK with a drive does not swap to it */
   usb_mounted = false;
   ok = filesystemChooseStorage(true);
   usb_mounted = true;
   stop_all();
   filesystemReset();
   v = lun0_volume();
   snprintf(why, sizeof why, "chosen %d (want 0), LUN 0 after BREAK on %s (want sd)", ok, v);
   check("*FX147,203,5 with no drive: refused, the choice unchanged", !ok && !strcmp(v, "sd"), why);

   /* the drive chosen, then gone at a BREAK: the card; back at the next:
      the drive */
   ok = filesystemChooseStorage(true);
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

   scsiReset(0);
   test_bsfatread_failure();
   test_sense_set();
   test_fcode_short();                  /* after test_jukebox: a VFS side with data */
   test_reset_abandons();
   test_readonly_image();
   test_stopped_geometry();
   test_title_cache();                  /* it remounts the card */
   test_storage();                      /* it moves the storage */
   test_storage_choice();               /* last: the choice lasts till restart */

   printf("%d passed, %d failed\n", passes, failures);
   return failures ? 1 : 0;
}
