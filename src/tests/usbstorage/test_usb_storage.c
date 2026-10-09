/* Host tests for usb_storage.c - see run_tests.sh. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usb/tusb_config.h"
#include <tusb.h>
#include "rpi/systimer.h"
#include "rpi/sdcard.h"
#include "BeebSCSI/fatfs/ff.h"
#include "BeebSCSI/fatfs/diskio.h"
#include "usb_storage.h"
#include "watchdog.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* ---- The clock: every read of it moves it on 1 ms ----------------------- */

static uint64_t clock64;
static uint32_t now_us;              /* its low half, as RPI_GetSystemTime */
uint32_t RPI_GetSystemTime(void) { clock64 += 1000u; return now_us = (uint32_t)clock64; }
uint64_t RPI_GetSystemTime64(void) { clock64 += 1000u; now_us = (uint32_t)clock64; return clock64; }

/* ---- The watchdog: fed from inside the waits ---------------------------- */

static int feeds;
void watchdog_feed(void) { feeds++; }

/* ---- The SD card: never there ------------------------------------------- */

static int sd_calls;
int sdhost_init_device(struct block_device **dev) { (void)dev; sd_calls++; return -1; }
size_t sd_read(struct block_device *dev, uint8_t *buf, size_t n, uint32_t b)
{ (void)dev; (void)buf; (void)n; (void)b; sd_calls++; return 0; }
size_t sd_write(struct block_device *dev, const uint8_t *buf, size_t n, uint32_t b)
{ (void)dev; (void)buf; (void)n; (void)b; sd_calls++; return 0; }

/* ---- Fake drives --------------------------------------------------------- */

enum behaviour { ANSWER, NEVER, FAIL, UNPLUG, UNPLUG_SWAP };

typedef struct {
   bool     mounted;
   uint8_t *image;
   uint32_t blocks, block_size;
   enum behaviour how;
} drive_t;

static drive_t drives[CFG_TUH_DEVICE_MAX + 1];

static struct {
   bool busy;
   bool write;
   uint8_t daddr;
   uint8_t *buf;
   uint32_t lba;
   uint16_t count;
   tuh_msc_complete_cb_t cb;
   int ticks;                        /* tuh_task calls before the answer */
} cmd;
static int read10_calls, max_count;

bool tuh_msc_mounted(uint8_t a) { return a <= CFG_TUH_DEVICE_MAX && drives[a].mounted; }
uint32_t tuh_msc_get_block_count(uint8_t a, uint8_t lun) { (void)lun; return drives[a].blocks; }
uint32_t tuh_msc_get_block_size(uint8_t a, uint8_t lun) { (void)lun; return drives[a].block_size; }
bool tuh_vid_pid_get(uint8_t a, uint16_t *vid, uint16_t *pid) { *vid = 0x1234; *pid = a; return true; }
void tuh_int_handler(uint8_t rhport, bool in_isr) { (void)rhport; (void)in_isr; }

bool tuh_msc_read10(uint8_t a, uint8_t lun, void *buffer, uint32_t lba,
                    uint16_t count, tuh_msc_complete_cb_t cb, uintptr_t arg)
{
   (void)lun; (void)arg;
   if (!tuh_msc_mounted(a) || cmd.busy)
      return false;
   read10_calls++;
   if (count > max_count)
      max_count = count;
   cmd = (typeof(cmd)){ true, false, a, buffer, lba, count, cb, 3 };
   return true;
}

static int write10_calls;
bool tuh_msc_write10(uint8_t a, uint8_t lun, void const *buffer, uint32_t lba,
                     uint16_t count, tuh_msc_complete_cb_t cb, uintptr_t arg)
{
   (void)lun; (void)arg;
   if (!tuh_msc_mounted(a) || cmd.busy)
      return false;
   write10_calls++;
   if (count > max_count)
      max_count = count;
   cmd = (typeof(cmd)){ true, true, a, (uint8_t *)(uintptr_t)buffer, lba, count, cb, 3 };
   return true;
}

