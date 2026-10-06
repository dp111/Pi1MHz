/* Host stub for everything chainboot.c includes besides chainboot.h: one
   header, which run_tests.sh puts at each of the paths it includes. */
#ifndef CHAINBOOT_STUBS_H
#define CHAINBOOT_STUBS_H

#include <stdbool.h>
#include <stdint.h>

/* Pi1MHz.h */
void _copyandreboot(void *src, int num_bytes);
/* rpi/asm-helpers.h, rpi/cache.h, rpi/rpi.h, rpi/systimer.h */
void _disable_interrupts(void);
void disable_data_cache(void);
void RPI_ChainBootMark(void);
uint32_t RPI_GetSystemTime(void);
/* wifi/sdio.h, usb/mtp_fs.h */
void sdio_runtime_prepare_for_warm_reboot(void);
void mtp_fs_prepare_for_warm_reboot(void);
/* videoplayer.h, rpi/h264dec.h */
bool videoplayer_active(void);
bool h264dec_running(void);
/* BeebSCSI/filesystem.h */
bool filesystemEject(void);
bool filesystemInsert(void);

#endif
