#pragma once
/* Host-test stub of FatFs ff.h - what wifi_service.c uses; test_wifisvc.c
   answers it from its in-memory card. */
#include <stdint.h>
typedef enum { FR_OK = 0, FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY, FR_NO_FILE,
               FR_NO_PATH } FRESULT;
typedef struct { uint32_t fsize; } FILINFO;
FRESULT f_stat(const char *path, FILINFO *fno);
