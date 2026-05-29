#include "cyw43.h"

#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include "../BeebSCSI/filesystem.h"
#include "../rpi/rpi.h"

/* WiFi chip / firmware blob is selected at compile time from the
   ARM architecture the toolchain is targeting:

     scripts/rpi.cmake  -> -mcpu=arm1176jzf-s   (__ARM_ARCH == 6)
       Pi Zero W only -> always BCM43430 -> brcmfmac43430-sdio.*

     scripts/rpi3.cmake -> -march=armv8-a       (__ARM_ARCH == 8)
       Pi Zero 2 W  -> BCM43430B0 -> brcmfmac43430b0-sdio.*
       Pi 3 B+      -> BCM43455   -> brcmfmac43455-sdio.*
       Pi 4         -> BCM43455   -> brcmfmac43455-sdio.*

     The ARMv8 build can't pick at compile time because the same
     binary runs on all three boards.  It preloads BOTH firmware
     sets (43430b0 in the primary g_cyw43_* slots, 43455 in the alt
     slots below); after the SDIO runtime reads chip_id and
     socramrev it calls cyw43_select_chip_variant() to free the
     wrong one and (if needed) swap the alt set into primary.
     If a board's firmware files are missing the corresponding
     slot stays empty and cyw43_select_chip_variant errors out
     cleanly.

     Note on naming.  Pi-OS dmesg says
       brcmf_fw_alloc_request: using brcm/brcmfmac43430b0-sdio for
       chip BCM43430/2
     but the actual file on disk in the trixie firmware-nonfree
     tree is brcmfmac43436-sdio.bin - the brcmfmac43430b0-sdio.*
     names are symlinks (specifically
     brcmfmac43430b0-sdio.raspberrypi,model-zero-2-w.bin -> the
     43436-sdio.bin underlying blob).  The 43436S sibling
     (note: trailing 's') is a DIFFERENT blob used on different
     boards and is NOT what the Pi Zero 2 W needs.  Use the
     underlying-blob filenames here so it's obvious which file
     the SD card actually has to contain. */
#if __ARM_ARCH >= 7
const char g_cyw43_firmware_path[] = "Pi1MHz/wifi/brcmfmac43436-sdio.bin";
const char g_cyw43_nvram_path[]    = "Pi1MHz/wifi/brcmfmac43436-sdio.txt";
const char g_cyw43_clm_path[]      = "Pi1MHz/wifi/brcmfmac43436-sdio.clm_blob";
/* Alt set: 43455 (Pi 3 B+ / Pi 4).  Static - sdio.c reaches them
   only via cyw43_select_chip_variant. */
static const char g_cyw43_alt_firmware_path[] = "Pi1MHz/wifi/brcmfmac43455-sdio.bin";
static const char g_cyw43_alt_nvram_path[]    = "Pi1MHz/wifi/brcmfmac43455-sdio.txt";
static const char g_cyw43_alt_clm_path[]      = "Pi1MHz/wifi/brcmfmac43455-sdio.clm_blob";
static uint8_t *g_cyw43_alt_firmware_data;
static uint8_t *g_cyw43_alt_nvram_data;
static uint8_t *g_cyw43_alt_clm_data;
static uint32_t g_cyw43_alt_firmware_length;
static uint32_t g_cyw43_alt_nvram_length;
static uint32_t g_cyw43_alt_clm_length;
#else
const char g_cyw43_firmware_path[] = "Pi1MHz/wifi/brcmfmac43430-sdio.bin";
const char g_cyw43_nvram_path[]    = "Pi1MHz/wifi/brcmfmac43430-sdio.txt";
const char g_cyw43_clm_path[]      = "Pi1MHz/wifi/brcmfmac43430-sdio.clm_blob";
#endif
uint8_t *g_cyw43_firmware_data;
uint8_t *g_cyw43_nvram_data;
uint8_t *g_cyw43_clm_data;
uint32_t g_cyw43_firmware_length;
uint32_t g_cyw43_nvram_length;
uint32_t g_cyw43_clm_length;

#if __ARM_ARCH >= 7
/* Free the alt (43455) slots.  Called both as part of the full
   cyw43_release_images teardown and as a side effect of
   cyw43_select_chip_variant choosing the 43436s primary. */
static void cyw43_release_alt_images(void)
{
   if (g_cyw43_alt_firmware_data != NULL) {
      free(g_cyw43_alt_firmware_data);
      g_cyw43_alt_firmware_data = NULL;
   }
   if (g_cyw43_alt_nvram_data != NULL) {
      free(g_cyw43_alt_nvram_data);
      g_cyw43_alt_nvram_data = NULL;
   }
   if (g_cyw43_alt_clm_data != NULL) {
      free(g_cyw43_alt_clm_data);
      g_cyw43_alt_clm_data = NULL;
   }
   g_cyw43_alt_firmware_length = 0u;
   g_cyw43_alt_nvram_length = 0u;
   g_cyw43_alt_clm_length = 0u;
}
#endif

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

#if __ARM_ARCH >= 7
   cyw43_release_alt_images();
#endif
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

