/* Regression tests for the Pi1MHz fixes to FatFs R0.16 (ffconf.h
   PI1MHZ_FF_*), from a third-party R0.16 bug report.  Real ff.c with the
   firmware's ffconf.h (plus f_mkfs) on a RAM disk.  Every case marked
   "stock fails" fails on the unmodified R0.16 code (all PI1MHZ_FF_* = 0):
   - f_rename moving a directory into its own subtree (FAT16, FAT32, exFAT)
     returned FR_OK and unlinked the tree                       - stock fails
   - f_expand counted free clusters at the end of the volume and at the
     start as one run: FR_INT_ERR and a cluster lost (FAT16)    - stock fails
   - f_mkdir on exFAT leaked the new directory's cluster when its entry
     could not be added (root directory full, volume full)       - stock fails
   - exFAT mount: 1 << 40 / 1 << 33 (UBSan) on a crafted boot sector, and a
     FAT too small for the cluster count was accepted           - stock fails
   The legitimate operations next to each (moves that must still work, a
   real contiguous run) pass on both. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "diskio.h"

/* ---- RAM disk ---- */

static unsigned char *ram;
static unsigned long nsec;

DSTATUS disk_initialize(BYTE d) { (void)d; return ram ? 0 : STA_NOINIT; }
DSTATUS disk_status(BYTE d)     { (void)d; return ram ? 0 : STA_NOINIT; }

DRESULT disk_read(BYTE d, BYTE *b, LBA_t s, UINT n)
{
   (void)d;
   if (s + n > nsec) return RES_PARERR;
   memcpy(b, ram + (size_t)s * 512, (size_t)n * 512);
   return RES_OK;
}

DRESULT disk_write(BYTE d, const BYTE *b, LBA_t s, UINT n)
{
   (void)d;
   if (s + n > nsec) return RES_PARERR;
   memcpy(ram + (size_t)s * 512, b, (size_t)n * 512);
   return RES_OK;
}

DRESULT disk_ioctl(BYTE d, BYTE c, void *p)
{
   (void)d;
   switch (c) {
   case CTRL_SYNC:        return RES_OK;
   case GET_SECTOR_COUNT: *(LBA_t *)p = nsec; return RES_OK;
   case GET_SECTOR_SIZE:  *(WORD *)p = 512; return RES_OK;
   case GET_BLOCK_SIZE:   *(DWORD *)p = 1; return RES_OK;
   default:               return RES_PARERR;
   }
}

DWORD get_fattime(void) { return 0; }

/* ---- helpers ---- */

static FATFS fs;
static BYTE work[8192];
static BYTE zeros[65536];
static int fails;

#define CHECK(c, msg) do { \
   int ok_ = (c); \
   printf("  %s %s\n", ok_ ? "PASS" : "FAIL", msg); \
   if (!ok_) fails++; \
} while (0)

static int format(BYTE type, unsigned long sectors, DWORD au)
{
   MKFS_PARM opt = { type, 1, 0, 0, au };

   free(ram);
   ram = calloc(sectors, 512);
   nsec = sectors;
   if (f_mkfs("0:", &opt, work, sizeof work) != FR_OK)
      return 1;
   f_mount(NULL, "0:", 0);
   return f_mount(&fs, "0:", 1) != FR_OK;
}

static DWORD free_now(void)          /* in memory, no remount */
{
   DWORD n;
   FATFS *p;
   f_getfree("0:", &n, &p);
   return n;
}

static DWORD free_remounted(void)    /* what is on the disk */
{
   f_mount(NULL, "0:", 0);
   f_mount(&fs, "0:", 1);
   return free_now();
}

static int exists(const char *path)
{
   FILINFO fi;
   return f_stat(path, &fi) == FR_OK;
}

/* ---- f_rename into its own subtree ---- */

static BYTE rn_type;
static unsigned long rn_sectors;

static void rn_setup(void)
{
   format(rn_type, rn_sectors, 0);
   f_mkdir("/a");
   f_mkdir("/a/b");
   f_mkdir("/z");
   f_mkdir("/ab");
   f_chdir("/");
}

