/* Host tests for the FAT service file API used by the Pi1MHz builds of
 * MMFS and MMFS2: a slot's own directory (22), directory blocks (21), kept
 * files reopened after a BBC reset (open mode bit 7), set attributes (19)
 * and rename in a slot's directory (12).  Commands are built as the Beeb
 * builds them and go through the real services port; FatFs is real, on a
 * RAM disk.  See run_tests.sh.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "Pi1MHz.h"
#include "services.h"
#include "config.h"
#include "BeebSCSI/fatfs/ff.h"
#include "BeebSCSI/fatfs/diskio.h"
#include "BeebSCSI/filesystem.h"

bool fat_service_file_in_use(const char *host_path);
void fat_service_init(void);

/* ---- platform stubs ---- */
uint32_t filesystemReadFile(const char *f, uint8_t **a, unsigned int m)
{ (void)f; (void)a; (void)m; return 0; }

static Pi1MHz_t pi;
Pi1MHz_t *const Pi1MHz = &pi;
uint32_t Pi1MHz_now_us;

#define SVC_BASE 0xA6u
static callback_func_ptr write_cb[256];
void Pi1MHz_Register_Memory(unsigned int access, unsigned int addr, callback_func_ptr fn)
{ if (access == WRITE_FRED) write_cb[addr & 0xff] = fn; }
static func_ptr poll_cb;
void Pi1MHz_Register_Poll(func_ptr fn, const char *name) { (void)name; poll_cb = fn; }
void Pi1MHz_MemoryWrite(uint32_t addr, uint8_t data)  { pi.Memory[addr & 0x1ff] = data; }
void Pi1MHz_MemoryWrite16(uint32_t addr, uint32_t data)
{ pi.Memory[addr & 0x1ff] = (uint8_t)data; pi.Memory[(addr + 1u) & 0x1ff] = (uint8_t)(data >> 8); }
void Pi1MHz_nIRQ_ASSERT(uint8_t src) { (void)src; }
void Pi1MHz_nIRQ_CLEAR(uint8_t src) { (void)src; }

const char *filesystemStorageRoot(void) { return ""; }
static bool on_usb;                       /* the storage decided at a reset */
bool filesystemStorageOnUsb(void) { return on_usb; }
bool filesystemMount(void) { return true; }
bool filesystemDismount(void) { return true; }
static bool (*eject_cb)(void);
void filesystemRegisterEject(bool (*eject)(void), void (*inserted)(void)) { eject_cb = eject; (void)inserted; }
static void (*remount_cb)(void);
void filesystemRegisterRemount(void (*closing)(void)) { remount_cb = closing; }
bool filesystemHostPathBusy(const char *path) { (void)path; return false; }
bool M5000_recording_path_busy(const char *path) { (void)path; return false; }
bool fujibus_service_path_busy(const char *path) { (void)path; return false; }
bool webserver_sd_space_now(uint64_t *t, uint64_t *f) { *t = 0; *f = 0; return false; }
unsigned char disk_type(void) { return 0; }

/* ---- RAM disk: drive 0 only ---- */
#define DISK_SECTORS (64u * 1024u * 2u)          /* 64 MB */
static uint8_t *disk;
DSTATUS disk_initialize(BYTE pdrv) { return pdrv ? STA_NOINIT : 0; }
DSTATUS disk_status(BYTE pdrv) { return pdrv ? STA_NOINIT : 0; }
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
   if (pdrv || sector + count > DISK_SECTORS) return RES_PARERR;
   memcpy(buff, disk + (size_t)sector * 512u, (size_t)count * 512u);
   return RES_OK;
}
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
   if (pdrv || sector + count > DISK_SECTORS) return RES_PARERR;
   memcpy(disk + (size_t)sector * 512u, buff, (size_t)count * 512u);
   return RES_OK;
}
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
   switch (cmd) {
   case CTRL_SYNC:        return RES_OK;
   case GET_SECTOR_COUNT: *(LBA_t *)buff = DISK_SECTORS; return RES_OK;
   case GET_SECTOR_SIZE:  *(WORD *)buff = 512; return RES_OK;
   case GET_BLOCK_SIZE:   *(DWORD *)buff = 1; return RES_OK;
   default:               return RES_PARERR;
   }
}

