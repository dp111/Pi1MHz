/* test_fujinet.c - host tests for the FujiNet device (src/fujinet).

   Everything goes through fujibus_answer() as whole packets, the way the
   Beeb reaches it.  Expected bytes come from fujinet-nio's docs and handlers
   (cited per test), and the checksum vectors from its own Python
   implementation (py/fujinet_tools/fujibus.py calc_checksum). */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fujibus.h"
#include "fn_devices.h"
#include "fn_disk.h"
#include "fn_store.h"

void fn_store_host_root(const char *root);
int fn_store_host_open_count(void);

static int s_fail, s_pass;
static char s_root[256];

#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; \
   printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- packet helpers ------------------------------------------------------ */

typedef struct { uint8_t b[4096]; uint16_t n; } buf_t;

static void put(buf_t *p, const void *d, size_t n) { memcpy(p->b + p->n, d, n); p->n = (uint16_t)(p->n + n); }
static void u8(buf_t *p, uint8_t v) { put(p, &v, 1); }
static void u16(buf_t *p, uint16_t v) { u8(p, (uint8_t)v); u8(p, (uint8_t)(v >> 8)); }
static void u32(buf_t *p, uint32_t v) { u16(p, (uint16_t)v); u16(p, (uint16_t)(v >> 16)); }
static void lstr(buf_t *p, const char *s) { u16(p, (uint16_t)strlen(s)); put(p, s, strlen(s)); }

static buf_t payload(void) { buf_t p = { .n = 0 }; u8(&p, 1); return p; }   /* version 1 */

typedef struct {
   bool answered;
   uint8_t status;
   uint8_t pkt[4096];
   uint16_t len;
   uint16_t dlen;         /* device payload is D(r): pkt + 7 */
} reply_t;

static reply_t call_raw(const uint8_t *pkt, uint16_t n)
{
   reply_t r = { 0 };
   r.answered = fujibus_answer(pkt, n, r.pkt, sizeof r.pkt, &r.len);
   if (r.answered) {
      r.status = r.pkt[6];
      r.dlen = (uint16_t)(r.len - 7);
      CHECK(r.pkt[5] == 1, "reply descriptor %u", r.pkt[5]);
      CHECK((r.pkt[2] | r.pkt[3] << 8) == r.len, "reply length field");
      CHECK(fb_checksum(r.pkt, r.len) == r.pkt[4], "reply checksum");
   }
   return r;
}

static reply_t call(uint8_t dev, uint8_t cmd, const buf_t *p)
{
   uint8_t pkt[4096];
   uint16_t n = (uint16_t)(6 + p->n);
   pkt[0] = dev; pkt[1] = cmd; pkt[2] = (uint8_t)n; pkt[3] = (uint8_t)(n >> 8);
   pkt[4] = 0; pkt[5] = 0;
   memcpy(pkt + 6, p->b, p->n);
   pkt[4] = fb_checksum(pkt, n);
   reply_t r = call_raw(pkt, n);
   CHECK(r.answered, "no reply to %02X/%02X", dev, cmd);
   CHECK(r.pkt[0] == dev && r.pkt[1] == cmd, "reply echoes device/command");
   return r;
}

#define D(r) ((r).pkt + 7)

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (uint32_t)rd16(p + 2) << 16; }

static void write_file(const char *rel, const void *d, size_t n)
{
   char p[512];
   snprintf(p, sizeof p, "%s%s", s_root, rel);
   FILE *f = fopen(p, "wb");
   fwrite(d, 1, n, f);
   fclose(f);
}

static long file_size(const char *rel)
{
   char p[512];
   struct stat st;
   snprintf(p, sizeof p, "%s%s", s_root, rel);
   return stat(p, &st) == 0 ? st.st_size : -1;
}

static void mkdir_rel(const char *rel)
{
   char p[512];
   snprintf(p, sizeof p, "%s%s", s_root, rel);
   mkdir(p, 0755);
}

