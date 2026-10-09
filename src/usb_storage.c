/* usb_storage.c - a USB flash drive on the Pi's port in host mode.

   TinyUSB's MSC host class enumerates the drive - TEST UNIT READY, then READ
   CAPACITY of LUN 0 - and reports it here from the main loop (tuh_task).
   One drive is taken; any others are passed over until it goes.  Stage 1 of
   docs/dev/usb-flash-storage-plan.md: nothing reads the drive yet.

   The callbacks run inside tuh_task: they only record, and must never start
   FatFs or other USB work (a later stage's disk read waits on tuh_task). */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "usb/tusb_config.h"
#include <tusb.h>

#include "usb_storage.h"

static bool     s_present;
static uint8_t  s_addr;              /* the drive taken */
static uint16_t s_vid, s_pid;
static uint32_t s_blocks, s_block_size;

void usb_storage_status(char *buf, size_t len)
{
   if (!s_present) {
      snprintf(buf, len, "none");
      return;
   }
   uint64_t mb = (uint64_t)s_blocks * s_block_size / (1024u * 1024u);
   snprintf(buf, len, "%04x:%04x %lu MB, %lu-byte blocks",
            (unsigned)s_vid, (unsigned)s_pid, (unsigned long)mb,
            (unsigned long)s_block_size);
}

/* ---- TinyUSB MSC host callbacks (main loop, from tuh_task) -------------- */

static void adopt(uint8_t dev_addr)
{
   s_addr = dev_addr;
   (void)tuh_vid_pid_get(dev_addr, &s_vid, &s_pid);
   s_blocks = tuh_msc_get_block_count(dev_addr, 0u);
   s_block_size = tuh_msc_get_block_size(dev_addr, 0u);
   s_present = true;
}

void tuh_msc_mount_cb(uint8_t dev_addr)
{
   if (!s_present)
      adopt(dev_addr);
}

void tuh_msc_umount_cb(uint8_t dev_addr)
{
   if (!s_present || dev_addr != s_addr)
      return;
   s_present = false;

   /* Another drive passed over at its mount: take it up now (as usb_mouse.c
      does for a second mouse).  The going device is still mounted here. */
   for (uint8_t a = 1u; a <= CFG_TUH_DEVICE_MAX; a++)
      if (a != dev_addr && tuh_msc_mounted(a)) {
         adopt(a);
         return;
      }
}
