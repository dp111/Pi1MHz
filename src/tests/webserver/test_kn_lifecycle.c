/* Host tests for the lifetime of a connection's per-request resources -
 * above all the kernel.now RAM buffer (kn_buf), which, if it outlives the
 * request that allocated it, turns the NEXT body on that kept-alive
 * connection into a chain-boot image (review 2026-10-06 P3 / W10).
 *
 * Extracted VERBATIM from a copy of webserver.c by extract.awk (see
 * run_tests.sh): the kernel.now helpers, the upload form's body sink
 * (upload_write, upload_finish, upload_fail), the three teardown paths
 * (conn_close, ws_err, conn_reset_for_next_request) and the WebDAV PUT body
 * sink (dav_put_consume[_chunked] -> dav_put_write_bytes -> dav_put_finish).  Everything they call that touches the SD card, lwIP,
 * the HTML builder or chainboot is stubbed below to record what happened.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ws_defines.inc"
#include "ws_conn_stubs.h"
#include "ws_conn.inc"

/* ---- free() tracking: conn_close / ws_err free the conn itself, so
   "was kn_buf released" can only be seen from the allocator's side. ---- */
static void *freed[64];
static size_t n_freed;
static void track_free(void *p)
{
   if (p != NULL && n_freed < sizeof freed / sizeof freed[0])
      freed[n_freed++] = p;
   (free)(p);
}
static bool was_freed(const void *p)
{
   for (size_t i = 0u; i < n_freed; i++)
      if (freed[i] == p)
         return true;
   return false;
}
#define free(p) track_free(p)

/* ---- FatFs ---- */
typedef enum { FR_OK = 0, FR_DISK_ERR = 1, FR_NO_FILE = 4 } FRESULT;
typedef struct { uint32_t fsize; uint16_t fdate, ftime; uint8_t fattrib; } FILINFO;
#define AM_DIR 0x10
static int f_close_calls, f_unlink_calls, f_rename_calls;
static char last_unlink[WS_PATH_MAX + 8u];
static FRESULT f_close(FIL *f) { (void)f; f_close_calls++; return FR_OK; }
static FRESULT f_unlink(const char *p)
{
   f_unlink_calls++;
   strncpy(last_unlink, p, sizeof last_unlink - 1u);
   return FR_OK;
}
static FRESULT f_rename(const char *a, const char *b) { (void)a; (void)b; f_rename_calls++; return FR_OK; }
static FRESULT f_stat(const char *p, FILINFO *f) { (void)p; (void)f; return FR_NO_FILE; }
static FRESULT f_utime(const char *p, const FILINFO *f) { (void)p; (void)f; return FR_OK; }

/* ---- lwIP ---- */
typedef int8_t err_t;
#define ERR_OK   0
#define ERR_RST (-14)
static int tcp_close_calls, tcp_abort_calls;
static void tcp_arg(struct tcp_pcb *p, void *a) { (void)p; (void)a; }
static void tcp_recv(struct tcp_pcb *p, void *f) { (void)p; (void)f; }
static void tcp_sent(struct tcp_pcb *p, void *f) { (void)p; (void)f; }
static void tcp_poll(struct tcp_pcb *p, void *f, int i) { (void)p; (void)f; (void)i; }
static void tcp_err(struct tcp_pcb *p, void *f) { (void)p; (void)f; }
static void tcp_abort(struct tcp_pcb *p) { (void)p; tcp_abort_calls++; }
static err_t tcp_close(struct tcp_pcb *p) { (void)p; tcp_close_calls++; return ERR_OK; }

/* ---- the rest of webserver.c the extracted functions call ---- */
#define WS_BUSY_MSG "busy"
static int upload_discard_calls, copy_discard_calls, slot_release_calls, live_remove_calls;
static void upload_discard_temp(ws_conn_t *c) { (void)c; upload_discard_calls++; }
static void copy_discard_temp(ws_conn_t *c) { (void)c; copy_discard_calls++; }
static void ws_copy_slot_release(const ws_conn_t *c) { (void)c; slot_release_calls++; }
static void ws_live_remove(const ws_conn_t *c) { (void)c; live_remove_calls++; }
static bool wifi_debug_enabled(void) { return false; }
static void wifi_debug_printf(const char *f, ...) { (void)f; }