/* ---- the Beeb's side: command blocks in the top JIM pages ---- */
#define BUF 0x10000u                     /* a 512-byte buffer in disc RAM */
static uint32_t cp_of(uint8_t slot) { return DISC_RAM_BASE | 0xFF0000u | ((uint32_t)(0xF0u + slot) << 8); }
static uint8_t *blk(uint8_t slot) { return &Pi1MHz->JIM_ram[cp_of(slot)]; }
static uint8_t go(uint8_t slot)
{
   write_cb[SVC_BASE + 4](TEST_GPIO(SVC_BASE + 4, 0xF0u + slot));
   poll_cb();
   return pi.Memory[SVC_BASE + 4];
}
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static uint8_t f_open_cmd(uint8_t slot, const char *name, uint8_t mode)
{
   memset(blk(slot), 0, 256);
   blk(slot)[0] = 2; blk(slot)[2] = mode;
   strcpy((char *)blk(slot) + 3, name);
   return go(slot);
}
static uint8_t f_close_cmd(uint8_t slot) { memset(blk(slot), 0, 16); blk(slot)[0] = 3; return go(slot); }
static uint8_t rw_cmd(uint8_t slot, uint8_t cmd, uint32_t offset, uint32_t len)
{
   memset(blk(slot), 0, 16);
   put32(blk(slot), (len << 8) | cmd);
   put32(blk(slot) + 4, BUF);
   put32(blk(slot) + 8, offset);
   return go(slot);
}
static uint8_t slot_chdir(uint8_t slot, const char *name)
{ memset(blk(slot), 0, 256); blk(slot)[0] = 22; strcpy((char *)blk(slot) + 1, name); return go(slot); }
static uint8_t dir_block(uint8_t slot, uint32_t n)
{ memset(blk(slot), 0, 16); blk(slot)[0] = 21; put32(blk(slot) + 4, BUF); put32(blk(slot) + 8, n); return go(slot); }
static uint8_t set_attr(uint8_t slot, const char *name, uint8_t mask, uint8_t val)
{ memset(blk(slot), 0, 256); blk(slot)[0] = 19; blk(slot)[1] = mask; blk(slot)[2] = val; strcpy((char *)blk(slot) + 3, name); return go(slot); }
static uint8_t rename_cmd(uint8_t slot, const char *a, const char *b)
{
   memset(blk(slot), 0, 256); blk(slot)[0] = 12;
   strcpy((char *)blk(slot) + 1, a);
   strcpy((char *)blk(slot) + 2 + strlen(a), b);
   return go(slot);
}
static uint8_t shared_chdir(const char *name)
{ memset(blk(3), 0, 256); blk(3)[0] = 11; strcpy((char *)blk(3) + 1, name); return go(3); }
static unsigned int boot_id(uint8_t slot)
{ memset(blk(slot), 0, 16); blk(slot)[0] = 20; go(slot); return (unsigned int)(blk(slot)[1] | (blk(slot)[2] << 8)); }
static uint8_t *buf(void) { return &Pi1MHz->JIM_ram[DISC_RAM_BASE + BUF]; }

/* entry k of the last directory block, as "NAME    EXT" + attrs */
static const uint8_t *ent(int k) { return buf() + 32 * k; }
static int ent_is(int k, const char *name11) { return memcmp(ent(k), name11, 11) == 0; }
static uint32_t ent_id(int k) { const uint8_t *e = ent(k); return e[26] | (e[27] << 8) | ((uint32_t)e[20] << 16); }

static int checks, fails;
static void ok(int cond, const char *what)
{
   checks++;
   if (!cond) { fails++; printf("  FAIL: %s\n", what); }
   else         printf("  ok: %s\n", what);
}