static void test_rename(BYTE type, unsigned long sectors, const char *name)
{
   FRESULT r;
   FIL f;

   printf("rename on %s\n", name);
   rn_type = type;
   rn_sectors = sectors;

   rn_setup(); r = f_rename("/a", "/a/b/a");
   CHECK(r == FR_DENIED && exists("/a/b"), "/a -> /a/b/a refused, tree intact");
   rn_setup(); r = f_rename("/a", "/a/a");
   CHECK(r == FR_DENIED && exists("/a/b"), "/a -> /a/a refused");
   rn_setup(); f_chdir("/a/b"); r = f_rename("/a", "../c");
   CHECK(r == FR_DENIED && exists("/a/b"), "/a -> ../c from /a/b refused");
   rn_setup(); f_chdir("/a/b"); r = f_rename("/a", "x");
   CHECK(r == FR_DENIED && exists("/a/b"), "/a -> x (relative, cwd /a/b) refused");

   rn_setup(); r = f_rename("/a", "/z/a");
   CHECK(r == FR_OK && exists("/z/a/b"), "/a -> /z/a allowed");
   rn_setup(); r = f_rename("/a", "/ab/a");
   CHECK(r == FR_OK && exists("/ab/a/b"), "/a -> /ab/a (prefix-named sibling) allowed");
   rn_setup(); r = f_rename("/a/b", "/b2");
   CHECK(r == FR_OK && exists("/b2") && exists("/a"), "/a/b -> /b2 (up to the root) allowed");
   rn_setup(); r = f_rename("/a", "/a2");
   CHECK(r == FR_OK && exists("/a2/b"), "/a -> /a2 in place allowed");
   rn_setup(); f_chdir("/a/b"); r = f_rename("/z", "../z2");
   CHECK(r == FR_OK && exists("/a/z2"), "/z -> ../z2 from /a/b allowed");
   rn_setup();
   f_open(&f, "/file", FA_CREATE_ALWAYS | FA_WRITE);
   f_close(&f);
   r = f_rename("/file", "/a/b/file");
   CHECK(r == FR_OK && exists("/a/b/file"), "file move unaffected");
}

/* ---- f_expand across the wrap ---- */

static void test_expand_wrap(BYTE type, unsigned long sectors, DWORD au, const char *name)
{
   FIL f;
   UINT bw;
   FRESULT r;
   DWORD csz, before, after;

   printf("f_expand wrap on %s\n", name);
   if (format(type, sectors, au)) {
      CHECK(0, "format");
      return;
   }
   csz = (DWORD)fs.csize * 512;

   /* head: the first 4 data clusters; body: everything else.  Freeing the
      last 2 and the head leaves 2 + 4 free clusters, split by the wrap. */
   f_open(&f, "/head", FA_CREATE_ALWAYS | FA_WRITE);
   f_write(&f, zeros, 4 * csz, &bw);
   f_close(&f);
   f_open(&f, "/body", FA_CREATE_ALWAYS | FA_WRITE);
   do {
      r = f_write(&f, zeros, csz, &bw);
   } while (r == FR_OK && bw == csz);
   f_lseek(&f, f_size(&f) - 2 * csz);
   f_truncate(&f);
   f_close(&f);
   f_unlink("/head");

   before = free_now();      /* no remount: the search starts after body */
   f_open(&f, "/big", FA_CREATE_ALWAYS | FA_WRITE);
   r = f_expand(&f, 5 * csz, 1);
   f_close(&f);
   f_unlink("/big");
   after = free_remounted();
   printf("    free %lu, f_expand(5 clusters) = %d, free after %lu\n",
          (unsigned long)before, r, (unsigned long)after);
   CHECK(r == FR_DENIED, "no 5-cluster run: FR_DENIED");
   CHECK(after == before, "no cluster lost");

   f_open(&f, "/ok", FA_CREATE_ALWAYS | FA_WRITE);
   r = f_expand(&f, 4 * csz, 1);
   f_close(&f);
   CHECK(r == FR_OK, "a real 4-cluster run is still found");
   CHECK(free_remounted() == before - 4, "and allocated");
}

/* ---- exFAT f_mkdir unwind ---- */

/* Leaves a recognisable pattern where f_mkdir's locals will be, so a field
   it never sets is the same garbage every run. */
static void poison_stack(BYTE v)
{
   volatile BYTE big[32768];
   memset((void *)big, v, sizeof big);
}

/* Creates empty files /e0000.. in the root.  With stop_at_growth, returns
   the index of the file that made the root directory take a new cluster. */