static void unplug(uint8_t a)
{
   tuh_msc_umount_cb(a);             /* called while still mounted, as TinyUSB does */
   drives[a].mounted = false;
   if (cmd.busy && cmd.daddr == a)
      cmd.busy = false;
}

void tuh_task(void)
{
   if (!cmd.busy || --cmd.ticks > 0)
      return;
   drive_t *d = &drives[cmd.daddr];
   switch (d->how) {
   case NEVER:
      return;
   case UNPLUG:
      unplug(cmd.daddr);
      return;
   case UNPLUG_SWAP:
      drives[cmd.daddr + 1].mounted = true;   /* the other drive is there already */
      unplug(cmd.daddr);
      return;
   case ANSWER:
   case FAIL:
      break;
   }
   msc_cbw_t cbw = { 0, cmd.count * 512u };
   msc_csw_t csw = { 0, 0, 0, d->how == FAIL ? MSC_CSW_STATUS_FAILED : MSC_CSW_STATUS_PASSED };
   if (d->how == ANSWER && cmd.write)
      memcpy(d->image + (size_t)cmd.lba * 512u, cmd.buf, cmd.count * 512u);
   else if (d->how == ANSWER)
      memcpy(cmd.buf, d->image + (size_t)cmd.lba * 512u, cmd.count * 512u);
   tuh_msc_complete_data_t data = { &cbw, &csw, NULL, 0 };
   cmd.busy = false;
   cmd.cb(cmd.daddr, &data);
}

/* ---- usb.c's usb_service: the host's work, plus a drive arriving ------- */

static int service_calls;
static int arrive_after = -1;        /* usb_service calls before drive 2 mounts */
static bool usb_is_device;
bool usb_service(void)
{
   if (usb_is_device)
      return false;
   service_calls++;
   tuh_task();
   if (arrive_after >= 0 && service_calls == arrive_after) {
      drives[2].mounted = true;
      tuh_msc_mount_cb(2);
   }
   usb_storage_poll();
   return true;
}

/* ---- A FAT12 1.44 MB image ------------------------------------------------
   HELLO.TXT (cluster 2), A.TXT (3), B.TXT (4), BIG.BIN (5-44, 40 sectors, a
   pattern of its sector numbers). */

#define IMG_BLOCKS 2880u
#define DATA0      33u                /* the sector of cluster 2 */
static const char hello[] = "Hello from the USB drive\n";

static void fat12_set(uint8_t *fat, unsigned n, unsigned v)
{
   unsigned off = n + n / 2u;
   if (n & 1u) {
      fat[off]     = (uint8_t)((fat[off] & 0x0Fu) | (v << 4));
      fat[off + 1] = (uint8_t)(v >> 4);
   } else {
      fat[off]     = (uint8_t)v;
      fat[off + 1] = (uint8_t)((fat[off + 1] & 0xF0u) | ((v >> 8) & 0x0Fu));
   }
}

static void dirent(uint8_t *e, const char name[11], unsigned cluster, unsigned size)
{
   memcpy(e, name, 11);
   e[11] = 0x20;
   e[26] = (uint8_t)cluster; e[27] = (uint8_t)(cluster >> 8);
   e[28] = (uint8_t)size; e[29] = (uint8_t)(size >> 8);
   e[30] = (uint8_t)(size >> 16); e[31] = (uint8_t)(size >> 24);
}

