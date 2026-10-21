/* fsroute.c - the FatFs calls, routed by path to FatFs or to the SMB share.
   See fsroute.h.  The share's half: each call starts one libsmb2
   operation and waits for it in smb_client_wait(); one operation at a time
   (the firmware's file calls are synchronous), so its state is static. */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fsroute.h"
#include "smb/smb_client.h"
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>

/* The mark on a FIL or DIR that lives on the share.  fs_type 0, so FatFs's
   validate() refuses it (FR_INVALID_OBJECT) if one ever reaches FatFs. */
static FATFS net_fs;

/* LUN images, the VFS side, its video, cfgs; the FAT service's 16 handles
   (MMFS), FujiNet's 12 and a Music 5000 recording */
#define NET_FILES      48
#define NET_DIRS       24              /* the FAT service's 16, FujiNet's walk, lookups */
/* Measured 2026-10-16 (video over WiFi from Windows): a window grown to
   256 KB, and 4 READs in flight, each made playback SLOWER than this. */
#define CACHE_SIZE     32768u          /* read-ahead window per file */
#define IO_CHUNK       65536u          /* one READ/WRITE: a single credit */

typedef struct {
   FIL              *fp;               /* NULL = free */
   struct smb2fh    *fh;
   uint32_t          session;
   uint8_t          *cache;            /* read-ahead window, allocated on first use */
   FSIZE_t           cache_off;
   UINT              cache_len;
} net_file_t;

typedef struct {
   DIR              *dp;
   struct smb2dir   *dir;
   uint32_t          session;
} net_dir_t;

static net_file_t files[NET_FILES];
static net_dir_t  dirs[NET_DIRS];

/* ---- one operation ------------------------------------------------------- */

static struct {
   volatile bool done;
   int           status;
   void         *data;
   uintptr_t     gen;                  /* a late callback of an older op is ignored */
} op;

static void forget_cb(struct smb2_context *smb2, int status, void *data, void *arg)
{
   (void)smb2; (void)status; (void)data; (void)arg;
}

static void op_cb(struct smb2_context *smb2, int status, void *data, void *arg)
{
   (void)smb2;
   if ((uintptr_t)arg != op.gen)
      return;
   op.status = status;
   op.data = data;
   op.done = true;
}

static void *op_start(void)
{
   op.gen++;
   op.done = false;
   op.status = -EIO;
   op.data = NULL;
   return (void *)op.gen;
}

#define LINK_LOST  (-100000)           /* not an errno: the wait failed */

/* rc is what the *_async call returned: non-zero = never started */
static int op_finish(int rc)
{
   if (rc != 0)
      return rc < 0 ? rc : -EIO;
   if (!smb_client_wait(&op.done))
      return LINK_LOST;
   return op.status;
}

static FRESULT fr_of(int status)
{
   if (status >= 0)        return FR_OK;
   switch (status) {
   case LINK_LOST:         return FR_NOT_READY;
   case -ENOENT:           return FR_NO_FILE;
   case -ENOTDIR:          return FR_NO_PATH;
   case -EEXIST:           return FR_EXIST;
   case -EACCES:
   case -EPERM:
   case -EBUSY:
   case -ENOSPC:
   case -ENOTEMPTY:
   case -EISDIR:           return FR_DENIED;
   case -EROFS:            return FR_WRITE_PROTECTED;
   case -EINVAL:
   case -ENAMETOOLONG:     return FR_INVALID_NAME;
   default:                return FR_DISK_ERR;
   }
}

/* Usable for a new operation: connected (connecting if need be) and not
   inside the network poll. */
static FRESULT ready(void)
{
   if (!smb_client_can_wait())
      return FR_TIMEOUT;
   return smb_client_connect() ? FR_OK : FR_NOT_READY;
}

/* ---- paths -------------------------------------------------------------- */

bool fsr_is_net_path(const TCHAR *path)
{
   return path != NULL && path[0] == '2' && path[1] == ':';
}

/* "2:/BeebSCSI0/scsi0.dat" -> "BeebSCSI0\scsi0.dat" (share-relative).  A
   path with no drive (FatFs's rename target) is taken as on the share.
   NULL when it does not fit: cut short it would name another file. */
