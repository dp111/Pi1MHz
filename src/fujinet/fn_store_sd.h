/* fn_store_sd.h - the SD card ("sd0") backend behind fn_store.c: FatFs on the
   Pi (fn_store_fatfs.c), a temporary directory in the host tests.  Same
   contracts as fn_store.h, without the filesystem name. */
#ifndef FN_STORE_SD_H
#define FN_STORE_SD_H

#include "fn_store.h"

#define FN_SD_HANDLES 16     /* handles 0..15 */

fn_handle fn_sd_open(const char *path, fn_open_mode mode);
bool fn_sd_read (fn_handle h, uint32_t offset, void *buf, uint32_t len);
bool fn_sd_write(fn_handle h, uint32_t offset, const void *buf, uint32_t len);
bool fn_sd_size (fn_handle h, uint32_t *size);
bool fn_sd_sync (fn_handle h);
void fn_sd_close(fn_handle h);
bool fn_sd_is_dir(const char *path);
bool fn_sd_mkdirs(const char *path);
bool fn_sd_delete(const char *path);
bool fn_sd_dir_entry(const char *path, uint32_t index, fn_dirent *out);

#endif