#if __ARM_ARCH >= 7
   /* ARMv8 build: optimistically try to load the BCM43455 firmware
      set into the alt slots.  If they're missing (typical Pi Zero 2 W
      install where the user only put the 43436s files on the SD
      card), that's fine - cyw43_select_chip_variant will just have
      nothing to swap in, and if the chip turns out to need the 43455
      blob anyway it will error out then with a clear message.  The
      reverse case (43436s missing but 43455 present, on a Pi 3 B+
      install) works the same way: select_chip_variant swaps alt to
      primary and frees the empty 43436s placeholder. */
   g_cyw43_alt_firmware_length = filesystemReadFile(g_cyw43_alt_firmware_path,
                                                    &g_cyw43_alt_firmware_data,
                                                    0);
   if (g_cyw43_alt_firmware_length == 0u) {
      if (g_cyw43_alt_firmware_data != NULL) {
         free(g_cyw43_alt_firmware_data);
         g_cyw43_alt_firmware_data = NULL;
      }
      LOG_INFO("CYW43 firmware (alt) not found: %s\n", g_cyw43_alt_firmware_path);
   } else {
      g_cyw43_alt_nvram_length = filesystemReadFile(g_cyw43_alt_nvram_path,
                                                    &g_cyw43_alt_nvram_data,
                                                    0);
      if (g_cyw43_alt_nvram_length == 0u) {
         LOG_INFO("CYW43 NVRAM (alt) not found: %s\n", g_cyw43_alt_nvram_path);
         cyw43_release_alt_images();
      } else {
         g_cyw43_alt_clm_length = filesystemReadFile(g_cyw43_alt_clm_path,
                                                     &g_cyw43_alt_clm_data,
                                                     0);
         if (g_cyw43_alt_clm_length == 0u) {
            if (g_cyw43_alt_clm_data != NULL) {
               free(g_cyw43_alt_clm_data);
               g_cyw43_alt_clm_data = NULL;
            }
            LOG_INFO("CYW43 CLM (alt) not found (optional): %s\n",
                     g_cyw43_alt_clm_path);
         }
      }
   }
#endif

   return true;
}

#if __ARM_ARCH >= 7
/* The chip-family identifier (chip_id) is shared by the BCM43430A1
   (Pi Zero W) and the BCM43430B0 (Pi Zero 2 W): both report 43430
   and both report chip_revision = 2.  The two are differentiated by
   socramrev - the B0 silicon adds RAM and SDIO controller revisions
   and reports socramrev >= 23 (the real boards we've seen come out
   at rev 25).  The BCM43455 (Pi 3 B+ / Pi 4) reports a different
   chip_id entirely (43455) and needs its own firmware blob.

   Strictly speaking the ARMv8 toolchain build can only run on Pi
   Zero 2 W / Pi 3 / Pi 4 hardware - the ARMv6 build is for Pi
   Zero W - so seeing chip_id == 43430 with socramrev <= 22 on an
   ARMv8 boot would be a build/board mismatch.  Refuse rather than
   silently feed it the 43436 blob. */
bool cyw43_select_chip_variant(uint16_t chip_id, uint8_t socramrev)
{
   bool need_alt = false;

   if (chip_id == 43455u) {
      need_alt = true;
   } else if (chip_id == 43430u) {
      /* 43430-family.  Anything < 23 is a real BCM43430, which on
         an ARMv8 build is a hardware/toolchain mismatch.  Fall
         through and try the (43436s) primary anyway - the firmware
         download will probably fail, but with a clearer message
         than "wrong chip_id". */
      (void)socramrev;
      need_alt = false;
   } else {
      /* Unknown chip_id; let sdio.c surface the "unsupported chip"
         error.  Drop the alt set so we don't keep its memory
         tied up forever. */
      LOG_INFO("CYW43 select: unrecognised chip_id=%u; refusing to download firmware\n",
               (unsigned int)chip_id);
      cyw43_release_alt_images();
      return false;
   }

   if (need_alt) {
      /* Replace primary with alt, free the old primary. */
      free(g_cyw43_firmware_data);
      free(g_cyw43_nvram_data);
      free(g_cyw43_clm_data);
      g_cyw43_firmware_data    = g_cyw43_alt_firmware_data;
      g_cyw43_nvram_data       = g_cyw43_alt_nvram_data;
      g_cyw43_clm_data         = g_cyw43_alt_clm_data;
      g_cyw43_firmware_length  = g_cyw43_alt_firmware_length;
      g_cyw43_nvram_length     = g_cyw43_alt_nvram_length;
      g_cyw43_clm_length       = g_cyw43_alt_clm_length;
      g_cyw43_alt_firmware_data = NULL;
      g_cyw43_alt_nvram_data    = NULL;
      g_cyw43_alt_clm_data      = NULL;
      g_cyw43_alt_firmware_length = 0u;
      g_cyw43_alt_nvram_length    = 0u;
      g_cyw43_alt_clm_length      = 0u;
   } else {
      /* Keep primary (43436s); free alt (43455). */
      cyw43_release_alt_images();
   }

   /* Final check: did we end up with a usable firmware+NVRAM pair?
      A common failure mode is "ARMv8 build, but SD card only has the
      Pi 3/Pi 4 (43455) blobs and the chip is a Pi Zero 2 W" or vice
      versa.  Log specifically what is missing so the user does not
      have to dig through filesystemReadFile messages. */
   if (g_cyw43_firmware_data == NULL || g_cyw43_firmware_length == 0u
       || g_cyw43_nvram_data == NULL || g_cyw43_nvram_length == 0u) {
      LOG_INFO("CYW43 select: chip_id=%u needs %s firmware but it is not on the SD card\n",
               (unsigned int)chip_id,
               need_alt ? "43455" : "43436");
      return false;
   }
   return true;
}
#endif