/* ---- packet layer --------------------------------------------------------- */

static void test_packet_layer(void)
{
   /* Reference checksums: fujinet-nio calc_checksum, byte 4 zeroed. */
   uint8_t v1[14] = { 0xFC, 0x03, 0x0E, 0, 0, 0, 1, 1, 0xdf, 0x06, 0, 0, 8, 0 };
   uint8_t v2[512], v3[300];
   for (int i = 0; i < 256; i++) { v2[i] = (uint8_t)i; v2[511 - i] = (uint8_t)i; }
   memset(v3, 0xFF, sizeof v3);
   CHECK(fb_checksum(v1, 14) == 0xFD, "checksum v1 %02X", fb_checksum(v1, 14));
   CHECK(fb_checksum(v2, 512) == 0xFB, "checksum v2 %02X", fb_checksum(v2, 512));
   v3[4] = 0x12;   /* byte 4 is ignored */
   CHECK(fb_checksum(v3, 300) == 0xFF, "checksum v3 %02X", fb_checksum(v3, 300));

   /* What a serial device would drop: no reply. */
   uint8_t pkt[16] = { 0xFC, 0x05, 8, 0, 0, 0, 1, 1 };
   pkt[4] = fb_checksum(pkt, 8);
   reply_t r = call_raw(pkt, 8);
   CHECK(r.answered, "well-formed Info answered");
   CHECK(!call_raw(pkt, 5).answered, "short packet dropped");
   CHECK(!call_raw(pkt, 9).answered, "length field disagreeing dropped");
   pkt[4] ^= 1;
   CHECK(!call_raw(pkt, 8).answered, "bad checksum dropped");

   buf_t p = payload();
   r = call(0x42, 1, &p);
   CHECK(r.status == FB_DEVICE_NOT_FOUND && r.dlen == 0, "unknown device");
   r = call(FB_DEV_DISK, 0x7E, &p);
   CHECK(r.status == FB_UNSUPPORTED, "unknown disk command");
   buf_t bad = { .n = 0 }; u8(&bad, 2); u8(&bad, 1);
   r = call(FB_DEV_DISK, 0x05, &bad);
   CHECK(r.status == FB_INVALID_REQUEST, "bad version");
}

/* ---- host + resolution -------------------------------------------------- */

static reply_t host_set(const char *spec)
{
   buf_t p = payload(); lstr(&p, spec);
   return call(FB_DEV_HOST, 0x02, &p);
}

static void expect_current(const char *uri, const char *display)
{
   buf_t p = payload();
   reply_t r = call(FB_DEV_HOST, 0x01, &p);
   uint16_t hl = rd16(D(r) + 1), dl = rd16(D(r) + 3);
   CHECK(r.status == FB_OK && hl == strlen(uri) && !memcmp(D(r) + 5, uri, hl) &&
         dl == strlen(display) && !memcmp(D(r) + 5 + hl, display, dl),
         "current host %.*s (%.*s), want %s (%s)", hl, D(r) + 5, dl, D(r) + 5 + hl, uri, display);
}

static void expect_resolve(const char *spec, const char *want)
{
   char uri[256];
   bool ok = fn_host_resolve(spec, uri, sizeof uri);
   CHECK(ok && strcmp(uri, want) == 0, "resolve '%s' -> '%s', want '%s'", spec, ok ? uri : "(fail)", want);
}