static void make_file(const char *path, uint32_t size, uint8_t seed)
{
   FIL f; UINT bw;
   static uint8_t data[65536];
   for (uint32_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 7u + seed);
   assert(f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK);
   for (uint32_t done = 0; done < size; done += bw) {
      UINT n = size - done > sizeof data ? (UINT)sizeof data : (UINT)(size - done);
      assert(f_write(&f, data, n, &bw) == FR_OK && bw == n);
   }
   f_close(&f);
}
static int data_ok(uint32_t offset, uint32_t len, uint8_t seed)
{
   for (uint32_t i = 0; i < len; i++)
      if (buf()[i] != (uint8_t)(((offset + i) % 65536u) * 7u + seed)) return 0;
   return 1;
}

static void format(BYTE fmt)
{
   static FATFS fs;
   static uint8_t work[FF_MAX_SS * 4];
   const MKFS_PARM opt = { fmt, 0, 0, 0, 0 };
   memset(disk, 0, (size_t)DISK_SECTORS * 512u);
   f_mount(NULL, "", 0);
   assert(f_mkfs("", &opt, work, sizeof work) == FR_OK);
   assert(f_mount(&fs, "", 1) == FR_OK);
}

/* a BBC reset, as the firmware runs it: the remount hook, then init again */
static void bbc_reset(void)
{
   if (remount_cb) remount_cb();
   fat_service_init();
}

