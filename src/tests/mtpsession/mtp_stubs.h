/* Host stand-in for every project header usb/mtp_fs.c includes, except
   FatFs (real, on a RAM disk) and TinyUSB (real headers; the few driver
   functions the file calls are stubbed by the test).  run_tests.sh puts
   this behind each include path. */
#ifndef MTP_STUBS_H
#define MTP_STUBS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* scripts/gitversion.h */
#define RELEASENAME "test"

/* BeebSCSI/filesystem.h - the host-write interlock, observed by the test */
bool   filesystemMount(void);
bool   filesystemReadLunStatus(uint8_t lunNumber);
bool   filesystemHostPathBusy(const char *path);
int8_t filesystemLunFromHostPath(const char *path);
void   filesystemHostLockLun(int8_t lunNumber, bool lock);
bool   filesystemHostLunRevoked(uint8_t lunNumber);

/* services.h */
bool fat_service_file_in_use(const char *host_path);
bool beeb_path_busy(const char *host_path);

/* rpi/systimer.h */
uint32_t RPI_GetSystemTime(void);

/* bsp/board_api.h */
size_t board_usb_get_serial(uint16_t desc_str1[], size_t max_chars);

/* wifi/webserver.h */
bool webserver_sd_space_now(uint64_t *total, uint64_t *free_bytes);

/* rpi/exceptions.h */
_Noreturn void reboot_now(void);

/* chainboot.h */
#define CHAINBOOT_MAX_IMAGE (4u * 1024u * 1024u)
bool chainboot_image_ok(const uint8_t *image, uint32_t length);
const char *chainboot_refusal(void);
bool chainboot_request(uint8_t *image, uint32_t length, uint32_t capacity);

#endif
