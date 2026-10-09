/* usb_storage.c - a USB flash drive on the Pi's port in host mode.

   TinyUSB's MSC host class enumerates the drive - TEST UNIT READY, then READ
   CAPACITY of LUN 0 - and reports it here from the main loop (tuh_task).
   One drive is taken; any others are passed over until it goes.  Stage 2 of
   docs/dev/usb-flash-storage-plan.md: the drive is FatFs volume "1:",
   read-only, and only /status looks at it.

   The callbacks run inside tuh_task: they only record.  FatFs work is done
   from usb_storage_poll, as a read (usb_storage_read) itself runs tuh_task
   while it waits. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "usb/tusb_config.h"
#include <tusb.h>

#include "BeebSCSI/fatfs/ff.h"
#include "rpi/systimer.h"
#include "usb_storage.h"

/* A drive's own housekeeping can hold a command for a second or more; the
   Beeb has no timeouts, so a slow answer beats an error.  Kept below the
   shortest watchdog= that is sensible with a drive (2 s and up). */
#define READ_TIMEOUT_US 1500000u

/* Reads land here, not in the caller's buffer: after a timeout the command
   is still queued with this buffer, and nothing can take it back. */
#define BOUNCE_BLOCKS 32u
static uint8_t s_bounce[BOUNCE_BLOCKS * 512u];

static bool     s_present;
static uint8_t  s_addr;              /* the drive taken */
static uint16_t s_vid, s_pid;
static uint32_t s_blocks, s_block_size;
static bool     s_wedged;            /* a read timed out: no more until it goes */
static uint32_t s_generation;        /* counts drives taken */

/* Set in the callbacks, acted on in usb_storage_poll. */
static bool     s_mount_due, s_unmount_due;

static FATFS    s_fs;
static FRESULT  s_mount_result = FR_NOT_READY;
static bool     s_mounted;
static uint32_t s_root_entries;

/* One read at a time, waited for. */
static volatile bool s_done;
static bool     s_ok;

static const char *fs_type_name(BYTE t)
{
   return t == FS_FAT12 ? "FAT12" : t == FS_FAT16 ? "FAT16" :
          t == FS_FAT32 ? "FAT32" : "?";
}

void usb_storage_status(char *buf, size_t len)
{
   if (!s_present) {
      snprintf(buf, len, "none");
      return;
   }
   uint64_t mb = (uint64_t)s_blocks * s_block_size / (1024u * 1024u);
   int n = snprintf(buf, len, "%04x:%04x %lu MB", (unsigned)s_vid,
                    (unsigned)s_pid, (unsigned long)mb);
   if (n < 0 || (size_t)n >= len)
      return;
   if (s_block_size != 512u)
      snprintf(buf + n, len - (size_t)n, ", %lu-byte blocks unusable",
               (unsigned long)s_block_size);
   else if (s_wedged)
      snprintf(buf + n, len - (size_t)n, ", timed out");
   else if (s_mounted)
      snprintf(buf + n, len - (size_t)n, ", %s, %lu in root",
               fs_type_name(s_fs.fs_type), (unsigned long)s_root_entries);
   else
      snprintf(buf + n, len - (size_t)n, ", no volume (%d)", (int)s_mount_result);
}

/* ---- Reading ------------------------------------------------------------ */

bool usb_storage_usable(void)
{
   return s_present && s_block_size == 512u && !s_wedged;
}

static bool read_complete(uint8_t dev_addr, tuh_msc_complete_data_t const *cb)
{
   (void)dev_addr;
   s_ok = cb->csw->status == MSC_CSW_STATUS_PASSED && cb->csw->data_residue == 0u;
   s_done = true;
   return true;
}

static bool read_chunk(uint32_t lba, uint32_t count)
{
   s_done = false;
   s_ok = false;
   if (!tuh_msc_read10(s_addr, 0u, s_bounce, lba, (uint16_t)count, read_complete, 0))
      return false;
   const uint32_t gen = s_generation;
   uint32_t t0 = RPI_GetSystemTime();
   while (!s_done) {
      if (!s_present || s_generation != gen)
         return false;                 /* unplugged: tuh_msc_umount_cb ran */
      if (RPI_GetSystemTime() - t0 > READ_TIMEOUT_US) {
         s_wedged = true;
         return false;
      }
      tuh_int_handler(BOARD_TUH_RHPORT, false);
      tuh_task();
   }
   return s_ok;
}

bool usb_storage_read(uint8_t *buf, uint32_t lba, uint32_t count)
{
   while (count != 0u) {
      if (!usb_storage_usable())
         return false;
      uint32_t n = count < BOUNCE_BLOCKS ? count : BOUNCE_BLOCKS;
      if (!read_chunk(lba, n))
         return false;
      memcpy(buf, s_bounce, n * 512u);
      buf += n * 512u;
      lba += n;
      count -= n;
   }
   return true;
}

/* ---- The volume --------------------------------------------------------- */

static void mount_volume(void)
{
   s_mounted = false;
   s_root_entries = 0u;
   s_mount_result = f_mount(&s_fs, "1:", 1);
   if (s_mount_result != FR_OK)
      return;
   s_mounted = true;
   DIR dir;
   FILINFO fi;
   if (f_opendir(&dir, "1:/") == FR_OK) {
      while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0] != '\0')
         s_root_entries++;
      f_closedir(&dir);
   }
}

void usb_storage_poll(void)
{
   if (s_unmount_due) {
      s_unmount_due = false;
      (void)f_mount(NULL, "1:", 0);    /* no disk access */
      s_mounted = false;
      s_mount_result = FR_NOT_READY;
   }
   if (s_mount_due) {
      s_mount_due = false;
      if (usb_storage_usable())
         mount_volume();
   }
}

/* ---- TinyUSB MSC host callbacks (main loop, from tuh_task) -------------- */

static void adopt(uint8_t dev_addr)
{
   s_addr = dev_addr;
   s_generation++;
   (void)tuh_vid_pid_get(dev_addr, &s_vid, &s_pid);
   s_blocks = tuh_msc_get_block_count(dev_addr, 0u);
   s_block_size = tuh_msc_get_block_size(dev_addr, 0u);
   s_wedged = false;
   s_present = true;
   s_mount_due = true;
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
   s_mount_due = false;
   s_unmount_due = true;

   /* Another drive passed over at its mount: take it up now (as usb_mouse.c
      does for a second mouse).  The going device is still mounted here. */
   for (uint8_t a = 1u; a <= CFG_TUH_DEVICE_MAX; a++)
      if (a != dev_addr && tuh_msc_mounted(a)) {
         adopt(a);
         return;
      }
}
