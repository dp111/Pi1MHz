/* Host tests for request handling above the parsers: what a response puts
 * on the wire (conn_pump) and what the WebDAV MOVE/COPY route lets through
 * (review 2026-10-06 W2, W3).
 *
 * Extracted VERBATIM from a copy of webserver.c by extract.awk (see
 * run_tests.sh): conn_pump with its tcp_write wrapper, and
 * route_dav_move_or_copy with the path parsers it calls.  lwIP, FatFs and
 * the rest of webserver.c are stubbed below to record what happened.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ws_defines.inc"
#include "ws_conn_stubs.h"
#include "ws_conn.inc"

/* ---- lwIP: tcp_write appends to a captured "wire" ---- */
typedef uint16_t u16_t;
typedef int8_t   err_t;
#define ERR_OK   0
#define ERR_MEM (-1)
#define TCP_MSS 1460
#define TCP_WRITE_FLAG_COPY 0x01
static char   wire[65536];
static size_t wire_len;
static u16_t tcp_sndbuf(struct tcp_pcb *p) { (void)p; return 0xffffu; }
static err_t tcp_write(struct tcp_pcb *p, const void *d, u16_t n, int f)
{
   (void)p; (void)f;
   assert(wire_len + n <= sizeof wire);
   memcpy(wire + wire_len, d, n);
   wire_len += n;
   return ERR_OK;
}
static err_t tcp_output(struct tcp_pcb *p) { (void)p; return ERR_OK; }

/* ---- FatFs ---- */
typedef unsigned int UINT;
typedef enum { FR_OK = 0, FR_DISK_ERR = 1, FR_NO_FILE = 4 } FRESULT;
typedef struct { uint32_t fsize; uint16_t fdate, ftime; uint8_t fattrib; char fname[256]; } FILINFO;
#define AM_DIR   0x10
#define FA_READ  0x01
#define FA_WRITE 0x02
#define FA_CREATE_ALWAYS 0x08
static int f_read_calls, f_rename_calls, f_unlink_calls, f_open_calls;
static const char *stat_dir;     /* the one directory that "exists" */
static const char *stat_file;    /* the one file that "exists" */
static FRESULT f_read(FIL *f, void *b, UINT n, UINT *br)
{
   (void)f; memset(b, 'F', n); *br = n; f_read_calls++; return FR_OK;
}
static FRESULT f_stat(const char *p, FILINFO *fno)
{
   memset(fno, 0, sizeof *fno);
   /* LUN 0's folder, as FatFs finds it by its 8.3 alias */
   if (strcasecmp(p, "/BEEBSC~1") == 0) {
      strcpy(fno->fname, "BeebSCSI0");
      fno->fattrib = AM_DIR;
      return FR_OK;
   }
   if (stat_dir != NULL && strcasecmp(p, stat_dir) == 0) { fno->fattrib = AM_DIR; return FR_OK; }
   if (stat_file != NULL && strcasecmp(p, stat_file) == 0) return FR_OK;
   return FR_NO_FILE;
}
static FRESULT f_rename(const char *a, const char *b) { (void)a; (void)b; f_rename_calls++; return FR_OK; }
static FRESULT f_unlink(const char *p) { (void)p; f_unlink_calls++; return FR_OK; }
static FRESULT f_open(FIL *f, const char *p, int m) { (void)f; (void)p; (void)m; f_open_calls++; return FR_OK; }
static FRESULT f_close(FIL *f) { (void)f; return FR_OK; }

/* ---- framebuffer (conn_pump's BMP body) ---- */
static int fb_rows_rendered;
static bool framebuffer_export_get_info(framebuffer_export_info_t *i)
{
   memset(i, 0, sizeof *i); i->width = 4u; i->height = 2u; return true;
}
static void fb_render_row(const framebuffer_export_info_t *i, uint32_t row,
                          uint8_t *dst, uint32_t n)
{
   (void)i; (void)row; memset(dst, 'P', n); fb_rows_rendered++;
}

/* ---- the rest of webserver.c ---- */
#define WS_BUSY_MSG "busy"
static int reset_calls, close_calls, err_calls, err_status, moved_calls;
static void conn_reset_for_next_request(ws_conn_t *c) { (void)c; reset_calls++; }
static bool conn_close(ws_conn_t *c, bool a) { (void)c; (void)a; close_calls++; return false; }
static bool ws_error(ws_conn_t *c, int status, const char *t, const char *m)
{
   (void)c; (void)t; (void)m; err_calls++; err_status = status; return true;
}
static bool dav_move_copy_send_response(ws_conn_t *c, bool existed)
{
   (void)c; (void)existed; moved_calls++; return true;
}
/* As filesystemHostPathBusy with LUN 0 started: its own files, or a
   folder at or above the one holding them. */
