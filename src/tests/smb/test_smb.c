/* Host test of storage=smb's file layer: fsroute.c, smb/smb_client.c,
   smb/smb_crypto.c and libsmb2 itself, against a real Samba (run_tests.sh
   starts one with signing mandatory, as Windows 11 has it).  Only the
   socket shim is the test's (host_sock_shim.c, real sockets in place of
   lwIP).  Every write is checked against the file on the server's disk. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/random.h>
#include <time.h>
#include <arpa/inet.h>

#include "fsroute.h"
#include "smb/smb_client.h"
#include "config.h"
#include "watchdog.h"
#include "net_dns.h"
#include "rpi/systimer.h"
#include "rpi/hwrng.h"
#include "wifi/wifi.h"
#include "wifi/wifi_lwip.h"
#include "Pi1MHz.h"

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

/* ---- the firmware around it --------------------------------------------- */

static const char *cfg_server, *cfg_share = "Beeb", *cfg_user, *cfg_pass;

const char *config_get(const char *key)
{
   if (strcmp(key, "smb_server") == 0)   return cfg_server;
   if (strcmp(key, "smb_share") == 0)    return cfg_share;
   if (strcmp(key, "smb_user") == 0)     return cfg_user;
   if (strcmp(key, "smb_password") == 0) return cfg_pass;
   return NULL;
}

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

static bool in_poll;
static unsigned net_polls;
/* At power-on there is no address until the WiFi boot ladder has run:
   only wifi_boot() brings one, as on the Pi (SDIO, join, lwIP). */
static wifi_lwip_context_t net_ctx = { false };
static unsigned boot_steps;
void wifi_boot(void)
{
   if (++boot_steps == 3u)
      net_ctx.address_ready = true;
}
volatile Pi1MHz_break_t Pi1MHz_break;
static unsigned break_after;           /* a BBC reset (nRST edge) after this many polls */
void wifi_lwip_poll(void)
{
   net_polls++;
   if (break_after != 0u && --break_after == 0u)
      Pi1MHz_break.edges++;            /* as the nRST IRQ does */
}
bool wifi_lwip_in_poll(void) { return in_poll; }
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
   return ERR_ARG;                     /* the tests use a dotted address */
}

/* FatFs: only the pass-through is checked - nothing on drive 2 reaches it */
static unsigned fatfs_calls;
#define FATFS_STUB(sig) sig { fatfs_calls++; return FR_INT_ERR; }
FATFS_STUB(FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode))
FATFS_STUB(FRESULT f_close(FIL *fp))
FATFS_STUB(FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br))
FATFS_STUB(FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw))
FATFS_STUB(FRESULT f_lseek(FIL *fp, FSIZE_t ofs))
FATFS_STUB(FRESULT f_truncate(FIL *fp))
FATFS_STUB(FRESULT f_sync(FIL *fp))
FATFS_STUB(FRESULT f_expand(FIL *fp, FSIZE_t fsz, BYTE opt))
FATFS_STUB(FRESULT f_opendir(DIR *dp, const TCHAR *path))
FATFS_STUB(FRESULT f_closedir(DIR *dp))
FATFS_STUB(FRESULT f_readdir(DIR *dp, FILINFO *fno))
FATFS_STUB(FRESULT f_mkdir(const TCHAR *path))
FATFS_STUB(FRESULT f_unlink(const TCHAR *path))
FATFS_STUB(FRESULT f_rename(const TCHAR *path_old, const TCHAR *path_new))
FATFS_STUB(FRESULT f_stat(const TCHAR *path, FILINFO *fno))
FATFS_STUB(FRESULT f_chmod(const TCHAR *path, BYTE attr, BYTE mask))
FATFS_STUB(FRESULT f_getfree(const TCHAR *path, DWORD *nclst, FATFS **fatfs))

/* ---- helpers ------------------------------------------------------------ */

static const char *share_dir;          /* the share's folder on this machine */

