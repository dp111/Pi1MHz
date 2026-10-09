/* usb_storage.c - a USB flash drive on the Pi's port in host mode.

   TinyUSB's MSC host class enumerates the drive - TEST UNIT READY, then READ
   CAPACITY of LUN 0 - and reports it here from the main loop (tuh_task).
   One drive is taken; any others are passed over until it goes.  The drive
   is FatFs volume "1:" (docs/dev/usb-flash-storage-plan.md); with
   storage=usb the Beeb's storage comes from it (filesystemStorageRoot).

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
#include "usb.h"
#include "watchdog.h"
#include "usb_storage.h"

/* A drive's own housekeeping can hold a command for a second or more: a
   Kingston DataTraveler (0951:1666) took up to 1665 ms for one 16 KB write,
   22 of 42521 over 100 ms, while 330 MB was copied onto it (reads, 3 ms at
   most).  The Beeb has no timeouts, so a slow answer beats an error, and the
   wait feeds the watchdog: the limit is only for a drive that has stopped. */
#define COMMAND_TIMEOUT_US 10000000u

/* Transfers go through here, not the caller's buffer: after a timeout the
   command is still queued with this buffer, and nothing can take it back. */
#define BOUNCE_BLOCKS 32u
static uint8_t s_bounce[BOUNCE_BLOCKS * 512u];

static bool     s_present;
static uint8_t  s_addr;              /* the drive taken */
static uint16_t s_vid, s_pid;
static uint32_t s_blocks, s_block_size;
static bool     s_wedged;            /* a command timed out: no more until it goes */
static uint32_t s_generation;        /* counts drives taken */

/* Set in the callbacks, acted on in usb_storage_poll. */
static bool     s_mount_due, s_unmount_due;

static FATFS    s_fs;
static FRESULT  s_mount_result = FR_NOT_READY;
static bool     s_mounted;
static uint32_t s_root_entries;

/* One command at a time, waited for. */
static volatile bool s_done;
static bool     s_ok;
static uint32_t s_boot_wait_ms;      /* /status: the power-on wait, if any */

static const char *fs_type_name(BYTE t)
{
   return t == FS_FAT12 ? "FAT12" : t == FS_FAT16 ? "FAT16" :
          t == FS_FAT32 ? "FAT32" : t == FS_EXFAT ? "exFAT" : "?";
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
      snprintf(buf + n, len - (size_t)n, ", %s, %lu in root, boot wait %lu ms",
               fs_type_name(s_fs.fs_type), (unsigned long)s_root_entries,
               (unsigned long)s_boot_wait_ms);
   else
      snprintf(buf + n, len - (size_t)n, ", no volume (%d)", (int)s_mount_result);
}

/* ---- Reading and writing ----------------------------------------------- */

bool usb_storage_usable(void)
{
   return s_present && s_block_size == 512u && !s_wedged;
}

bool usb_storage_mounted(void)
{
   return s_mounted && usb_storage_usable();
}

static bool command_complete(uint8_t dev_addr, tuh_msc_complete_data_t const *cb)
{
   (void)dev_addr;
   s_ok = cb->csw->status == MSC_CSW_STATUS_PASSED && cb->csw->data_residue == 0u;
   s_done = true;
   return true;
}

/* Wait for the command just issued, running the USB host meanwhile. */
static bool command_wait(void)
{
   const uint32_t gen = s_generation;
   uint32_t t0 = RPI_GetSystemTime();
   while (!s_done) {
      if (!s_present || s_generation != gen)
         return false;                 /* unplugged: tuh_msc_umount_cb ran */
      if (RPI_GetSystemTime() - t0 > COMMAND_TIMEOUT_US) {
         s_wedged = true;
         return false;
      }
      tuh_int_handler(BOARD_TUH_RHPORT, false);
      tuh_task();
      watchdog_feed();
   }
   return s_ok;
}

bool usb_storage_read(uint8_t *buf, uint32_t lba, uint32_t count)
{
   while (count != 0u) {
      if (!usb_storage_usable())
         return false;
      uint32_t n = count < BOUNCE_BLOCKS ? count : BOUNCE_BLOCKS;
      s_done = false;
      if (!tuh_msc_read10(s_addr, 0u, s_bounce, lba, (uint16_t)n, command_complete, 0) ||
          !command_wait())
         return false;
      memcpy(buf, s_bounce, n * 512u);
      buf += n * 512u;
      lba += n;
      count -= n;
   }
   return true;
}

bool usb_storage_write(const uint8_t *buf, uint32_t lba, uint32_t count)
{
   while (count != 0u) {
      if (!usb_storage_usable())
         return false;
      uint32_t n = count < BOUNCE_BLOCKS ? count : BOUNCE_BLOCKS;
      memcpy(s_bounce, buf, n * 512u);
      s_done = false;
      if (!tuh_msc_write10(s_addr, 0u, s_bounce, lba, (uint16_t)n, command_complete, 0) ||
          !command_wait())
         return false;
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

bool usb_storage_wait_for_drive(uint64_t until_us)
{
   /* 64-bit: a 32-bit deadline compared by difference comes back into the
      window 2^31 us (35.8 min) after it passes, and a BREAK then would wait
      up to that long for a drive that is not there. */
   uint32_t t0 = RPI_GetSystemTime();
   while (!usb_storage_mounted()) {
      if (RPI_GetSystemTime64() >= until_us)
         break;
      if (!usb_service())
         break;                        /* USB is a device, not a host */
      watchdog_feed();
   }
   s_boot_wait_ms = (RPI_GetSystemTime() - t0) / 1000u;
   return usb_storage_mounted();
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
