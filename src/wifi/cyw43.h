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

/* Rewrite the in-memory brcmfmac NVRAM so the chip uses the
   supplied MAC instead of its factory OTP value.  Any existing
   macaddr=... line is removed first; the new line is appended.
   Must be called AFTER cyw43_preload_images() and BEFORE the SDIO
   runtime hands the NVRAM blob to the firmware loader.  Returns
   false if the NVRAM was not preloaded or the rewrite ran out of
   memory - in either case the caller can safely proceed with the
   unmodified NVRAM (the chip falls back to its OTP MAC). */
bool cyw43_patch_nvram_macaddr(const uint8_t mac[6]);

#endif