static bool lun0_started;
static bool beeb_path_busy(const char *p)
{
   return lun0_started
       && (strncasecmp(p, "/BeebSCSI0/scsi0", 16) == 0
           || strcasecmp(p, "/BeebSCSI0") == 0 || strcmp(p, "/") == 0);
}
static void ws_fs_mutated(void) {}
static void mtp_fs_notify_object_removed(const char *p) { (void)p; }
static void mtp_fs_notify_object_added(const char *p) { (void)p; }
static void mtp_fs_notify_object_changed(const char *p) { (void)p; }
static int8_t filesystemLunFromHostPath(const char *p) { (void)p; return -1; }
static void filesystemHostLockLun(int8_t l, bool k) { (void)l; (void)k; }
static ws_conn_t *g_ws_active_copy;
static int8_t     g_ws_copy_lun = -1;

#include "ws_routes.inc"

/* ------------------------------------------------------------------ */

static int checks, fails;
static void ok(int cond, const char *what)
{
   checks++;
   if (!cond) { fails++; printf("  FAIL: %s\n", what); }
   else         printf("  ok: %s\n", what);
}

static int pcb_token;     /* stands in for a live pcb: never dereferenced */

static ws_conn_t *new_conn(void)
{
   ws_conn_t *c = calloc(1u, sizeof *c);
   assert(c != NULL);
   c->keep_alive = true;
   c->pcb = (struct tcp_pcb *)(void *)&pcb_token;
   return c;
}

/* Install a prebuilt response the way ws_install_response does. */
static void install(ws_conn_t *c, const char *resp, size_t len, conn_state_t st)
{
   free(c->out);
   c->out = malloc(len);
   assert(c->out != NULL);
   memcpy(c->out, resp, len);
   c->out_len = len;
   c->out_sent = 0u;
   c->bytes_queued = c->bytes_acked = 0u;
   c->state = st;
   wire_len = 0u;
}

static bool wire_is(const char *s)
{
   return wire_len == strlen(s) && memcmp(wire, s, wire_len) == 0;
}

/* A MOVE/COPY request as the client sends it. */
static void dav_request(ws_conn_t *c, const char *method, const char *dest)
{
   snprintf(c->reqhdr, sizeof c->reqhdr,
            "%s /x HTTP/1.1\r\nHost: pi\r\nDestination: %s\r\n\r\n",
            method, dest);
   c->reqhdr_len = strlen(c->reqhdr);
   err_calls = err_status = moved_calls = 0;
   f_rename_calls = f_unlink_calls = f_open_calls = 0;
}