int main(void)
{
   pi.JIM_ram = calloc(1, 32u * 1024u * 1024u);
   pi.JIM_ram_size = 2;                       /* DISC_RAM_BASE == 0 */
   disk = calloc(DISK_SECTORS, 512u);
   assert(pi.JIM_ram && disk);
   services_emulator_init(0, SVC_BASE);
   fat_service_init();

   puts("== FAT32 volume ==");
   format(FM_FAT32);
   assert(f_mkdir("/GAMES") == FR_OK);
   make_file("/GAMES/Elite.ssd", 102400, 1);
   make_file("/GAMES/Revs (Acornsoft).dsd", 204800, 2);
   make_file("/.hidden", 10, 3);
   make_file("/BEEB.MMB", 8192, 4);

   puts("-- this boot's id (20)");
   {
      Pi1MHz_now_us = 0x12345678u;
      memset(blk(11), 0, 16); blk(11)[0] = 20;
      go(11);
      unsigned int id = (unsigned int)(blk(11)[1] | (blk(11)[2] << 8));
      ok(id != 0u, "a nonzero id at +1,+2 (an older Pi leaves the zeros)");
      Pi1MHz_now_us = 0x9ABCDEF0u;
      bbc_reset();
      memset(blk(11), 0, 16); blk(11)[0] = 20;
      go(11);
      ok((unsigned int)(blk(11)[1] | (blk(11)[2] << 8)) == id, "the same after a BBC reset, time gone by");
   }

   puts("-- a slot's own directory (22)");
   ok(slot_chdir(11, "") == FR_OK, "slot 11: \"\" is the root");
   ok(slot_chdir(11, "GAMES.") == FR_OK, "\"GAMES.\" (MMFS2: no extension) is GAMES");
   ok(slot_chdir(11, "...") == FR_OK && dir_block(11, 0) == FR_OK && ent_is(0, "GAMES      "),
      "\"...\" is \"..\": back at the root");
   ok(slot_chdir(11, "GAMES") == FR_OK, "slot 11: GAMES again");
   ok(slot_chdir(11, "NOPE") == FR_NO_PATH, "a missing directory is refused");
   ok(slot_chdir(11, "Elite.ssd") == FR_NO_PATH, "a file is not a directory");
   ok(dir_block(11, 0) == FR_OK && ent_is(2, "ELITE   SSD"), "...and the slot stays in GAMES");
   ok(shared_chdir("/") == FR_OK, "the shared directory moves (SD explorer)");
   ok(dir_block(11, 0) == FR_OK && ent_is(2, "ELITE   SSD"), "slot 11's directory does not");

   puts("-- directory blocks (21)");
   dir_block(11, 0);
   ok(ent_is(0, ".          ") && (ent(0)[11] & AM_DIR), "subdirectory: \".\" first");
   ok(ent_is(1, "..         ") && ent_id(1) == 0u, "\"..\" next, cluster 0 = the root");
   ok(ent_is(2, "ELITE   SSD") && ent(2)[28] == 0x00 && ent(2)[29] == 0x90 && ent(2)[30] == 0x01,
      "Elite.ssd as its short name, size 102400");
   ok(memcmp(ent(3), "REVS(A~1DSD", 11) == 0, "a long name: FatFs's own short name");
   ok(ent_id(2) != 0u && ent_id(2) < 0x100000u && ent_id(2) != ent_id(3),
      "nonzero 20-bit ids, distinct");
   ok(ent(4)[0] == 0, "then the end of the directory");
   ok(dir_block(11, 1) == FR_OK && ent(0)[0] == 0, "block 1 of a short directory is empty");
   slot_chdir(11, "..");
   dir_block(11, 0);
   {
      int games = 0, hidden = 0, mmb = 0, dots = 0;
      for (int k = 0; k < 16 && ent(k)[0]; k++) {
         if (ent_is(k, "GAMES      ") && (ent(k)[11] & AM_DIR)) games = 1;
         if (ent(k)[0] == '.') dots++;
         if (memcmp(ent(k), "HIDDEN", 6) == 0) hidden = 1;
         if (ent_is(k, "BEEB    MMB")) mmb = 1;
      }
      ok(games && mmb && !dots && !hidden, "root: no \".\"/\"..\", dot-files left out");
   }

   puts("-- kept files (open mode bit 7) and a BBC reset");
   slot_chdir(11, "GAMES");
   ok(f_open_cmd(11, "ELITE.SSD", 0x80 | FA_READ | FA_WRITE) == FR_OK, "open by 8.3 name, kept");
   ok(fat_service_file_in_use("/GAMES/Elite.ssd"), "in use: WebDAV/MTP may not overwrite it");
   ok(rw_cmd(11, 4, 1000, 512) == FR_OK && data_ok(1000, 512, 1), "seek+read");
   bbc_reset();
   ok(!fat_service_file_in_use("/GAMES/Elite.ssd"), "the reset closed it: protection released");
   ok(dir_block(11, 0) == FR_OK && ent_is(2, "ELITE   SSD"), "the slot's directory survives the reset");
   ok(rw_cmd(11, 4, 51200, 512) == FR_OK && data_ok(51200, 512, 1), "next read reopens it");
   ok(fat_service_file_in_use("/GAMES/Elite.ssd"), "...and protects it again");
   memset(buf(), 0xA5, 512);
   bbc_reset();
   ok(rw_cmd(11, 5, 0, 512) == FR_OK, "a write after a reset reopens it too");
   {
      FIL f; UINT br; uint8_t b[4];
      bbc_reset();                             /* close, so FatFs sees it */
      f_open(&f, "/GAMES/Elite.ssd", FA_READ); f_read(&f, b, 4, &br); f_close(&f);
      ok(b[0] == 0xA5 && b[3] == 0xA5, "...and the data reached the file");
   }
   ok(f_unlink("/GAMES/Elite.ssd") == FR_OK, "(the file deleted under a reset)");
   ok(rw_cmd(11, 4, 0, 512) == FR_NO_FILE, "the read fails: never another file's data");
   make_file("/GAMES/Elite.ssd", 102400, 9);
   ok(rw_cmd(11, 4, 0, 512) == FR_OK && data_ok(0, 512, 9), "a new file of that name is reopened");
   memset(buf(), 0x5A, 512);
   ok(rw_cmd(11, 4, 102400 - 100, 512) == 20 && data_ok(102400 - 100, 100, 9) &&
      buf()[100] == 0 && buf()[511] == 0, "a read past the end: short (20), the rest zeros");
   bbc_reset();
   ok(f_close_cmd(11) == FR_OK, "closing a kept file a reset closed: OK");
   ok(rw_cmd(11, 4, 0, 512) == FR_INVALID_OBJECT, "...and then it is forgotten");
   ok(f_open_cmd(11, "ELITE.SSD", FA_READ) == FR_OK, "open without bit 7");
   bbc_reset();
   ok(rw_cmd(11, 4, 0, 512) == FR_INVALID_OBJECT, "is not kept over a reset");

   puts("-- set attributes (19), rename (12)");
   ok(set_attr(11, "ELITE.SSD", AM_RDO, AM_RDO) == FR_OK, "lock");
   dir_block(11, 0);
   ok(ent_is(2, "ELITE   SSD") && (ent(2)[11] & AM_RDO), "read-only in the directory block");
   ok(set_attr(11, "ELITE.SSD", AM_RDO, 0) == FR_OK, "unlock");
   dir_block(11, 0);
   ok(!(ent(2)[11] & AM_RDO), "read-only cleared");
   ok(set_attr(11, "ELITE.SSD", AM_DIR, AM_DIR) == FR_OK, "the directory bit cannot be set");
   dir_block(11, 0);
   ok(!(ent(2)[11] & AM_DIR), "...it is masked off");
   ok(rename_cmd(11, "ELITE.SSD", "ELITE2.SSD") == FR_OK, "rename in the slot's directory");
   {
      FILINFO fi;
      ok(f_stat("/GAMES/ELITE2.SSD", &fi) == FR_OK && f_stat("/GAMES/Elite.ssd", &fi) == FR_NO_FILE,
         "...where the slot is, not the shared directory");
   }

   puts("-- clients without slot directories are unchanged");
   ok(shared_chdir("/GAMES") == FR_OK, "shared chdir");
   ok(f_open_cmd(5, "ELITE2.SSD", FA_READ) == FR_OK, "a relative open on slot 5 uses it");
   ok(fat_service_file_in_use("/GAMES/ELITE2.SSD"), "recorded joined to the shared directory");
   f_close_cmd(5);
   ok(f_open_cmd(14, "/BEEB.MMB", 0x80 | FA_READ | FA_WRITE) == FR_OK, "MMFS: /BEEB.MMB kept on slot 14");
   bbc_reset();
   ok(rw_cmd(14, 4, 512, 512) == FR_OK && data_ok(512, 512, 4), "reopened after a reset");
   f_close_cmd(14);

   puts("-- a kept file locked, renamed, on other storage; an eject");
   ok(f_open_cmd(11, "ELITE2.SSD", 0x80 | FA_READ | FA_WRITE) == FR_OK, "kept read-write");
   ok(set_attr(11, "ELITE2.SSD", AM_RDO, AM_RDO) == FR_OK, "then locked");
   bbc_reset();
   ok(rw_cmd(11, 4, 0, 512) == FR_OK && data_ok(0, 512, 9), "a reset reopens it, read only");
   ok(rw_cmd(11, 5, 0, 512) == FR_DENIED, "...where a write is refused");
   ok(set_attr(11, "ELITE2.SSD", AM_RDO, 0) == FR_OK, "unlocked");
   on_usb = true;
   bbc_reset();
   ok(rw_cmd(11, 4, 0, 512) == FR_NO_FILE, "the storage changed (USB): not reopened there");
   on_usb = false;
   ok(rw_cmd(11, 4, 0, 512) == FR_OK, "back on the card: reopened");
   ok(rename_cmd(11, "ELITE2.SSD", "ELITE3.SSD") == FR_OK, "renamed while kept");
   ok(fat_service_file_in_use("/GAMES/ELITE3.SSD"), "the interlock follows the new name");
   bbc_reset();
   ok(rw_cmd(11, 4, 512, 512) == FR_OK && data_ok(512, 512, 9), "a reset reopens it by the new name");
   ok(slot_chdir(12, "") == FR_OK && rename_cmd(12, "GAMES", "GAMES2") == FR_OK,
      "its directory renamed by another slot");
   bbc_reset();
   ok(rw_cmd(11, 4, 0, 512) == FR_OK && data_ok(0, 512, 9), "...is followed: still reopened");
   ok(dir_block(11, 0) == FR_OK && ent_is(2, "ELITE3  SSD"), "...and the slot is in GAMES2");
   ok(rename_cmd(12, "GAMES2", "GAMES") == FR_OK, "(and back)");
   ok(f_rename("/GAMES", "/GAMES2") == FR_OK, "(the directory renamed by the host - not followed)");
   bbc_reset();
   { uint8_t r = rw_cmd(11, 4, 0, 512);
     ok(r == FR_NO_PATH || r == FR_NO_FILE, "...so the reopen fails cleanly");
     if (r != FR_NO_PATH && r != FR_NO_FILE) printf("    got %u\n", r); }
   ok(f_rename("/GAMES2", "/GAMES") == FR_OK, "(and back)");
   {
      unsigned int id = boot_id(12);
      ok(eject_cb && eject_cb(), "an eject");
      ok(rw_cmd(11, 4, 0, 512) == FR_INVALID_OBJECT, "...forgets the kept file");
      unsigned int id2 = boot_id(12);
      ok(id2 != id && id2 != 0u, "...and changes the boot id");
      ok(slot_chdir(11, "GAMES") == FR_OK && dir_block(11, 0) == FR_OK && ent_is(2, "ELITE3  SSD"),
         "the slot starts again from the root");
   }

   puts("== exFAT volume (no short names) ==");
   format(FM_EXFAT);
   make_file("/Revs (Acornsoft).dsd", 204800, 5);
   make_file("/ELITE.SSD", 102400, 6);
   make_file("/a very long name indeed.ssd", 1000, 7);
   bbc_reset();
   ok(slot_chdir(11, "/") == FR_OK && dir_block(11, 0) == FR_OK, "list the root");
   {
      char revs[12] = { 0 }, longn[12] = { 0 };
      int elite = 0;
      for (int k = 0; k < 16 && ent(k)[0]; k++) {
         if (ent_is(k, "ELITE   SSD")) elite = 1;
         if (memcmp(ent(k), "REVS~", 5) == 0 && memcmp(ent(k) + 8, "DSD", 3) == 0) memcpy(revs, ent(k), 11);
         if (memcmp(ent(k), "AVER~", 5) == 0) memcpy(longn, ent(k), 11);
      }
      ok(elite, "a valid 8.3 name is kept as it is");
      ok(revs[0] != 0, "a long name: first four, '~' and a hash");
      ok(longn[0] != 0, "...spaces left out");
      if (revs[0]) {
         char name[13];
         int n = 0;
         for (int i = 0; i < 8 && revs[i] != ' '; i++) name[n++] = revs[i];
         name[n++] = '.';
         for (int i = 8; i < 11 && revs[i] != ' '; i++) name[n++] = revs[i];
         name[n] = '\0';
         ok(f_open_cmd(11, name, 0x80 | FA_READ) == FR_OK, "it opens by that made-up name");
         ok(rw_cmd(11, 4, 102400, 512) == FR_OK && data_ok(102400, 512, 5), "...the right file");
         ok(fat_service_file_in_use("/Revs (Acornsoft).dsd"), "recorded under its real name");
         bbc_reset();
         ok(rw_cmd(11, 4, 0, 512) == FR_OK && data_ok(0, 512, 5), "and reopened after a reset");
         ok(set_attr(11, name, AM_RDO, AM_RDO) == FR_OK, "set attributes by the made-up name");
      }
   }

   printf("%d checks, %d failures\n", checks, fails);
   if (fails == 0) puts("FATFILES TESTS PASSED");
   return fails ? 1 : 0;
}
