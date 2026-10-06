/* Host tests for the MTP session lifecycle in usb/mtp_fs.c (Review
   2026-10-06 U3).

   The MTP callbacks are driven the way TinyUSB's class driver drives them
   (mtp_device.c: a command, then data OUT/IN, then the completion), and the
   device-level events the way usbd.c raises them: tud_umount_cb on
   UNPLUGGED (or SET_CONFIGURATION 0), tud_mount_cb on every SET_CONFIGURATION
   - which follows each bus reset, so a re-enumeration with no UNPLUGGED
   seen still reaches it - tud_suspend_cb on a bus suspend, and the class's
   Device Reset callback.  Two RAM disks stand for two SD cards. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tusb.h>
#include <device/usbd_pvt.h>

#include "mtp_stubs.h"
#include "usb/mtp_fs.h"
#include "BeebSCSI/fatfs/ff.h"
#include "BeebSCSI/fatfs/diskio.h"

/* ---- TinyUSB's weak device callbacks (usbd.c), for mtp_fs.c to override */
TU_ATTR_WEAK void tud_mount_cb(void) {}
TU_ATTR_WEAK void tud_umount_cb(void) {}
TU_ATTR_WEAK void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; }

/* ---- allocations made by mtp_fs.c (-Dmalloc/-Dfree) ---------------------- */
#define BIG_ALLOC (1024u * 1024u)       /* the kernel.now buffer, nothing else */
static void *big_ptr;
void *test_malloc(size_t n)
{
   void *p = malloc(n);
   if (p && n >= BIG_ALLOC)
      big_ptr = p;
   return p;
}
void test_free(void *p)
{
   if (p && p == big_ptr)
      big_ptr = NULL;
   free(p);
}

/* ---- RAM disks: card A and card B --------------------------------------- */
#define DISK_SECTORS (64u * 1024u * 2u)          /* 64 MB of 512-byte sectors */
static uint8_t *card_a, *card_b, *disk;

DSTATUS disk_initialize(BYTE pdrv) { (void)pdrv; return 0; }
DSTATUS disk_status(BYTE pdrv) { (void)pdrv; return 0; }
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
   (void)pdrv;
   if (sector + count > DISK_SECTORS) return RES_PARERR;
   memcpy(buff, disk + (size_t)sector * 512u, (size_t)count * 512u);
   return RES_OK;
}
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
   (void)pdrv;
   if (sector + count > DISK_SECTORS) return RES_PARERR;
   memcpy(disk + (size_t)sector * 512u, buff, (size_t)count * 512u);
   return RES_OK;
}
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
   (void)pdrv;
   switch (cmd) {
   case CTRL_SYNC:        return RES_OK;
   case GET_SECTOR_COUNT: *(LBA_t *)buff = DISK_SECTORS; return RES_OK;
   case GET_SECTOR_SIZE:  *(WORD *)buff = 512; return RES_OK;
   case GET_BLOCK_SIZE:   *(DWORD *)buff = 1; return RES_OK;
   default:               return RES_PARERR;
   }
}

/* ---- the filesystem layer around FatFs (BeebSCSI/filesystem.c) ---------- */
static FATFS fso;
static bool fs_mounted, fs_ejected;
static bool lun_locked[16];
#define LUN_PATH "/scsi3.dat"            /* the one host path that is a LUN image */
#define LUN_NUM  3

bool filesystemMount(void)
{
   if (fs_mounted) return true;
   if (fs_ejected) return false;
   fs_mounted = (f_mount(&fso, "", 1) == FR_OK);
   return fs_mounted;
}
bool filesystemReadLunStatus(uint8_t lun) { (void)lun; return false; }
bool filesystemHostPathBusy(const char *path) { (void)path; return false; }
int8_t filesystemLunFromHostPath(const char *path)
{
   return strcmp(path, LUN_PATH) == 0 ? LUN_NUM : -1;
}
void filesystemHostLockLun(int8_t lun, bool lock)
{
   if (lun >= 0 && lun < 16) lun_locked[lun] = lock;
}
bool filesystemHostLunRevoked(uint8_t lun) { (void)lun; return false; }
bool fat_service_file_in_use(const char *p) { (void)p; return false; }
bool beeb_path_busy(const char *p) { (void)p; return false; }

/* filesystemEject / filesystemInsert, as far as MTP sees them: the eject
   hook first, then the volume goes; a mount of the next card, then the
   inserted hook. */
