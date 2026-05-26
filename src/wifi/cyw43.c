#include "cyw43.h"

#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include "../BeebSCSI/filesystem.h"
#include "../rpi/rpi.h"

const char g_cyw43_firmware_path[] = "Pi1MHz/wifi/brcmfmac43430-sdio.bin";
const char g_cyw43_nvram_path[] = "Pi1MHz/wifi/brcmfmac43430-sdio.txt";
uint8_t *g_cyw43_firmware_data;
uint8_t *g_cyw43_nvram_data;
uint32_t g_cyw43_firmware_length;
uint32_t g_cyw43_nvram_length;

void cyw43_release_images(void)
{
   if (g_cyw43_firmware_data != NULL) {
      free(g_cyw43_firmware_data);
      g_cyw43_firmware_data = NULL;
   }

   if (g_cyw43_nvram_data != NULL) {
      free(g_cyw43_nvram_data);
      g_cyw43_nvram_data = NULL;
   }

   g_cyw43_firmware_length = 0u;
   g_cyw43_nvram_length = 0u;
}

bool cyw43_preload_images(void)
{
   cyw43_release_images();

   g_cyw43_firmware_length = filesystemReadFile(g_cyw43_firmware_path,
                                                &g_cyw43_firmware_data,
                                                0);
   if (g_cyw43_firmware_length == 0u) {
      LOG_INFO("CYW43 firmware image not found: %s\n", g_cyw43_firmware_path);
      return false;
   }

   g_cyw43_nvram_length = filesystemReadFile(g_cyw43_nvram_path,
                                             &g_cyw43_nvram_data,
                                             0);
   if (g_cyw43_nvram_length == 0u) {
      LOG_INFO("CYW43 NVRAM image not found: %s\n", g_cyw43_nvram_path);
      cyw43_release_images();
      return false;
   }

   return true;
}