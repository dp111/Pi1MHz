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
/* Runtime chip-variant selection for the ARMv8 build.  Pi 3 B
   (BCM43430A1), both Pi Zero 2 W radios (BCM43436 and BCM43436S)
   and Pi 3 B+ / Pi 4 (BCM43455) share the same toolchain, so the
   ARMv8 build of Pi1MHz preloads ALL FOUR firmware sets and then
   frees the ones it does not need once the SDIO runtime has
   identified the chip.  Called from sdio.c after
   sdio_backplane_scan_cores populates chip_id, chip_revision and
   socramrev.  ARMv6 builds hardcode 43430 and do not expose this
   function.

   The three chip_id=43430 boards are separated the way Linux does
   it: socramrev stands in for device-tree board matching (< 23 is
   the Pi 3 B), and chip_revision picks between the two Zero 2 W
   radios (revision 1 is 43436s, 2 and up is 43436).

   Returns true if a firmware+NVRAM pair matching the chip survives
   in the primary slots; false if the matching blob was not on the
   SD card (LOG_INFO is emitted explaining which blob is missing) or
   the chip_id is unrecognised.  The caller (sdio.c) sets the runtime
   error string and aborts boot when false is returned. */
bool cyw43_select_chip_variant(uint16_t chip_id, uint8_t chip_revision, uint8_t socramrev);
#endif

#endif
