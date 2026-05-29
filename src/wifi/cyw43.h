#ifndef WIFI_CYW43_H
#define WIFI_CYW43_H

#include <stdbool.h>
#include <stdint.h>

extern const char g_cyw43_firmware_path[];
extern const char g_cyw43_nvram_path[];
extern const char g_cyw43_clm_path[];
extern uint8_t *g_cyw43_firmware_data;
extern uint8_t *g_cyw43_nvram_data;
extern uint8_t *g_cyw43_clm_data;
extern uint32_t g_cyw43_firmware_length;
extern uint32_t g_cyw43_nvram_length;
extern uint32_t g_cyw43_clm_length;

bool cyw43_preload_images(void);
void cyw43_release_images(void);
void cyw43_release_boot_images(void);

#if __ARM_ARCH >= 7
/* Runtime chip-variant selection for the ARMv8 build.  Pi Zero 2 W
   (BCM43436S, chip_id=43430 socramrev>=23) and Pi 3 B+ / Pi 4
   (BCM43455, chip_id=43455) share the same toolchain, so the
   ARMv8 build of Pi1MHz preloads BOTH firmware sets and then frees
   the wrong one once the SDIO runtime has identified the chip.
   Called from sdio.c after sdio_backplane_scan_cores populates
   chip_id and socramrev.  ARMv6 builds hardcode 43430 and do not
   expose this function. */
void cyw43_select_chip_variant(uint16_t chip_id, uint8_t socramrev);
#endif

#endif
