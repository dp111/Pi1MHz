#pragma once
/* Host-test stub of FatFs ff.h - only what secure_service_wolfssh.c uses. */
#include <stdint.h>
typedef unsigned int UINT;
typedef uint32_t     FSIZE_t;
typedef enum { FR_OK = 0, FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY, FR_NO_FILE,
               FR_NO_PATH, FR_INVALID_NAME, FR_DENIED, FR_EXIST } FRESULT;
#define FA_READ 0x01
typedef struct { FSIZE_t fsize; const uint8_t *data; } FIL;
typedef struct { FSIZE_t fsize; } FILINFO;
FRESULT f_open(FIL *fp, const char *path, uint8_t mode);
FRESULT f_close(FIL *fp);
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT f_stat(const char *path, FILINFO *fno);
FRESULT f_mkdir(const char *path);
#define f_size(fp) ((fp)->fsize)
