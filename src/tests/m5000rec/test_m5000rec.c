/* Host test: a Music 5000 recording must reach the card whole even when the
   card is remounted part-way through the flush.  Review 2026-10-06 C1.

   The flush (M5000_emulator.c) writes the WAV one 64 KB slice per poll
   pass from a FIL it holds open across passes.  filesystemReset() - run by
   every BBC reset (harddisc_emulator_init) and every jukebox
   (hd_juke_service) - re-registers the FatFs volume, which invalidates
   that FIL: f_write then fails, f_close fails, and the directory entry is
   left at size 0.

   Real M5000_emulator.c, real filesystem.c, real FatFs on a RAM disk.  The
   synth is driven by its own poll function; a WAV's expected bytes are
   the JIM capture buffer itself (header included, which rec_stop wrote). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Pi1MHz.h"
#include "config.h"
#include "videoplayer.h"
#include "rpi/audio.h"
#include "rpi/systimer.h"
#include "rpi/fileparser.h"
#include "BeebSCSI/fatfs/ff.h"
#include "BeebSCSI/fatfs/diskio.h"
#include "BeebSCSI/filesystem.h"
#include "M5000_emulator.h"

/* ---- platform stubs ---------------------------------------------------- */

static Pi1MHz_t pi_struct;
Pi1MHz_t * const Pi1MHz = &pi_struct;
uint8_t fx_register[256];
volatile Pi1MHz_break_t Pi1MHz_break;

static func_ptr polls[8];
static unsigned int npolls;
void Pi1MHz_Register_Poll(func_ptr fn, const char *name)
{
   (void)name;
   for (unsigned int i = 0; i < npolls; i++)
      if (polls[i] == fn)
         return;
   polls[npolls++] = fn;
}

#define AUDIO_FRAMES 4096u
static int16_t audio_buf[AUDIO_FRAMES * 2u];
int16_t *audio_write_ptr(const audio_producer_t *p, uint32_t *contig_frames)
{
   (void)p;
   *contig_frames = AUDIO_FRAMES;
   return audio_buf;
}
void audio_commit(uint32_t frames) { (void)frames; }
void audio_claim(const audio_producer_t *p) { (void)p; }
bool audio_out_is_hdmi(void) { return false; }
bool rpi_audio_beeb_muted(void) { return false; }

const char *config_get(const char *key) { (void)key; return NULL; }
bool config_beeb_write_protected(void) { return false; }
uint32_t RPI_GetSystemTime(void) { return 0; }
void videoplayer_media_changed(void) {}
int parse_findindex(const char *k, const parserkey a[]) { (void)k; (void)a; return -1; }
int parse_readfile(const char *f, const char *o, const parserkey k[], parserkeyvalue v[])
{ (void)f; (void)o; (void)k; (void)v; return 0; }
void parse_releasekeyvalues(parserkeyvalue v[], int n) { (void)v; (void)n; }

/* ---- RAM disk ---------------------------------------------------------- */

#define DISK_SECTORS (128u * 1024u * 2u)        /* 128 MB of 512-byte sectors */
static uint8_t *disk;

static bool card_gone;                  /* the card will not initialise again */
DSTATUS disk_initialize(BYTE pdrv) { (void)pdrv; return card_gone ? STA_NOINIT : 0; }
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

/* ---- harness ----------------------------------------------------------- */

#define INSTANCE   3u
#define REC_BASE   0x100000u

static int failures, passes;
static unsigned int recording;          /* Musics%03u.wav of the next flush */

static void poll(void)
{
   for (unsigned int i = 0; i < npolls; i++)
      polls[i]();
}

/* One loud channel on the Music 5000: the capture only starts at the first
   non-zero sample.  The control block is cleared by every init. */
static void synth_play(void)
{
   uint8_t *ram = &Pi1MHz->JIM_ram[0x3000];
   for (unsigned int i = 0; i < 128u; i++)
      ram[i] = (uint8_t)(0x80u | (i & 0x7Fu));   /* waveform 0 */
   uint8_t *c = ram + 0xE00;
   c[0x00] = 0x00;          /* FREQlo (bit 0 clear: not a phase load) */
   c[0x10] = 0x00;
   c[0x20] = 0x08;          /* FREQhi: a carry every 32 samples */
   c[0x50] = 0x00;          /* waveform 0 */
   c[0x60] = 0x7F;          /* amplitude */
   c[0x70] = 0x08;          /* centre pan */
}

/* init_emulator after a BBC reset: the poll table starts empty, the reset
   count goes up, then the inits run in table order - Rampage (whose
   JIM_Init.bin read goes through filesystemReadFile, as config_load's
   does), Harddisc, M5000. */
static void bbc_reset(bool harddisc, bool m5000)
{
   npolls = 0;
   Pi1MHz_break.inits++;
   uint8_t *cfg = NULL;
   if (filesystemReadFile("Pi1MHz/Pi1MHz.cfg", &cfg, 0) || cfg)
      free(cfg);
   if (harddisc)
      filesystemReset();
   if (m5000) {
      M5000_emulator_init(INSTANCE, 0);
      synth_play();
   }
}

