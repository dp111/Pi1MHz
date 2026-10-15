/* fsroute.h for the host tests that build filesystem.c without storage=smb:
   every fsr_* call is FatFs's own, as it is in the firmware for any path not
   on the share.  run_tests.sh copies it into the test tree as fsroute.h. */
#ifndef FSROUTE_H
#define FSROUTE_H
#include <stdbool.h>
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
static inline bool fsr_is_net_path(const char *path) { (void)path; return false; }
static inline bool fsr_net_connect(void) { return false; }
static inline void fsr_reset(void) {}
#endif
