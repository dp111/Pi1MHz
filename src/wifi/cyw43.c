#include "cyw43.h"

#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include "../BeebSCSI/filesystem.h"
#include "../rpi/rpi.h"

const char g_cyw43_firmware_path[] = "Pi1MHz/wifi/brcmfmac43430-sdio.bin";
const char g_cyw43_nvram_path[] = "Pi1MHz/wifi/brcmfmac43430-sdio.txt";
const char g_cyw43_clm_path[] = "Pi1MHz/wifi/brcmfmac43430-sdio.clm_blob";
uint8_t *g_cyw43_firmware_data;
uint8_t *g_cyw43_nvram_data;
uint8_t *g_cyw43_clm_data;
uint32_t g_cyw43_firmware_length;
uint32_t g_cyw43_nvram_length;
uint32_t g_cyw43_clm_length;

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

   if (g_cyw43_clm_data != NULL) {
      free(g_cyw43_clm_data);
      g_cyw43_clm_data = NULL;
   }

   g_cyw43_firmware_length = 0u;
   g_cyw43_nvram_length = 0u;
   g_cyw43_clm_length = 0u;
}

/* Free only the firmware and NVRAM images, keeping the CLM blob.  This is
   called once the firmware has been downloaded to the chip: the firmware
   and NVRAM are no longer needed in host RAM, but the CLM blob still has
   to survive until the clmload iovar download, which runs later (after
   the firmware has booted).  Freeing the CLM here was the bug that left
   sdio_runtime_download_clm() with a NULL pointer. */
void cyw43_release_boot_images(void)
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

   /* Defensive: if filesystemReadFile partially populated *_data even
      on failure (length 0) we'd leak that buffer on every retry.
      cyw43_release_images() free()s any non-NULL slot, and we call
      it before each early-return path so the leak window is closed
      regardless of how filesystemReadFile reports its failures. */
   g_cyw43_firmware_length = filesystemReadFile(g_cyw43_firmware_path,
                                                &g_cyw43_firmware_data,
                                                0);
   if (g_cyw43_firmware_length == 0u) {
      LOG_INFO("CYW43 firmware image not found: %s\n", g_cyw43_firmware_path);
      cyw43_release_images();
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

   /* The CLM (Country Locale Matrix - the chip's regulatory database) is
      optional: if the blob is absent the firmware falls back to its
      built-in minimal regulatory data, so a missing file must NOT fail
      the boot.  When present it is downloaded to the chip via the
      "clmload" iovar so the "country" setting has a full country table. */
   g_cyw43_clm_length = filesystemReadFile(g_cyw43_clm_path,
                                           &g_cyw43_clm_data,
                                           0);
   if (g_cyw43_clm_length == 0u) {
      /* CLM is optional - if filesystemReadFile allocated then failed,
         free that slot via the central release path before clearing
         the pointer, so we don't leak the partial buffer. */
      if (g_cyw43_clm_data != NULL) {
         free(g_cyw43_clm_data);
         g_cyw43_clm_data = NULL;
      }
      LOG_INFO("CYW43 CLM blob not found (optional): %s\n", g_cyw43_clm_path);
   }

   return true;
}