static void card_eject(void)
{
   if (!mtp_fs_eject()) { printf("FAIL: mtp_fs_eject refused\n"); exit(1); }
   fs_ejected = true;
   fs_mounted = false;
   (void)f_mount(NULL, "", 0);
}
static void card_insert(uint8_t *card)
{
   disk = card;
   fs_ejected = false;
   if (!filesystemMount()) { printf("FAIL: card did not mount\n"); exit(1); }
   mtp_fs_inserted();
}

/* ---- platform ------------------------------------------------------------ */
static uint32_t now_us;
uint32_t RPI_GetSystemTime(void) { return now_us; }
size_t board_usb_get_serial(uint16_t s[], size_t n) { (void)s; (void)n; return 0; }
bool webserver_sd_space_now(uint64_t *t, uint64_t *f) { (void)t; (void)f; return false; }
_Noreturn void reboot_now(void) { printf("FAIL: reboot_now\n"); exit(1); }
bool chainboot_image_ok(const uint8_t *i, uint32_t l) { (void)i; (void)l; return true; }
const char *chainboot_refusal(void) { return NULL; }
bool chainboot_request(uint8_t *i, uint32_t l, uint32_t c) { (void)l; (void)c; test_free(i); return true; }

/* ---- TinyUSB device stack, as far as mtp_fs.c calls it ------------------- */
static bool usb_configured = true;          /* tud_mounted() */
static bool usb_suspended;
static int  event_xfers;                    /* transfers queued on the event EP */
static bool event_claimed;

bool tud_inited(void) { return true; }
bool tud_mounted(void) { return usb_configured; }
bool tud_suspended(void) { return usb_suspended; }
bool tud_connect(void) { return true; }
bool tud_disconnect(void) { return true; }
bool usbd_edpt_claim(uint8_t rhport, uint8_t ep)
{
   (void)rhport; (void)ep;
   if (event_claimed) return false;
   event_claimed = true;
   return true;
}
bool usbd_edpt_release(uint8_t rhport, uint8_t ep) { (void)rhport; (void)ep; event_claimed = false; return true; }
bool usbd_edpt_xfer(uint8_t rhport, uint8_t ep, uint8_t *buf, uint16_t n, bool isr)
{
   (void)rhport; (void)ep; (void)buf; (void)n; (void)isr;
   event_xfers++;
   event_claimed = false;               /* the host takes it at once */
   return true;
}

static uint16_t resp_code;                  /* 0: no response sent */
static bool data_in_sent, data_out_armed;
static uint8_t data_in[CFG_TUD_MTP_EP_BUFSIZE];

bool tud_mtp_data_send(mtp_container_info_t *c)
{
   uint32_t n = c->header->len < sizeof data_in ? c->header->len : sizeof data_in;
   memcpy(data_in, c->header, n);
   data_in_sent = true;
   return true;
}
bool tud_mtp_data_receive(mtp_container_info_t *c) { (void)c; data_out_armed = true; return true; }
bool tud_mtp_response_send(mtp_container_info_t *c) { resp_code = c->header->code; return true; }

/* ---- the class driver's side of a transaction (mtp_device.c) ------------- */
static mtp_container_command_t command;
static uint8_t epbuf[CFG_TUD_MTP_EP_BUFSIZE];
static uint32_t xferred;                    /* bytes of this data phase so far */

static tud_mtp_cb_data_t cb_for(uint8_t phase)
{
   tud_mtp_cb_data_t cb = {
      .phase = phase,
      .command_container = &command,
      .io_container = {
         .header = (mtp_container_header_t *)epbuf,
         .payload = epbuf + sizeof(mtp_container_header_t),
         .payload_bytes = sizeof epbuf - sizeof(mtp_container_header_t),
      },
      .xfer_result = XFER_RESULT_SUCCESS,
   };
   return cb;
}

/* Run a command.  Returns its response code, or 0 when it is waiting for a
   data OUT phase; a data IN phase is completed here (data_in holds it). */
