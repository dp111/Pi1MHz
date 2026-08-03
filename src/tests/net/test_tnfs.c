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

   printf("\n%d checks, %d failures\n", checks, failures);
   if (failures) { printf("TNFS CODEC TESTS FAILED\n"); return 1; }
   printf("TNFS CODEC TESTS PASSED\n");
   return 0;
}