static void test_host(void)
{
   buf_t p = payload();
   reply_t r = call(FB_DEV_HOST, 0x01, &p);
   CHECK(r.status == FB_DEVICE_NOT_FOUND, "no current host yet");
   CHECK(!fn_host_resolve("game.ssd", (char[256]){0}, 256), "relative spec without a host fails");

   mkdir_rel("/img"); mkdir_rel("/img/sub");
   /* The filesystem name keeps its case, as in fujinet-nio's FsPrefixResolver. */
   CHECK(host_set("SD0:img").status == FB_OK, "set SD0:img");
   expect_current("SD0:/img", "/img");
   CHECK(host_set("sub").status == FB_OK, "set relative sub");
   expect_current("SD0:/img/sub", "/img/sub");
   CHECK(host_set("..").status == FB_OK, "set ..");
   expect_current("SD0:/img", "/img");
   CHECK(host_set("nowhere").status == FB_IO_ERROR, "missing directory refused");
   CHECK(host_set("zz0:/").status == FB_IO_ERROR, "unknown filesystem refused");
   expect_current("SD0:/img", "/img");

   /* History: most recent first, deduplicated. */
   p = payload(); u16(&p, 0); u16(&p, 200);
   r = call(FB_DEV_HOST, 0x03, &p);
   const char *want = "0 SD0:/img\n1 SD0:/img/sub\n";
   CHECK(r.status == FB_OK && rd16(D(r) + 4) == strlen(want) &&
         !memcmp(D(r) + 6, want, strlen(want)) && D(r)[1] == 0,
         "history text '%.*s'", rd16(D(r) + 4), D(r) + 6);
   p = payload(); u16(&p, 2); u16(&p, 5);
   r = call(FB_DEV_HOST, 0x03, &p);
   CHECK(D(r)[1] == 1 && rd16(D(r) + 4) == 5 && !memcmp(D(r) + 6, "SD0:/", 5), "history paged");
   p = payload(); u8(&p, 1);
   CHECK(call(FB_DEV_HOST, 0x04, &p).status == FB_OK, "select history 1");
   expect_current("SD0:/img/sub", "/img/sub");
   p = payload(); u8(&p, 5);
   CHECK(call(FB_DEV_HOST, 0x04, &p).status == FB_IO_ERROR, "select missing index");
   p = payload(); u8(&p, 1);
   CHECK(call(FB_DEV_HOST, 0x05, &p).status == FB_OK, "delete history 1");
   p = payload(); u16(&p, 0); u16(&p, 200);
   r = call(FB_DEV_HOST, 0x03, &p);
   CHECK(rd16(D(r) + 4) == strlen("0 SD0:/img/sub\n"), "one entry left");

   CHECK(host_set("sd0:/img").status == FB_OK, "back to /img");

   /* fujinet-nio resolver rules (path_resolvers/). */
   expect_resolve("a.ssd", "sd0:/img/a.ssd");
   expect_resolve("/x/./y/../z", "sd0:/x/z");
   expect_resolve("sd0:foo//bar/", "sd0:/foo/bar");
   expect_resolve("tnfs://h:16384/a/../b", "tnfs://h:16384/b");
   expect_resolve("TNFS://h", "TNFS://h/");
   expect_resolve("tnfs://h/", "tnfs://h/");
   fn_app_write("fujinet-nio", "current-host", 0, (const uint8_t *)"tnfs://h/dir", 12);
   expect_resolve("x.ssd", "tnfs://h/dir/x.ssd");
   expect_resolve("..", "tnfs://h/");
   expect_resolve("/top", "tnfs://h/top");          /* fujinet-nio: "tnfs:/top" */
   CHECK(host_set("sd0:/img").status == FB_OK, "restore sd0 host");
}

/* ---- app store ------------------------------------------------------------- */

