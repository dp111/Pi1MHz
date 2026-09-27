/* fn_store_host.c - the SD card backend (fn_store_sd.h) over a POSIX
   directory, for the host tests: "sd0" is fn_store_host_root()'s directory. */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fn_store_sd.h"

static char s_root[512];
static int s_fd[16];
static bool s_init;

void fn_store_host_root(const char *root)
{
   snprintf(s_root, sizeof s_root, "%s", root);
   for (int i = 0; i < 16; i++) s_fd[i] = -1;
   s_init = true;
}

int fn_store_host_open_count(void)
{
   int n = 0;
   for (int i = 0; i < 16; i++) n += s_fd[i] >= 0;
   return n;
}

static bool real(const char *path, char *out)
{
   if (!s_init || path[0] != '/') return false;
   return snprintf(out, 1024, "%s%s", s_root, path) < 1024;
}

fn_handle fn_sd_open(const char *path, fn_open_mode mode)
{
   char p[1024];
   if (!real(path, p)) return FN_NO_HANDLE;
   struct stat st;
   if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) return FN_NO_HANDLE;
   int flags = mode == FN_OPEN_READ ? O_RDONLY
             : mode == FN_OPEN_UPDATE ? O_RDWR
             : mode == FN_OPEN_CREATE ? O_RDWR | O_CREAT | O_TRUNC
             : O_RDWR | O_CREAT | O_EXCL;
   for (int i = 0; i < 16; i++) {
      if (s_fd[i] < 0) {
         int fd = open(p, flags, 0644);
         if (fd < 0) return FN_NO_HANDLE;
         s_fd[i] = fd;
         return i;
      }
   }
   return FN_NO_HANDLE;
}

static int fd_of(fn_handle h) { return (h >= 0 && h < 16) ? s_fd[h] : -1; }

bool fn_sd_read(fn_handle h, uint32_t off, void *buf, uint32_t len)
{
   int fd = fd_of(h);
   return fd >= 0 && pread(fd, buf, len, off) == (ssize_t)len;
}

bool fn_sd_write(fn_handle h, uint32_t off, const void *buf, uint32_t len)
{
   int fd = fd_of(h);
   return fd >= 0 && pwrite(fd, buf, len, off) == (ssize_t)len;
}

bool fn_sd_size(fn_handle h, uint32_t *size)
{
   struct stat st;
   int fd = fd_of(h);
   if (fd < 0 || fstat(fd, &st) != 0) return false;
   *size = (uint32_t)st.st_size;
   return true;
}

bool fn_sd_sync(fn_handle h) { return fd_of(h) >= 0; }

void fn_sd_close(fn_handle h)
{
   int fd = fd_of(h);
   if (fd >= 0) { close(fd); s_fd[h] = -1; }
}

bool fn_sd_is_dir(const char *path)
{
   char p[1024];
   struct stat st;
   return real(path, p) && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

bool fn_sd_mkdirs(const char *path)
{
   char p[1024];
   if (!real(path, p)) return false;
   for (char *s = p + strlen(s_root) + 1; ; s++) {
      if (*s == '/' || *s == '\0') {
         char c = *s;
         *s = '\0';
         if (mkdir(p, 0755) != 0 && errno != EEXIST) return false;
         *s = c;
         if (!c) break;
      }
   }
   return true;
}

bool fn_sd_delete(const char *path)
{
   char p[1024];
   return real(path, p) && unlink(p) == 0;
}

/* readdir order is not stable across calls on every filesystem, so list
   sorted: the device must not depend on the order either way. */
static int cmp(const struct dirent **a, const struct dirent **b)
{
   return strcmp((*b)->d_name, (*a)->d_name);   /* reversed on purpose */
}
static int keep(const struct dirent *d)
{
   return strcmp(d->d_name, ".") && strcmp(d->d_name, "..");
}

bool fn_sd_dir_entry(const char *path, uint32_t index, fn_dirent *out)
{
   char p[1024], f[1400];
   struct dirent **list;
   if (!real(path, p)) return false;
   int n = scandir(p, &list, keep, cmp);
   if (n < 0) return false;
   bool ok = index < (uint32_t)n;
   if (ok) {
      struct stat st;
      snprintf(out->name, sizeof out->name, "%s", list[index]->d_name);
      snprintf(f, sizeof f, "%s/%s", p, list[index]->d_name);
      ok = stat(f, &st) == 0;
      out->is_dir = S_ISDIR(st.st_mode);
      out->size = out->is_dir ? 0 : (uint32_t)st.st_size;
      out->mtime = (uint32_t)st.st_mtime;
   }
   for (int i = 0; i < n; i++) free(list[i]);
   free(list);
   return ok;
}
