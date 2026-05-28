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

#endif