static void test_appstore(void)
{
   buf_t p = payload(); lstr(&p, "config-nio"); lstr(&p, "mappings");
   u32(&p, 0); u16(&p, 4); put(&p, "\x01\x02\x03\x04", 4);
   reply_t r = call(FB_DEV_APPSTORE, 0x03, &p);
   CHECK(r.status == FB_OK && rd16(D(r) + 8) == 4, "write");

   p = payload(); lstr(&p, "config-nio"); lstr(&p, "mappings"); u32(&p, 1); u16(&p, 16);
   r = call(FB_DEV_APPSTORE, 0x02, &p);
   CHECK(r.status == FB_OK && D(r)[1] == 3 && rd32(D(r) + 4) == 1 &&
         rd16(D(r) + 8) == 3 && !memcmp(D(r) + 10, "\x02\x03\x04", 3), "read from offset 1, EOF|exists");
   p = payload(); lstr(&p, "config-nio"); lstr(&p, "mappings"); u32(&p, 0); u16(&p, 2);
   r = call(FB_DEV_APPSTORE, 0x02, &p);
   CHECK(D(r)[1] == 2 && rd16(D(r) + 8) == 2, "short read is not EOF");

   p = payload(); lstr(&p, "config-nio"); lstr(&p, "mappings");
   r = call(FB_DEV_APPSTORE, 0x04, &p);
   CHECK(r.status == FB_OK && D(r)[1] == 1, "delete existing");
   r = call(FB_DEV_APPSTORE, 0x04, &p);
   CHECK(r.status == FB_OK && D(r)[1] == 0, "delete again: nothing there");
   p = payload(); lstr(&p, "config-nio"); lstr(&p, "mappings"); u32(&p, 0); u16(&p, 16);
   r = call(FB_DEV_APPSTORE, 0x02, &p);
   CHECK(r.status == FB_OK && D(r)[1] == 1 && rd16(D(r) + 8) == 0, "read missing: EOF, not exists");

   /* Keys that are not file-name safe still work. */
   CHECK(fn_app_write("n s", "a/b:c", 0, (const uint8_t *)"v", 1), "escaped key write");
   uint8_t b[4]; uint16_t got = 0; bool ex = false;
   CHECK(fn_app_read("n s", "a/b:c", 0, b, 4, &got, &ex) && ex && got == 1 && b[0] == 'v', "escaped key read");
   CHECK(!fn_app_write("ns", "..", 0, (const uint8_t *)"v", 1), "'..' refused as a key");
}

/* ---- slot catalogue ------------------------------------------------------ */

static void test_slotcat(void)
{
   buf_t p = payload(); u8(&p, 3); u8(&p, 0); lstr(&p, "a.ssd");
   reply_t r = call(FB_DEV_SLOTCAT, 0x02, &p);
   CHECK(r.status == FB_OK && D(r)[1] == 1 && D(r)[2] == 3 &&
         rd16(D(r) + 3) == 14 && !memcmp(D(r) + 5, "sd0:/img/a.ssd", 14), "put resolves relative");
   p = payload(); u8(&p, 7); u8(&p, 2); lstr(&p, "tnfs://server.example.org/games/long-name.ssd");
   r = call(FB_DEV_SLOTCAT, 0x02, &p);
   CHECK(r.status == FB_OK && D(r)[1] == 3, "put read-only canonical");

   p = payload(); u8(&p, 3);
   r = call(FB_DEV_SLOTCAT, 0x01, &p);
   CHECK(r.status == FB_OK && rd16(D(r) + 3) == 14, "get 3");
   p = payload(); u8(&p, 4);
   CHECK(call(FB_DEV_SLOTCAT, 0x01, &p).status == FB_DEVICE_NOT_FOUND, "get empty");

   /* Range 0..15, formatted. */
   p = payload(); u8(&p, 0); u8(&p, 15); u8(&p, 0); u8(&p, 2); u8(&p, 255); u16(&p, 200);
   r = call(FB_DEV_SLOTCAT, 0x04, &p);
   const char *want = "3: sd0:/img/a.ssd\n7: tnfs://server.example.org/games/long-name.ssd\n";
   uint16_t el = rd16(D(r) + 5);
   CHECK(r.status == FB_OK && D(r)[1] == 2 && D(r)[3] == 2 && D(r)[4] == 2 &&
         D(r)[7] == 0x88 && D(r)[8] == 0 && el == strlen(want) &&
         !memcmp(D(r) + 9, want, el), "range formatted '%.*s'", el, D(r) + 9);
   /* Binary, tail-truncated to 10 bytes, room for one entry only. */
   p = payload(); u8(&p, 0); u8(&p, 15); u8(&p, 0); u8(&p, 1); u8(&p, 10); u16(&p, 2 + 13 + 5);
   r = call(FB_DEV_SLOTCAT, 0x04, &p);
   /* presence(2) + entry 3 (3+10) = 15 fits in 20; entry 7 would not. */
   CHECK(r.status == FB_OK && D(r)[1] == 1 && D(r)[2] == 7 && D(r)[4] == 1 &&
         rd16(D(r) + 5) == 13 && D(r)[9] == 3 && D(r)[10] == 5 && D(r)[11] == 10 &&
         !memcmp(D(r) + 12, "/img/a.ssd", 10), "range binary tail/more");
   p = payload(); u8(&p, 3);
   r = call(FB_DEV_SLOTCAT, 0x03, &p);
   CHECK(r.status == FB_OK && D(r)[1] == 1, "delete 3");
   p = payload(); u8(&p, 3);
   CHECK(call(FB_DEV_SLOTCAT, 0x01, &p).status == FB_DEVICE_NOT_FOUND, "3 gone");
}