#define PATH_MAX_LEN   255u
static char path_a[PATH_MAX_LEN + 1u], path_b[PATH_MAX_LEN + 1u];

static const char *share_path(const TCHAR *path, char *buf)
{
   if (path == NULL) {
      buf[0] = '\0';
      return buf;
   }
   if (fsr_is_net_path(path))
      path += 2;
   while (*path == '/' || *path == '\\')
      path++;
   size_t n = 0;
   for (; *path != '\0'; path++) {
      if (n == PATH_MAX_LEN)
         return NULL;
      buf[n++] = (*path == '/') ? '\\' : *path;
   }
   while (n > 0u && buf[n - 1u] == '\\')
      n--;                             /* "2:/dir/" names the directory */
   buf[n] = '\0';
   return buf;
}

/* ---- FILINFO from an SMB stat -------------------------------------------- */

static void fill_info(FILINFO *fno, const char *name, const struct smb2_stat_64 *st)
{
   fno->fsize = (FSIZE_t)st->smb2_size;
   /* FatFs's AM_RDO/HID/SYS/DIR/ARC are the Windows attribute bits */
   fno->fattrib = (BYTE)(st->smb2_attributes & (AM_RDO | AM_HID | AM_SYS | AM_DIR | AM_ARC));
   if (st->smb2_type == SMB2_TYPE_DIRECTORY)
      fno->fattrib |= AM_DIR;
   time_t t = (time_t)st->smb2_mtime;
   struct tm tm;
   if (gmtime_r(&t, &tm) != NULL && tm.tm_year >= 80) {
      fno->fdate = (WORD)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
      fno->ftime = (WORD)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
   } else {
      fno->fdate = (WORD)((0 << 9) | (1 << 5) | 1);   /* 1980-01-01 */
      fno->ftime = 0;
   }
   if (name != NULL) {
      strncpy(fno->fname, name, sizeof fno->fname - 1u);
      fno->fname[sizeof fno->fname - 1u] = '\0';
   } else {
      fno->fname[0] = '\0';
   }
   fno->altname[0] = '\0';
}

/* ---- files -------------------------------------------------------------- */

static net_file_t *net_file(FIL *fp)
{
   if (fp == NULL || fp->obj.fs != &net_fs)
      return NULL;
   WORD i = fp->obj.id;
   if (i == 0u || i > NET_FILES || files[i - 1u].fp != fp)
      return NULL;
   return &files[i - 1u];
}

/* A handle from an earlier session is dead: its connection is gone. */
static bool live(const net_file_t *f)
{
   return f->session != 0u && f->session == smb_client_session();
}

/* What every call on an open share file checks first: its connection is
   the one in force, its mode allows the call (need 0: any), and the caller
   can wait for the network. */
static FRESULT usable(const FIL *fp, const net_file_t *f, BYTE need)
{
   if (!live(f))
      return FR_NOT_READY;
   if (need != 0u && !(fp->flag & need))
      return FR_DENIED;
   if (!smb_client_can_wait())
      return FR_TIMEOUT;
   return FR_OK;
}

static void release(net_file_t *f)
{
   free(f->cache);
   if (f->fp != NULL) {
      f->fp->obj.fs = NULL;
      f->fp->obj.id = 0;
   }
   memset(f, 0, sizeof *f);
}

/* Close a share file and free its slot.  The CLOSE is sent, not waited
   for: every write was answered when it was made, and the server takes
   requests in order, so a reopen cannot overtake it.  A BBC reset closes
   each started LUN, and waiting a round trip per close there held the
   Beeb's re-init up past its reset (1.4 s measured, 2026-10-16); inside
   the poll a wait is not allowed at all. */
static void close_file(net_file_t *f)
{
   if (live(f)) {
      (void)smb2_close_async(smb_client_context(), f->fh, forget_cb, NULL);
      smb_client_kick();
   }
   release(f);
}

static struct smb2_stat_64 st_buf;

/* The CREATE of an open: its reply's handle and size, for the op it ends. */
static smb2_file_id open_fid;
static uint64_t open_size;

