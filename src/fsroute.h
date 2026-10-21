/* fsroute.h - the FatFs calls, routed by path to FatFs or to the SMB share.

   storage=smb puts the Beeb's storage on a network share
   (docs/dev/smb-network-storage-plan.md).  Its paths are on drive "2:",
   which FatFs never sees: the fsr_* calls below send them to the share
   (smb/smb_client.c) and everything else straight to FatFs, unchanged.

   The signatures, types and FRESULT codes are FatFs's own, so a converted
   caller changes only the name.  A file or directory on the share is still
   a FIL or DIR: its obj.fs points at a dummy FATFS (which FatFs's own
   validation rejects, so one passed to FatFs by mistake fails safe), and
   fptr and obj.objsize are kept current, so f_tell/f_size/f_eof work.
   Fast seek is accepted and ignored - seeking costs nothing on a share.

   Main loop only.  Called from inside the network poll (an lwIP callback)
   a call on the share fails at once with FR_TIMEOUT. */
#ifndef FSROUTE_H
#define FSROUTE_H

#include <stdbool.h>
#include <stdint.h>
#include "BeebSCSI/fatfs/ff.h"

/* Is this path on the share? */
bool fsr_is_net_path(const TCHAR *path);

FRESULT fsr_open(FIL *fp, const TCHAR *path, BYTE mode);
FRESULT fsr_close(FIL *fp);
FRESULT fsr_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT fsr_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT fsr_lseek(FIL *fp, FSIZE_t ofs);
FRESULT fsr_sync(FIL *fp);
FRESULT fsr_expand(FIL *fp, FSIZE_t fsz, BYTE opt);
FRESULT fsr_opendir(DIR *dp, const TCHAR *path);
FRESULT fsr_closedir(DIR *dp);
FRESULT fsr_readdir(DIR *dp, FILINFO *fno);
FRESULT fsr_mkdir(const TCHAR *path);
FRESULT fsr_unlink(const TCHAR *path);
FRESULT fsr_rename(const TCHAR *path_old, const TCHAR *path_new);
FRESULT fsr_stat(const TCHAR *path, FILINFO *fno);
FRESULT fsr_chmod(const TCHAR *path, BYTE attr, BYTE mask);

/* Free space on the volume path is on, in bytes - not FatFs's f_getfree,
   whose clusters a share does not have.  For a FatFs path: f_getfree. */
FRESULT fsr_free_bytes(const TCHAR *path, uint64_t *free_bytes);

/* A file on the share whose connection has gone (a lost link): every call
   on it fails until it is closed.  False for a card or drive file. */
bool fsr_lost(FIL *fp);

/* Log on to the share (storage=smb), connecting if need be: false when it
   cannot be reached.  Main loop only; may wait (see smb_client_connect). */
bool fsr_net_connect(void);

/* A share is set up in Pi1MHz.cfg (smb_server, smb_share). */
bool fsr_net_configured(void);

/* This caller may wait for the share: not from inside the network poll. */
bool fsr_net_can_wait(void);

/* A BBC reset: every handle on the share is closed by its owner before
   this (filesystemDismount); forget any that were not, and let a lost link
   reconnect at the next use. */
void fsr_reset(void);

#endif