static uint16_t op(uint16_t code, uint32_t p0, uint32_t p1, uint32_t p2)
{
   memset(&command, 0, sizeof command);
   command.header.len = sizeof(mtp_container_header_t) + 12u;
   command.header.type = MTP_CONTAINER_TYPE_COMMAND_BLOCK;
   command.header.code = code;
   command.params[0] = p0; command.params[1] = p1; command.params[2] = p2;
   memset(epbuf, 0, sizeof epbuf);
   ((mtp_container_header_t *)epbuf)->len = sizeof(mtp_container_header_t);
   resp_code = 0; data_in_sent = data_out_armed = false; xferred = 0;

   tud_mtp_cb_data_t cb = cb_for(MTP_PHASE_COMMAND);
   (void)tud_mtp_command_received_cb(&cb);
   if (resp_code || !data_in_sent)
      return resp_code;
   cb = cb_for(MTP_PHASE_DATA_COMPLETE);
   cb.io_container.header->len = sizeof(mtp_container_header_t);
   (void)tud_mtp_data_complete_cb(&cb);
   return resp_code;
}

/* One OUT packet of the data phase; `total` is the host's container length
   (header + payload).  Returns the response once the phase is complete. */
static uint16_t data_out(const void *payload, uint32_t n, uint32_t total)
{
   bool first = (xferred == 0);
   tud_mtp_cb_data_t cb = cb_for(MTP_PHASE_DATA);
   if (first) {
      mtp_container_header_t *h = cb.io_container.header;
      h->len = total;
      h->type = MTP_CONTAINER_TYPE_DATA_BLOCK;
      h->code = command.header.code;
      memcpy(cb.io_container.payload, payload, n);
      cb.io_container.payload_bytes = n;
      xferred = sizeof(mtp_container_header_t) + n;
   } else {
      cb.io_container.payload = epbuf;
      memcpy(epbuf, payload, n);
      cb.io_container.payload_bytes = n;
      xferred += n;
   }
   cb.total_xferred_bytes = xferred;
   resp_code = 0;
   (void)tud_mtp_data_xfer_cb(&cb);
   if (resp_code || xferred < total)
      return resp_code;
   cb = cb_for(MTP_PHASE_DATA_COMPLETE);
   cb.io_container.header->len = sizeof(mtp_container_header_t);
   (void)tud_mtp_data_complete_cb(&cb);
   return resp_code;
}

/* SendObjectInfo for a file `name` of `size` bytes in the root. */
static uint16_t send_object_info(const char *name, uint32_t size)
{
   uint8_t ds[256];
   mtp_object_info_header_t h = {
      .storage_id = 0x00010001u, .object_format = MTP_OBJ_FORMAT_UNDEFINED,
      .object_compressed_size = size, .parent_object = 0u,
   };
   size_t k = 0, len = strlen(name);
   memcpy(ds, &h, sizeof h); k = sizeof h;
   ds[k++] = (uint8_t)(len + 1u);
   for (size_t i = 0; i <= len; i++) { ds[k++] = (uint8_t)name[i]; ds[k++] = 0; }
   ds[k++] = 0; ds[k++] = 0; ds[k++] = 0;           /* created, modified, keywords */
   if (op(MTP_OP_SEND_OBJECT_INFO, 0x00010001u, 0xFFFFFFFFu, 0) != 0 || !data_out_armed)
      return 0xFFFF;
   return data_out(ds, (uint32_t)k, (uint32_t)(sizeof(mtp_container_header_t) + k));
}

/* SendObject, first packet only: the host is mid-copy. */
static bool send_object_first_packet(uint32_t size)
{
   static uint8_t body[CFG_TUD_MTP_EP_BUFSIZE];
   memset(body, 0xA5, sizeof body);
   if (op(MTP_OP_SEND_OBJECT, 0, 0, 0) != 0 || !data_out_armed)
      return false;
   data_out_armed = false;
   uint32_t n = CFG_TUD_MTP_EP_BUFSIZE - sizeof(mtp_container_header_t);
   return data_out(body, n, sizeof(mtp_container_header_t) + size) == 0 && data_out_armed;
}

/* Names of the root's objects, as the host would list them. */
static int root_names(char names[][32], int max)
{
   if (op(MTP_OP_GET_OBJECT_HANDLES, 0x00010001u, 0, 0xFFFFFFFFu) != MTP_RESP_OK)
      return -1;
   uint32_t count, handles[16];
   memcpy(&count, data_in + 12, 4);
   if (count > 16) count = 16;
   memcpy(handles, data_in + 16, count * 4u);
   int n = 0;
   for (uint32_t i = 0; i < count && n < max; i++) {
      if (op(MTP_OP_GET_OBJECT_INFO, handles[i], 0, 0) != MTP_RESP_OK)
         continue;
      const uint8_t *s = data_in + 12 + sizeof(mtp_object_info_header_t);
      int j;
      for (j = 0; j < s[0] - 1 && j < 31; j++)
         names[n][j] = (char)s[1 + 2 * j];
      names[n][j] = '\0';
      n++;
   }
   return n;
}