static void open_cb(struct smb2_context *smb2, int status, void *data, void *arg)
{
   if ((uintptr_t)arg != op.gen)
      return;
   if (status == SMB2_STATUS_SUCCESS) {
      const struct smb2_create_reply *rep = (const struct smb2_create_reply *)data;
      memcpy(open_fid, rep->file_id, SMB2_FD_SIZE);
      open_size = rep->end_of_file;
   }
   op_cb(smb2, status == SMB2_STATUS_SUCCESS ? 0 : -nterror_to_errno((uint32_t)status), NULL, arg);
}

/* The CREATE is this file's own, not smb2_open's: that one shares the file
   for reading and writing only, so while the Beeb held an image open (a
   disc in an MMFS2 drive) the server refused to rename it - which FatFs
   does.  Sharing it for delete too allows that.  The size comes with the
   reply. */
FRESULT fsr_open(FIL *fp, const TCHAR *path, BYTE mode)
{
   /* FatFs lets an open FIL be opened again, and the FAT service does
      that.  A share file's slot (its server handle and read cache) would
      then be lost with nothing to free it, so close it first - before
      either kind of open, which each overwrite the FIL. */
   for (int i = 0; i < NET_FILES; i++)
      if (fp != NULL && files[i].fp == fp)
         close_file(&files[i]);
   if (!fsr_is_net_path(path))
      return f_open(fp, path, mode);
   if (fp == NULL)
      return FR_INVALID_OBJECT;
   fp->obj.fs = NULL;
   const char *p = share_path(path, path_a);
   if (p == NULL)
      return FR_INVALID_NAME;
   FRESULT r = ready();
   if (r != FR_OK)
      return r;
   net_file_t *f = NULL;
   for (int i = 0; i < NET_FILES; i++)
      if (files[i].fp == NULL) {
         f = &files[i];
         break;
      }
   if (f == NULL)
      return FR_TOO_MANY_OPEN_FILES;

   struct smb2_create_request cr;
   memset(&cr, 0, sizeof cr);
   cr.requested_oplock_level = SMB2_OPLOCK_LEVEL_NONE;
   cr.impersonation_level = SMB2_IMPERSONATION_IMPERSONATION;
   cr.desired_access = SMB2_FILE_READ_ATTRIBUTES;
   if (mode & FA_READ)
      cr.desired_access |= SMB2_FILE_READ_DATA | SMB2_FILE_READ_EA;
   if (mode & FA_WRITE)
      cr.desired_access |= SMB2_FILE_WRITE_DATA | SMB2_FILE_WRITE_EA | SMB2_FILE_WRITE_ATTRIBUTES;
   cr.share_access = SMB2_FILE_SHARE_READ | SMB2_FILE_SHARE_WRITE | SMB2_FILE_SHARE_DELETE;
   cr.create_disposition = (mode & FA_CREATE_NEW) ? SMB2_FILE_CREATE
                         : (mode & FA_CREATE_ALWAYS) ? SMB2_FILE_OVERWRITE_IF
                         : (mode & (FA_OPEN_ALWAYS | FA_OPEN_APPEND)) ? SMB2_FILE_OPEN_IF
                         : SMB2_FILE_OPEN;
   cr.create_options = SMB2_FILE_NON_DIRECTORY_FILE;
   cr.name = p;

   struct smb2_context *smb2 = smb_client_context();
   struct smb2_pdu *pdu = smb2_cmd_create_async(smb2, &cr, open_cb, op_start());
   if (pdu == NULL)
      return FR_NOT_ENOUGH_CORE;
   smb2_queue_pdu(smb2, pdu);
   int s = op_finish(0);
   if (s < 0)
      return fr_of(s);
   struct smb2fh *fh = smb2_fh_from_file_id(smb2, &open_fid);
   if (fh == NULL) {
      struct smb2_close_request cl;
      memset(&cl, 0, sizeof cl);
      memcpy(cl.file_id, open_fid, SMB2_FD_SIZE);
      pdu = smb2_cmd_close_async(smb2, &cl, forget_cb, NULL);
      if (pdu != NULL) {
         smb2_queue_pdu(smb2, pdu);
         smb_client_kick();
      }
      return FR_NOT_ENOUGH_CORE;
   }

   memset(f, 0, sizeof *f);
   f->fp = fp;
   f->fh = fh;
   f->session = smb_client_session();
   fp->obj.fs = &net_fs;
   fp->obj.id = (WORD)((f - files) + 1);
   fp->obj.objsize = (FSIZE_t)open_size;
   fp->flag = (BYTE)(mode & (FA_READ | FA_WRITE));
   fp->err = 0;
   fp->fptr = (mode & FA_OPEN_APPEND) ? fp->obj.objsize : 0;
#if FF_USE_FASTSEEK
   fp->cltbl = NULL;
#endif
   return FR_OK;
}