static uint8_t *make_image(void)
{
   uint8_t *img = calloc(IMG_BLOCKS, 512u);
   uint8_t *b = img;
   b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;
   memcpy(b + 3, "MSDOS5.0", 8);
   b[11] = 0x00; b[12] = 0x02;       /* 512 bytes a sector */
   b[13] = 1;                        /* a sector a cluster */
   b[14] = 1;                        /* reserved */
   b[16] = 2;                        /* FATs */
   b[17] = 224;                      /* root entries */
   b[19] = (uint8_t)IMG_BLOCKS; b[20] = (uint8_t)(IMG_BLOCKS >> 8);
   b[21] = 0xF0;
   b[22] = 9;                        /* sectors a FAT */
   b[24] = 18; b[26] = 2;
   b[38] = 0x29;
   memcpy(b + 54, "FAT12   ", 8);
   b[510] = 0x55; b[511] = 0xAA;
   for (unsigned f = 0; f < 2u; f++) {
      uint8_t *fat = img + (1u + f * 9u) * 512u;
      fat12_set(fat, 0, 0xFF0);
      fat12_set(fat, 1, 0xFFF);
      fat12_set(fat, 2, 0xFFF);
      fat12_set(fat, 3, 0xFFF);
      fat12_set(fat, 4, 0xFFF);
      for (unsigned c = 5; c < 44u; c++)
         fat12_set(fat, c, c + 1u);
      fat12_set(fat, 44, 0xFFF);
   }
   uint8_t *root = img + 19u * 512u;
   dirent(root,       "HELLO   TXT", 2, sizeof hello - 1u);
   dirent(root + 32,  "A       TXT", 3, 1);
   dirent(root + 64,  "B       TXT", 4, 1);
   dirent(root + 96,  "BIG     BIN", 5, 40u * 512u);
   memcpy(img + DATA0 * 512u, hello, sizeof hello - 1u);
   img[(DATA0 + 1u) * 512u] = 'A';
   img[(DATA0 + 2u) * 512u] = 'B';
   for (unsigned s = 0; s < 40u; s++)
      memset(img + (DATA0 + 3u + s) * 512u, (int)(s + 1u), 512u);
   return img;
}

/* ---- Helpers ---------------------------------------------------------------- */

static uint8_t *image;

static void plug(uint8_t a, enum behaviour how, uint32_t block_size)
{
   drives[a] = (drive_t){ true, image, IMG_BLOCKS, block_size, how };
   tuh_msc_mount_cb(a);
}

static void pull(uint8_t a)
{
   unplug(a);
   usb_storage_poll();
}

static const char *status(void)
{
   static char buf[96];
   usb_storage_status(buf, sizeof buf);
   return buf;
}

static bool status_has(const char *s)
{
   bool ok = strstr(status(), s) != NULL;
   if (!ok)
      printf("  status \"%s\" has no \"%s\"\n", status(), s);
   return ok;
}

/* ---- Tests ----------------------------------------------------------------- */

static void test_no_drive(void)
{
   CHECK(strcmp(status(), "none") == 0);
   CHECK(!usb_storage_usable());
   CHECK(disk_status(1) & STA_NOINIT);
   uint8_t buf[512];
   CHECK(disk_read(1, buf, 0, 1) == RES_ERROR);
}

static void test_mount_and_read(void)
{
   plug(2, ANSWER, 512u);
   CHECK(strstr(status(), "no volume") != NULL);   /* not until the poll */
   usb_storage_poll();
   CHECK(status_has("1234:0002 1 MB, FAT12, 4 in root"));
   CHECK(usb_storage_mounted());

   FIL f;
   char buf[64] = {0};
   UINT got = 0;
   CHECK(f_open(&f, "1:/HELLO.TXT", FA_READ) == FR_OK);
   CHECK(f_read(&f, buf, sizeof buf, &got) == FR_OK);
   CHECK(got == sizeof hello - 1u && memcmp(buf, hello, got) == 0);
   f_close(&f);

   /* Un-prefixed paths are still the SD card's "0:", which has no volume
      here: the drive's mount did not register one or touch the card. */
   CHECK(f_open(&f, "HELLO.TXT", FA_READ) == FR_NOT_ENABLED);
   CHECK(sd_calls == 0);
}

static void test_chunks(void)
{
   /* BIG.BIN's 40 sectors in one disk_read (FatFs asks for up to a
      cluster's worth at once) go to the drive in pieces no bigger than the
      bounce buffer. */
   static uint8_t buf[40u * 512u];
   max_count = 0;
   int calls = read10_calls;
   CHECK(disk_read(1, buf, DATA0 + 3u, 40u) == RES_OK);
   CHECK(read10_calls - calls == 2);
   bool same = true;
   for (unsigned s = 0; s < 40u; s++)
      for (unsigned i = 0; i < 512u; i++)
         same = same && buf[s * 512u + i] == (uint8_t)(s + 1u);
   CHECK(same);
   CHECK(max_count == 32);
}

