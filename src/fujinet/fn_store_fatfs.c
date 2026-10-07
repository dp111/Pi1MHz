/* fn_store_fatfs.c - the SD card ("sd0") backend on the Pi, through FatFs
   (fn_store_sd.h).  Main loop only (fujibus_service_poll); never FIQ.

   Beeb_write_protect (docs/user/mmfs.md): every write the Beeb makes is
   silently ignored and reported as success, as MMFS and ADFS writes are.
   A file opened for writing is opened for reading and its writes are
   dropped; a file created is a stand-in that takes writes and holds
   nothing; mkdir and delete do nothing.  VFS volumes (/BeebVFS*) are read
   only media and are treated the same way whatever the setting. */

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "../Pi1MHz.h"
#include "../config.h"
#include "../BeebSCSI/fatfs/ff.h"
#include "fn_store_sd.h"

#define HANDLES 12u
_Static_assert(HANDLES <= FN_SD_HANDLES, "SD handles overlap TNFS handles");

/* The file objects are big and need no zeroing at boot: the open flags say
   which are live (the fat_service pattern). */
NOINIT_SECTION static FIL s_fil[HANDLES];
static bool s_open[HANDLES];
static bool s_discard[HANDLES];        /* writes are dropped (protected) */
static bool s_stand_in[HANDLES];       /* no file behind it (protected create) */

static FIL *fil(fn_handle h)
{
   return (h >= 0 && (unsigned)h < HANDLES && s_open[h] && !s_stand_in[h]) ? &s_fil[h] : NULL;
}

static bool live(fn_handle h)
{
   return h >= 0 && (unsigned)h < HANDLES && s_open[h];
}

/* May the Beeb change this path on the card? */
static bool protected_path(const char *path)
{
   return config_beeb_write_protected() || strncasecmp(path, "/BeebVFS", 8) == 0;
}

fn_handle fn_sd_open(const char *path, fn_open_mode mode)
{
   if (path[0] != '/')
      return FN_NO_HANDLE;
   BYTE fa = mode == FN_OPEN_READ ? FA_READ
           : mode == FN_OPEN_UPDATE ? (BYTE)(FA_READ | FA_WRITE)
           : mode == FN_OPEN_CREATE ? (BYTE)(FA_READ | FA_WRITE | FA_CREATE_ALWAYS)
           : (BYTE)(FA_READ | FA_WRITE | FA_CREATE_NEW);
   bool discard = mode != FN_OPEN_READ && protected_path(path);
   for (unsigned int i = 0; i < HANDLES; i++) {
      if (!s_open[i]) {
         s_discard[i] = discard;
         s_stand_in[i] = false;
         if (f_open(&s_fil[i], path, discard ? FA_READ : fa) != FR_OK) {
            if (!discard || mode == FN_OPEN_UPDATE)
               return FN_NO_HANDLE;
            s_stand_in[i] = true;           /* a create: nothing to read */
         }
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
   if (live(h) && s_discard[h])
      return true;                          /* protected: reported, not written */
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
   if (live(h) && s_stand_in[h]) {
      *size = 0;
      return true;
   }
   if (!f)
      return false;
   *size = (uint32_t)f_size(f);
   return true;
}

bool fn_sd_sync(fn_handle h)
{
   FIL *f = fil(h);
   if (live(h) && s_discard[h])
      return true;
   return f && f_sync(f) == FR_OK;
}

void fn_sd_close(fn_handle h)
{
   FIL *f = fil(h);
   if (f)
      (void)f_close(f);
   if (live(h))
      s_open[h] = false;
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
   if (protected_path(path))
      return true;                          /* protected: reported, not made */
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
   if (path[0] == '/' && protected_path(path))
      return true;                          /* protected: reported, not deleted */
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

/* A walk in progress.  Every lister asks for entries 0, 1, 2, ... in one
   synchronous loop, so the directory stays open between calls and the next
   entry is one f_readdir away: a walk is O(n), not O(n^2).  Any other index
   or path reopens the directory and skips to it, and a new walk always
   starts at 0, so a walk abandoned part way, or a card swapped since, is
   never read from. */
static DIR s_walk_dir;
static bool s_walk_open;
static uint32_t s_walk_next;              /* the index the next read gives */
static char s_walk_path[FN_STORE_MAX_PATH];

static void walk_close(void)
{
   if (s_walk_open)
      (void)f_closedir(&s_walk_dir);
   s_walk_open = false;
}

/* The next entry other than "." and "..": false at the end or on an error,
   and the walk is then closed. */
static bool walk_read(FILINFO *info)
{
   for (;;) {
      if (f_readdir(&s_walk_dir, info) != FR_OK || info->fname[0] == '\0') {
         walk_close();
         return false;
      }
      if (strcmp(info->fname, ".") != 0 && strcmp(info->fname, "..") != 0) {
         s_walk_next++;
         return true;
      }
   }
}

bool fn_sd_dir_entry(const char *path, uint32_t index, fn_dirent *out)
{
   FILINFO info;
   if (path[0] != '/' || strlen(path) >= sizeof s_walk_path)
      return false;
   if (!s_walk_open || index != s_walk_next || strcmp(path, s_walk_path) != 0) {
      walk_close();
      if (f_opendir(&s_walk_dir, path) != FR_OK)
         return false;
      s_walk_open = true;
      s_walk_next = 0;
      snprintf(s_walk_path, sizeof s_walk_path, "%s", path);
      while (s_walk_next < index)
         if (!walk_read(&info))
            return false;
   }
   if (!walk_read(&info))
      return false;
   snprintf(out->name, sizeof out->name, "%s", info.fname);
   out->is_dir = (info.fattrib & AM_DIR) != 0;
   out->size = out->is_dir ? 0 : (uint32_t)info.fsize;
   out->mtime = fat_time(info.fdate, info.ftime);
   return true;
}
