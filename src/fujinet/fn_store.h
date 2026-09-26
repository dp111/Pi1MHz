/* fn_store.h - the storage the FujiNet devices reach, and nothing else.

   A path is a filesystem name plus an absolute path within it ("sd0" +
   "/games/elite.ssd").  The Pi implements this over FatFs in
   fn_store_fatfs.c, called only from the main loop; the host tests
   implement it over a temporary directory.  Handles are small integers so
   the devices never see a FatFs type.

   Every call returns true on success. */
#ifndef FN_STORE_H
#define FN_STORE_H

#include <stdbool.h>
#include <stdint.h>

#define FN_STORE_MAX_PATH   256u   /* including the terminator */
#define FN_STORE_NAME_MAX   255u   /* a directory entry's basename */

typedef int fn_handle;             /* >= 0 when open */
#define FN_NO_HANDLE (-1)

typedef enum {
   FN_OPEN_READ,                   /* existing file, read only */
   FN_OPEN_UPDATE,                 /* existing file, read and write */
   FN_OPEN_CREATE,                 /* create or truncate, read and write */
   FN_OPEN_CREATE_NEW              /* create; fail if it exists */
} fn_open_mode;

/* Is `fs` a filesystem this device has (case-insensitive)?  Sets *canon to
   its canonical lower-case name. */
bool fn_store_known_fs(const char *fs, const char **canon);

fn_handle fn_store_open(const char *fs, const char *path, fn_open_mode mode);
bool fn_store_read (fn_handle h, uint32_t offset, void *buf, uint32_t len);
bool fn_store_write(fn_handle h, uint32_t offset, const void *buf, uint32_t len);
bool fn_store_size (fn_handle h, uint32_t *size);
bool fn_store_sync (fn_handle h);
void fn_store_close(fn_handle h);

bool fn_store_is_dir(const char *fs, const char *path);
bool fn_store_mkdirs(const char *fs, const char *path);   /* every missing level */
bool fn_store_delete(const char *fs, const char *path);

/* Directory listing, one entry at a time: index 0, 1, 2... until false.
   Stateless across calls, so a paged listing costs a rescan per page -
   fine for the few hundred entries a disk folder holds. */
typedef struct {
   char name[FN_STORE_NAME_MAX + 1];
   bool is_dir;
   uint32_t size;
   uint32_t mtime;                 /* seconds since 1970, 0 if unknown */
} fn_dirent;

bool fn_store_dir_entry(const char *fs, const char *path, uint32_t index,
                        fn_dirent *out);

#endif