FRESULT fsr_close(FIL *fp)
{
   net_file_t *f = net_file(fp);
   if (f == NULL)
      return (fp != NULL && fp->obj.fs == &net_fs) ? FR_INVALID_OBJECT : f_close(fp);
   close_file(f);
   return FR_OK;
}

/* Read [off, off+n) into buf: chunks of IO_CHUNK, short at end of file. */
static int pread_all(net_file_t *f, uint8_t *buf, FSIZE_t off, UINT n, UINT *got)
{
   *got = 0;
   while (n > 0u) {
      uint32_t chunk = n > IO_CHUNK ? IO_CHUNK : n;
      int s = op_finish(smb2_pread_async(smb_client_context(), f->fh, buf, chunk,
                                         (uint64_t)off, op_cb, op_start()));
      if (s < 0)
         return s;
      if (s == 0)
         break;                        /* end of file */
      *got += (UINT)s;
      buf += s;
      off += (FSIZE_t)s;
      n -= (UINT)s;
   }
   return 0;
}

FRESULT fsr_read(FIL *fp, void *buff, UINT btr, UINT *br)
{
   net_file_t *f = net_file(fp);
   if (f == NULL)
      return f_read(fp, buff, btr, br);
   *br = 0;
   FRESULT r = usable(fp, f, FA_READ);
   if (r != FR_OK)
      return r;
   FSIZE_t left = fp->obj.objsize > fp->fptr ? fp->obj.objsize - fp->fptr : 0;
   if ((FSIZE_t)btr > left)
      btr = (UINT)left;
   uint8_t *dst = (uint8_t *)buff;
   while (btr > 0u) {
      /* from the window */
      if (f->cache_len != 0u && fp->fptr >= f->cache_off &&
          fp->fptr < f->cache_off + f->cache_len) {
         UINT at = (UINT)(fp->fptr - f->cache_off);
         UINT n = f->cache_len - at;
         if (n > btr)
            n = btr;
         memcpy(dst, f->cache + at, n);
         dst += n; btr -= n; *br += n; fp->fptr += n;
         continue;
      }
      /* a big read goes straight to the caller's buffer */
      if (btr >= CACHE_SIZE) {
         UINT got;
         int s = pread_all(f, dst, fp->fptr, btr, &got);
         *br += got; fp->fptr += got;
         if (s < 0)
            return fr_of(s);
         break;
      }
      /* a small one fills the window first */
      if (f->cache == NULL && (f->cache = malloc(CACHE_SIZE)) == NULL) {
         UINT got;
         int s = pread_all(f, dst, fp->fptr, btr, &got);
         *br += got; fp->fptr += got;
         return fr_of(s);
      }
      UINT got;
      FSIZE_t want = fp->obj.objsize - fp->fptr;
      int s = pread_all(f, f->cache, fp->fptr,
                        (UINT)(want < CACHE_SIZE ? want : CACHE_SIZE), &got);
      f->cache_off = fp->fptr;
      f->cache_len = (s < 0) ? 0u : got;
      if (s < 0)
         return fr_of(s);
      if (got == 0u)
         break;                        /* shorter than its size said */
   }
   return FR_OK;
}

FRESULT fsr_write(FIL *fp, const void *buff, UINT btw, UINT *bw)
{
   net_file_t *f = net_file(fp);
   if (f == NULL)
      return f_write(fp, buff, btw, bw);
   *bw = 0;
   FRESULT r = usable(fp, f, FA_WRITE);
   if (r != FR_OK)
      return r;
   /* the window no longer matches the file where they overlap */
   if (f->cache_len != 0u && fp->fptr < f->cache_off + f->cache_len &&
       fp->fptr + btw > f->cache_off)
      f->cache_len = 0u;
   const uint8_t *src = (const uint8_t *)buff;
   while (btw > 0u) {
      uint32_t chunk = btw > IO_CHUNK ? IO_CHUNK : btw;
      int s = op_finish(smb2_pwrite_async(smb_client_context(), f->fh, src, chunk,
                                          (uint64_t)fp->fptr, op_cb, op_start()));
      if (s < 0)
         return fr_of(s);
      if (s == 0)
         return FR_DENIED;             /* the share is full */
      src += s; btw -= (UINT)s; *bw += (UINT)s; fp->fptr += (FSIZE_t)s;
      if (fp->fptr > fp->obj.objsize)
         fp->obj.objsize = fp->fptr;
   }
   return FR_OK;
}

