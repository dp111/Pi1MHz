/* fn_store_fatfs.c - the SD card ("sd0") backend on the Pi, through FatFs
   (fn_store_sd.h).  Main loop only (fujibus_service_poll); never FIQ. */

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "../Pi1MHz.h"
#include "../BeebSCSI/fatfs/ff.h"
#include "fn_store_sd.h"

#define HANDLES 12u
_Static_assert(HANDLES <= FN_SD_HANDLES, "SD handles overlap TNFS handles");

/* The file objects are big and need no zeroing at boot: the open flags say
   which are live (the fat_service pattern). */
NOINIT_SECTION static FIL s_fil[HANDLES];
static bool s_open[HANDLES];

static FIL *fil(fn_handle h)
{
   return (h >= 0 && (unsigned)h < HANDLES && s_open[h]) ? &s_fil[h] : NULL;
}

fn_handle fn_sd_open(const char *path, fn_open_mode mode)
{
   if (path[0] != '/')
      return FN_NO_HANDLE;
   BYTE fa = mode == FN_OPEN_READ ? FA_READ
           : mode == FN_OPEN_UPDATE ? (BYTE)(FA_READ | FA_WRITE)
           : mode == FN_OPEN_CREATE ? (BYTE)(FA_READ | FA_WRITE | FA_CREATE_ALWAYS)
           : (BYTE)(FA_READ | FA_WRITE | FA_CREATE_NEW);
   for (unsigned int i = 0; i < HANDLES; i++) {
      if (!s_open[i]) {
         if (f_open(&s_fil[i], path, fa) != FR_OK)
            return FN_NO_HANDLE;
         s_open[i] = true;
         return (fn_handle)i;
      }
   }
   return FN_NO_HANDLE;
}

bool fn_sd_read(fn_handle h, uint32_t offset, void *buf, uint32_t len)
{
   FIL *f = fil(h);
   UINT got = 0;
   return f && f_lseek(f, offset) == FR_OK && f_read(f, buf, len, &got) == FR_OK &&
          got == len;
}

/* A write past the end must leave the gap as zeros, as a POSIX sparse file
   does: FatFs extends a file by allocating clusters without clearing them. */
bool fn_sd_write(fn_handle h, uint32_t offset, const void *buf, uint32_t len)
{
   static const BYTE zeros[256];
   FIL *f = fil(h);
   UINT done = 0;
   if (!f)
      return false;
   for (FSIZE_t pos = f_size(f); pos < offset; pos += done) {
      UINT chunk = (UINT)(offset - pos < sizeof zeros ? offset - pos : sizeof zeros);
      if (f_lseek(f, pos) != FR_OK || f_write(f, zeros, chunk, &done) != FR_OK ||
          done != chunk)
         return false;
   }
   return f_lseek(f, offset) == FR_OK && f_write(f, buf, len, &done) == FR_OK &&
          done == len;
}

bool fn_sd_size(fn_handle h, uint32_t *size)
{
   FIL *f = fil(h);
   if (!f)
      return false;
   *size = (uint32_t)f_size(f);
   return true;
}

bool fn_sd_sync(fn_handle h)
{
   FIL *f = fil(h);
   return f && f_sync(f) == FR_OK;
}

void fn_sd_close(fn_handle h)
{
   FIL *f = fil(h);
   if (f) {
      (void)f_close(f);
      s_open[h] = false;
   }
}

bool fn_sd_is_dir(const char *path)
{
   FILINFO info;
   if (path[0] != '/')
      return false;
   if (path[1] == '\0')
      return true;                      /* f_stat cannot stat the root */
   return f_stat(path, &info) == FR_OK && (info.fattrib & AM_DIR);
}

bool fn_sd_mkdirs(const char *path)
{
   char p[FN_STORE_MAX_PATH];
   if (path[0] != '/' || snprintf(p, sizeof p, "%s", path) >= (int)sizeof p)
      return false;
   for (char *s = p + 1; ; s++) {
      if (*s == '/' || *s == '\0') {
         char c = *s;
         *s = '\0';
         FRESULT r = f_mkdir(p);
         if (r != FR_OK && r != FR_EXIST)
            return false;
         *s = c;
         if (!c)
            return true;
      }
   }
}

bool fn_sd_delete(const char *path)
{
   return path[0] == '/' && f_unlink(path) == FR_OK;
}

/* FAT's local date and time, taken as UTC seconds since 1970. */
static uint32_t fat_time(WORD date, WORD time)
{
   if (date == 0)
      return 0;
   int y = 1980 + (date >> 9), m = (date >> 5) & 15, d = date & 31;
   if (m < 1 || m > 12 || d < 1)
      return 0;
   /* Days from civil (Howard Hinnant). */
   y -= m <= 2;
   int era = y / 400;
   int yoe = y - era * 400;
   int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
   int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
   uint32_t days = (uint32_t)(era * 146097 + doe - 719468);
   return days * 86400u + (uint32_t)(time >> 11) * 3600u +
          (uint32_t)((time >> 5) & 63) * 60u + (uint32_t)(time & 31) * 2u;
}

bool fn_sd_dir_entry(const char *path, uint32_t index, fn_dirent *out)
{
   DIR dir;
   FILINFO info;
   if (path[0] != '/' || f_opendir(&dir, path) != FR_OK)
      return false;
   bool found = false;
   for (uint32_t i = 0; ; ) {
      if (f_readdir(&dir, &info) != FR_OK || info.fname[0] == '\0')
         break;
      if (strcmp(info.fname, ".") == 0 || strcmp(info.fname, "..") == 0)
         continue;
      if (i++ == index) {
         snprintf(out->name, sizeof out->name, "%s", info.fname);
         out->is_dir = (info.fattrib & AM_DIR) != 0;
         out->size = out->is_dir ? 0 : (uint32_t)info.fsize;
         out->mtime = fat_time(info.fdate, info.ftime);
         found = true;
         break;
      }
   }
   (void)f_closedir(&dir);
   return found;
}