/* ---- disk ----------------------------------------------------------------- */

static reply_t disk_mount(uint8_t slot, uint8_t flags, const char *uri)
{
   buf_t p = payload(); u8(&p, slot); u8(&p, flags); u8(&p, 0); u16(&p, 0); lstr(&p, uri);
   return call(FB_DEV_DISK, 0x01, &p);
}

static reply_t disk_read(uint8_t slot, uint32_t lba, uint16_t max)
{
   buf_t p = payload(); u8(&p, slot); u32(&p, lba); u16(&p, max);
   return call(FB_DEV_DISK, 0x03, &p);
}

static reply_t disk_write(uint8_t slot, uint32_t lba, const uint8_t *d, uint16_t n)
{
   buf_t p = payload(); u8(&p, slot); u32(&p, lba); u16(&p, n); put(&p, d, n);
   return call(FB_DEV_DISK, 0x04, &p);
}

static void test_disk(void)
{
   /* Create an 80-track SSD: fujinet-nio's minimal DFS catalogue. */
   buf_t p = payload(); u8(&p, 0); u8(&p, 2); u16(&p, 256); u32(&p, 800); lstr(&p, "new.ssd");
   reply_t r = call(FB_DEV_DISK, 0x07, &p);
   CHECK(r.status == FB_OK && D(r)[4] == 2 && rd16(D(r) + 5) == 256 && rd32(D(r) + 7) == 800, "create");
   CHECK(file_size("/img/new.ssd") == 204800, "created size %ld", file_size("/img/new.ssd"));
   r = call(FB_DEV_DISK, 0x07, &p);
   CHECK(r.status == FB_INVALID_REQUEST, "create again without overwrite");
   p = payload(); u8(&p, 0); u8(&p, 2); u16(&p, 256); u32(&p, 500); lstr(&p, "bad.ssd");
   CHECK(call(FB_DEV_DISK, 0x07, &p).status == FB_INVALID_REQUEST, "SSD must be 400/800");

   r = disk_mount(1, 0, "new.ssd");
   CHECK(r.status == FB_OK && D(r)[1] == 1 && D(r)[4] == 1 && D(r)[5] == 2 &&
         rd16(D(r) + 6) == 256 && rd32(D(r) + 8) == 800, "mount SSD");
   r = disk_read(1, 0, 256);
   CHECK(r.status == FB_OK && rd16(D(r) + 9) == 256 && !memcmp(D(r) + 11, "BLANK", 5), "catalogue title");
   r = disk_read(1, 1, 256);
   CHECK(D(r)[11 + 6] == 3 && D(r)[11 + 7] == 0x20, "catalogue sector count 800");

   uint8_t sec[256];
   for (int i = 0; i < 256; i++) sec[i] = (uint8_t)(i ^ 0x5A);
   r = disk_write(1, 5, sec, 256);
   CHECK(r.status == FB_OK && rd32(D(r) + 5) == 5 && rd16(D(r) + 9) == 256, "write sector 5");
   r = disk_read(1, 5, 300);
   CHECK(r.status == FB_OK && D(r)[1] == 0 && rd16(D(r) + 9) == 256 &&
         !memcmp(D(r) + 11, sec, 256), "read back sector 5");
   r = disk_read(1, 5, 8);
   CHECK(D(r)[1] == 1 && rd16(D(r) + 9) == 8 && !memcmp(D(r) + 11, sec, 8), "truncated read");
   CHECK(disk_read(1, 800, 256).status == FB_INVALID_REQUEST, "lba out of range");
   CHECK(disk_write(1, 6, sec, 255).status == FB_INVALID_REQUEST, "short write refused");

   /* A sparse image: catalogue says 800 sectors, file holds 4. */
   uint8_t img[1024] = { 0 };
   img[0x106] = 3; img[0x107] = 0x20; img[3 * 256] = 0xEE;
   write_file("/img/sparse.ssd", img, sizeof img);
   r = disk_mount(2, 0, "sd0:/img/sparse.ssd");
   CHECK(r.status == FB_OK && rd32(D(r) + 8) == 800, "sparse mounts as 800 sectors");
   r = disk_read(2, 3, 256);
   CHECK(D(r)[11] == 0xEE, "sparse in-file sector");
   r = disk_read(2, 700, 256);
   CHECK(r.status == FB_OK && rd16(D(r) + 9) == 256 && D(r)[11] == 0 && D(r)[11 + 255] == 0, "sparse past EOF reads zeros");
   CHECK(disk_write(2, 10, sec, 256).status == FB_OK, "write past EOF");
   CHECK(file_size("/img/sparse.ssd") == 11 * 256, "gap zero-filled, size %ld", file_size("/img/sparse.ssd"));
   r = disk_read(2, 7, 256);
   CHECK(D(r)[11] == 0, "gap is zeros");

   /* Read-only mount. */
   r = disk_mount(3, 1, "new.ssd");
   CHECK(r.status == FB_OK && D(r)[1] == 3, "read-only mount effective");
   CHECK(disk_write(3, 5, sec, 256).status == FB_INVALID_REQUEST, "write to RO refused");

   /* A bad image, a missing one, an unsupported type. */
   uint8_t junk[1024] = { 0 };
   write_file("/img/junk.ssd", junk, sizeof junk);
   CHECK(disk_mount(4, 0, "junk.ssd").status == FB_INVALID_REQUEST, "catalogue count 0 is a bad image");
   CHECK(disk_mount(4, 0, "missing.ssd").status == FB_INVALID_REQUEST, "missing file");
   write_file("/img/two.dsd", junk, sizeof junk);
   CHECK(disk_mount(4, 0, "two.dsd").status == FB_UNSUPPORTED, "DSD unsupported, as upstream");
   CHECK(disk_mount(0, 0, "new.ssd").status == FB_INVALID_REQUEST, "slot 0");
   CHECK(disk_mount(9, 0, "new.ssd").status == FB_INVALID_REQUEST, "slot 9");
   CHECK(disk_read(4, 0, 256).status == FB_NOT_READY, "empty slot not ready");

   /* Info: fujinet-nio sets hasGeometry and hasLastError. */
   p = payload(); u8(&p, 1);
   r = call(FB_DEV_DISK, 0x05, &p);
   CHECK(r.status == FB_OK && r.dlen == 13 && D(r)[1] == 0x39 && D(r)[5] == 2 &&
         rd16(D(r) + 6) == 256 && rd32(D(r) + 8) == 800, "info flags %02X", D(r)[1]);
   p = payload(); u8(&p, 1);
   call(FB_DEV_DISK, 0x06, &p);
   r = call(FB_DEV_DISK, 0x05, &p);
   CHECK(D(r)[1] == 0x31, "changed cleared");

   /* Lazy mount: all-zero reply, opened by first use. */
   r = disk_mount(5, 2, "new.ssd");
   CHECK(r.status == FB_OK && r.dlen == 12 && D(r)[0] == 1 && D(r)[4] == 5 &&
         D(r)[1] == 0 && D(r)[5] == 0, "lazy mount reply");
   r = disk_read(5, 5, 256);
   CHECK(r.status == FB_OK && !memcmp(D(r) + 11, sec, 256), "lazy mount opens on read");
   r = disk_mount(6, 2, "gone.ssd");
   CHECK(r.status == FB_OK, "lazy mount of a missing file is recorded");
   p = payload(); u8(&p, 6);
   CHECK(call(FB_DEV_DISK, 0x05, &p).status == FB_INVALID_REQUEST, "and fails when used");

   /* ListMounts: formatted only, 0-based units. */
   p = payload(); u8(&p, 1); u16(&p, 0); u16(&p, 0); u16(&p, 0); u16(&p, 400);
   r = call(FB_DEV_DISK, 0x0D, &p);
   const char *want = "0: AUTO sd0:/img/new.ssd\n1: AUTO sd0:/img/sparse.ssd\n"
                      "2: RO sd0:/img/new.ssd\n4: AUTO sd0:/img/new.ssd\n5: AUTO sd0:/img/gone.ssd\n";
   uint16_t el = rd16(D(r) + 8);
   CHECK(r.status == FB_OK && D(r)[1] == 2 && rd16(D(r) + 6) == 5 && el == strlen(want) &&
         !memcmp(D(r) + 10, want, el), "list mounts '%.*s'", el, D(r) + 10);
   p = payload(); u8(&p, 0); u16(&p, 0); u16(&p, 0); u16(&p, 0); u16(&p, 400);
   CHECK(call(FB_DEV_DISK, 0x0D, &p).status == FB_INVALID_REQUEST, "binary list refused, as upstream");

   /* Reinitialize slot 1 as 40 tracks. */
   p = payload(); u8(&p, 1); u16(&p, 256); u32(&p, 400);
   r = call(FB_DEV_DISK, 0x0C, &p);
   CHECK(r.status == FB_OK && rd32(D(r) + 8) == 400 && file_size("/img/new.ssd") == 102400, "reinitialize");

   /* BeginHostSession: everything unmounted, no boot image configured. */
   p = payload(); u8(&p, 1);
   r = call(FB_DEV_DISK, 0x0B, &p);
   CHECK(r.status == FB_OK && r.dlen == 5 && D(r)[1] == 0, "begin session, no boot");
   CHECK(disk_read(1, 0, 256).status == FB_NOT_READY && disk_read(3, 0, 256).status == FB_NOT_READY, "all unmounted");
   CHECK(fn_store_host_open_count() == 0, "no handles leaked: %d", fn_store_host_open_count());
   p = payload(); u8(&p, 1);
   CHECK(call(FB_DEV_DISK, 0x0A, &p).status == FB_NOT_READY, "restore boot without one");
   fn_disk_set_boot("sd0:/img/new.ssd", true);
   r = call(FB_DEV_DISK, 0x0B, &p);
   CHECK(r.status == FB_OK && r.dlen == 12 && D(r)[1] == 3 && rd32(D(r) + 8) == 400, "begin session mounts boot");
   fn_disk_set_boot("", true);
}