static void test_write(void)
{
   /* Writable: a new file through FatFs lands on the drive's image, in
      pieces no bigger than the bounce buffer, and reads back. */
   FIL f;
   UINT n = 0;
   static uint8_t data[40u * 512u];
   for (unsigned i = 0; i < sizeof data; i++)
      data[i] = (uint8_t)(i * 7u);
   CHECK(f_open(&f, "1:/NEW.BIN", FA_WRITE | FA_CREATE_NEW) == FR_OK);
   CHECK(f_write(&f, data, sizeof data, &n) == FR_OK && n == sizeof data);
   CHECK(f_close(&f) == FR_OK);
   static uint8_t back[sizeof data];
   CHECK(f_open(&f, "1:/NEW.BIN", FA_READ) == FR_OK);
   CHECK(f_read(&f, back, sizeof back, &n) == FR_OK && n == sizeof back);
   f_close(&f);
   CHECK(memcmp(back, data, sizeof data) == 0);
   CHECK(f_unlink("1:/NEW.BIN") == FR_OK);

   /* The raw path: 40 sectors in two commands, the image changed. */
   max_count = 0;
   int calls = write10_calls;
   CHECK(disk_write(1, data, 2000u, 40u) == RES_OK);
   CHECK(write10_calls - calls == 2 && max_count == 32);
   CHECK(memcmp(image + 2000u * 512u, data, sizeof data) == 0);
}

static void test_write_timeout(void)
{
   /* A write that never completes is a timeout, as a read: the drive is
      not used again until it is replugged. */
   drives[2].how = NEVER;
   uint8_t buf[512] = {0};
   CHECK(usb_storage_write(buf, 2000u, 1) == false);
   CHECK(!usb_storage_usable());
   CHECK(disk_write(1, buf, 2000u, 1) == RES_ERROR);
   drives[2].how = ANSWER;
   pull(2);
   cmd.busy = false;
}

static void test_wait_for_drive(void)
{
   /* Power-on: the drive turns up while the decision waits. */
   CHECK(!usb_storage_mounted());
   service_calls = 0;
   arrive_after = 5;
   CHECK(usb_storage_wait_for_drive(clock64 + 5000000u) == true);
   CHECK(service_calls == 5);
   CHECK(usb_storage_mounted());
   CHECK(status_has("FAT12"));
   arrive_after = -1;

   /* Already there: no waiting at all. */
   service_calls = 0;
   CHECK(usb_storage_wait_for_drive(clock64 + 5000000u) == true && service_calls == 0);
   pull(2);

   /* No drive: until the time given, then no. */
   uint32_t t0 = now_us;
   CHECK(usb_storage_wait_for_drive(clock64 + 200000u) == false);
   CHECK(now_us - t0 >= 200000u && now_us - t0 < 300000u);

   /* The time already past: no waiting. */
   service_calls = 0;
   CHECK(usb_storage_wait_for_drive(clock64 - 1000u) == false && service_calls == 0);

   /* Long past: 2^31 us and more after the deadline (a BREAK 36 minutes on).
      A 32-bit difference sees that as before it again and waited. */
   uint64_t deadline = clock64;
   clock64 += 0x80000000ull + 5000u;
   service_calls = 0;
   CHECK(usb_storage_wait_for_drive(deadline) == false && service_calls == 0);

   /* USB a device, not a host: nothing to wait for. */
   usb_is_device = true;
   t0 = now_us;
   CHECK(usb_storage_wait_for_drive(clock64 + 5000000u) == false);
   CHECK(now_us - t0 < 100000u);
   usb_is_device = false;
}

