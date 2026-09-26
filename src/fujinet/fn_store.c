/* fn_store.c - route fn_store.h calls to the SD card or to TNFS.

   "sd0" goes to fn_store_sd.h's backend and "tnfs" to fn_tnfs.c; handles
   below FN_TNFS_HANDLE_BASE are the SD card's.  Only TNFS ever makes a call
   wait (see "waiting on the network" in fn_store.h). */

#include <strings.h>

#include "fn_store.h"
#include "fn_store_sd.h"
#include "fn_tnfs.h"

static bool s_pending;

static bool is_sd(const char *fs)   { return strcasecmp(fs, "sd0") == 0; }
static bool is_tnfs(const char *fs) { return strcasecmp(fs, "tnfs") == 0; }
static bool tnfs_h(fn_handle h)     { return h >= FN_TNFS_HANDLE_BASE; }

/* Note a TNFS call's "not yet" for fn_store_pending(). */
static void note(void)
{
   if (fn_tnfs_take_pending())
      s_pending = true;
}

bool fn_store_known_fs(const char *fs, const char **canon)
{
   if (is_sd(fs))   { *canon = "sd0";  return true; }
   if (is_tnfs(fs)) { *canon = "tnfs"; return true; }
   return false;
}

fn_handle fn_store_open(const char *fs, const char *path, fn_open_mode mode)
{
   if (is_sd(fs))
      return path[0] == '/' ? fn_sd_open(path, mode) : FN_NO_HANDLE;
   if (!is_tnfs(fs))
      return FN_NO_HANDLE;
   fn_handle h = fn_tnfs_open(path, mode);
   note();
   return h;
}

bool fn_store_read(fn_handle h, uint32_t offset, void *buf, uint32_t len)
{
   if (!tnfs_h(h))
      return fn_sd_read(h, offset, buf, len);
   bool ok = fn_tnfs_read(h, offset, buf, len);
   note();
   return ok;
}

bool fn_store_write(fn_handle h, uint32_t offset, const void *buf, uint32_t len)
{
   if (!tnfs_h(h))
      return fn_sd_write(h, offset, buf, len);
   bool ok = fn_tnfs_write(h, offset, buf, len);
   note();
   return ok;
}

bool fn_store_size(fn_handle h, uint32_t *size)
{
   if (!tnfs_h(h))
      return fn_sd_size(h, size);
   bool ok = fn_tnfs_size(h, size);
   note();
   return ok;
}

/* A TNFS write is on the server when its reply arrives: nothing to flush. */
bool fn_store_sync(fn_handle h)
{
   return tnfs_h(h) ? h != FN_NO_HANDLE : fn_sd_sync(h);
}

void fn_store_close(fn_handle h)
{
   if (tnfs_h(h)) fn_tnfs_close(h);
   else           fn_sd_close(h);
}

bool fn_store_is_dir(const char *fs, const char *path)
{
   if (is_sd(fs))
      return path[0] == '/' && fn_sd_is_dir(path);
   if (!is_tnfs(fs))
      return false;
   bool ok = fn_tnfs_is_dir(path);
   note();
   return ok;
}

/* Directories and deletions are only needed on the SD card (the app store). */
bool fn_store_mkdirs(const char *fs, const char *path)
{
   return is_sd(fs) && path[0] == '/' && fn_sd_mkdirs(path);
}

bool fn_store_delete(const char *fs, const char *path)
{
   return is_sd(fs) && path[0] == '/' && fn_sd_delete(path);
}

bool fn_store_dir_entry(const char *fs, const char *path, uint32_t index, fn_dirent *out)
{
   if (is_sd(fs))
      return path[0] == '/' && fn_sd_dir_entry(path, index, out);
   if (!is_tnfs(fs))
      return false;
   bool ok = fn_tnfs_dir_entry(path, index, out);
   note();
   return ok;
}

bool fn_store_pending(void)        { return s_pending; }
void fn_store_clear_pending(void)  { s_pending = false; }

void fn_store_request_end(void)
{
   s_pending = false;
   fn_tnfs_request_end();
}

void fn_store_request_abort(void)
{
   s_pending = false;
   fn_tnfs_request_abort();
}

void fn_store_poll(uint32_t now_ms)
{
   fn_tnfs_poll(now_ms);
}