static bool disk_read(const char *rel, uint8_t *buf, size_t n, long off)
{
   char p[512];
   snprintf(p, sizeof p, "%s/%s", share_dir, rel);
   FILE *f = fopen(p, "rb");
   if (f == NULL)
      return false;
   bool ok = fseek(f, off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
   fclose(f);
   return ok;
}

static long disk_size(const char *rel)
{
   char p[512];
   struct stat st;
   snprintf(p, sizeof p, "%s/%s", share_dir, rel);
   return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

static void pattern(uint8_t *b, size_t n, uint32_t seed)
{
   for (size_t i = 0; i < n; i++)
      b[i] = (uint8_t)((i * 131u + seed * 7u + (i >> 8)) & 0xFFu);
}

/* ---- the tests ---------------------------------------------------------- */

#define IMAGE_BYTES  (10u * 1024u * 1024u)

static void test_image_lifecycle(void)
{
   static char why[200];
   FIL f;
   FRESULT r = fsr_mkdir("2:/BeebSCSI0");
   check("mkdir on the share", r == FR_OK || r == FR_EXIST, "mkdir failed");

   r = fsr_open(&f, "2:/BeebSCSI0/scsi0.dat", FA_READ | FA_WRITE | FA_CREATE_ALWAYS);
   snprintf(why, sizeof why, "FRESULT %d", (int)r);
   check("create an image (FA_CREATE_ALWAYS)", r == FR_OK, why);
   if (r != FR_OK)
      return;

   /* FORMAT: f_expand(..., 1) on the empty file sets its size */
   r = fsr_expand(&f, IMAGE_BYTES, 1);
   snprintf(why, sizeof why, "FRESULT %d, f_size %llu, on disk %ld", (int)r,
            (unsigned long long)f_size(&f), disk_size("BeebSCSI0/scsi0.dat"));
   check("f_expand sets the image's size (FORMAT)",
         r == FR_OK && f_size(&f) == IMAGE_BYTES && disk_size("BeebSCSI0/scsi0.dat") == (long)IMAGE_BYTES, why);

   /* ADFS-style 256-byte sector writes at scattered sectors */
   static const uint32_t sectors[] = { 0, 1, 2, 7, 255, 4096, 40000 };
   bool all = true;
   for (size_t i = 0; i < sizeof sectors / sizeof sectors[0]; i++) {
      uint8_t buf[256];
      UINT bw;
      pattern(buf, sizeof buf, sectors[i]);
      if (fsr_lseek(&f, (FSIZE_t)sectors[i] * 256u) != FR_OK ||
          fsr_write(&f, buf, sizeof buf, &bw) != FR_OK || bw != sizeof buf ||
          f_tell(&f) != (FSIZE_t)(sectors[i] + 1u) * 256u)
         all = false;
   }
   check("256-byte sector writes, f_tell follows", all, "a write or seek failed");

   /* a big write (a 64 KB multi-chunk run) */
   static uint8_t big[200000];
   pattern(big, sizeof big, 99);
   UINT bw = 0;
   r = fsr_lseek(&f, 2000000u);           /* clear of the sectors above */
   if (r == FR_OK)
      r = fsr_write(&f, big, sizeof big, &bw);
   snprintf(why, sizeof why, "FRESULT %d, wrote %u", (int)r, bw);
   check("a 200 KB write in one call", r == FR_OK && bw == sizeof big, why);

   r = fsr_close(&f);
   check("close", r == FR_OK, "close failed");

   /* what the server's disk has */
   all = true;
   for (size_t i = 0; i < sizeof sectors / sizeof sectors[0]; i++) {
      uint8_t want[256], got[256];
      pattern(want, sizeof want, sectors[i]);
      if (!disk_read("BeebSCSI0/scsi0.dat", got, sizeof got, (long)sectors[i] * 256L) ||
          memcmp(want, got, sizeof got) != 0)
         all = false;
   }
   static uint8_t disk_big[sizeof big];
   all = all && disk_read("BeebSCSI0/scsi0.dat", disk_big, sizeof disk_big, 2000000L) &&
         memcmp(disk_big, big, sizeof big) == 0;
   check("the server's file has every byte written", all, "content differs on disk");

   /* read back through the share: small reads (the window), then big */
   r = fsr_open(&f, "2:/BeebSCSI0/scsi0.dat", FA_READ);
   check("reopen read-only", r == FR_OK && f_size(&f) == IMAGE_BYTES, "reopen failed");
   if (r != FR_OK)
      return;
   all = true;
   for (size_t i = 0; i < sizeof sectors / sizeof sectors[0]; i++) {
      uint8_t want[256], got[256];
      UINT br;
      pattern(want, sizeof want, sectors[i]);
      if (fsr_lseek(&f, (FSIZE_t)sectors[i] * 256u) != FR_OK ||
          fsr_read(&f, got, sizeof got, &br) != FR_OK || br != sizeof got ||
          memcmp(want, got, sizeof got) != 0)
         all = false;
   }
   check("256-byte sector reads match", all, "a sector read back differs");

   /* consecutive small reads are served from the window: count the waits */
   unsigned before = net_polls;
   uint8_t s[256];
   UINT br;
   fsr_lseek(&f, 0);
   fsr_read(&f, s, sizeof s, &br);                  /* fills the window */
   unsigned after_first = net_polls;
   for (int i = 0; i < 60; i++)
      fsr_read(&f, s, sizeof s, &br);               /* 60 more sectors, same 32 KB */
   snprintf(why, sizeof why, "polls: first read %u, next 60 reads %u",
            after_first - before, net_polls - after_first);
   check("sectors within the read-ahead window need no network", net_polls == after_first, why);

   static uint8_t rb[sizeof big];
   r = fsr_lseek(&f, 2000000u);
   if (r == FR_OK)
      r = fsr_read(&f, rb, sizeof rb, &br);
   check("a 200 KB read matches", r == FR_OK && br == sizeof rb && memcmp(rb, big, sizeof rb) == 0,
         "big read differs");

   /* a read of many IO chunks, and one cut short by the end of the file */
   static uint8_t huge[700000], disk_huge[sizeof huge];
   r = fsr_lseek(&f, 1700000u);
   if (r == FR_OK)
      r = fsr_read(&f, huge, sizeof huge, &br);
   all = r == FR_OK && br == sizeof huge &&
         disk_read("BeebSCSI0/scsi0.dat", disk_huge, sizeof disk_huge, 1700000L) &&
         memcmp(huge, disk_huge, sizeof huge) == 0;
   check("a 700 KB read (many IO chunks) matches", all, "content differs");
   r = fsr_lseek(&f, IMAGE_BYTES - 100000u);
   if (r == FR_OK)
      r = fsr_read(&f, huge, 300000u, &br);
   all = r == FR_OK && br == 100000u &&
         disk_read("BeebSCSI0/scsi0.dat", disk_huge, 100000u, (long)(IMAGE_BYTES - 100000u)) &&
         memcmp(huge, disk_huge, 100000u) == 0;
   snprintf(why, sizeof why, "FRESULT %d, %u bytes", (int)r, (unsigned)br);
   check("a read past the end stops at it", all, why);

   /* straight-on small reads as the video player does (4 KB pieces) */
   r = fsr_lseek(&f, 1500000u);
   all = r == FR_OK;
   for (UINT done = 0; all && done < 1200000u; done += 4096u) {
      UINT bp;
      uint8_t piece[4096];
      all = fsr_read(&f, piece, sizeof piece, &bp) == FR_OK && bp == sizeof piece &&
            disk_read("BeebSCSI0/scsi0.dat", disk_huge, sizeof piece, 1500000L + (long)done) &&
            memcmp(piece, disk_huge, sizeof piece) == 0;
   }
   check("1.2 MB of straight-on 4 KB reads match", all, "a piece differs");

   /* read-only: a seek past the end is clipped, as FatFs does */
   r = fsr_lseek(&f, IMAGE_BYTES + 5000u);
   check("read-only seek past the end clips to the size", r == FR_OK && f_tell(&f) == IMAGE_BYTES, "not clipped");
   r = fsr_read(&f, rb, 10, &br);
   check("reading at the end gives 0 bytes", r == FR_OK && br == 0, "read past end");
   uint8_t one = 0;
   r = fsr_write(&f, &one, 1, &bw);
   check("a write to a read-only open is refused", r == FR_DENIED, "write allowed");
   fsr_close(&f);
}

static void test_names(void)
{
   static char why[200];
   FILINFO fi;
   FRESULT r = fsr_stat("2:/BeebSCSI0/scsi0.dat", &fi);
   snprintf(why, sizeof why, "FRESULT %d size %llu name %s attr %02x", (int)r,
            (unsigned long long)fi.fsize, fi.fname, fi.fattrib);
   check("stat a file", r == FR_OK && fi.fsize == IMAGE_BYTES && strcmp(fi.fname, "scsi0.dat") == 0 &&
         !(fi.fattrib & AM_DIR) && fi.fdate != 0, why);
   r = fsr_stat("2:/BeebSCSI0", &fi);
   check("stat a directory", r == FR_OK && (fi.fattrib & AM_DIR), "not a directory");
   r = fsr_stat("2:/BeebSCSI0/nothing.dat", &fi);
   check("stat a missing file is FR_NO_FILE", r == FR_NO_FILE, "wrong code");

   /* a cfg next to it, then list the directory */
   FIL f;
   UINT bw;
   const char cfg[] = "Title=Network disc\n";
   r = fsr_open(&f, "2:/BeebSCSI0/scsi0.cfg", FA_WRITE | FA_CREATE_ALWAYS);
   if (r == FR_OK) {
      r = fsr_write(&f, cfg, sizeof cfg - 1, &bw);
      fsr_close(&f);
   }
   check("write a small file", r == FR_OK, "cfg write failed");
   r = fsr_open(&f, "2:/BeebSCSI0/scsi0.cfg", FA_WRITE | FA_OPEN_EXISTING);
   snprintf(why, sizeof why, "FRESULT %d, size %lu", (int)r, (unsigned long)f_size(&f));
   check("a write-only open knows the file's size", r == FR_OK && f_size(&f) == sizeof cfg - 1, why);
   if (r == FR_OK)
      fsr_close(&f);

   static char long_name[400];
   memset(long_name, 'x', sizeof long_name - 1u);
   memcpy(long_name, "2:/", 3);
   unsigned polls = net_polls;
   r = fsr_stat(long_name, &fi);
   check("a path too long for the share is refused, not cut short",
         r == FR_INVALID_NAME && net_polls == polls, "accepted");

   DIR d;
   r = fsr_opendir(&d, "2:/BeebSCSI0");
   bool seen_dat = false, seen_cfg = false, seen_dot = false;
   int n = 0;
   while (r == FR_OK) {
      r = fsr_readdir(&d, &fi);
      if (r != FR_OK || fi.fname[0] == '\0')
         break;
      n++;
      if (strcmp(fi.fname, "scsi0.dat") == 0) seen_dat = true;
      if (strcmp(fi.fname, "scsi0.cfg") == 0) seen_cfg = fi.fsize == sizeof cfg - 1;
      if (fi.fname[0] == '.') seen_dot = true;
   }
   fsr_closedir(&d);
   snprintf(why, sizeof why, "%d entries, dat %d cfg %d dot %d", n, seen_dat, seen_cfg, seen_dot);
   check("readdir lists the files, no dot entries", seen_dat && seen_cfg && !seen_dot, why);

   r = fsr_opendir(&d, "2:/NoSuchDir");
   check("opendir a missing directory is FR_NO_PATH", r == FR_NO_PATH, "wrong code");

   /* safewrite's dance: rename, unlink */
   r = fsr_rename("2:/BeebSCSI0/scsi0.cfg", "2:/BeebSCSI0/scsi0.bak");
   check("rename", r == FR_OK && disk_size("BeebSCSI0/scsi0.bak") == (long)(sizeof cfg - 1), "rename failed");
   r = fsr_rename("2:/BeebSCSI0/scsi0.bak", "/BeebSCSI0/scsi0.cfg");
   check("rename with a driveless target (FatFs style)", r == FR_OK && disk_size("BeebSCSI0/scsi0.cfg") > 0,
         "rename back failed");
   r = fsr_unlink("2:/BeebSCSI0/scsi0.cfg");
   check("unlink a file", r == FR_OK && disk_size("BeebSCSI0/scsi0.cfg") < 0, "still there");
   r = fsr_mkdir("2:/BeebSCSI0/empty");
   if (r == FR_OK)
      r = fsr_unlink("2:/BeebSCSI0/empty");
   check("unlink an empty directory", r == FR_OK, "rmdir failed");
}

/* fsr_chmod and fsr_free_bytes: the FAT service's set-attributes (MMFS2
   *DACCESS) and free space on the share */
static void test_attributes_and_space(void)
{
   static char why[200];
   FIL f, g;
   FILINFO fi;
   UINT bw;
   uint8_t b[512];
   pattern(b, sizeof b, 3);
   FRESULT r = fsr_open(&f, "2:/attr.ssd", FA_WRITE | FA_CREATE_ALWAYS);
   if (r == FR_OK) {
      r = fsr_write(&f, b, sizeof b, &bw);
      fsr_close(&f);
   }
   check("make a file to lock", r == FR_OK, "write failed");

   r = fsr_chmod("2:/attr.ssd", AM_RDO, AM_RDO);
   FRESULT s = fsr_stat("2:/attr.ssd", &fi);
   snprintf(why, sizeof why, "chmod %d stat %d attr %02x", (int)r, (int)s, fi.fattrib);
   check("lock: read-only set", r == FR_OK && s == FR_OK && (fi.fattrib & AM_RDO), why);
   r = fsr_open(&f, "2:/attr.ssd", FA_READ | FA_WRITE);
   snprintf(why, sizeof why, "FRESULT %d", (int)r);
   check("...a read-write open is refused (FR_DENIED)", r == FR_DENIED, why);
   if (r == FR_OK)
      fsr_close(&f);
   r = fsr_open(&f, "2:/attr.ssd", FA_READ);
   check("...a read-only open is not", r == FR_OK, "refused");
   if (r == FR_OK)
      fsr_close(&f);

   r = fsr_chmod("2:/attr.ssd", 0, AM_RDO);
   s = fsr_stat("2:/attr.ssd", &fi);
   snprintf(why, sizeof why, "chmod %d stat %d attr %02x", (int)r, (int)s, fi.fattrib);
   check("unlock: read-only cleared", r == FR_OK && s == FR_OK && !(fi.fattrib & AM_RDO), why);

   r = fsr_open(&g, "2:/attr.ssd", FA_READ | FA_WRITE);
   FRESULT c = fsr_chmod("2:/attr.ssd", AM_RDO, AM_RDO);
   s = fsr_stat("2:/attr.ssd", &fi);
   snprintf(why, sizeof why, "open %d chmod %d attr %02x", (int)r, (int)c, fi.fattrib);
   check("lock a file held open read-write (a mounted disc)", r == FR_OK && c == FR_OK &&
         (fi.fattrib & AM_RDO), why);
   if (r == FR_OK)
      fsr_close(&g);
   (void)fsr_chmod("2:/attr.ssd", 0, AM_RDO);

   r = fsr_chmod("2:/attr.ssd", AM_DIR, AM_DIR);
   s = fsr_stat("2:/attr.ssd", &fi);
   check("the directory bit cannot be set", r == FR_OK && s == FR_OK && !(fi.fattrib & AM_DIR),
         "it was");
   r = fsr_chmod("2:/BeebSCSI0", AM_HID, AM_HID);
   s = fsr_stat("2:/BeebSCSI0", &fi);
   snprintf(why, sizeof why, "chmod %d attr %02x", (int)r, fi.fattrib);
   check("a directory's attributes: still a directory", r == FR_OK && s == FR_OK &&
         (fi.fattrib & AM_DIR) && (fi.fattrib & AM_HID), why);
   (void)fsr_chmod("2:/BeebSCSI0", 0, AM_HID);
   r = fsr_chmod("2:/nothing.ssd", AM_RDO, AM_RDO);
   snprintf(why, sizeof why, "FRESULT %d", (int)r);
   check("chmod of a missing file is FR_NO_FILE", r == FR_NO_FILE, why);
   check("chmod of the root is refused, as FatFs", fsr_chmod("2:/", AM_RDO, AM_RDO) == FR_INVALID_NAME,
         "accepted");

   /* FatFs renames a file that is open; so must the share (an MMFS2 disc
      renamed while it is in a drive) */
   r = fsr_open(&g, "2:/attr.ssd", FA_READ | FA_WRITE);
   c = fsr_rename("2:/attr.ssd", "2:/attr2.ssd");
   UINT br = 0;
   uint8_t rb[16];
   FRESULT rd = (r == FR_OK) ? fsr_read(&g, rb, sizeof rb, &br) : r;
   snprintf(why, sizeof why, "open %d rename %d read %d", (int)r, (int)c, (int)rd);
   check("rename a file held open; the handle still reads it", r == FR_OK && c == FR_OK &&
         rd == FR_OK && br == sizeof rb && memcmp(rb, b, sizeof rb) == 0, why);
   if (r == FR_OK)
      fsr_close(&g);
   (void)fsr_unlink("2:/attr2.ssd");
   (void)fsr_unlink("2:/attr.ssd");

   uint64_t free_bytes = 0;
   struct statvfs v;
   r = fsr_free_bytes("2:/", &free_bytes);
   uint64_t host = (statvfs(share_dir, &v) == 0) ? (uint64_t)v.f_bavail * v.f_frsize : 0;
   uint64_t diff = free_bytes > host ? free_bytes - host : host - free_bytes;
   snprintf(why, sizeof why, "FRESULT %d, %llu bytes, host says %llu", (int)r,
            (unsigned long long)free_bytes, (unsigned long long)host);
   check("free space on the share (within 1% of the server's disc)", r == FR_OK && free_bytes > 0 &&
         diff <= host / 100u + 65536u, why);
}

static void test_routing(void)
{
   FIL f;
   unsigned before = fatfs_calls;
   (void)fsr_open(&f, "/BeebSCSI0/scsi0.dat", FA_READ);
   (void)fsr_open(&f, "1:/BeebSCSI0/scsi0.dat", FA_READ);
   FILINFO fi;
   (void)fsr_stat("0:/x", &fi);
   check("paths not on drive 2 go to FatFs", fatfs_calls == before + 3, "not passed through");

   in_poll = true;
   FRESULT r = fsr_open(&f, "2:/BeebSCSI0/scsi0.dat", FA_READ);
   in_poll = false;
   check("inside the network poll: refused at once (FR_TIMEOUT)", r == FR_TIMEOUT, "not refused");
}

/* The server going away mid-session, a wrong password, a dead server: every
   call fails promptly with FR_NOT_READY (a disc error on the Beeb, never a
   hang), and a BBC reset (fsr_reset) lets the next call log on again. */
static void test_failures(void)
{
   static char why[200];
   FIL f;
   FRESULT r = fsr_open(&f, "2:/BeebSCSI0/scsi0.dat", FA_READ);
   check("open before the server stops", r == FR_OK, "open failed");
   const char *stop = getenv("SMB_TEST_STOP");    /* stops smbd */
   const char *start = getenv("SMB_TEST_START");  /* starts it again */
   if (stop == NULL || start == NULL || r != FR_OK)
      return;
   if (system(stop) != 0)
      printf("     (stop command failed)\n");

   uint8_t buf[256];
   UINT br;
   uint64_t t0 = now_us();
   fsr_lseek(&f, 300000u);                        /* outside the read-ahead window */
   r = fsr_read(&f, buf, sizeof buf, &br);
   uint64_t ms = (now_us() - t0) / 1000u;
   snprintf(why, sizeof why, "FRESULT %d after %llu ms", (int)r, (unsigned long long)ms);
   check("server gone: a read fails with FR_NOT_READY within the 10 s limit",
         r == FR_NOT_READY && ms <= 10500u, why);
   printf("     %s\n", why);

   t0 = now_us();
   FILINFO fi;
   r = fsr_stat("2:/BeebSCSI0/scsi0.dat", &fi);
   ms = (now_us() - t0) / 1000u;
   snprintf(why, sizeof why, "FRESULT %d after %llu ms", (int)r, (unsigned long long)ms);
   check("after the link is lost, the next call fails at once", r == FR_NOT_READY && ms < 50u, why);
   r = fsr_close(&f);
   check("closing a handle of the lost session does not touch the network", r == FR_OK, "close failed");

   if (system(start) != 0)
      printf("     (start command failed)\n");
   t0 = now_us();
   r = fsr_stat("2:/BeebSCSI0/scsi0.dat", &fi);
   ms = (now_us() - t0) / 1000u;
   snprintf(why, sizeof why, "FRESULT %d after %llu ms", (int)r, (unsigned long long)ms);
   check("server back, no BBC reset yet: the link stays lost (no logon)",
         r == FR_NOT_READY && ms < 50u, why);
   fsr_reset();                                   /* a BBC reset */
   r = fsr_stat("2:/BeebSCSI0/scsi0.dat", &fi);
   char st[160];
   smb_client_status(st, sizeof st);
   check("after a reset the next call logs on again", r == FR_OK && fi.fsize == IMAGE_BYTES, st);

   /* a server that stops answering (a pulled cable): the 10 s limit, then
      every call fails at once; the late reply, if any, lands nowhere */
   const char *freeze = getenv("SMB_TEST_FREEZE"), *thaw = getenv("SMB_TEST_THAW");
   if (freeze != NULL && thaw != NULL && fsr_open(&f, "2:/BeebSCSI0/scsi0.dat", FA_READ) == FR_OK) {
      if (system(freeze) != 0)
         printf("     (freeze command failed)\n");
      t0 = now_us();
      fsr_lseek(&f, 600000u);
      r = fsr_read(&f, buf, sizeof buf, &br);
      ms = (now_us() - t0) / 1000u;
      snprintf(why, sizeof why, "FRESULT %d after %llu ms", (int)r, (unsigned long long)ms);
      check("server silent: the read gives up at the 10 s limit with FR_NOT_READY",
            r == FR_NOT_READY && ms >= 9900u && ms <= 11000u, why);
      printf("     %s\n", why);
      if (system(thaw) != 0)
         printf("     (thaw command failed)\n");
      fsr_close(&f);
      fsr_reset();
      r = fsr_stat("2:/BeebSCSI0/scsi0.dat", &fi);
      check("... and after the reset it works again", r == FR_OK, "no reconnect");

      /* a close is sent, not waited for: a BBC reset closes every started
         LUN while the Beeb is held in reset */
      if (fsr_open(&f, "2:/BeebSCSI0/scsi0.dat", FA_READ) == FR_OK) {
         if (system(freeze) != 0)
            printf("     (freeze command failed)\n");
         t0 = now_us();
         r = fsr_close(&f);
         ms = (now_us() - t0) / 1000u;
         if (system(thaw) != 0)
            printf("     (thaw command failed)\n");
         snprintf(why, sizeof why, "FRESULT %d after %llu ms", (int)r, (unsigned long long)ms);
         check("a close does not wait for the server", r == FR_OK && ms < 50u, why);
         r = fsr_stat("2:/BeebSCSI0/scsi0.dat", &fi);
         smb_client_status(st, sizeof st);
         check("... and its late answer leaves the session working", r == FR_OK, st);
      }

      /* a BREAK while a request waits on that silent server: the wait ends
         at once, so the poll loop re-initialises while nRST is still low */
      if (fsr_open(&f, "2:/BeebSCSI0/scsi0.dat", FA_READ) == FR_OK) {
         if (system(freeze) != 0)
            printf("     (freeze command failed)\n");
         break_after = 200u;
         t0 = now_us();
         fsr_lseek(&f, 900000u);
         r = fsr_read(&f, buf, sizeof buf, &br);
         ms = (now_us() - t0) / 1000u;
         break_after = 0u;
         smb_client_status(st, sizeof st);
         snprintf(why, sizeof why, "FRESULT %d after %llu ms: %s", (int)r, (unsigned long long)ms, st);
         check("a BBC reset during a wait ends it at once", r == FR_NOT_READY && ms < 1000u &&
               strstr(st, "BBC reset") != NULL, why);
         printf("     %s\n", why);
         if (system(thaw) != 0)
            printf("     (thaw command failed)\n");
         fsr_close(&f);
         fsr_reset();                             /* the BREAK's re-init */
         r = fsr_stat("2:/BeebSCSI0/scsi0.dat", &fi);
         check("... and the re-init's reset logs on again", r == FR_OK, "no reconnect");
      }
   }

   /* an edge counted but not yet handled (a call after the edge, inside the
      same poll callback): no wait at all until the re-init has run */
   Pi1MHz_break.edges++;
   t0 = now_us();
   r = fsr_stat("2:/BeebSCSI0/scsi0.dat", &fi);
   ms = (now_us() - t0) / 1000u;
   snprintf(why, sizeof why, "FRESULT %d after %llu ms", (int)r, (unsigned long long)ms);
   check("after an unhandled nRST edge a call ends at once", r == FR_NOT_READY && ms < 50u, why);
   fsr_reset();
   r = fsr_stat("2:/BeebSCSI0/scsi0.dat", &fi);
   check("... and works once the re-init has run", r == FR_OK, "no reconnect");

   /* a wrong password: refused at logon, reported, no hang */
   const char *good = cfg_pass;
   cfg_pass = "wrong";
   if (system(stop) == 0 && system(start) == 0) {
      fsr_reset();
      (void)fsr_stat("2:/x", &fi);                /* the old session is dead: log on again */
      fsr_reset();
      t0 = now_us();
      r = fsr_stat("2:/BeebSCSI0/scsi0.dat", &fi);
      ms = (now_us() - t0) / 1000u;
      smb_client_status(st, sizeof st);
      snprintf(why, sizeof why, "FRESULT %d after %llu ms: %s", (int)r, (unsigned long long)ms, st);
      check("a wrong password: FR_NOT_READY, promptly, with the reason on /status",
            r == FR_NOT_READY && ms < 2000u && strstr(st, "not connected") != NULL &&
            strstr(st, "logon") != NULL, why);
      printf("     %s\n", why);
   }
   cfg_pass = good;
   fsr_reset();
}

int main(void)
{
   cfg_server = getenv("SMB_TEST_SERVER");
   cfg_user   = getenv("SMB_TEST_USER");
   cfg_pass   = getenv("SMB_TEST_PASS");
   share_dir  = getenv("SMB_TEST_DIR");
   if (cfg_server == NULL || share_dir == NULL) {
      printf("SKIP: no SMB_TEST_SERVER / SMB_TEST_DIR\n");
      return 0;
   }
   uint64_t t0 = now_us();
   bool up = smb_client_connect();
   char st[160];
   smb_client_status(st, sizeof st);
   check("log on to the share (signing mandatory)", up, st);
   check("at power-on the wait steps the WiFi boot until there is an address",
         net_ctx.address_ready, "wifi_boot never called");
   printf("     logon took %llu ms; %s\n", (unsigned long long)((now_us() - t0) / 1000u), st);
   if (!up) {
      printf("%d passed, %d failed\n", passed, failed);
      return 1;
   }
   test_image_lifecycle();
   test_names();
   test_attributes_and_space();
   test_routing();

   test_failures();
   printf("%d passed, %d failed\n", passed, failed);
   return failed ? 1 : 0;
}