/* ---- file ------------------------------------------------------------------- */

static void test_file(void)
{
   buf_t p = payload(); lstr(&p, "sd0:/img"); u16(&p, 0); u16(&p, 400); u8(&p, 0x06); u8(&p, 40);
   reply_t r = call(FB_DEV_FILE, 0x02, &p);
   uint16_t count = rd16(D(r) + 6), el = rd16(D(r) + 8);
   char text[1024];
   memcpy(text, D(r) + 10, el);
   text[el] = '\0';
   /* junk.ssd new.ssd sparse.ssd sub/ two.dsd - bad.ssd was never created. */
   CHECK(r.status == FB_OK && D(r)[1] == 4 && count == 5, "list count %u flags %02X", count, D(r)[1]);
   CHECK(strstr(text, "junk.ssd\n") && strstr(text, "d       0") && strstr(text, " sub/\n") &&
         strstr(text, " 100.0K ") && strstr(text, "   2.8K ") && strncmp(text, "- ", 2) == 0,
         "listing:\n%s", text);
   CHECK(strstr(text, "new.ssd") < strstr(text, "sparse.ssd"), "sorted");
   /* Paged: one line at a time. */
   p = payload(); lstr(&p, "sd0:/img"); u16(&p, 1); u16(&p, 50); u8(&p, 0x06); u8(&p, 40);
   r = call(FB_DEV_FILE, 0x02, &p);
   CHECK(r.status == FB_OK && rd16(D(r) + 6) == 1 && (D(r)[1] & 1), "paged with more");
   p = payload(); lstr(&p, "sd0:/nope"); u16(&p, 0); u16(&p, 50);
   CHECK(call(FB_DEV_FILE, 0x02, &p).status == FB_IO_ERROR, "missing directory");
   p = payload(); lstr(&p, "zz9:/"); u16(&p, 0); u16(&p, 50);
   CHECK(call(FB_DEV_FILE, 0x02, &p).status == FB_DEVICE_NOT_FOUND, "unknown filesystem");
}