static int err_calls, err_status, sent_calls, challenge_calls, flush_calls;
static bool ws_error(ws_conn_t *c, int status, const char *t, const char *m)
{
   (void)c; (void)t; (void)m;
   err_calls++;
   err_status = status;
   return true;
}
static bool dav_put_send_response(ws_conn_t *c) { (void)c; sent_calls++; return true; }
static bool ws_send_auth_challenge(ws_conn_t *c, bool stale) { (void)c; (void)stale; challenge_calls++; return true; }
static bool dav_put_flush(ws_conn_t *c) { c->dav_put_buf_len = 0u; flush_calls++; return true; }
static bool beeb_path_busy(const char *p) { (void)p; return false; }
static void ws_fs_mutated(void) {}
static bool filesystemReadLunStatus(uint8_t lun) { (void)lun; return false; }
static bool upload_flush(ws_conn_t *c) { c->up_buf_len = 0u; return true; }
static void upload_build_paths(const ws_conn_t *c, char *full, size_t fsz,
                               char *tmp, size_t tsz)
{
   (void)c;
   snprintf(full, fsz, "/up.ssd");
   snprintf(tmp, tsz, "/up.ssd.part");
}
static void mtp_fs_notify_object_added(const char *p) { (void)p; }
static void mtp_fs_notify_object_changed(const char *p) { (void)p; }

typedef struct { char *data; size_t len; size_t cap; bool failed; } ws_strbuf_t;
static void sb_init(ws_strbuf_t *b) { memset(b, 0, sizeof *b); }
static void sb_puts(ws_strbuf_t *b, const char *s) { (void)b; (void)s; }
static void sb_printf(ws_strbuf_t *b, const char *f, ...) { (void)b; (void)f; }
static void sb_html(ws_strbuf_t *b, const char *s) { (void)b; (void)s; }
static void page_open(ws_strbuf_t *b, const char *t) { (void)b; (void)t; }
static void page_close(ws_strbuf_t *b) { (void)b; }
static void append_files_url(ws_strbuf_t *b, const char *d) { (void)b; (void)d; }
static int html_calls, html_status;
static bool ws_finish_html(ws_conn_t *c, int status, const char *t, ws_strbuf_t *b)
{
   (void)c; (void)t; (void)b;
   html_calls++;
   html_status = status;
   return true;
}

/* ---- chainboot: the real image check, and a recorder for the jump ---- */
#define CHAINBOOT_MAX_IMAGE (4u * 1024u * 1024u)
static int boot_calls;
static const char *chainboot_refusal(void) { return NULL; }
static bool chainboot_image_ok(const uint8_t *image, uint32_t length)
{
   /* as chainboot.c: an ARM branch at offset 0 */
   if (image == NULL || length < 4u || length > CHAINBOOT_MAX_IMAGE)
      return false;
   return image[3] == 0xeau;
}
static bool chainboot_request(uint8_t *image, uint32_t length, uint32_t cap)
{
   (void)length; (void)cap;
   boot_calls++;
   (free)(image);              /* chainboot owns it now */
   return true;                /* taken */
}

#include "ws_kn.inc"

/* ------------------------------------------------------------------ */

static int checks, fails;
static void ok(int cond, const char *what)
{
   checks++;
   if (!cond) { fails++; printf("  FAIL: %s\n", what); }
   else         printf("  ok: %s\n", what);
}

static void zero_counters(void)
{
   f_close_calls = f_unlink_calls = f_rename_calls = 0;
   tcp_close_calls = tcp_abort_calls = 0;
   upload_discard_calls = copy_discard_calls = 0;
   slot_release_calls = live_remove_calls = 0;
   err_calls = err_status = sent_calls = challenge_calls = flush_calls = 0;
   html_calls = html_status = 0;
   boot_calls = 0;
   n_freed = 0u;
}

static ws_conn_t *new_conn(void)
{
   ws_conn_t *c = calloc(1u, sizeof *c);
   assert(c != NULL);
   c->keep_alive = true;
   return c;
}

