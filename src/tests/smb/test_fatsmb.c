/* Host test of the FAT service on the network share (storage=smb): the real
   fat_service.c and services_emulator.c, driven through the services port as
   the Pi1MHz builds of MMFS and MMFS2 drive it, with the storage root "2:"
   - so every file call goes through fsroute.c and libsmb2 to the Samba that
   run_tests.sh starts.  The card is real FatFs on a RAM disk: the Pi's own
   /Pi1MHz folder must stay there.  Every write is checked against the file
   on the server's disk. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/random.h>
#include <time.h>
#include <arpa/inet.h>

#include "Pi1MHz.h"
#include "services.h"
#include "config.h"
#include "fsroute.h"
#include "smb/smb_client.h"
#include "BeebSCSI/fatfs/ff.h"
#include "BeebSCSI/fatfs/diskio.h"
#include "BeebSCSI/filesystem.h"
#include "watchdog.h"
#include "net_dns.h"
#include "rpi/systimer.h"
#include "rpi/hwrng.h"
#include "wifi/wifi_lwip.h"

bool fat_service_file_in_use(const char *host_path);
void fat_service_init(void);

static int passed, failed;

static void check(const char *what, bool ok, const char *why)
{
   if (ok) {
      passed++;
      printf("PASS %s\n", what);
   } else {
      failed++;
      printf("FAIL %s: %s\n", what, why ? why : "");
   }
}

/* ---- the firmware around the FAT service ---------------------------------- */

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

/* The storage, as filesystem.c decides it: "2:" the share, "" the card. */
static const char *storage_root = "2:";
const char *filesystemStorageRoot(void) { return storage_root; }
bool filesystemStorageOnUsb(void) { return storage_root[0] == '1'; }
bool filesystemStorageOnSmb(void) { return storage_root[0] == '2'; }
bool filesystemStorageOffCard(void) { return storage_root[0] != '\0'; }
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

/* Pi1MHz.cfg, as config_load reads it: the share's logon */
static char cfg_text[512];
uint32_t filesystemReadFile(const char *f, uint8_t **a, unsigned int m)
{
   (void)f; (void)m;
   size_t n = strlen(cfg_text);
   *a = malloc(n);
   memcpy(*a, cfg_text, n);
   return (uint32_t)n;
}

/* ---- the network around the SMB client (as test_smb.c) -------------------- */