static void emulators_init(void)
{
   bbc_reset(true, true);
}

static void record(unsigned int passes_)
{
   fx_register[INSTANCE] = 1;
   for (unsigned int i = 0; i < passes_; i++)
      poll();
}

static void stop(void)
{
   fx_register[INSTANCE] = 0;
   poll();                              /* rec_stop: the flush is armed */
}

static const char *name_of(unsigned int n)
{
   static char fn[24];
   snprintf(fn, sizeof fn, "Musics%03u.wav", n);
   return fn;
}

/* Into the flush: the file is open and two slices are written. */
static bool mid_flush(const char *fn)
{
   poll();                              /* f_open */
   if (!M5000_recording_path_busy(fn)) return false;
   poll();
   poll();
   return M5000_recording_path_busy(fn);
}

static void drain(const char *fn)
{
   poll();                              /* a flush only just armed opens its file here */
   for (unsigned int i = 0; i < 100000u && M5000_recording_path_busy(fn); i++)
      poll();
   for (unsigned int i = 0; i < 4u; i++)
      poll();
}

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

/* The file on the card must be the capture buffer, byte for byte - the
   first `want` bytes of it, or (want == 0) all of it. */
static void verify_len(const char *test, const char *fn, uint32_t want)
{
   const uint8_t *j = &Pi1MHz->JIM_ram[REC_BASE];
   uint32_t whole = (uint32_t)j[4] | (uint32_t)j[5] << 8 | (uint32_t)j[6] << 16 | (uint32_t)j[7] << 24;
   whole += 8u;
   static char why[160];
   if (want == 0u)
      want = whole;

   if (whole < 4u * 64u * 1024u) {
      snprintf(why, sizeof why, "precondition: capture only %lu bytes (synth silent?)", (unsigned long)whole);
      check(test, false, why);
      return;
   }
   FILINFO fi;
   FRESULT r = f_stat(fn, &fi);
   if (r != FR_OK) {
      snprintf(why, sizeof why, "%s: f_stat %d", fn, (int)r);
      check(test, false, why);
      return;
   }
   if (fi.fsize != want) {
      snprintf(why, sizeof why, "%s is %lu bytes, recording is %lu",
               fn, (unsigned long)fi.fsize, (unsigned long)want);
      check(test, false, why);
      return;
   }
   FIL f;
   uint8_t *buf = malloc(want);
   UINT got = 0;
   bool same = buf && f_open(&f, fn, FA_READ) == FR_OK &&
               f_read(&f, buf, want, &got) == FR_OK && got == want &&
               memcmp(buf, j, want) == 0;
   f_close(&f);
   free(buf);
   snprintf(why, sizeof why, "%s content differs from the capture", fn);
   check(test, same, why);
}

static void verify(const char *test, const char *fn)
{
   verify_len(test, fn, 0u);
}

/* ---- tests ------------------------------------------------------------- */

static void test_plain_flush(const char *test)
{
   const char *fn = name_of(recording++);
   record(64);
   stop();
   drain(fn);
   verify(test, fn);
}

static void test_break_mid_flush(void)
{
   const char *fn = name_of(recording++);
   record(64);
   stop();
   if (!mid_flush(fn)) { check("BREAK mid-flush", false, "flush not under way"); return; }
   emulators_init();                    /* BBC reset: init_emulator's order */
   drain(fn);
   verify("BREAK mid-flush", fn);
}

static void test_jukebox_mid_flush(void)
{
   const char *fn = name_of(recording++);
   record(64);
   stop();
   if (!mid_flush(fn)) { check("jukebox mid-flush", false, "flush not under way"); return; }
   filesystemReset();                   /* hd_juke_service, before the swap */
   drain(fn);
   verify("jukebox mid-flush", fn);
}

static void test_dismount_mid_flush(void)
{
   const char *fn = name_of(recording++);
   record(64);
   stop();
   if (!mid_flush(fn)) { check("dismount mid-flush", false, "flush not under way"); return; }
   filesystemDismount();                /* the FAT service's f unmount */
   drain(fn);
   filesystemMount();
   /* The Beeb said unmounted: the flush must not write on (FatFs would
      mount the card behind this layer's back), so the WAV stops, closed,
      at the two slices written. */
   verify_len("dismount mid-flush", fn, 2u * 64u * 1024u);
}

/* Harddisc disabled: nothing calls filesystemReset, but every BBC reset's
   config_load (and JIM_Init.bin) read goes through filesystemReadFile -
   which, while this layer has the card unmounted, mounts it. */