static bool exists(const char *path)
{
   FILINFO fno;
   return f_stat(path, &fno) == FR_OK;
}

static void make_file(const char *path)
{
   FIL f;
   UINT bw;
   if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK ||
       f_write(&f, "x", 1, &bw) != FR_OK || f_close(&f) != FR_OK) {
      printf("FAIL: could not create %s\n", path);
      exit(1);
   }
}

/* ---- harness ------------------------------------------------------------- */
static int failures, passes;
static void check(const char *name, bool ok, const char *fmt, ...)
{
   if (ok) { passes++; printf("ok   %s\n", name); return; }
   failures++;
   printf("FAIL %s: ", name);
   va_list ap;
   va_start(ap, fmt);
   vprintf(fmt, ap);
   va_end(ap);
   printf("\n");
}

/* Back to a plugged-in, configured device with no session, whatever the
   test before left: a Device Reset/re-enumeration on the fixed code, a
   CloseSession on the old one. */
static void fresh(void)
{
   usb_configured = true; usb_suspended = false;
   tud_mount_cb();
   (void)op(MTP_OP_CLOSE_SESSION, 0, 0, 0);
   memset(lun_locked, 0, sizeof lun_locked);
}

/* An upload into a LUN image is under way: session open, the ".part"
   created, the LUN locked, one packet of the body in. */
static bool upload_under_way(void)
{
   if (op(MTP_OP_OPEN_SESSION, 1, 0, 0) != MTP_RESP_OK) return false;
   if (send_object_info(LUN_PATH + 1, 100000u) != MTP_RESP_OK) return false;
   if (!exists(LUN_PATH ".part") || !lun_locked[LUN_NUM]) return false;
   return send_object_first_packet(100000u);
}

static void check_upload_dropped(const char *what)
{
   char name[96];
   snprintf(name, sizeof name, "%s: the .part is removed", what);
   check(name, !exists(LUN_PATH ".part"), "%s still on the card", LUN_PATH ".part");
   snprintf(name, sizeof name, "%s: the LUN host lock is released", what);
   check(name, !lun_locked[LUN_NUM], "LUN %d still locked", LUN_NUM);
   snprintf(name, sizeof name, "%s: the target is untouched", what);
   check(name, !exists(LUN_PATH), "%s was created", LUN_PATH);
}

static void test_unplug_mid_upload(void)
{
   fresh();
   check("unplug: upload under way", upload_under_way(), "setup failed");
   usb_configured = false;                  /* DCD_EVENT_UNPLUGGED */
   tud_umount_cb();
   check_upload_dropped("unplug");

   int before = event_xfers;
   mtp_fs_notify_object_added("/x");        /* WebDAV wrote a file meanwhile */
   check("unplug: no event is sent to an unconfigured endpoint",
         event_xfers == before, "%d event(s) queued", event_xfers - before);

   usb_configured = true;                   /* plugged in again */
   tud_mount_cb();
   uint16_t r = op(MTP_OP_OPEN_SESSION, 1, 0, 0);
   check("unplug: the next OpenSession succeeds", r == MTP_RESP_OK, "response 0x%04x", r);
}

/* The Pi may never see UNPLUGGED (no VBUS sense): the replug's bus reset
   and SET_CONFIGURATION are all it gets. */
static void test_reenumeration_only(void)
{
   fresh();
   check("re-enumeration: upload under way", upload_under_way(), "setup failed");
   tud_mount_cb();
   check_upload_dropped("re-enumeration");
   uint16_t r = op(MTP_OP_OPEN_SESSION, 1, 0, 0);
   check("re-enumeration: the next OpenSession succeeds", r == MTP_RESP_OK, "response 0x%04x", r);
}

static void test_device_reset(void)
{
   fresh();
   check("device reset: upload under way", upload_under_way(), "setup failed");
   tusb_control_request_t req = { .bRequest = MTP_REQ_RESET };
   tud_mtp_request_cb_data_t rcb = { .stage = CONTROL_STAGE_ACK, .request = &req };
   check("device reset: the request is accepted", tud_mtp_request_device_reset_cb(&rcb), "stalled");
   check_upload_dropped("device reset");
   uint16_t r = op(MTP_OP_OPEN_SESSION, 1, 0, 0);
   check("device reset: the next OpenSession succeeds", r == MTP_RESP_OK, "response 0x%04x", r);
}

