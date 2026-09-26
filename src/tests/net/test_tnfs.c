/* Host tests for the TNFS wire codec (net_tnfs.c) - build/parse round-trips,
 * boundary/truncation handling, EAGAIN backoff, seq/cmd matching. */

#include "net_tnfs.h"
#include <stdio.h>
#include <string.h>

static int checks, failures;
#define CHECK(cond, msg) do { checks++; \
   if (cond) printf("  ok: %s\n", msg); \
   else { printf("  FAIL: %s\n", msg); failures++; } } while (0)

int main(void)
{
   uint8_t buf[512];
   size_t  n;

   printf("== TNFS request builders ==\n");

   /* MOUNT: connid 0, version LE, mountpoint, empty user/pass NULs */
   n = tnfs_build_mount(buf, sizeof buf, 0x2Au, "/share", NULL, NULL);
   CHECK(n == 4u + 2u + 7u + 1u + 1u, "mount length = hdr+ver+\"/share\\0\"+2 NULs");
   CHECK(buf[0] == 0 && buf[1] == 0, "mount connid = 0 in header");
   CHECK(buf[2] == 0x2Au && buf[3] == TNFS_CMD_MOUNT, "mount seq + cmd");
   CHECK(buf[4] == 0x02 && buf[5] == 0x01, "version 1.2 little-endian (0x0102)");
   CHECK(memcmp(buf + 6, "/share", 7) == 0, "mountpoint string + NUL");
   CHECK(buf[13] == 0 && buf[14] == 0, "empty user + password NULs");

   /* OPEN: flags/mode LE then path */
   n = tnfs_build_open(buf, sizeof buf, 0x1234u, 0x03u,
                       TNFS_O_RDONLY, 0x01EDu, "/dir/file.dsk");
   CHECK(buf[0] == 0x34 && buf[1] == 0x12, "open connid LE in header");
   CHECK(buf[3] == TNFS_CMD_OPEN, "open cmd");
   CHECK(buf[4] == 0x01 && buf[5] == 0x00, "open flags O_RDONLY LE");
   CHECK(buf[6] == 0xED && buf[7] == 0x01, "open mode 0x01ED LE");
   CHECK(memcmp(buf + 8, "/dir/file.dsk", 14) == 0, "open path + NUL");
   CHECK(n == 8u + 14u, "open total length");

   /* READ: fd then size LE */
   n = tnfs_build_read(buf, sizeof buf, 0x0001u, 5u, 0x07u, 512u);
   CHECK(n == 7u && buf[4] == 0x07u && buf[5] == 0x00 && buf[6] == 0x02,
         "read = hdr + fd + size(512) LE");

   /* WRITE: fd + size LE + data */
   n = tnfs_build_write(buf, sizeof buf, 0x0001u, 8u, 0x02u, (const uint8_t *)"DATA", 4u);
   CHECK(n == 4u + 1u + 2u + 4u, "write = hdr + fd + size + data");
   CHECK(buf[3] == TNFS_CMD_WRITE && buf[4] == 0x02u && buf[5] == 0x04 && buf[6] == 0x00,
         "write fd + size(4) LE");
   CHECK(memcmp(buf + 7, "DATA", 4) == 0, "write payload");

   /* CLOSE / READDIR / CLOSEDIR: hdr + one handle byte */
   n = tnfs_build_close(buf, sizeof buf, 1u, 6u, 0x07u);
   CHECK(n == 5u && buf[3] == TNFS_CMD_CLOSE && buf[4] == 0x07u, "close = hdr + fd");

   /* overflow: builder returns 0 rather than truncating */
   CHECK(tnfs_build_mount(buf, 6u, 0u, "/toolongforbuf", NULL, NULL) == 0u,
         "builder returns 0 when the buffer is too small");
   CHECK(tnfs_build_read(buf, 4u, 0u, 0u, 0u, 0u) == 0u,
         "builder returns 0 when even the body won't fit");

   printf("== TNFS reply parsing ==\n");
   {
      tnfs_reply_t r;
      uint16_t ver = 0, retry = 0;
      uint8_t  fd = 0;
      const uint8_t *data = NULL;
      uint16_t dlen = 0;
      const char *name = NULL;
      uint32_t sz = 0;

      /* MOUNT reply: connid assigned, status OK, ver, retry-ms */
      uint8_t mrep[] = { 0x99, 0x00, 0x2A, TNFS_CMD_MOUNT,
                         TNFS_OK, 0x02, 0x01, 0xE8, 0x03 };  /* ver 1.2, retry 1000 */
      CHECK(tnfs_parse_reply(mrep, sizeof mrep, 0x2Au, TNFS_CMD_MOUNT, &r),
            "parse mount reply");
      CHECK(r.connid == 0x0099u, "mount reply session id from header");
      CHECK(r.status == TNFS_OK, "mount status OK");
      CHECK(tnfs_reply_mount(&r, &ver, &retry) && ver == 0x0102u && retry == 1000u,
            "mount reply: server ver 1.2 + retry 1000 ms");

      /* seq/cmd mismatch is rejected */
      CHECK(!tnfs_parse_reply(mrep, sizeof mrep, 0x2Bu, TNFS_CMD_MOUNT, &r),
            "wrong seq -> not our reply");
      CHECK(!tnfs_parse_reply(mrep, sizeof mrep, 0x2Au, TNFS_CMD_OPEN, &r),
            "wrong cmd -> not our reply");

      /* too short (header without status) is rejected */
      CHECK(!tnfs_parse_reply(mrep, 4u, 0x2Au, TNFS_CMD_MOUNT, &r),
            "header-only packet -> rejected");

      /* OPEN reply -> fd */
      { uint8_t orep[] = { 0x99, 0x00, 0x03, TNFS_CMD_OPEN, TNFS_OK, 0x07 };
        CHECK(tnfs_parse_reply(orep, sizeof orep, 0x03u, TNFS_CMD_OPEN, &r)
              && tnfs_reply_open(&r, &fd) && fd == 0x07u, "open reply -> fd 7"); }

      /* WRITE reply -> bytes written */
      { uint16_t wrote = 0; uint8_t wrep[] = { 0x99,0x00, 0x08, TNFS_CMD_WRITE, TNFS_OK, 0x04,0x00 };
        CHECK(tnfs_parse_reply(wrep, sizeof wrep, 0x08u, TNFS_CMD_WRITE, &r)
              && tnfs_reply_write(&r, &wrote) && wrote == 4u, "write reply -> 4 bytes written"); }

      /* READ reply -> length + data window */
      { uint8_t rrep[] = { 0x99, 0x00, 0x04, TNFS_CMD_READ, TNFS_OK,
                           0x04, 0x00, 'D','A','T','A' };
        CHECK(tnfs_parse_reply(rrep, sizeof rrep, 0x04u, TNFS_CMD_READ, &r)
              && tnfs_reply_read(&r, &data, &dlen)
              && dlen == 4u && memcmp(data, "DATA", 4) == 0, "read reply -> 4 bytes DATA");
        /* claimed length past the datagram end is rejected */
        { uint8_t bad[] = { 0x99,0x00,0x04,TNFS_CMD_READ,TNFS_OK, 0xFF,0x00, 'X' };
          CHECK(tnfs_parse_reply(bad, sizeof bad, 0x04u, TNFS_CMD_READ, &r)
                && !tnfs_reply_read(&r, &data, &dlen),
                "read length beyond datagram -> rejected (no over-read)"); } }

      /* READ at EOF: status 0x21, no body */
      { uint8_t erep[] = { 0x99,0x00,0x04,TNFS_CMD_READ, TNFS_EOF };
        CHECK(tnfs_parse_reply(erep, sizeof erep, 0x04u, TNFS_CMD_READ, &r)
              && r.status == TNFS_EOF && !tnfs_reply_read(&r, &data, &dlen),
              "read EOF status, no data"); }

      /* READDIR entry name */
      { uint8_t drep[] = { 0x99,0x00,0x05,TNFS_CMD_READDIR, TNFS_OK,
                           'G','A','M','E','.','D','S','K', 0 };
        CHECK(tnfs_parse_reply(drep, sizeof drep, 0x05u, TNFS_CMD_READDIR, &r)
              && tnfs_reply_readdir(&r, &name) && strcmp(name, "GAME.DSK") == 0,
              "readdir reply -> \"GAME.DSK\""); }
      /* an unterminated name is rejected (no run-off-the-end read) */
      { uint8_t drep[] = { 0x99,0x00,0x05,TNFS_CMD_READDIR, TNFS_OK, 'N','O','E','N','D' };
        CHECK(tnfs_parse_reply(drep, sizeof drep, 0x05u, TNFS_CMD_READDIR, &r)
              && !tnfs_reply_readdir(&r, &name), "unterminated readdir name -> rejected"); }

      /* STAT size field (offset 6 in the body) */
      { uint8_t srep[24] = { 0x99,0x00,0x06,TNFS_CMD_STAT, TNFS_OK };
        srep[5+6] = 0x00; srep[5+7] = 0x10; srep[5+8]=0; srep[5+9]=0; /* size = 0x1000 */
        CHECK(tnfs_parse_reply(srep, sizeof srep, 0x06u, TNFS_CMD_STAT, &r)
              && tnfs_reply_stat_size(&r, &sz) && sz == 0x1000u,
              "stat reply -> size 0x1000"); }

      /* EAGAIN backoff is exposed */
      { uint8_t arep[] = { 0x99,0x00,0x07,TNFS_CMD_READ, TNFS_EAGAIN, 0xF4,0x01 };
        CHECK(tnfs_parse_reply(arep, sizeof arep, 0x07u, TNFS_CMD_READ, &r)
              && r.status == TNFS_EAGAIN && r.backoff_ms == 500u,
              "EAGAIN reply -> backoff 500 ms"); }
   }

   printf("== LSEEK / STAT mode ==\n");
   n = tnfs_build_lseek(buf, sizeof buf, 0x1234u, 9u, 0x05u, TNFS_SEEK_SET, 0x00030200);
   CHECK(n == 10u && buf[3] == TNFS_CMD_LSEEK && buf[4] == 0x05 && buf[5] == TNFS_SEEK_SET &&
         buf[6] == 0x00 && buf[7] == 0x02 && buf[8] == 0x03 && buf[9] == 0x00,
         "lseek: fd, whence, position LE");
   CHECK(tnfs_build_lseek(buf, 9u, 1u, 1u, 1u, 0u, 0) == 0u, "lseek refuses a short buffer");
   { uint8_t srep[] = { 0x01,0x00,0x04,TNFS_CMD_STAT, TNFS_OK, 0xED,0x41, 0,0,0,0, 0,0,0,0 };
     tnfs_reply_t r; uint16_t mode = 0;
     CHECK(tnfs_parse_reply(srep, sizeof srep, 4u, TNFS_CMD_STAT, &r) &&
           tnfs_reply_stat_mode(&r, &mode) && (mode & TNFS_S_IFMT) == TNFS_S_IFDIR,
           "stat mode 0x41ED is a directory"); }

   printf("== shared request engine (tnfs_xfer) ==\n");
   {
      tnfs_xfer_t x;
      tnfs_reply_t r;
      memset(&x, 0, sizeof x);
      x.seq = 3u; x.connid = 0x0042u; x.retry_ms = 100u;
      size_t len = tnfs_build_read(x.req, sizeof x.req, x.connid, x.seq, 1u, 256u);
      tnfs_xfer_begin(&x, (uint16_t)len, 1000u);
      CHECK(tnfs_xfer_tick(&x, 1099u) == TNFS_X_WAIT, "no resend before the deadline");
      CHECK(tnfs_xfer_tick(&x, 1100u) == TNFS_X_SEND, "resend at the deadline");
      uint8_t good[]  = { 0x42,0x00,3u,TNFS_CMD_READ, TNFS_OK, 0x00,0x00 };
      uint8_t stale[] = { 0x42,0x00,2u,TNFS_CMD_READ, TNFS_OK, 0x00,0x00 };
      uint8_t other[] = { 0x43,0x00,3u,TNFS_CMD_READ, TNFS_OK, 0x00,0x00 };
      uint8_t busy[]  = { 0x42,0x00,3u,TNFS_CMD_READ, TNFS_EAGAIN, 0xF4,0x01 };
      CHECK(tnfs_xfer_reply(&x, stale, sizeof stale, 1200u, &r) == TNFS_X_IGNORED, "stale seq ignored");
      CHECK(tnfs_xfer_reply(&x, other, sizeof other, 1200u, &r) == TNFS_X_IGNORED, "foreign session ignored");
      CHECK(tnfs_xfer_reply(&x, busy, sizeof busy, 1200u, &r) == TNFS_X_BUSY, "EAGAIN backs off");
      CHECK(tnfs_xfer_tick(&x, 1699u) == TNFS_X_WAIT && tnfs_xfer_tick(&x, 1700u) == TNFS_X_SEND,
            "the server's 500 ms backoff is honoured");
      CHECK(tnfs_xfer_reply(&x, good, sizeof good, 1800u, &r) == TNFS_X_DONE && r.ok, "matching reply done");
      /* Silence: TNFS_RETRIES resends, then fail. */
      tnfs_xfer_begin(&x, (uint16_t)len, 0u);
      int sends = 0;
      uint32_t t = 0;
      tnfs_x res;
      while ((res = tnfs_xfer_tick(&x, t)) != TNFS_X_FAIL && t < 100000u) {
         if (res == TNFS_X_SEND) sends++;
         t += 10u;
      }
      CHECK(res == TNFS_X_FAIL && sends == (int)TNFS_RETRIES, "silence: TNFS_RETRIES resends, then fail");
      /* Sustained EAGAIN is bounded. */
      tnfs_xfer_begin(&x, (uint16_t)len, 0u);
      int busies = 0;
      while ((res = tnfs_xfer_reply(&x, busy, sizeof busy, 0u, &r)) == TNFS_X_BUSY) busies++;
      CHECK(res == TNFS_X_FAIL && busies == (int)TNFS_EAGAIN_MAX, "sustained EAGAIN bounded");
   }

   printf("\n%d checks, %d failures\n", checks, failures);
   if (failures) { printf("TNFS CODEC TESTS FAILED\n"); return 1; }
   printf("TNFS CODEC TESTS PASSED\n");
   return 0;
}