/* ---- fuzz: random requests with valid framing must never crash ------------ */

static void test_fuzz(void)
{
   static const uint8_t devs[] = { 0xF0, 0xF1, 0xF2, 0xFC, 0xFD, 0xFE };
   srand(1);
   for (int i = 0; i < 20000; i++) {
      uint8_t pkt[300];
      uint16_t n = (uint16_t)(6 + rand() % 200);
      pkt[0] = devs[rand() % 6];
      pkt[1] = (uint8_t)(rand() % 16);
      pkt[2] = (uint8_t)n; pkt[3] = (uint8_t)(n >> 8); pkt[4] = 0; pkt[5] = 0;
      for (int k = 6; k < n; k++) pkt[k] = (uint8_t)rand();
      if (n > 6) pkt[6] = 1;                         /* mostly version 1 */
      if (n > 7 && pkt[0] == FB_DEV_DISK) pkt[7] = (uint8_t)(1 + rand() % 8);
      /* Never let it create or delete outside the test tree: the store is
         rooted there anyway. */
      pkt[4] = fb_checksum(pkt, n);
      uint8_t reply[1024];
      uint16_t rl = 0;
      bool ok = fujibus_answer(pkt, n, reply, sizeof reply, &rl);
      if (!ok || rl < 7 || fb_checksum(reply, rl) != reply[4]) {
         s_fail++;
         printf("FAIL fuzz %d\n", i);
         break;
      }
   }
   s_pass++;
   fn_disk_reset();
   CHECK(fn_store_host_open_count() == 0, "fuzz leaked %d handles", fn_store_host_open_count());
}

int main(void)
{
   snprintf(s_root, sizeof s_root, "/tmp/fujinet-test-XXXXXX");
   if (!mkdtemp(s_root)) return 2;
   fn_store_host_root(s_root);

   test_packet_layer();
   test_host();
   test_appstore();
   test_slotcat();
   test_disk();
   test_file();
   test_fuzz();

   char cmd[300];
   snprintf(cmd, sizeof cmd, "rm -rf %s", s_root);
   if (system(cmd)) {}
   printf("%d passed, %d failed\n", s_pass, s_fail);
   return s_fail ? 1 : 0;
}