int main(void)
{
   static const char hdr404[] =
      "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n"
      "Connection: keep-alive\r\n\r\n";
   static const char resp404[] =
      "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n"
      "Connection: keep-alive\r\n\r\nNo such.\n";

   puts("== W2: HEAD sends the header block only ==");
   {
      ws_conn_t *c = new_conn();

      install(c, resp404, sizeof resp404 - 1u, CONN_SEND_MEM);
      (void)conn_pump(c);
      ok(wire_len == sizeof resp404 - 1u, "GET: the in-memory body is sent");

      c->is_head = true;
      install(c, resp404, sizeof resp404 - 1u, CONN_SEND_MEM);
      (void)conn_pump(c);
      ok(wire_is(hdr404), "HEAD: an error page goes out as its header only");
      ok(c->producing_done, "HEAD: nothing is left to produce");
      c->bytes_acked = c->bytes_queued;
      reset_calls = 0;
      (void)conn_pump(c);
      ok(reset_calls == 1,
         "HEAD: the ACK of the header completes the response (keep-alive)");
      free(c->out);
      free(c);
   }

   puts("== W2: HEAD /framebuffer.bmp sends no BMP bytes ==");
   {
      static const char bmp_hdr[] =
         "HTTP/1.1 200 OK\r\nContent-Type: image/bmp\r\n"
         "Content-Length: 78\r\n\r\n";
      char resp[sizeof bmp_hdr - 1u + 54u];
      ws_conn_t *c = new_conn();

      memcpy(resp, bmp_hdr, sizeof bmp_hdr - 1u);
      memset(resp + sizeof bmp_hdr - 1u, 'B', 54u);
      c->is_head = true;
      install(c, resp, sizeof resp, CONN_SEND_FB);
      c->fb_info.width = 4u;
      c->fb_info.height = 2u;
      fb_rows_rendered = 0;
      (void)conn_pump(c);
      ok(wire_is(bmp_hdr), "HEAD: the 54-byte BMP header is not sent");
      ok(fb_rows_rendered == 0, "HEAD: no pixel rows rendered");
      free(c->out);
      free(c);
   }

   puts("== W2: HEAD of a file sends its header, no body read ==");
   {
      static const char fhdr[] =
         "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n";
      ws_conn_t *c = new_conn();

      c->is_head = true;
      install(c, fhdr, sizeof fhdr - 1u, CONN_SEND_FILE);
      c->dl_remaining = 100u;
      f_read_calls = 0;
      (void)conn_pump(c);
      ok(wire_is(fhdr) && f_read_calls == 0, "HEAD: file header only, no f_read");
      free(c->out);
      free(c);
   }

   puts("== W3: MOVE a folder into itself is refused ==");
   {
      ws_conn_t *c = new_conn();
      stat_dir = "/x";
      stat_file = NULL;

      dav_request(c, "MOVE", "http://pi/x/sub");
      (void)route_dav_move_or_copy(c, "/x", true);
      ok(f_rename_calls == 0 && err_calls == 1
         && (err_status == 403 || err_status == 409),
         "MOVE /x -> /x/sub refused, nothing renamed");

      dav_request(c, "MOVE", "http://pi/X/Sub/deeper");
      (void)route_dav_move_or_copy(c, "/x", true);
      ok(f_rename_calls == 0 && err_calls == 1,
         "MOVE /x -> /X/Sub/deeper refused (FAT is case-blind)");

      dav_request(c, "MOVE", "http://pi/x./sub");
      (void)route_dav_move_or_copy(c, "/x", true);
      ok(f_rename_calls == 0 && err_calls == 1,
         "MOVE /x -> /x./sub refused (same folder to FatFs)");

      dav_request(c, "MOVE", "http://pi/x");
      (void)route_dav_move_or_copy(c, "/x", true);
      ok(f_rename_calls == 0 && err_calls == 1 && err_status == 403,
         "MOVE /x -> /x still refused as the same object");

      dav_request(c, "MOVE", "http://pi/xy");
      (void)route_dav_move_or_copy(c, "/x", true);
      ok(f_rename_calls == 1 && err_calls == 0 && moved_calls == 1,
         "MOVE /x -> /xy (a sibling sharing the prefix) still allowed");

      dav_request(c, "MOVE", "http://pi/y/x");
      (void)route_dav_move_or_copy(c, "/x", true);
      ok(f_rename_calls == 1 && err_calls == 0 && moved_calls == 1,
         "MOVE /x -> /y/x still allowed");

      stat_dir = NULL;
      stat_file = "/f.txt";
      dav_request(c, "MOVE", "http://pi/f.txt.bak");
      (void)route_dav_move_or_copy(c, "/f.txt", true);
      ok(f_rename_calls == 1 && err_calls == 0,
         "MOVE /f.txt -> /f.txt.bak still allowed");
      free(c);
   }

   puts("== W1: an 8.3 alias cannot get past the LUN busy check ==");
   {
      ws_conn_t *c = new_conn();
      stat_dir = NULL;
      stat_file = "/BeebSCSI0/scsi0.dat";
      lun0_started = true;

      dav_request(c, "MOVE", "http://pi/away.dat");
      (void)route_dav_move_or_copy(c, "/BEEBSC~1/scsi0.dat", true);
      ok(f_rename_calls == 0 && err_calls == 1 && err_status == 423,
         "MOVE /BEEBSC~1/scsi0.dat with LUN 0 started is 423 Locked");

      dav_request(c, "COPY", "http://pi/BEEBSC~1/scsi0.dat");
      stat_file = "/other.dat";
      (void)route_dav_move_or_copy(c, "/other.dat", false);
      ok(f_open_calls == 0 && err_calls == 1 && err_status == 423,
         "COPY onto /BEEBSC~1/scsi0.dat with LUN 0 started is 423 Locked");

      lun0_started = false;
      stat_file = "/BeebSCSI0/scsi0.dat";
      dav_request(c, "MOVE", "http://pi/away.dat");
      (void)route_dav_move_or_copy(c, "/BEEBSC~1/scsi0.dat", true);
      ok(f_rename_calls == 1 && err_calls == 0,
         "with LUN 0 stopped the alias spelling still works");
      free(c);
   }

   printf("\n%d checks, %d failures\n", checks, fails);
   return fails ? 1 : 0;
}