static void test_failed_command(void)
{
   /* A CHECK CONDITION is a read error, not a dead drive. */
   drives[2].how = FAIL;
   uint8_t buf[512];
   CHECK(usb_storage_read(buf, 0, 1) == false);
   CHECK(usb_storage_usable());
   drives[2].how = ANSWER;
   CHECK(usb_storage_read(buf, 0, 1) == true && buf[510] == 0x55);
}

static void test_timeout(void)
{
   drives[2].how = NEVER;
   uint8_t buf[512];
   uint32_t t0 = now_us;
   CHECK(usb_storage_read(buf, 0, 1) == false);
   CHECK(now_us - t0 >= 10000000u && now_us - t0 < 10100000u);
   CHECK(feeds > 0);                          /* a short watchdog= survives it */
   CHECK(!usb_storage_usable());
   CHECK(status_has("timed out"));
   /* No more commands to it: it still holds the last. */
   int calls = read10_calls;
   CHECK(usb_storage_read(buf, 0, 1) == false);
   CHECK(read10_calls == calls);
   CHECK(disk_status(1) & STA_NOINIT);

   /* Pulled out and back in: usable again. */
   pull(2);
   CHECK(strcmp(status(), "none") == 0);
   cmd.busy = false;
   plug(3, ANSWER, 512u);
   usb_storage_poll();
   CHECK(status_has("FAT12, 4 in root"));
}

static void test_unplug_mid_read(void)
{
   drives[3].how = UNPLUG;
   uint8_t buf[512];
   uint32_t t0 = now_us;
   CHECK(usb_storage_read(buf, 0, 1) == false);
   CHECK(now_us - t0 < 100000u);              /* at once, not a timeout */
   usb_storage_poll();
   CHECK(strcmp(status(), "none") == 0);
   FIL f;
   CHECK(f_open(&f, "1:/HELLO.TXT", FA_READ) == FR_NOT_ENABLED);  /* forgotten */
}

static void test_unplug_mid_read_other_taken(void)
{
   /* The drive goes mid-read and a second, already there, is taken in its
      umount callback: the read ends at once and the new drive is not
      marked timed out by it. */
   plug(4, ANSWER, 512u);
   usb_storage_poll();
   drives[5] = (drive_t){ false, image, IMG_BLOCKS, 512u, ANSWER };
   drives[4].how = UNPLUG_SWAP;
   uint8_t buf[512];
   uint32_t t0 = now_us;
   CHECK(usb_storage_read(buf, 0, 1) == false);
   CHECK(now_us - t0 < 100000u);
   CHECK(usb_storage_usable());
   usb_storage_poll();
   CHECK(status_has("1234:0005 1 MB, FAT12, 4 in root"));
   pull(5);
}

static void test_second_drive_waits(void)
{
   plug(2, ANSWER, 512u);
   plug(6, ANSWER, 512u);                     /* passed over */
   usb_storage_poll();
   CHECK(status_has("1234:0002"));
   pull(2);
   CHECK(status_has("1234:0006"));             /* taken when the first goes */
   CHECK(status_has("FAT12"));
   pull(6);
}

static void test_big_blocks(void)
{
   plug(7, ANSWER, 4096u);
   usb_storage_poll();
   CHECK(status_has("4096-byte blocks unusable"));
   CHECK(!usb_storage_usable());
   FIL f;
   CHECK(f_open(&f, "1:/HELLO.TXT", FA_READ) == FR_NOT_ENABLED);  /* never mounted */
   CHECK(disk_status(1) & STA_NOINIT);
   pull(7);
}

int main(void)
{
   image = make_image();
   test_no_drive();
   test_mount_and_read();
   test_chunks();
   test_write();
   test_failed_command();
   test_timeout();
   test_unplug_mid_read();
   test_unplug_mid_read_other_taken();
   test_second_drive_waits();
   test_big_blocks();
   plug(2, ANSWER, 512u);
   usb_storage_poll();
   test_write_timeout();
   test_wait_for_drive();
   free(image);
   if (failures) {
      printf("usbstorage: %d failed\n", failures);
      return 1;
   }
   printf("usbstorage: all passed\n");
   return 0;
}
