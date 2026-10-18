/* fsroute.h for the host tests that build filesystem.c without storage=smb:
   every fsr_* call is FatFs's own, as it is in the firmware for any path not
   on the share.  run_tests.sh copies it into the test tree as fsroute.h. */
#ifndef FSROUTE_H
#define FSROUTE_H
#include <stdbool.h>
#include <stdint.h>
#include "BeebSCSI/fatfs/ff.h"
#define fsr_open     f_open
#define fsr_close    f_close
#define fsr_read     f_read
#define fsr_write    f_write
#define fsr_lseek    f_lseek
#define fsr_truncate f_truncate
#define fsr_sync     f_sync
#define fsr_expand   f_expand
#define fsr_opendir  f_opendir
#define fsr_closedir f_closedir
#define fsr_readdir  f_readdir
#define fsr_mkdir    f_mkdir
#define fsr_unlink   f_unlink
#define fsr_rename   f_rename
#define fsr_stat     f_stat
#define fsr_chmod    f_chmod
/* a macro, so a test whose stub ff.h has no f_getfree only meets it if used */
#define fsr_free_bytes(path, out) ({ DWORD fsr_c_; FATFS *fsr_fs_; \
   FRESULT fsr_r_ = f_getfree((path), &fsr_c_, &fsr_fs_); \
   if (fsr_r_ == FR_OK) *(out) = (uint64_t)fsr_c_ * fsr_fs_->csize * 512u; fsr_r_; })
static inline bool fsr_is_net_path(const char *path) { (void)path; return false; }
#ifdef FSR_NET_HOOKS
/* the test plays the share: whether one is set up, reachable, waitable */
bool fsr_net_connect(void);
bool fsr_net_configured(void);
bool fsr_net_can_wait(void);
#else
static inline bool fsr_net_connect(void) { return false; }
static inline bool fsr_net_configured(void) { return false; }
static inline bool fsr_net_can_wait(void) { return true; }
#endif
static inline void fsr_reset(void) {}
#endif