static int root_fill(int n, int stop_at_growth)
{
   char name[16];
   FIL f;
   int i;

   for (i = 0; i < n; i++) {
      DWORD b = free_now();
      snprintf(name, sizeof name, "/e%04d", i);
      if (f_open(&f, name, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
         return -1;
      f_close(&f);
      if (stop_at_growth && free_now() < b)
         return i;
   }
   return n;
}

static void test_mkdir_unwind(BYTE pattern)
{
   FIL f;
   UINT bw;
   FRESULT r;
   DWORD before, in_mem, after;
   int k, keep_ok;
   static BYTE rb[4096];

   printf("exFAT f_mkdir unwind, stack poisoned with 0x%02X\n", pattern);
   /* How many entries fill the root's first cluster; then the same again,
      stopping one short, so the root is full. */
   format(FM_EXFAT | FM_SFD, 16384, 4096);
   f_mkdir("/d");
   k = root_fill(100000, 1);
   format(FM_EXFAT | FM_SFD, 16384, 4096);
   f_mkdir("/d");
   root_fill(k, 0);
   /* Fill the volume (in /d, not the root), then free exactly one cluster. */
   f_open(&f, "/d/keep", FA_CREATE_ALWAYS | FA_WRITE);
   for (int i = 0; i < 8; i++)
      f_write(&f, zeros, 4096, &bw);
   f_close(&f);
   f_open(&f, "/d/body", FA_CREATE_ALWAYS | FA_WRITE);
   do {
      r = f_write(&f, zeros, 4096, &bw);
   } while (r == FR_OK && bw == 4096);
   f_lseek(&f, f_size(&f) - 4096);
   f_truncate(&f);
   f_close(&f);

   before = free_remounted();
   poison_stack(pattern);
   r = f_mkdir("/newdir");       /* takes the last cluster, cannot extend the root */
   in_mem = free_now();
   f_chmod("/d/keep", 0, 0);     /* f_mkdir's failure path does not sync: any later op does */
   after = free_remounted();
   keep_ok = f_open(&f, "/d/keep", FA_READ) == FR_OK &&
             f_read(&f, rb, sizeof rb, &bw) == FR_OK && bw == sizeof rb;
   f_close(&f);
   printf("    root full at %d files; free before %lu, f_mkdir = %d, free after %lu (in memory %lu)\n",
          k, (unsigned long)before, r, (unsigned long)after, (unsigned long)in_mem);
   CHECK(before == 1 && r == FR_DENIED, "setup: one free cluster, root full, mkdir refused");
   CHECK(after == before && in_mem == before, "the failed mkdir gave back exactly its one cluster");
   CHECK(keep_ok, "other files intact");
}

/* ---- exFAT crafted boot sectors ---- */

static void test_crafted_boot(void)
{
   BYTE save[512];

   puts("exFAT crafted boot sectors");
   format(FM_EXFAT | FM_SFD, 16384, 4096);      /* no partition table: the VBR is sector 0 */
   f_mount(NULL, "0:", 0);
   memcpy(save, ram, sizeof save);

   ram[108] = 40;                               /* BytesPerSectorShift */
   CHECK(f_mount(&fs, "0:", 1) == FR_NO_FILESYSTEM, "BytsPerSecEx = 40 refused");
   f_mount(NULL, "0:", 0);
   memcpy(ram, save, sizeof save);

   ram[109] = 33;                               /* SectorsPerClusterShift */
   CHECK(f_mount(&fs, "0:", 1) == FR_NO_FILESYSTEM, "SecPerClusEx = 33 refused");
   f_mount(NULL, "0:", 0);
   memcpy(ram, save, sizeof save);

   ram[84] = 1; ram[85] = ram[86] = ram[87] = 0;   /* FatLength = 1 sector */
   CHECK(f_mount(&fs, "0:", 1) == FR_NO_FILESYSTEM, "FatSzEx = 1 refused");
   f_mount(NULL, "0:", 0);
   memcpy(ram, save, sizeof save);

   CHECK(f_mount(&fs, "0:", 1) == FR_OK, "the real boot sector still mounts");
}

int main(void)
{
   test_rename(FM_FAT, 8192, "FAT16");
   test_rename(FM_FAT32, 140000, "FAT32");
   test_rename(FM_EXFAT, 65536, "exFAT");
   test_expand_wrap(FM_FAT, 8192, 512, "FAT16");
   test_expand_wrap(FM_EXFAT, 16384, 4096, "exFAT");
   test_mkdir_unwind(0x00);
   test_mkdir_unwind(0xAA);
   test_crafted_boot();
   printf("%s: %d failed\n", fails ? "FATFS TESTS FAILED" : "FATFS TESTS PASSED", fails);
   free(ram);
   return fails != 0;
}
