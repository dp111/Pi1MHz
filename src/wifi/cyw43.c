#include "cyw43.h"

#include <stdio.h>
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

/* Case-insensitive prefix match on the first n bytes. */
static bool cyw43_nvram_line_starts_with(const uint8_t *line, uint32_t line_len,
                                         const char *prefix)
{
   uint32_t i = 0u;

   while (prefix[i] != '\0') {
      uint8_t c;

      if (i >= line_len)
         return false;
      c = line[i];
      if (c >= 'A' && c <= 'Z')
         c = (uint8_t)(c + ('a' - 'A'));
      if (c != (uint8_t)prefix[i])
         return false;
      ++i;
   }
   return true;
}

bool cyw43_patch_nvram_macaddr(const uint8_t mac[6])
{
   char        line[32];
   int         line_len;
   uint8_t    *out;
   uint32_t    out_len = 0u;
   const uint8_t *in;
   const uint8_t *end;

   if (mac == NULL || g_cyw43_nvram_data == NULL || g_cyw43_nvram_length == 0u)
      return false;

   line_len = snprintf(line, sizeof(line),
                       "macaddr=%02X:%02X:%02X:%02X:%02X:%02X\n",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
   if (line_len <= 0 || (size_t)line_len >= sizeof(line))
      return false;

   /* Worst-case: no existing macaddr= line, so the new buffer is
      old_length + new_line.  If the original DID contain one, our
      copy will skip it and the final length will be smaller; the
      surplus is harmless. */
   out = malloc((size_t)g_cyw43_nvram_length + (size_t)line_len + 1u);
   if (out == NULL)
      return false;

   in  = g_cyw43_nvram_data;
   end = g_cyw43_nvram_data + g_cyw43_nvram_length;

   while (in < end) {
      const uint8_t *line_end = in;
      uint32_t line_size;
      bool drop_line;

      while (line_end < end && *line_end != '\n')
         ++line_end;
      line_size = (uint32_t)(line_end - in);

      /* Skip non-commented macaddr=... lines so the appended one is
         the only definition the firmware sees.  Comments
         (lines starting with '#') and every other line are kept. */
      drop_line = (line_size >= 8u)
                  && (in[0] != '#')
                  && cyw43_nvram_line_starts_with(in, line_size, "macaddr=");

      if (!drop_line) {
         memcpy(&out[out_len], in, line_size);
         out_len += line_size;
         if (line_end < end)
            out[out_len++] = '\n';
      }

      in = (line_end < end) ? (line_end + 1u) : end;
   }

   memcpy(&out[out_len], line, (size_t)line_len);
   out_len += (uint32_t)line_len;

   free(g_cyw43_nvram_data);
   g_cyw43_nvram_data = out;
   g_cyw43_nvram_length = out_len;
   return true;
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

   /* The CLM (Country Locale Matrix - the chip's regulatory database) is
      optional: if the blob is absent the firmware falls back to its
      built-in minimal regulatory data, so a missing file must NOT fail
      the boot.  When present it is downloaded to the chip via the
      "clmload" iovar so the "country" setting has a full country table. */
   g_cyw43_clm_length = filesystemReadFile(g_cyw43_clm_path,
                                           &g_cyw43_clm_data,
                                           0);
   if (g_cyw43_clm_length == 0u) {
      g_cyw43_clm_data = NULL;
      LOG_INFO("CYW43 CLM blob not found (optional): %s\n", g_cyw43_clm_path);
   }

   return true;
}