static int set_size(net_file_t *f, FSIZE_t size)
{
   int s = op_finish(smb2_ftruncate_async(smb_client_context(), f->fh, (uint64_t)size,
                                          op_cb, op_start()));
   if (s >= 0) {
      f->fp->obj.objsize = size;
      if (f->cache_len != 0u && f->cache_off + f->cache_len > size)
         f->cache_len = 0u;
   }
   return s;
}

FRESULT fsr_lseek(FIL *fp, FSIZE_t ofs)
{
   net_file_t *f = net_file(fp);
   if (f == NULL)
      return f_lseek(fp, ofs);
   if (!live(f))
      return FR_NOT_READY;
#if FF_USE_FASTSEEK
   if (ofs == CREATE_LINKMAP)
      return FR_OK;                    /* nothing to map: seeking is free here */
#endif
   if (ofs > fp->obj.objsize) {
      if (!(fp->flag & FA_WRITE))
         ofs = fp->obj.objsize;        /* as FatFs: clipped when read-only */
      else {
         if (!smb_client_can_wait())
            return FR_TIMEOUT;
         int s = set_size(f, ofs);     /* as FatFs: the file grows */
         if (s < 0)
            return fr_of(s);
      }
   }
   fp->fptr = ofs;
   return FR_OK;
}

FRESULT fsr_truncate(FIL *fp)
{
   net_file_t *f = net_file(fp);
   if (f == NULL)
      return f_truncate(fp);
   FRESULT r = usable(fp, f, FA_WRITE);
   if (r != FR_OK)
      return r;
   return fr_of(set_size(f, fp->fptr));
}

FRESULT fsr_sync(FIL *fp)
{
   net_file_t *f = net_file(fp);
   if (f == NULL)
      return f_sync(fp);
   if (!live(f))
      return FR_NOT_READY;
   if (!(fp->flag & FA_WRITE))
      return FR_OK;
   if (!smb_client_can_wait())
      return FR_TIMEOUT;
   return fr_of(op_finish(smb2_fsync_async(smb_client_context(), f->fh, op_cb, op_start())));
}

/* FatFs: allocate fsz bytes to an empty file; with opt=1 now, and the file
   is then that size.  On a share: set the size (opt=0 has nothing to
   prepare). */
FRESULT fsr_expand(FIL *fp, FSIZE_t fsz, BYTE opt)
{
   net_file_t *f = net_file(fp);
   if (f == NULL)
      return f_expand(fp, fsz, opt);
   if (!live(f))
      return FR_NOT_READY;
   if (fsz == 0u || fp->obj.objsize != 0u || !(fp->flag & FA_WRITE))
      return FR_DENIED;
   if (!opt)
      return FR_OK;
   if (!smb_client_can_wait())
      return FR_TIMEOUT;
   return fr_of(set_size(f, fsz));
}

/* ---- directories -------------------------------------------------------- */

static net_dir_t *net_dir(DIR *dp)
{
   if (dp == NULL || dp->obj.fs != &net_fs)
      return NULL;
   WORD i = dp->obj.id;
   if (i == 0u || i > NET_DIRS || dirs[i - 1u].dp != dp)
      return NULL;
   return &dirs[i - 1u];
}

