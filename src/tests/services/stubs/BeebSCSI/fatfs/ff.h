#pragma once
/* Host-test stub of FatFs ff.h - the types and calls fat_service.c uses.
   The test harness implements the functions. */
#include <stdint.h>

typedef unsigned int  UINT;
typedef uint32_t      DWORD;
typedef uint8_t       BYTE;

typedef enum {
   FR_OK = 0, FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY, FR_NO_FILE,
   FR_NO_PATH, FR_INVALID_NAME, FR_DENIED, FR_EXIST, FR_INVALID_OBJECT,
   FR_WRITE_PROTECTED, FR_INVALID_DRIVE, FR_NOT_ENABLED, FR_NO_FILESYSTEM,
   FR_MKFS_ABORTED, FR_TIMEOUT, FR_LOCKED, FR_NOT_ENOUGH_CORE,
   FR_TOO_MANY_OPEN_FILES, FR_INVALID_PARAMETER
} FRESULT;

/* f_open mode flags (match ff.h) */
#define FA_READ          0x01
#define FA_WRITE         0x02
#define FA_OPEN_EXISTING 0x00
#define FA_CREATE_NEW    0x04
#define FA_CREATE_ALWAYS 0x08
#define FA_OPEN_ALWAYS   0x10
#define FA_OPEN_APPEND   0x30

typedef struct { uint32_t fsize; } FIL;
typedef struct { int dummy; } DIR;
typedef struct { char fname[256]; } FILINFO;
typedef struct { uint32_t csize; } FATFS;

FRESULT f_open(FIL *fp, const char *path, uint8_t mode);
FRESULT f_close(FIL *fp);
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT f_lseek(FIL *fp, uint32_t ofs);
FRESULT f_opendir(DIR *dp, const char *path);
FRESULT f_closedir(DIR *dp);
FRESULT f_readdir(DIR *dp, FILINFO *fno);
FRESULT f_mkdir(const char *path);
FRESULT f_chdir(const char *path);
FRESULT f_getcwd(char *buff, UINT len);
FRESULT f_rename(const char *path_old, const char *path_new);
FRESULT f_getfree(const char *path, DWORD *nclst, FATFS **fatfs);
FRESULT f_unlink(const char *path);

#define f_size(fp) ((fp)->fsize)
