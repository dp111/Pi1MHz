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

/* ---- directory reads made by mtp_fs.c (-Df_readdir): a cache walk -------- */
static unsigned readdirs;
FRESULT test_f_readdir(DIR *dp, FILINFO *fno)
{
   readdirs++;
   return f_readdir(dp, fno);
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

/* The root's objects, as the host would list them: name and size. */
#define MAX_LISTED 8
static char     listed[MAX_LISTED][32];
static uint32_t listed_size[MAX_LISTED];
static int list_root(void)
{
   if (op(MTP_OP_GET_OBJECT_HANDLES, 0x00010001u, 0, 0xFFFFFFFFu) != MTP_RESP_OK)
      return -1;
   uint32_t count, handles[16];
   memcpy(&count, data_in + 12, 4);
   if (count > 16) count = 16;
   memcpy(handles, data_in + 16, count * 4u);
   int n = 0;
   for (uint32_t i = 0; i < count && n < MAX_LISTED; i++) {
      if (op(MTP_OP_GET_OBJECT_INFO, handles[i], 0, 0) != MTP_RESP_OK)
         continue;
      mtp_object_info_header_t h;
      memcpy(&h, data_in + 12, sizeof h);
      listed_size[n] = h.object_compressed_size;
      const uint8_t *s = data_in + 12 + sizeof h;
      int j;
      for (j = 0; j < s[0] - 1 && j < 31; j++)
         listed[n][j] = (char)s[1 + 2 * j];
      listed[n][j] = '\0';
      n++;
   }
   return n;
}

/* Size of `name` in the last listing, or -1 if it was not listed. */
static long listed_as(const char *name, int n)
{
   for (int i = 0; i < n; i++)
      if (strcmp(listed[i], name) == 0)
         return (long)listed_size[i];
   return -1;
}

static bool exists(const char *path)
{
   FILINFO fno;
   return f_stat(path, &fno) == FR_OK;
}

static void make_file(const char *path, UINT size)
{
   FIL f;
   UINT bw;
   if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK ||
       f_write(&f, "xxxxxxxx", size, &bw) != FR_OK || f_close(&f) != FR_OK) {
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
   test before left: a re-enumeration on the fixed code, a CloseSession on
   the old one. */
static void fresh(void)
{
   usb_configured = true; usb_suspended = false;
   tud_mount_cb();
   (void)op(MTP_OP_CLOSE_SESSION, 0, 0, 0);
   memset(lun_locked, 0, sizeof lun_locked);
}

/* The rest of the SendObject body after a drop: what a host that did not
   see it go sends next. */
static uint16_t send_object_next_packet(void)
{
   static uint8_t body[CFG_TUD_MTP_EP_BUFSIZE];
   return data_out(body, sizeof body, sizeof(mtp_container_header_t) + 100000u);
}

/* The session is open without a new OpenSession: a command that needs one
   is not refused SESSION_NOT_OPEN. */
static bool session_still_open(void)
{
   uint16_t r = send_object_info("probe.txt", 1u);
   return r == MTP_RESP_OK;
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
/* tud_mount_cb runs inside SET_CONFIGURATION, before its status stage: it
   must do no SD work there.  The transfer goes on the next main-loop poll. */
static void test_reenumeration_then_poll(void)
{
   fresh();
   check("re-enumeration: upload under way", upload_under_way(), "setup failed");
   tud_mount_cb();
   check("re-enumeration: no SD work inside SET_CONFIGURATION",
         exists(LUN_PATH ".part") && lun_locked[LUN_NUM], "the transfer was dropped there");
   mtp_fs_cache_poll();
   check_upload_dropped("re-enumeration, then the poll");
   uint16_t r = op(MTP_OP_OPEN_SESSION, 1, 0, 0);
   check("re-enumeration: the next OpenSession succeeds", r == MTP_RESP_OK, "response 0x%04x", r);
}

/* ... or before the next command, if that comes first. */
static void test_reenumeration_then_command(void)
{
   fresh();
   check("re-enumeration, command first: upload under way", upload_under_way(), "setup failed");
   tud_mount_cb();
   uint16_t r = op(MTP_OP_OPEN_SESSION, 1, 0, 0);
   check("re-enumeration, command first: OpenSession succeeds", r == MTP_RESP_OK, "response 0x%04x", r);
   check_upload_dropped("re-enumeration, command first");
}

/* An OpenSession while one is open: the host has lost the old session. */
static void test_reopen(void)
{
   fresh();
   check("reopen: upload under way", upload_under_way(), "setup failed");
   uint16_t r = op(MTP_OP_OPEN_SESSION, 2, 0, 0);
   check("reopen: OpenSession on an open session answers OK", r == MTP_RESP_OK, "response 0x%04x", r);
   check_upload_dropped("reopen");
}

/* Device Reset abandons the transaction; the session is the host's call. */
static void test_device_reset(void)
{
   fresh();
   check("device reset: upload under way", upload_under_way(), "setup failed");
   tusb_control_request_t req = { .bRequest = MTP_REQ_RESET };
   tud_mtp_request_cb_data_t rcb = { .stage = CONTROL_STAGE_ACK, .request = &req };
   check("device reset: the request is accepted", tud_mtp_request_device_reset_cb(&rcb), "stalled");
   check_upload_dropped("device reset");
   check("device reset: the session is kept", session_still_open(), "refused");
   uint16_t r = op(MTP_OP_OPEN_SESSION, 1, 0, 0);
   check("device reset: an OpenSession after it succeeds", r == MTP_RESP_OK, "response 0x%04x", r);
}

/* A suspend mid-transfer means the host has gone (a host suspends only an
   idle bus): drop the transfer, but keep the session - a host that merely
   suspended an idle device resumes into it.  One that carries on with the
   dropped transfer is answered an error, not left hanging. */
static void test_suspend_mid_upload(void)
{
   fresh();
   check("suspend: upload under way", upload_under_way(), "setup failed");
   usb_suspended = true;
   tud_suspend_cb(false);
   check_upload_dropped("suspend");
   usb_suspended = false;
   uint16_t r = send_object_next_packet();
   check("suspend: the rest of the body is answered GENERAL_ERROR",
         r == MTP_RESP_GENERAL_ERROR, "response 0x%04x", r);
   check("suspend: the session survives it", session_still_open(), "refused");
}

static void test_suspend_between_info_and_object(void)
{
   fresh();
   uint16_t r = op(MTP_OP_OPEN_SESSION, 1, 0, 0);
   r = (r == MTP_RESP_OK) ? send_object_info(LUN_PATH + 1, 100000u) : r;
   check("suspend after SendObjectInfo: set up", r == MTP_RESP_OK, "response 0x%04x", r);
   usb_suspended = true;
   tud_suspend_cb(false);
   usb_suspended = false;
   check_upload_dropped("suspend after SendObjectInfo");
   r = op(MTP_OP_SEND_OBJECT, 0, 0, 0);
   check("suspend after SendObjectInfo: SendObject is answered GENERAL_ERROR",
         r == MTP_RESP_GENERAL_ERROR, "response 0x%04x", r);
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

/* Card A: old.txt, same.txt (1 byte).  Card B: new.txt, same.txt (5 bytes).
   *FX147 with a host attached: the eject takes the device off the bus; the
   card goes in; the device comes back and the host enumerates while the
   main loop polls. */
static void test_card_swap(void)
{
   fresh();
   (void)op(MTP_OP_OPEN_SESSION, 1, 0, 0);
   int n = list_root();
   check("card swap: card A lists old.txt and same.txt (1 byte)",
         n == 2 && listed_as("old.txt", n) == 1 && listed_as("same.txt", n) == 1,
         "%d objects", n);

   card_eject();
   usb_configured = false;                  /* the host has let go */
   card_insert(card_b);
   unsigned walked = readdirs;
   for (int i = 0; i < 200; i++)            /* the main loop, while it enumerates */
      mtp_fs_cache_poll();
   check("card swap: the walk runs while the host enumerates", readdirs > walked,
         "no directory read");
   usb_configured = true;
   tud_mount_cb();
   walked = readdirs;
   (void)op(MTP_OP_OPEN_SESSION, 1, 0, 0);
   n = list_root();
   check("card swap: the host's queries walk nothing inline", readdirs == walked,
         "%u directory reads", readdirs - walked);
   check("card swap: card A's objects are gone", listed_as("old.txt", n) < 0, "old.txt still listed");
   check("card swap: card B's objects are listed", listed_as("new.txt", n) == 1,
         "new.txt not listed (%d objects)", n);
   check("card swap: a path on both cards is card B's file", listed_as("same.txt", n) == 5,
         "same.txt listed at %ld bytes", listed_as("same.txt", n));

   /* An ordinary change keeps stale-while-rebuild: still answered from the
      live cache, and the rebuild brings the new file in. */
   make_file("/added.txt", 1);
   mtp_fs_notify_fs_changed();
   walked = readdirs;
   n = list_root();
   check("change: answered from the live cache meanwhile", n == 2 && readdirs == walked,
         "%d objects, %u directory reads", n, readdirs - walked);
   now_us += 1100000u;
   for (int i = 0; i < 200; i++)
      mtp_fs_cache_poll();
   n = list_root();
   check("change: the background rebuild lists it", n == 3 && listed_as("added.txt", n) == 1,
         "%d objects", n);
}

/* The probe's own control: a host that queries before the walk has run
   gets the rest of it inline, and the count sees it. */
static void test_card_swap_query_before_walk(void)
{
   card_eject();
   card_insert(card_a);
   tud_mount_cb();
   unsigned walked = readdirs;
   (void)op(MTP_OP_OPEN_SESSION, 1, 0, 0);
   int n = list_root();
   check("swap, no poll: the walk is finished inline (probe control)", readdirs > walked,
         "no directory read");
   check("swap, no poll: card A is listed", n == 2 && listed_as("same.txt", n) == 1,
         "%d objects", n);
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
      make_file(c ? "/new.txt" : "/old.txt", 1);
      make_file("/same.txt", c ? 5 : 1);
      f_mount(NULL, "", 0);
   }
   disk = card_a;

   test_unplug_mid_upload();
   test_reenumeration_then_poll();
   test_reenumeration_then_command();
   test_reopen();
   test_device_reset();
   test_suspend_mid_upload();
   test_suspend_between_info_and_object();
   test_kernel_buffer_freed();
   test_card_swap();                        /* last two: they swap the cards */
   test_card_swap_query_before_walk();

   printf("\nmtpsession: %d passed, %d failed\n", passes, failures);
   free(card_a);
   free(card_b);
   return failures ? 1 : 0;
}