FRESULT fsr_opendir(DIR *dp, const TCHAR *path)
{
   /* as fsr_open: a DIR opened again gives up its listing first */
   for (int i = 0; i < NET_DIRS; i++)
      if (dp != NULL && dirs[i].dp == dp) {
         if (dirs[i].session == smb_client_session() && smb_client_context() != NULL)
            smb2_closedir(smb_client_context(), dirs[i].dir);
         memset(&dirs[i], 0, sizeof dirs[i]);
      }
   if (!fsr_is_net_path(path))
      return f_opendir(dp, path);
   if (dp == NULL)
      return FR_INVALID_OBJECT;
   dp->obj.fs = NULL;
   const char *p = share_path(path, path_a);
   if (p == NULL)
      return FR_INVALID_NAME;
   FRESULT r = ready();
   if (r != FR_OK)
      return r;
   net_dir_t *d = NULL;
   for (int i = 0; i < NET_DIRS; i++)
      if (dirs[i].dp == NULL) {
         d = &dirs[i];
         break;
      }
   if (d == NULL)
      return FR_TOO_MANY_OPEN_FILES;
   int s = op_finish(smb2_opendir_async(smb_client_context(), p, op_cb, op_start()));
   if (s < 0)
      return (s == -ENOENT) ? FR_NO_PATH : fr_of(s);
   d->dp = dp;
   d->dir = (struct smb2dir *)op.data;
   d->session = smb_client_session();
   dp->obj.fs = &net_fs;
   dp->obj.id = (WORD)((d - dirs) + 1);
   return FR_OK;
}

FRESULT fsr_closedir(DIR *dp)
{
   net_dir_t *d = net_dir(dp);
   if (d == NULL)
      return (dp != NULL && dp->obj.fs == &net_fs) ? FR_INVALID_OBJECT : f_closedir(dp);
   /* the listing is held locally: closing it needs no network */
   if (d->session == smb_client_session() && smb_client_context() != NULL)
      smb2_closedir(smb_client_context(), d->dir);
   dp->obj.fs = NULL;
   dp->obj.id = 0;
   memset(d, 0, sizeof *d);
   return FR_OK;
}

FRESULT fsr_readdir(DIR *dp, FILINFO *fno)
{
   net_dir_t *d = net_dir(dp);
   if (d == NULL)
      return f_readdir(dp, fno);
   if (d->session != smb_client_session() || smb_client_context() == NULL)
      return FR_NOT_READY;
   if (fno == NULL) {                  /* f_rewinddir */
      smb2_rewinddir(smb_client_context(), d->dir);
      return FR_OK;
   }
   for (;;) {
      struct smb2dirent *e = smb2_readdir(smb_client_context(), d->dir);
      if (e == NULL) {
         fno->fname[0] = '\0';         /* end of the directory */
         return FR_OK;
      }
      /* FatFs never returns the dot entries */
      if (strcmp(e->name, ".") == 0 || strcmp(e->name, "..") == 0)
         continue;
      fill_info(fno, e->name, &e->st);
      return FR_OK;
   }
}

/* ---- by name ------------------------------------------------------------ */

FRESULT fsr_stat(const TCHAR *path, FILINFO *fno)
{
   if (!fsr_is_net_path(path))
      return f_stat(path, fno);
   const char *p = share_path(path, path_a);
   if (p == NULL)
      return FR_INVALID_NAME;
   FRESULT r = ready();
   if (r != FR_OK)
      return r;
   if (p[0] == '\0')
      return FR_INVALID_NAME;          /* as FatFs: the root has no entry */
   int s = op_finish(smb2_stat_async(smb_client_context(), p, &st_buf, op_cb, op_start()));
   if (s < 0)
      return fr_of(s);
   if (fno != NULL) {
      const char *name = strrchr(p, '\\');
      fill_info(fno, name ? name + 1 : p, &st_buf);
   }
   return FR_OK;
}

FRESULT fsr_mkdir(const TCHAR *path)
{
   if (!fsr_is_net_path(path))
      return f_mkdir(path);
   const char *p = share_path(path, path_a);
   if (p == NULL)
      return FR_INVALID_NAME;
   FRESULT r = ready();
   if (r != FR_OK)
      return r;
   return fr_of(op_finish(smb2_mkdir_async(smb_client_context(), p, op_cb, op_start())));
}