/* A suspend mid-transfer means the host has gone (a host suspends only an
   idle bus): drop the transfer, but keep the session - a host that merely
   suspended an idle device resumes into it. */
static void test_suspend_mid_upload(void)
{
   fresh();
   check("suspend: upload under way", upload_under_way(), "setup failed");
   usb_suspended = true;
   tud_suspend_cb(false);
   check_upload_dropped("suspend");
   usb_suspended = false;
   uint16_t r = op(MTP_OP_OPEN_SESSION, 1, 0, 0);
   check("suspend: the session survives it", r == MTP_RESP_SESSION_ALREADY_OPEN,
         "response 0x%04x", r);
}

static void test_kernel_buffer_freed(void)
{
   fresh();
   uint16_t r = op(MTP_OP_OPEN_SESSION, 1, 0, 0);
   r = (r == MTP_RESP_OK) ? send_object_info("kernel.now", BIG_ALLOC) : r;
   check("kernel.now: buffer allocated", r == MTP_RESP_OK && big_ptr != NULL,
         "response 0x%04x, buffer %p", r, big_ptr);
   usb_configured = false;
   tud_umount_cb();
   check("kernel.now: unplug frees the buffer", big_ptr == NULL, "%p still held", big_ptr);
}

static void test_card_swap(void)
{
   fresh();
   char names[8][32];
   (void)op(MTP_OP_OPEN_SESSION, 1, 0, 0);
   int n = root_names(names, 8);
   check("card swap: card A lists old.txt", n == 1 && strcmp(names[0], "old.txt") == 0,
         "%d objects, first '%s'", n, n > 0 ? names[0] : "");

   card_eject();                            /* *FX147: off the bus, card out */
   card_insert(card_b);                     /* the other card, back on the bus */
   tud_mount_cb();                          /* the host enumerates afresh */
   (void)op(MTP_OP_OPEN_SESSION, 1, 0, 0);
   n = root_names(names, 8);
   bool old_seen = false, new_seen = false;
   for (int i = 0; i < n; i++) {
      old_seen |= strcmp(names[i], "old.txt") == 0;
      new_seen |= strcmp(names[i], "new.txt") == 0;
   }
   check("card swap: card A's objects are gone", !old_seen, "old.txt still listed");
   check("card swap: card B's objects are listed", new_seen, "new.txt not listed (%d objects)", n);

   /* An ordinary change keeps stale-while-rebuild: still answered from the
      live cache, and the rebuild brings the new file in. */
   make_file("/added.txt");
   mtp_fs_notify_fs_changed();
   n = root_names(names, 8);
   check("change: answered from the live cache meanwhile", n == 1, "%d objects", n);
   now_us += 1100000u;
   for (int i = 0; i < 50; i++)
      mtp_fs_cache_poll();
   n = root_names(names, 8);
   check("change: the background rebuild lists it", n == 2, "%d objects", n);
}

int main(void)
{
   static uint8_t work[FF_MAX_SS * 4];
   static FATFS mkfs_fs;
   const MKFS_PARM opt = { FM_FAT32, 0, 0, 0, 0 };
   card_a = calloc(DISK_SECTORS, 512u);
   card_b = calloc(DISK_SECTORS, 512u);
   if (!card_a || !card_b) return 1;
   for (int c = 0; c < 2; c++) {
      disk = c ? card_b : card_a;
      f_mount(&mkfs_fs, "", 0);
      if (f_mkfs("", &opt, work, sizeof work) != FR_OK || f_mount(&mkfs_fs, "", 1) != FR_OK) {
         printf("FAIL: could not format RAM disk %c\n", 'A' + c);
         return 1;
      }
      make_file(c ? "/new.txt" : "/old.txt");
      f_mount(NULL, "", 0);
   }
   disk = card_a;

   test_unplug_mid_upload();
   test_reenumeration_only();
   test_device_reset();
   test_suspend_mid_upload();
   test_kernel_buffer_freed();
   test_card_swap();                        /* last: it leaves card B in */

   printf("\nmtpsession: %d passed, %d failed\n", passes, failures);
   free(card_a);
   free(card_b);
   return failures ? 1 : 0;
}
