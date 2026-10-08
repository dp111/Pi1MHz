/* Host stub for everything chainboot.c includes besides chainboot.h: one
   header, which run_tests.sh puts at each of the paths it includes. */
#ifndef CHAINBOOT_STUBS_H
#define CHAINBOOT_STUBS_H

#include <stdbool.h>
#include <stdint.h>

/* rpi/rpi.h */
#define LOG_DEBUG(...)
/* Pi1MHz.h */
void _copyandreboot(void *src, int num_bytes);
_Noreturn void reboot_now(void);
/* rpi/asm-helpers.h, rpi/cache.h, rpi/rpi.h, rpi/systimer.h */
void _disable_interrupts(void);
void RPI_ChainBootMark(void);
uint32_t RPI_GetSystemTime(void);
/* rpi/audio.h */
void audio_stop_dma(void);
/* wifi/sdio.h, usb/mtp_fs.h */
void sdio_runtime_prepare_for_warm_reboot(void);
void mtp_fs_prepare_for_warm_reboot(void);
void mtp_fs_inserted(void);
/* videoplayer.h (the last two only for an older chainboot.c, which
   refused on video - the negative control) */
bool videoplayer_shutdown(void);
bool videoplayer_active(void);
bool h264dec_running(void);
/* BeebSCSI/filesystem.h */
bool filesystemEject(void);
bool filesystemInsert(void);
bool filesystemEjected(void);

#endif