/* FatFs's f_unlink removes a file or an empty directory. */
FRESULT fsr_unlink(const TCHAR *path)
{
   if (!fsr_is_net_path(path))
      return f_unlink(path);
   const char *p = share_path(path, path_a);
   if (p == NULL)
      return FR_INVALID_NAME;
   FRESULT r = ready();
   if (r != FR_OK)
      return r;
   int s = op_finish(smb2_stat_async(smb_client_context(), p, &st_buf, op_cb, op_start()));
   if (s < 0)
      return fr_of(s);
   if (st_buf.smb2_type == SMB2_TYPE_DIRECTORY)
      s = op_finish(smb2_rmdir_async(smb_client_context(), p, op_cb, op_start()));
   else
      s = op_finish(smb2_unlink_async(smb_client_context(), p, op_cb, op_start()));
   return fr_of(s);
}

/* FatFs: attr's bits named in mask (read-only, hidden, system, archive)
   are set or cleared.  On the share: CREATE for the attributes alone (which
   no share mode refuses, so a file the Beeb holds open is changed too),
   SET_INFO of FileBasicInformation - times 0, "leave as they are" - and
   CLOSE, sent as one compound.  The share's own bits (directory and the
   rest) are kept. */
static const uint8_t compound_fid[SMB2_FD_SIZE] = {
   0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
   0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};
static uint32_t chmod_status;          /* the compound's first failure */

static void chmod_step_cb(struct smb2_context *smb2, int status, void *data, void *arg)
{
   (void)smb2; (void)data;
   if ((uintptr_t)arg == op.gen && chmod_status == 0u)
      chmod_status = (uint32_t)status;
}

static void chmod_last_cb(struct smb2_context *smb2, int status, void *data, void *arg)
{
   chmod_step_cb(smb2, status, data, arg);
   op_cb(smb2, -nterror_to_errno(chmod_status), NULL, arg);
}

FRESULT fsr_chmod(const TCHAR *path, BYTE attr, BYTE mask)
{
   if (!fsr_is_net_path(path))
      return f_chmod(path, attr, mask);
   const char *p = share_path(path, path_a);
   if (p == NULL)
      return FR_INVALID_NAME;
   FRESULT r = ready();
   if (r != FR_OK)
      return r;
   if (p[0] == '\0')
      return FR_INVALID_NAME;          /* as FatFs: the root has no entry */
   struct smb2_context *smb2 = smb_client_context();
   int s = op_finish(smb2_stat_async(smb2, p, &st_buf, op_cb, op_start()));
   if (s < 0)
      return fr_of(s);
   mask &= (BYTE)(AM_RDO | AM_HID | AM_SYS | AM_ARC);
   uint32_t attrs = (st_buf.smb2_attributes & ~(uint32_t)mask) | (uint32_t)(attr & mask);
   if (st_buf.smb2_type == SMB2_TYPE_DIRECTORY)
      attrs |= SMB2_FILE_ATTRIBUTE_DIRECTORY;
   attrs &= ~(uint32_t)SMB2_FILE_ATTRIBUTE_NORMAL;
   if (attrs == 0u)
      attrs = SMB2_FILE_ATTRIBUTE_NORMAL;   /* 0 would mean "leave as they are" */

   struct smb2_create_request cr;
   memset(&cr, 0, sizeof cr);
   cr.requested_oplock_level = SMB2_OPLOCK_LEVEL_NONE;
   cr.impersonation_level = SMB2_IMPERSONATION_IMPERSONATION;
   cr.desired_access = SMB2_FILE_READ_ATTRIBUTES | SMB2_FILE_WRITE_ATTRIBUTES;
   cr.share_access = SMB2_FILE_SHARE_READ | SMB2_FILE_SHARE_WRITE | SMB2_FILE_SHARE_DELETE;
   cr.create_disposition = SMB2_FILE_OPEN;
   cr.name = p;
   struct smb2_file_basic_info bi;
   memset(&bi, 0, sizeof bi);
   bi.file_attributes = attrs;
   struct smb2_set_info_request si;
   memset(&si, 0, sizeof si);
   si.info_type = SMB2_0_INFO_FILE;
   si.file_info_class = SMB2_FILE_BASIC_INFORMATION;
   memcpy(si.file_id, compound_fid, SMB2_FD_SIZE);
   si.input_data = &bi;
   struct smb2_close_request cl;
   memset(&cl, 0, sizeof cl);
   memcpy(cl.file_id, compound_fid, SMB2_FD_SIZE);

   void *gen = op_start();
   chmod_status = 0u;
   struct smb2_pdu *pdu = smb2_cmd_create_async(smb2, &cr, chmod_step_cb, gen);
   if (pdu == NULL)
      return FR_NOT_ENOUGH_CORE;
   struct smb2_pdu *next = smb2_cmd_set_info_async(smb2, &si, chmod_step_cb, gen);
   if (next == NULL) {
      smb2_free_pdu(smb2, pdu);
      return FR_NOT_ENOUGH_CORE;
   }
   smb2_add_compound_pdu(smb2, pdu, next);
   next = smb2_cmd_close_async(smb2, &cl, chmod_last_cb, gen);
   if (next == NULL) {
      smb2_free_pdu(smb2, pdu);
      return FR_NOT_ENOUGH_CORE;
   }
   smb2_add_compound_pdu(smb2, pdu, next);
   smb2_queue_pdu(smb2, pdu);
   return fr_of(op_finish(0));
}