static void test_readfile_mid_flush(void)
{
   const char *fn = name_of(recording++);
   filesystemDismount();                /* unmounted as far as this layer knows */
   record(64);
   stop();
   if (!mid_flush(fn)) { check("BREAK mid-flush, Harddisc disabled", false, "flush not under way"); return; }
   bbc_reset(false, true);
   drain(fn);
   filesystemMount();
   verify("BREAK mid-flush, Harddisc disabled", fn);
}

/* Between the remount's close and the reopen the file became another of
   the same length (a card swap, a host write): it must not be appended to. */
static void test_swapped_file(void)
{
   const char *fn = name_of(recording++);
   record(64);
   stop();
   if (!mid_flush(fn)) { check("same-size impostor", false, "flush not under way"); return; }
   filesystemReset();                   /* a jukebox: the WAV is closed */
   static uint8_t other[2u * 64u * 1024u];
   memset(other, 0x55, sizeof other);
   FIL f;
   UINT put = 0;
   bool made = f_unlink(fn) == FR_OK &&
               f_open(&f, fn, FA_CREATE_NEW | FA_WRITE) == FR_OK &&
               f_write(&f, other, sizeof other, &put) == FR_OK && put == sizeof other &&
               f_close(&f) == FR_OK;
   if (!made) { check("same-size impostor", false, "could not plant the impostor"); return; }
   drain(fn);
   FILINFO fi;
   static uint8_t back[sizeof other + 1u];
   UINT got = 0;
   bool same = f_stat(fn, &fi) == FR_OK && fi.fsize == sizeof other &&
               f_open(&f, fn, FA_READ) == FR_OK &&
               f_read(&f, back, sizeof back, &got) == FR_OK && got == sizeof other &&
               memcmp(back, other, sizeof other) == 0;
   f_close(&f);
   static char why[96];
   snprintf(why, sizeof why, "the impostor was written to (now %lu bytes)", (unsigned long)fi.fsize);
   check("same-size impostor", same, why);
}

/* A BBC reset with M5000 disabled (edited config, BeebSID): nothing will
   ever poll the flush again.  It must let go - closed at the length written
   - not leave the root busy and an eject waiting for ever. */
static void test_disabled_after_break(void)
{
   const char *fn = name_of(recording++);
   record(64);
   stop();
   if (!mid_flush(fn)) { check("BREAK mid-flush, M5000 disabled", false, "flush not under way"); return; }
   bbc_reset(true, false);
   bool busy = M5000_recording_path_busy("/");
   bool ejected = false;
   for (unsigned int i = 0; i < 16u && !ejected; i++) {
      poll();
      ejected = filesystemEject();
   }
   filesystemInsert();
   check("BREAK mid-flush, M5000 disabled: lets go", !busy && ejected,
         busy ? "the root is still busy" : "eject still waiting");
   verify_len("BREAK mid-flush, M5000 disabled", fn, 2u * 64u * 1024u);
   emulators_init();                    /* and back on for what follows */
}

/* The BREAK's remount fails (the card will not initialise): what was
   written must survive as a closed file of exactly that length, not a
   0-byte entry. */
static void test_break_card_gone(void)
{
   const char *fn = name_of(recording++);
   record(64);
   stop();
   if (!mid_flush(fn)) { check("BREAK mid-flush, no card", false, "flush not under way"); return; }
   card_gone = true;
   emulators_init();
   drain(fn);
   card_gone = false;
   filesystemMount();
   verify_len("BREAK mid-flush, no card", fn, 2u * 64u * 1024u);
}

static void test_break_while_recording(void)
{
   const char *fn = name_of(recording++);
   record(64);
   emulators_init();                    /* BREAK stops the recording */
   drain(fn);
   verify("BREAK while recording", fn);
}

static void test_eject_mid_flush(void)
{
   const char *fn = name_of(recording++);
   record(64);
   stop();
   if (!mid_flush(fn)) { check("eject mid-flush", false, "flush not under way"); return; }
   bool waited = !filesystemEject();
   for (unsigned int i = 0; i < 100000u && !filesystemEject(); i++)
      poll();
   bool insert = filesystemInsert();
   check("eject waits for the flush", waited && insert, "eject did not wait, or reinsert failed");
   verify("eject mid-flush", fn);
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

   Pi1MHz->JIM_ram_size = 3;            /* 48 MB: the capture gets 16 MB */
   Pi1MHz->JIM_ram = calloc((size_t)Pi1MHz->JIM_ram_size * JIM_RAM_STEP, 1);
   if (!Pi1MHz->JIM_ram)
      return 1;

   filesystemInitialise(0);
   emulators_init();                    /* power on */

   test_plain_flush("flush with no remount (control)");
   test_break_mid_flush();
   test_jukebox_mid_flush();
   test_dismount_mid_flush();
   test_break_card_gone();
   test_break_while_recording();
   test_eject_mid_flush();
   test_readfile_mid_flush();
   test_swapped_file();
   test_disabled_after_break();
   test_plain_flush("flush after all the above");

   printf("%d passed, %d failed\n", passes, failures);
   return failures ? 1 : 0;
}
