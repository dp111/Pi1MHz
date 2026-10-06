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
   /* The decoder outlives the player: its MMAL component and VCHIQ service
      are never torn down once started - not by closing the player, not by a
      BREAK - so after any video use since the last reboot there is something
      a chain-boot would orphan. */
   if (h264dec_running())
      return "Video has been used since the last reboot - reboot first.";
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
static uint8_t  s_stage;         /* where chainboot_poll has got to with it */
static bool     s_usb_off;       /* USB taken off the bus for the jump */
static bool     s_took_card;     /* the eject was ours, so a give-up returns it */

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
   s_stage = 0u;                  /* ...and waits a settle of its own, so its
                                     sender's answer gets out too */
   return true;
}

/* Give the image up and let the Pi carry on as it was: what this code took
   for the jump goes back, and nothing else - a card the user had ejected
   stays out, and USB with it.  The sender has had its OK already; there is
   no telling it otherwise. */
static void chainboot_abandon(void)
{
   free(s_image);
   s_image = NULL;
   s_stage = 0u;
   if (s_took_card)
      (void)filesystemInsert();          /* and with it USB (mtp_fs_inserted) */
   else if (s_usb_off && !filesystemEjected())
      mtp_fs_inserted();                 /* USB back; the host enumerates afresh */
   s_took_card = false;
   s_usb_off = false;
}

void chainboot_poll(void)
{
   static uint32_t settle_us;

   if (s_image == NULL)
      return;

   /* The player or the decoder may start while this waits, and then the
      image is given up rather than orphan the decoder.  The refusal is asked
      again before each step that would cost the Beeb something to undo. */
   if (s_stage == 0u) {
      settle_us = RPI_GetSystemTime() + CHAINBOOT_SETTLE_US;
      s_stage = 1u;
      return;
   }
   if (s_stage == 1u) {
      if ((int32_t)(RPI_GetSystemTime() - settle_us) < 0)
         return;
      if (chainboot_refusal() != NULL) {   /* nothing touched yet */
         chainboot_abandon();
         return;
      }
      mtp_fs_prepare_for_warm_reboot();   /* USB off the bus, so nothing is left in flight */
      s_usb_off = true;
      settle_us = RPI_GetSystemTime() + 50000u;
      s_stage = 2u;
      return;
   }
   if (s_stage == 2u) {
      if ((int32_t)(RPI_GetSystemTime() - settle_us) < 0)
         return;
      if (chainboot_refusal() != NULL) {   /* free: at most USB comes back */
         chainboot_abandon();
         return;
      }
      if (!filesystemEjected())
         s_took_card = true;    /* ours from here; one the user ejected is not */
      s_stage = 3u;
      return;
   }
   if (s_stage == 3u) {
      /* As the Beeb's own reboot (HD_CARD_REBOOT): every open file closed and
         the volume dismounted, so nothing unsynced - a FAT-service file, a
         recording, a half-written upload - is lost with lost clusters left
         behind.  A step per pass: a recording still being written out makes
         its subsystem wait. */
      if (!filesystemEject())
         return;
      s_stage = 4u;
      return;
   }

   /* Only a backstop now, for an eject that took several passes (a Music
      5000 recording being flushed).  Giving up here is not free for the
      Beeb: the eject stopped every LUN, and putting the card back mounts it
      without restarting them and resets the FAT directory to /Transfer, so
      a session in progress - a Domesday disc, say - loses its discs until
      the next BREAK.  Still better than orphaning the decoder. */
   if (chainboot_refusal() != NULL) {
      chainboot_abandon();
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