static struct smb2_statvfs vfs_buf;

FRESULT fsr_free_bytes(const TCHAR *path, uint64_t *free_bytes)
{
   if (!fsr_is_net_path(path)) {
      DWORD clusters;
      FATFS *fs;
      FRESULT r = f_getfree(path, &clusters, &fs);
      if (r == FR_OK)
         *free_bytes = (uint64_t)clusters * fs->csize * 512u;
      return r;
   }
   const char *p = share_path(path, path_a);
   if (p == NULL)
      return FR_INVALID_NAME;
   FRESULT r = ready();
   if (r != FR_OK)
      return r;
   int s = op_finish(smb2_statvfs_async(smb_client_context(), p, &vfs_buf, op_cb, op_start()));
   if (s < 0)
      return fr_of(s);
   *free_bytes = vfs_buf.f_bavail * (uint64_t)vfs_buf.f_bsize;
   return FR_OK;
}

/* FatFs: the new name is on the old one's drive, with or without "2:". */
FRESULT fsr_rename(const TCHAR *path_old, const TCHAR *path_new)
{
   /* A rename stays on one volume.  FatFs drops a drive on the new name,
      and share_path takes a name without one as on the share, so a new
      name on another volume - the other side of the share, or the card
      against a USB drive - would quietly rename the file in place under
      another name: refused instead. */
   int d_new = (path_new != NULL && path_new[0] >= '0' && path_new[0] <= '9' && path_new[1] == ':')
             ? path_new[0] : -1;
   int d_old = (path_old != NULL && path_old[0] >= '0' && path_old[0] <= '9' && path_old[1] == ':')
             ? path_old[0] : -1;
   if (d_new >= 0 && ((d_old >= 0 && d_new != d_old) || fsr_is_net_path(path_new) != fsr_is_net_path(path_old)))
      return FR_INVALID_DRIVE;
   if (!fsr_is_net_path(path_old))
      return f_rename(path_old, path_new);
   const char *p_old = share_path(path_old, path_a);
   const char *p_new = share_path(path_new, path_b);
   if (p_old == NULL || p_new == NULL)
      return FR_INVALID_NAME;
   FRESULT r = ready();
   if (r != FR_OK)
      return r;
   /* SMB renames onto an existing name are refused; FatFs refuses too
      (FR_EXIST), so no replace is asked for */
   return fr_of(op_finish(smb2_rename_async(smb_client_context(), p_old, p_new,
                                            op_cb, op_start())));
}

bool fsr_lost(FIL *fp)
{
   net_file_t *f = net_file(fp);
   return f != NULL && !live(f);
}

bool fsr_net_connect(void)
{
   return smb_client_can_wait() && smb_client_connect();
}

bool fsr_net_configured(void)
{
   return smb_client_configured();
}

bool fsr_net_can_wait(void)
{
   return smb_client_can_wait();
}

void fsr_reset(void)
{
   for (int i = 0; i < NET_FILES; i++)
      if (files[i].fp != NULL && !live(&files[i]))
         release(&files[i]);
   for (int i = 0; i < NET_DIRS; i++)
      if (dirs[i].dp != NULL && dirs[i].session != smb_client_session()) {
         dirs[i].dp->obj.fs = NULL;
         memset(&dirs[i], 0, sizeof dirs[i]);
      }
   smb_client_reset();
}