void watchdog_feed(void) {}
void audio_pump(void) {}
static uint64_t now_us(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
uint32_t RPI_GetSystemTime(void)   { return (uint32_t)now_us(); }
uint64_t RPI_GetSystemTime64(void) { return now_us(); }
void hwrng_start(void) {}
int hwrng_word(uint32_t *out, uint32_t wait_us)
{
   (void)wait_us;
   return getrandom(out, sizeof *out, 0) == (ssize_t)sizeof *out ? 0 : -1;
}
static wifi_lwip_context_t net_ctx = { true };
volatile Pi1MHz_break_t Pi1MHz_break;
void wifi_lwip_poll(void) {}
bool wifi_lwip_in_poll(void) { return false; }
const wifi_lwip_context_t *wifi_lwip_get_context(void) { return &net_ctx; }
int ipaddr_aton(const char *cp, ip_addr_t *addr)
{
   struct in_addr a;
   if (inet_aton(cp, &a) == 0)
      return 0;
   addr->addr = a.s_addr;
   return 1;
}
char *ipaddr_ntoa(const ip_addr_t *addr)
{
   struct in_addr a = { addr->addr };
   return inet_ntoa(a);
}
err_t net_dns_lookup(const char *name, ip_addr_t *addr, dns_found_callback found, void *arg)
{
   (void)name; (void)addr; (void)found; (void)arg;
   return ERR_ARG;
}

/* ---- the card: a RAM disk, drive 0 only ---------------------------------- */

#define DISK_SECTORS (16u * 1024u * 2u)          /* 16 MB */
static uint8_t *disk;
DSTATUS disk_initialize(BYTE pdrv) { return pdrv ? STA_NOINIT : 0; }
DSTATUS disk_status(BYTE pdrv) { return pdrv ? STA_NOINIT : 0; }
static unsigned raw_reads;
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
   if (pdrv || sector + count > DISK_SECTORS) return RES_PARERR;
   raw_reads++;                         /* a sector of the card read */
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

/* ---- the Beeb's side: command blocks in the top JIM pages (as fatfiles) --- */

#define BUF 0x10000u
static uint32_t cp_of(uint8_t slot) { return DISC_RAM_BASE | 0xFF0000u | ((uint32_t)(0xF0u + slot) << 8); }
static uint8_t *blk(uint8_t slot) { return &Pi1MHz->JIM_ram[cp_of(slot)]; }
static uint8_t go(uint8_t slot)
{
   write_cb[SVC_BASE + 4](TEST_GPIO(SVC_BASE + 4, 0xF0u + slot));
   poll_cb();
   return pi.Memory[SVC_BASE + 4];
}
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint8_t *buf(void) { return &Pi1MHz->JIM_ram[DISC_RAM_BASE + BUF]; }

static uint8_t open_cmd(uint8_t slot, const char *name, uint8_t mode)
{
   memset(blk(slot), 0, 256);
   blk(slot)[0] = 2; blk(slot)[2] = mode;
   strcpy((char *)blk(slot) + 3, name);
   return go(slot);
}
static uint8_t close_cmd(uint8_t slot) { memset(blk(slot), 0, 16); blk(slot)[0] = 3; return go(slot); }
static uint8_t rw_cmd(uint8_t slot, uint8_t cmd, uint32_t offset, uint32_t len)
{
   memset(blk(slot), 0, 16);
   put32(blk(slot), (len << 8) | cmd);
   put32(blk(slot) + 4, BUF);
   put32(blk(slot) + 8, offset);
   return go(slot);
}
static uint8_t name_cmd(uint8_t slot, uint8_t cmd, const char *name)
{ memset(blk(slot), 0, 256); blk(slot)[0] = cmd; strcpy((char *)blk(slot) + 1, name); return go(slot); }
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
static uint8_t buf_cmd(uint8_t slot, uint8_t cmd)
{ memset(blk(slot), 0, 16); blk(slot)[0] = cmd; put32(blk(slot) + 4, BUF); return go(slot); }

static const uint8_t *ent(int k) { return buf() + 32 * k; }
static int ent_is(int k, const char *name11) { return memcmp(ent(k), name11, 11) == 0; }

static void bbc_reset(void)
{
   if (remount_cb) remount_cb();
   fat_service_init();
}

/* ---- files on the share, checked on the server's disk --------------------- */

static const char *share_dir;

static uint8_t pat(uint32_t i, uint8_t seed) { return (uint8_t)((i % 65536u) * 7u + seed); }
static int data_ok(uint32_t offset, uint32_t len, uint8_t seed)
{
   for (uint32_t i = 0; i < len; i++)
      if (buf()[i] != pat(offset + i, seed)) return 0;
   return 1;
}

static void make_file(const char *path, uint32_t size, uint8_t seed)
{
   FIL f; UINT bw;
   static uint8_t data[65536];
   for (uint32_t i = 0; i < sizeof data; i++) data[i] = pat(i, seed);
   assert(fsr_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK);
   for (uint32_t done = 0; done < size; done += bw) {
      UINT n = size - done > sizeof data ? (UINT)sizeof data : (UINT)(size - done);
      assert(fsr_write(&f, data, n, &bw) == FR_OK && bw == n);
   }
   fsr_close(&f);
}

static bool server_read(const char *rel, uint8_t *b, size_t n, long off)
{
   char p[512];
   snprintf(p, sizeof p, "%s/%s", share_dir, rel);
   FILE *f = fopen(p, "rb");
   if (f == NULL)
      return false;
   bool ok = fseek(f, off, SEEK_SET) == 0 && fread(b, 1, n, f) == n;
   fclose(f);
   return ok;
}

static bool server_exists(const char *rel)
{
   char p[512];
   struct stat st;
   snprintf(p, sizeof p, "%s/%s", share_dir, rel);
   return stat(p, &st) == 0;
}

/* ---- the tests ------------------------------------------------------------ */

int main(void)
{
   const char *server = getenv("SMB_TEST_SERVER");
   share_dir = getenv("SMB_TEST_DIR");
   if (server == NULL || share_dir == NULL) {
      printf("SKIP: no SMB_TEST_SERVER / SMB_TEST_DIR\n");
      return 0;
   }
   snprintf(cfg_text, sizeof cfg_text,
            "storage=smb\nsmb_server=%s\nsmb_share=Beeb\nsmb_user=%s\nsmb_password=%s\n",
            server, getenv("SMB_TEST_USER"), getenv("SMB_TEST_PASS"));
   config_load("/Pi1MHz/Pi1MHz.cfg");

   pi.JIM_ram = calloc(1, 32u * 1024u * 1024u);
   pi.JIM_ram_size = 2;                       /* DISC_RAM_BASE == 0 */
   disk = calloc(DISK_SECTORS, 512u);
   assert(pi.JIM_ram && disk);
   {
      static FATFS fs;
      static uint8_t work[FF_MAX_SS * 4];
      const MKFS_PARM opt = { FM_ANY, 0, 0, 0, 0 };
      FIL f; UINT bw;
      assert(f_mkfs("", &opt, work, sizeof work) == FR_OK);
      assert(f_mount(&fs, "", 1) == FR_OK);
      assert(f_mkdir("/Pi1MHz") == FR_OK);
      assert(f_open(&f, "/Pi1MHz/TEST.rom", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK);
      assert(f_write(&f, "CARDROM!", 8, &bw) == FR_OK && bw == 8);
      f_close(&f);
   }
   check("log on to the share", smb_client_connect(), "no logon");

   (void)fsr_mkdir("2:/GAMES");
   make_file("2:/GAMES/Elite.ssd", 102400, 1);
   make_file("2:/Revs (Acornsoft).dsd", 204800, 2);
   make_file("2:/BEEB.MMB", 8192, 4);

   services_emulator_init(0, SVC_BASE);
   fat_service_init();

   puts("-- the volume");
   memset(blk(11), 0, 16); blk(11)[0] = 20;
   check("card type (20): block addressed on the share", go(11) == 1, "not 1");
   {
      memset(blk(3), 0, 16);
      blk(3)[0] = 0; blk(3)[1] = 0;
      put32(blk(3) + 4, BUF); put32(blk(3) + 8, 0); put32(blk(3) + 12, 1);
      unsigned before = raw_reads;
      uint8_t r = go(3);
      char why[64];
      snprintf(why, sizeof why, "result %u, card sectors read %u", r, raw_reads - before);
      check("raw sectors (0) refused: a share has none, and the card is not read in its place",
            r == RES_PARERR && raw_reads == before, why);
   }

   puts("-- MMFS: /BEEB.MMB kept on slot 14");
   check("open /BEEB.MMB kept, read-write", open_cmd(14, "/BEEB.MMB", 0x80 | FA_READ | FA_WRITE) == FR_OK,
         "refused");
   check("read 512 at 512", rw_cmd(14, 4, 512, 512) == FR_OK && data_ok(512, 512, 4), "wrong data");
   check("not a card path: WebDAV on the card is not held up", !fat_service_file_in_use("/BEEB.MMB"),
         "reported busy");
   memset(buf(), 0xC3, 512);
   check("write 512 at 1024", rw_cmd(14, 5, 1024, 512) == FR_OK, "refused");
   bbc_reset();
   {
      uint8_t b[512];
      check("...the bytes are on the server", server_read("BEEB.MMB", b, 512, 1024) &&
            b[0] == 0xC3 && b[511] == 0xC3, "not there");
   }
   check("after a BBC reset the next read reopens it", rw_cmd(14, 4, 2048, 512) == FR_OK &&
         data_ok(2048, 512, 4), "not reopened");
   close_cmd(14);

   puts("-- MMFS2: a slot's directory, directory blocks, kept images");
   check("slot 11 into GAMES (22)", name_cmd(11, 22, "GAMES") == FR_OK, "refused");
   check("a missing directory is refused", name_cmd(11, 22, "NOPE") == FR_NO_PATH, "accepted");
   check("a file is not a directory", name_cmd(11, 22, "Elite.ssd") == FR_NO_PATH, "accepted");
   dir_block(11, 0);
   check("directory block (21): \".\", \"..\", ELITE.SSD", ent_is(0, ".          ") &&
         ent_is(1, "..         ") && ent_is(2, "ELITE   SSD") && ent(2)[28] == 0x00 &&
         ent(2)[29] == 0x90 && ent(2)[30] == 0x01, "wrong entries");
   check("open ELITE.SSD kept", open_cmd(11, "ELITE.SSD", 0x80 | FA_READ | FA_WRITE) == FR_OK, "refused");
   check("read it", rw_cmd(11, 4, 51200, 512) == FR_OK && data_ok(51200, 512, 1), "wrong data");
   memset(buf(), 0x5A, 512);
   check("write a sector", rw_cmd(11, 5, 512, 512) == FR_OK, "refused");
   {
      uint8_t b[512];
      bbc_reset();
      check("...on the server", server_read("GAMES/Elite.ssd", b, 512, 512) && b[0] == 0x5A &&
            b[511] == 0x5A, "not there");
   }
   check("a read after the reset reopens it", rw_cmd(11, 4, 0, 512) == FR_OK && data_ok(0, 512, 1),
         "not reopened");
   memset(blk(11), 0, 16); blk(11)[0] = 6;
   check("size (6)", go(11) == FR_OK && get32(blk(11) + 8) == 102400u, "wrong size");

   check("slot 11 back to the root", name_cmd(11, 22, "..") == FR_OK, "refused");
   dir_block(11, 0);
   {
      char revs[13] = { 0 };
      int games = 0, mmb = 0;
      for (int k = 0; k < 16 && ent(k)[0]; k++) {
         if (ent_is(k, "GAMES      ") && (ent(k)[11] & AM_DIR)) games = 1;
         if (ent_is(k, "BEEB    MMB")) mmb = 1;
         if (memcmp(ent(k), "REVS~", 5) == 0 && memcmp(ent(k) + 8, "DSD", 3) == 0) {
            int n = 0;
            for (int i = 0; i < 8 && ent(k)[i] != ' '; i++) revs[n++] = (char)ent(k)[i];
            revs[n++] = '.';
            memcpy(revs + n, "DSD", 4);
         }
      }
      check("root: GAMES, BEEB.MMB, and a long name made 8.3", games && mmb && revs[0], "missing");
      check("...which opens by that made-up name", revs[0] &&
            open_cmd(12, revs, 0x80 | FA_READ) == FR_OK && rw_cmd(12, 4, 102400, 512) == FR_OK &&
            data_ok(102400, 512, 2), "not opened");
      close_cmd(12);
   }

   puts("-- lock (19), rename (12)");
   name_cmd(11, 22, "GAMES");
   check("lock ELITE.SSD (held open)", set_attr(11, "ELITE.SSD", AM_RDO, AM_RDO) == FR_OK, "refused");
   dir_block(11, 0);
   check("...read-only in the directory block", (ent(2)[11] & AM_RDO) != 0, "not");
   bbc_reset();
   check("a reset reopens a locked image read only", rw_cmd(11, 4, 0, 512) == FR_OK, "refused");
   check("...where a write is refused", rw_cmd(11, 5, 0, 512) == FR_DENIED, "accepted");
   check("unlock", set_attr(11, "ELITE.SSD", AM_RDO, 0) == FR_OK, "refused");
   dir_block(11, 0);
   check("...read-only cleared", (ent(2)[11] & AM_RDO) == 0, "still set");
   check("rename in the slot's directory", rename_cmd(11, "ELITE.SSD", "ELITE2.SSD") == FR_OK, "refused");
   check("...on the server", server_exists("GAMES/ELITE2.SSD") && !server_exists("GAMES/Elite.ssd"),
         "not renamed");
   bbc_reset();
   check("the kept image follows the rename", rw_cmd(11, 4, 1024, 512) == FR_OK &&
         data_ok(1024, 512, 1), "lost");

   puts("-- the shared directory (11, 18) and relative names");
   check("chdir GAMES", name_cmd(3, 11, "GAMES") == FR_OK, "refused");
   buf_cmd(3, 18);
   check("getcwd: /GAMES", !strcmp((char *)buf(), "/GAMES"), (char *)buf());
   check("chdir to a missing directory: FR_NO_PATH", name_cmd(3, 11, "NOPE") == FR_NO_PATH, "accepted");
   check("chdir to a file: FR_NO_PATH", name_cmd(3, 11, "ELITE2.SSD") == FR_NO_PATH, "accepted");
   buf_cmd(3, 18);
   check("...the directory unchanged", !strcmp((char *)buf(), "/GAMES"), (char *)buf());
   check("a relative open uses it (slot 5)", open_cmd(5, "ELITE2.SSD", FA_READ) == FR_OK &&
         rw_cmd(5, 4, 0, 512) == FR_OK && data_ok(0, 512, 1), "not found");
   close_cmd(5);
   check("mkdir (10), relative", name_cmd(3, 10, "NEWDIR") == FR_OK && server_exists("GAMES/NEWDIR"),
         "not made");
   check("unlink (16), relative", name_cmd(3, 16, "NEWDIR") == FR_OK && !server_exists("GAMES/NEWDIR"),
         "not removed");
   check("opendir (7) of the current directory", name_cmd(4, 7, "") == FR_OK, "refused");
   {
      int found = 0, n = 0;
      while (n++ < 20) {
         memset(blk(4), 0, 256); blk(4)[0] = 9;
         if (go(4) != FR_OK) break;
         if (!strcmp((char *)blk(4) + 4, "ELITE2.SSD")) found = 1;
      }
      check("readdir (9) lists it", found, "not listed");
      memset(blk(4), 0, 16); blk(4)[0] = 8;
      go(4);
   }
   check("chdir ..", name_cmd(3, 11, "..") == FR_OK, "refused");
   buf_cmd(3, 18);
   check("getcwd: /", !strcmp((char *)buf(), "/"), (char *)buf());

   puts("-- the Pi's own folder stays on the card");
   name_cmd(3, 11, "GAMES");
   check("Pi1MHz/TEST.rom, relative, with the share's directory in GAMES",
         open_cmd(6, "Pi1MHz/TEST.rom", FA_READ) == FR_OK && rw_cmd(6, 4, 0, 8) == FR_OK &&
         !memcmp(buf(), "CARDROM!", 8), "not the card's");
   close_cmd(6);
   check("/Pi1MHz/TEST.rom, absolute", open_cmd(6, "/Pi1MHz/TEST.rom", FA_READ) == FR_OK &&
         rw_cmd(6, 4, 0, 8) == FR_OK && !memcmp(buf(), "CARDROM!", 8), "not the card's");
   close_cmd(6);
   name_cmd(3, 11, "/");

   puts("-- free space (13)");
   {
      struct statvfs v;
      memset(blk(3), 0, 16); blk(3)[0] = 13;
      uint8_t r = go(3);
      uint64_t got = (uint64_t)get32(blk(3) + 8) * 256u;
      uint64_t host = (statvfs(share_dir, &v) == 0) ? (uint64_t)v.f_bavail * v.f_frsize : 0;
      uint64_t want = host > 0xFFFFFFFFull * 256u ? 0xFFFFFFFFull * 256u : host;
      uint64_t diff = got > want ? got - want : want - got;
      char why[120];
      snprintf(why, sizeof why, "result %u, %llu bytes, server %llu", r,
               (unsigned long long)got, (unsigned long long)host);
      check("the share's free space, in 256-byte units", r == FR_OK && got > 0 &&
            diff <= want / 100u + 65536u, why);
   }

   puts("-- storage changed, eject");
   check("kept again", open_cmd(11, "ELITE2.SSD", 0x80 | FA_READ) == FR_OK, "refused");
   storage_root = "";
   bbc_reset();
   check("the storage is now the card: not reopened there", rw_cmd(11, 4, 0, 512) == FR_NO_FILE,
         "reopened");
   storage_root = "2:";
   check("back on the share: reopened", rw_cmd(11, 4, 0, 512) == FR_OK && data_ok(0, 512, 1), "not");
   check("an eject", eject_cb && eject_cb(), "no eject");
   check("...forgets the kept image", rw_cmd(11, 4, 0, 512) == FR_INVALID_OBJECT, "kept");

   (void)fsr_unlink("2:/GAMES/ELITE2.SSD");
   (void)fsr_unlink("2:/GAMES");
   (void)fsr_unlink("2:/Revs (Acornsoft).dsd");
   (void)fsr_unlink("2:/BEEB.MMB");
   printf("%d passed, %d failed\n", passed, failed);
   return failed ? 1 : 0;
}
