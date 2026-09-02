#ifndef STUB_FF_H
#define STUB_FF_H
#include <stdint.h>
typedef unsigned int UINT;
typedef unsigned long FSIZE_t;
typedef int FRESULT;
#define FR_OK        0
#define FR_NO_FILE   4
#define FR_EXIST     8
#define FR_DENIED    7
#define FA_READ           0x01
#define FA_WRITE          0x02
#define FA_CREATE_ALWAYS  0x08
typedef struct { int slot; FSIZE_t size; FSIZE_t pos; } FIL;
typedef struct { FSIZE_t fsize; } FILINFO;
#define f_size(fp) ((fp)->size)
FRESULT f_open(FIL *fp, const char *path, unsigned char mode);
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT f_close(FIL *fp);
FRESULT f_stat(const char *path, FILINFO *fno);
FRESULT f_rename(const char *path_old, const char *path_new);
FRESULT f_unlink(const char *path);
#endif
