/* chainboot.c - restart the Pi into a kernel image held in RAM (kernel.now).

   Shared by MTP (a file called kernel.now copied to the Pi) and the
   webserver (PUT /kernel.now, or the upload form).  See chainboot.h. */
#include <stdlib.h>
#include <string.h>

#include "chainboot.h"
#include "Pi1MHz.h"
#include "BeebSCSI/filesystem.h"
#include "usb/mtp_fs.h"
#include "videoplayer.h"
#include "rpi/asm-helpers.h"
#include "rpi/audio.h"
#include "rpi/cache.h"
#include "rpi/h264dec.h"
#include "rpi/rpi.h"
#include "rpi/systimer.h"
#include "wifi/sdio.h"

bool chainboot_image_ok(const uint8_t *image, uint32_t length)
{
   /* A first word that is not an ARM branch means the transfer arrived
      damaged - and jumping into it produces a Pi that is silent before UART
      init and needs a power cycle, which has happened repeatedly.  Refusing
      costs a failed flash and leaves the machine running. */
   if (image == NULL || length < 4u || length > CHAINBOOT_MAX_IMAGE)
      return false;
   uint32_t first = (uint32_t)image[0] | ((uint32_t)image[1] << 8) |
                    ((uint32_t)image[2] << 16) | ((uint32_t)image[3] << 24);
   return (first & 0xff000000u) == 0xea000000u;
}

const char *chainboot_refusal(void)
{
   /* A chain-boot never shuts the VideoCore down: over an open player it
      orphans the GPU decoder, and video stays broken until a full reboot. */
   if (videoplayer_active())
      return "The video player is open - close it (or reboot) first.";
   /* The decoder outlives the player: once started it holds its GPU
      buffers until a reboot, even with the player closed (say, after a
      jukebox to a side without video). */
   if (h264dec_running())
      return "The video decoder has been started - reboot first.";
   return NULL;
}

/* A received image waits here until the main loop can act on it.
 *
 * The reboot used to happen inside the MTP callback, with interrupts off,
 * never returning - so the host's SendObject was never answered and the
 * device simply vanished mid-command, while USB was still connected and
 * possibly still moving data.  Copying a new image over the running kernel
 * and jumping into it while a controller may still touch memory is only safe
 * if nothing is in flight, which is why flashing an idle Pi always worked and
 * flashing one straight after a transfer left it hung with no USB and no
 * network - reproduced deliberately: four flashes idle all succeeded, one
 * flash immediately after a load test failed exactly that way.
 *
 * Deferring costs a moment and buys a clean answer for the sender, a chance
 * to take USB off the bus first, and a main loop that is between poll
 * callbacks rather than nested inside one. */
static uint8_t *s_image;
static uint32_t s_length;

/* Long enough for the sender's answer to get out: an MTP response on the
   wire, or an HTTP one through lwIP and the WiFi chip. */
#define CHAINBOOT_SETTLE_US 200000u

bool chainboot_request(uint8_t *image, uint32_t length, uint32_t capacity)
{
   uint32_t padded = (length + 63u) & ~63u;
   if (image == NULL || padded > capacity) {
      free(image);
      return false;
   }
   memset(image + length, 0, padded - length);
   free(s_image);                 /* a second request replaces the first */
   s_image = image;
   s_length = padded;
   return true;
}

void chainboot_poll(void)
{
   static uint8_t stage;
   static uint32_t settle_us;

   if (s_image == NULL)
      return;

   if (stage == 0u) {
      settle_us = RPI_GetSystemTime() + CHAINBOOT_SETTLE_US;
      stage = 1u;
      return;
   }
   if (stage == 1u) {
      if ((int32_t)(RPI_GetSystemTime() - settle_us) < 0)
         return;
      mtp_fs_prepare_for_warm_reboot();   /* USB off the bus, so nothing is left in flight */
      settle_us = RPI_GetSystemTime() + 50000u;
      stage = 2u;
      return;
   }
   if (stage == 2u) {
      /* As the Beeb's own reboot (HD_CARD_REBOOT): every open file closed and
         the volume dismounted, so nothing unsynced - a FAT-service file, a
         recording, a half-written upload - is lost with lost clusters left
         behind.  A step per pass: a recording still being written out makes
         its subsystem wait. */
      if (!filesystemEject())
         return;
      stage = 3u;
   }
   if ((int32_t)(RPI_GetSystemTime() - settle_us) < 0)
      return;

   /* The player or the decoder may have started while this waited.  Then
      the image is given up rather than orphan the decoder: the card goes
      back, and with it USB (mtp_fs_inserted), and the Pi carries on.  The
      sender has had its OK already; there is no telling it otherwise. */
   if (chainboot_refusal() != NULL) {
      free(s_image);
      s_image = NULL;
      stage = 0u;
      (void)filesystemInsert();
      return;
   }

   /* The chip keeps power across the warm jump, so tell it to stop signalling
      on DAT1 (CCCR 0x04, HOSTINTMASK) and hide the controller latch before the
      incoming kernel starts its bring-up over that same line. */
   sdio_runtime_prepare_for_warm_reboot();

   _disable_interrupts();
   audio_stop_dma();      /* the copy may run over its control blocks */
   RPI_ChainBootMark();   /* the incoming kernel_main learns it was chain-booted */
   /* Turn the D-cache off first, so the copier written under the kernel and
      the copy of the incoming image over it go straight to RAM: the copier
      then needs only the instruction side cleaned (arm-start.S), and the new
      kernel starts on a coherent image. */
   disable_data_cache();
   _copyandreboot(s_image, (int)s_length); /* never returns */
}