/* An ordinary authenticated PUT /x.ssd, as route_dav_put leaves it. */
static void start_plain_put(ws_conn_t *c, uint32_t len)
{
   c->dav_put_open = true;
   strcpy(c->dav_put_target, "/x.ssd");
   strcpy(c->dav_put_tmppath, "/x.ssd.part");
   c->dav_put_status = 201;
   c->dav_remaining = len;
   c->state = CONN_RECV_DAV_PUT;
}

static int pcb_token;     /* stands in for a live pcb: never dereferenced */

/* A body that begins with an ARM branch: what chainboot_image_ok accepts. */
static const uint8_t arm_body[16] = { 0x06, 0x00, 0x00, 0xea, 'B', 'B', 'C' };
/* Longer than the smallest kernel.now buffer (64 bytes, for a declared
   length of 8): what a client that under-declared its body sends. */
static const uint8_t long_body[100] = { 0x06, 0x00, 0x00, 0xea };

int main(void)
{
   int status;

   puts("== P3: failed upload-form kernel.now, then a plain PUT ==");
   {
      ws_conn_t *c = new_conn();
      zero_counters();
      ok(kn_begin(c, 0u, &status) == NULL && c->kn_buf != NULL,
         "upload form starts gathering kernel.now in RAM");
      (void)upload_fail(c, "Multi-file uploads are not supported");
      ok(html_calls == 1 && html_status == 400, "upload_fail answers 400");
      ok(c->kn_buf == NULL, "upload_fail leaves no kn_buf behind");
      conn_reset_for_next_request(c);         /* 400 ACKed, keep-alive */
      ok(c->kn_buf == NULL, "no kn_buf survives into the next request");

      zero_counters();
      start_plain_put(c, sizeof arm_body);
      (void)dav_put_consume(c, arm_body, sizeof arm_body);
      ok(boot_calls == 0, "the next PUT does NOT chain-boot");
      ok(f_rename_calls == 1 && sent_calls == 1 && err_calls == 0,
         "the next PUT is saved as the file it names (201)");
      conn_reset_for_next_request(c);
      (free)(c->kn_buf);
      (free)(c);
   }

   puts("== P3: malformed chunk size on a chunked kernel.now PUT ==");
   {
      ws_conn_t *c = new_conn();
      size_t consumed = 0u;
      zero_counters();
      ok(kn_begin(c, 0u, &status) == NULL, "chunked kernel.now PUT begins");
      c->dav_put_chunked = true;
      c->state = CONN_RECV_DAV_PUT;
      (void)dav_put_consume_chunked(c, (const uint8_t *)"zz\r\n", 4u, &consumed);
      ok(err_calls == 1 && err_status == 400, "malformed chunk size is a 400");
      ok(c->kn_buf == NULL, "the 400 leaves no kn_buf behind");
      conn_reset_for_next_request(c);
      ok(c->kn_buf == NULL, "no kn_buf survives into the next request");

      zero_counters();
      start_plain_put(c, 7u);
      (void)dav_put_consume(c, (const uint8_t *)"not arm", 7u);
      ok(err_calls == 0 && sent_calls == 1,
         "a non-kernel next PUT is saved, not refused with 422");
      conn_reset_for_next_request(c);
      (free)(c->kn_buf);
      (free)(c);
   }

   puts("== P3: unauthenticated drain after a failed kernel.now ==");
   {
      /* The drain path (process_request, bad/stale credentials) sets
         dav_put_draining with no file open and feeds dav_put_consume. */
      ws_conn_t *c = new_conn();
      size_t consumed = 0u;
      zero_counters();
      (void)kn_begin(c, 0u, &status);
      c->dav_put_chunked = true;
      c->state = CONN_RECV_DAV_PUT;
      (void)dav_put_consume_chunked(c, (const uint8_t *)"zz\r\n", 4u, &consumed);
      conn_reset_for_next_request(c);

      zero_counters();
      c->dav_put_open = false;
      c->dav_put_draining = true;
      c->dav_remaining = sizeof arm_body;
      c->state = CONN_RECV_DAV_PUT;
      (void)dav_put_consume(c, arm_body, sizeof arm_body);
      ok(boot_calls == 0, "an unauthenticated drain does NOT chain-boot");
      ok(challenge_calls == 1, "the drain ends in the 401 challenge");
      conn_reset_for_next_request(c);
      (free)(c->kn_buf);
      (free)(c);
   }

   puts("== W10: every kernel.now exit leaves no stale length/capacity ==");
   {
      ws_conn_t *c = new_conn();
      zero_counters();
      (void)kn_begin(c, 0u, &status);
      (void)kn_append(c, (const uint8_t *)"not arm", 7u);
      ok(kn_take(c, &status) != NULL && status == 422,
         "kn_take refuses a non-kernel image (422)");
      ok(c->kn_buf == NULL && c->kn_len == 0u && c->kn_cap == 0u,
         "kn_take refusal: kn_buf, kn_len and kn_cap all cleared");

      (void)kn_begin(c, 0u, &status);
      (void)kn_append(c, arm_body, sizeof arm_body);
      ok(kn_take(c, &status) == NULL && boot_calls == 1,
         "kn_take hands a kernel image to chainboot");
      ok(c->kn_buf == NULL && c->kn_len == 0u && c->kn_cap == 0u,
         "kn_take hand-over: kn_buf, kn_len and kn_cap all cleared");

      zero_counters();
      (void)kn_begin(c, 8u, &status);           /* said 8 bytes ...   */
      start_plain_put(c, sizeof long_body);     /* ... sends 100 */
      c->dav_put_open = false;
      (void)dav_put_write_bytes(c, long_body, sizeof long_body);
      ok(err_calls == 1 && err_status == 413, "DAV PUT overrun is a 413");
      ok(c->kn_buf == NULL && c->kn_len == 0u && c->kn_cap == 0u,
         "DAV PUT overrun: kn_buf, kn_len and kn_cap all cleared");
      conn_reset_for_next_request(c);
      (free)(c);
   }

   puts("== W8: upload-form kernel.now refusals carry their real status ==");
   {
      ws_conn_t *c = new_conn();
      zero_counters();
      (void)kn_begin(c, 8u, &status);
      c->up_state = UP_DATA;
      (void)upload_write(c, long_body, sizeof long_body);
      ok(html_calls == 1 && html_status == 413,
         "upload form: a kernel.now over its size is a 413, not a 400");
      ok(c->kn_buf == NULL && c->kn_len == 0u && c->kn_cap == 0u,
         "upload form overrun: kn_buf, kn_len and kn_cap all cleared");
      conn_reset_for_next_request(c);

      zero_counters();
      (void)kn_begin(c, 0u, &status);
      c->up_state = UP_DATA;
      (void)upload_write(c, (const uint8_t *)"not arm", 7u);
      (void)upload_finish(c);
      ok(boot_calls == 0 && html_calls == 1 && html_status == 422,
         "upload form: a non-kernel image is a 422, as on the DAV path");
      conn_reset_for_next_request(c);

      zero_counters();
      (void)kn_begin(c, 0u, &status);
      c->up_state = UP_DATA;
      (void)upload_write(c, arm_body, sizeof arm_body);
      (void)upload_finish(c);
      ok(boot_calls == 1 && html_calls == 1 && html_status == 200,
         "upload form: a kernel image still restarts (200)");
      conn_reset_for_next_request(c);
      (free)(c);
   }

   puts("== W5: a body nobody will read closes the connection ==");
   {
      ws_conn_t *c = new_conn();
      size_t consumed = 0u;
      zero_counters();
      c->state = CONN_RECV_HEADER;
      c->req_body_pending = true;
      ok(ws_body_unread(c), "PUT/POST answered from process_request, body to come");
      c->req_body_pending = false;
      ok(!ws_body_unread(c), "request whose body came with its headers");

      start_plain_put(c, 8u);
      c->dav_put_open = false;
      c->dav_put_draining = true;               /* no file: bytes go nowhere */
      (void)dav_put_consume(c, arm_body, 4u);
      ok(ws_body_unread(c), "PUT answered part way through its body");
      (void)dav_put_consume(c, arm_body + 4, 4u);
      ok(challenge_calls == 1 && !ws_body_unread(c),
         "drained PUT: the 401 keeps the connection");
      conn_reset_for_next_request(c);
      ok(!c->req_body_pending, "reset clears the pending-body note");

      c->dav_put_chunked = true;
      c->dav_put_draining = true;
      c->state = CONN_RECV_DAV_PUT;
      (void)dav_put_consume_chunked(c, (const uint8_t *)"zz\r\n", 4u, &consumed);
      ok(err_calls == 1 && ws_body_unread(c),
         "chunked PUT refused mid-body closes");
      conn_reset_for_next_request(c);
      c->dav_put_chunked = true;
      c->dav_put_draining = true;
      c->state = CONN_RECV_DAV_PUT;
      (void)dav_put_consume_chunked(c, (const uint8_t *)"2\r\nab\r\n0\r\n\r\n",
                                    15u, &consumed);
      ok(challenge_calls == 2 && !ws_body_unread(c),
         "chunked body read to its end keeps the connection");
      conn_reset_for_next_request(c);

      c->state = CONN_RECV_UPLOAD;
      c->up_state = UP_DATA;
      (void)upload_fail(c, "bad");
      ok(ws_body_unread(c), "upload refused mid-body closes");
      conn_reset_for_next_request(c);
      c->state = CONN_RECV_UPLOAD;
      c->up_state = UP_DATA;
      (void)upload_finish(c);
      ok(html_status == 200 && !ws_body_unread(c),
         "completed upload keeps the connection");
      conn_reset_for_next_request(c);
      (free)(c);
   }

   /* W10: every teardown releases every per-request resource. */
   for (int which = 0; which < 4; which++) {
      static const char *const name[4] = {
         "conn_reset_for_next_request", "conn_close(close)",
         "conn_close(abort)", "ws_err" };
      ws_conn_t *c = new_conn();
      uint8_t *kn;
      char *out;
      char what[96];

      printf("== W10: %s releases everything ==\n", name[which]);
      zero_counters();
      (void)kn_begin(c, 64u, &status);
      kn = c->kn_buf;
      out = malloc(16u);
      c->out = out;
      c->out_len = 16u;
      c->dl_open = c->up_file_open = c->dav_put_open = true;
      c->copy_src_open = c->copy_dst_open = true;
      strcpy(c->dav_put_tmppath, "/x.ssd.part");
      c->pcb = (struct tcp_pcb *)(void *)&pcb_token;

      switch (which) {
      case 0: conn_reset_for_next_request(c); break;
      case 1: (void)conn_close(c, false); break;
      case 2: (void)conn_close(c, true); break;
      default: ws_err(c, ERR_RST); break;
      }

      snprintf(what, sizeof what, "%s: all five files closed", name[which]);
      ok(f_close_calls == 5, what);
      snprintf(what, sizeof what, "%s: .part temps dropped", name[which]);
      ok(upload_discard_calls == 1 && copy_discard_calls == 1
         && f_unlink_calls >= 1 && strcmp(last_unlink, "/x.ssd.part") == 0, what);
      snprintf(what, sizeof what, "%s: COPY slot released", name[which]);
      ok(slot_release_calls == 1, what);
      snprintf(what, sizeof what, "%s: kn_buf and out freed", name[which]);
      ok(was_freed(kn) && was_freed(out), what);
      if (which == 0) {
         ok(c->kn_buf == NULL && c->kn_len == 0u && c->kn_cap == 0u
            && c->out == NULL && c->state == CONN_RECV_HEADER,
            "reset: conn ready for the next request");
         ok(live_remove_calls == 0 && tcp_close_calls == 0 && tcp_abort_calls == 0,
            "reset: conn stays live, pcb untouched");
         (free)(c->kn_buf);
         (free)(c);
      } else {
         snprintf(what, sizeof what, "%s: conn unlisted and freed", name[which]);
         ok(live_remove_calls == 1 && was_freed(c), what);
         snprintf(what, sizeof what, "%s: pcb torn down as before", name[which]);
         ok(which == 1 ? (tcp_close_calls == 1 && tcp_abort_calls == 0)
          : which == 2 ? (tcp_close_calls == 0 && tcp_abort_calls == 1)
          :              (tcp_close_calls == 0 && tcp_abort_calls == 0), what);
      }
   }

   printf("\n%d checks, %d failures\n", checks, fails);
   return fails ? 1 : 0;
}
