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
#include "fake_tnfs.h"
#include "fn_tnfs.h"
#include "fn_network.h"
#include "fake_net.h"
#include "fn_json.h"

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
   int passes;            /* service-loop passes it took (1 = no waiting) */
   uint8_t status;
   uint8_t pkt[4096];
   uint16_t len;
   uint16_t dlen;         /* device payload is D(r): pkt + 7 */
} reply_t;

/* The service loop: while the answer is pending, let the network deliver,
   let time pass and poll, then run the same packet again - as
   fujibus_service_poll does on the Pi. */
static reply_t call_raw(const uint8_t *pkt, uint16_t n)
{
   reply_t r = { 0 };
   fb_answer a;
   while ((a = fujibus_answer(pkt, n, r.pkt, sizeof r.pkt, &r.len)) == FB_ANSWER_PENDING &&
          ++r.passes < 200000) {
      fake_tnfs_step();
      fake_tnfs_advance(1);
      fn_store_poll();
      fn_network_poll();
   }
   r.passes++;
   CHECK(a != FB_ANSWER_PENDING, "request never finished waiting");
   if (a == FB_ANSWER_PENDING)
      fn_store_request_abort();     /* as the Pi does when the Beeb moves on */
   r.answered = a == FB_ANSWER_REPLY;
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

static void read_file_at(const char *rel, long off, void *d, size_t n)
{
   char p[512];
   snprintf(p, sizeof p, "%s%s", s_root, rel);
   FILE *f = fopen(p, "rb");
   memset(d, 0xA5, n);
   if (f) {
      if (fseek(f, off, SEEK_SET) == 0 && fread(d, 1, n, f) != n)
         memset(d, 0xA5, n);
      fclose(f);
   }
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

static void test_appstore_limits(void)
{
   /* offset 0 replaces the value (nio "wb"); a later offset updates it */
   buf_t p = payload(); lstr(&p, "t-ns"); lstr(&p, "k");
   u32(&p, 0); u16(&p, 6); put(&p, "ABCDEF", 6);
   CHECK(call(FB_DEV_APPSTORE, 0x03, &p).status == FB_OK, "write 6");
   p = payload(); lstr(&p, "t-ns"); lstr(&p, "k"); u32(&p, 0); u16(&p, 2); put(&p, "xy", 2);
   CHECK(call(FB_DEV_APPSTORE, 0x03, &p).status == FB_OK, "write 2 at 0");
   p = payload(); lstr(&p, "t-ns"); lstr(&p, "k"); u32(&p, 0); u16(&p, 16);
   reply_t r = call(FB_DEV_APPSTORE, 0x02, &p);
   CHECK(r.status == FB_OK && rd16(D(r) + 8) == 2 && !memcmp(D(r) + 10, "xy", 2),
         "offset 0 truncates: %u bytes", rd16(D(r) + 8));
   p = payload(); lstr(&p, "t-ns"); lstr(&p, "k"); u32(&p, 1); u16(&p, 1); put(&p, "Z", 1);
   call(FB_DEV_APPSTORE, 0x03, &p);
   p = payload(); lstr(&p, "t-ns"); lstr(&p, "k"); u32(&p, 0); u16(&p, 16);
   r = call(FB_DEV_APPSTORE, 0x02, &p);
   CHECK(rd16(D(r) + 8) == 2 && !memcmp(D(r) + 10, "xZ", 2), "offset 1 updates in place");
   /* a far offset is refused, not zero-filled up to */
   p = payload(); lstr(&p, "t-ns"); lstr(&p, "k"); u32(&p, 0xF0000000u); u16(&p, 1); put(&p, "!", 1);
   r = call(FB_DEV_APPSTORE, 0x03, &p);
   CHECK(r.status == FB_INVALID_REQUEST && r.passes == 1, "a far offset is refused at once");
   p = payload(); lstr(&p, "t-ns"); lstr(&p, "k"); u32(&p, 65535); u16(&p, 1); put(&p, "!", 1);
   CHECK(call(FB_DEV_APPSTORE, 0x03, &p).status == FB_OK, "the last byte of 64 KB is allowed");
   p = payload(); lstr(&p, "t-ns"); lstr(&p, "k");
   call(FB_DEV_APPSTORE, 0x04, &p);
}

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

static reply_t slot_range_bitmap(void)
{
   buf_t p = payload(); u8(&p, 0); u8(&p, 15); u8(&p, 0); u8(&p, 0); u8(&p, 255); u16(&p, 200);
   return call(FB_DEV_SLOTCAT, 0x04, &p);
}

static void test_slotcat(void)
{
   /* No slot directory yet (no card, or none ever put) is not latched as
      "all empty": slot 9 arriving on the card is seen. */
   reply_t r = slot_range_bitmap();
   CHECK(r.status == FB_OK && D(r)[7] == 0 && D(r)[8] == 0, "range with no slot directory");
   CHECK(fn_app_write("fujinet-slots", "9", 0, (const uint8_t *)"\0sd0:/x.ssd", 11), "slot 9 on the card");
   r = slot_range_bitmap();
   CHECK(r.status == FB_OK && D(r)[8] == 0x02, "slot 9 seen after an empty first scan");
   CHECK(fn_app_delete("fujinet-slots", "9", NULL), "slot 9 removed behind the cache");
   fn_slotcat_forget();

   buf_t p = payload(); u8(&p, 3); u8(&p, 0); lstr(&p, "a.ssd");
   r = call(FB_DEV_SLOTCAT, 0x02, &p);
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

   /* A card swap: the occupancy is forgotten and the new card scanned. */
   CHECK(fn_app_write("fujinet-slots", "12", 0, (const uint8_t *)"\0sd0:/y.ssd", 11), "slot 12 on the new card");
   r = slot_range_bitmap();
   CHECK(r.status == FB_OK && D(r)[7] == 0x80 && D(r)[8] == 0, "before the eject the old occupancy stands");
   fn_slotcat_forget();
   r = slot_range_bitmap();
   CHECK(r.status == FB_OK && D(r)[7] == 0x80 && D(r)[8] == 0x10, "after the eject slot 12 is seen");
   CHECK(fn_app_delete("fujinet-slots", "12", NULL), "slot 12 removed");
   fn_slotcat_forget();
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
   CHECK(disk_mount(4, 0, "two.dsd").status == FB_INVALID_REQUEST, "DSD catalogue count 0 is a bad image");
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

/* ---- DSD ---------------------------------------------------------------------
   A double-sided DFS image, as fujinet-nio 888135d: two SSD sides stored
   track-interleaved (track 0 side 0, track 0 side 1, track 1 side 0 ...),
   served as logical sectors through side 0 then side 1. */

/* File offset of a DSD's logical sector, for a side of `per_side` sectors. */
static long dsd_offset(uint32_t lba, uint32_t per_side)
{
   uint32_t side = lba / per_side, in_side = lba % per_side;
   return ((long)(in_side / 10u * 2u + side) * 10 + (long)(in_side % 10u)) * 256;
}

static void test_dsd(void)
{
   int open_before = fn_store_host_open_count();   /* test_disk leaves its boot image mounted */
   mkdir_rel("/dsd");

   /* Create: fujinet-nio's blank catalogue on each side, full size. */
   buf_t p = payload(); u8(&p, 0); u8(&p, 3); u16(&p, 256); u32(&p, 1600); lstr(&p, "sd0:/dsd/new.dsd");
   reply_t r = call(FB_DEV_DISK, 0x07, &p);
   CHECK(r.status == FB_OK && D(r)[4] == 3 && rd16(D(r) + 5) == 256 && rd32(D(r) + 7) == 1600, "create DSD");
   CHECK(file_size("/dsd/new.dsd") == 409600, "DSD created size %ld", file_size("/dsd/new.dsd"));
   uint8_t got[256];
   read_file_at("/dsd/new.dsd", 0, got, 5);
   CHECK(!memcmp(got, "BLANK", 5), "side 0 title in track 0 side 0");
   read_file_at("/dsd/new.dsd", 256 + 6, got, 2);
   CHECK(got[0] == 3 && got[1] == 0x20, "side 0 catalogue counts 800");
   read_file_at("/dsd/new.dsd", 2560, got, 5);
   CHECK(!memcmp(got, "BLANK", 5), "side 1 title in track 0 side 1");
   read_file_at("/dsd/new.dsd", 2560 + 256 + 6, got, 2);
   CHECK(got[0] == 3 && got[1] == 0x20, "side 1 catalogue counts 800");
   p = payload(); u8(&p, 0); u8(&p, 3); u16(&p, 256); u32(&p, 800); lstr(&p, "sd0:/dsd/forty.dsd");
   CHECK(call(FB_DEV_DISK, 0x07, &p).status == FB_OK && file_size("/dsd/forty.dsd") == 204800,
         "create 40-track DSD");
   p = payload(); u8(&p, 0); u8(&p, 3); u16(&p, 256); u32(&p, 900); lstr(&p, "sd0:/dsd/bad.dsd");
   CHECK(call(FB_DEV_DISK, 0x07, &p).status == FB_INVALID_REQUEST, "DSD must be 800/1600");
   p = payload(); u8(&p, 0); u8(&p, 3); u16(&p, 512); u32(&p, 1600); lstr(&p, "sd0:/dsd/bad.dsd");
   CHECK(call(FB_DEV_DISK, 0x07, &p).status == FB_INVALID_REQUEST, "DSD sectors are 256 bytes");

   /* Mount: type DSD, both sides' sectors. */
   r = disk_mount(7, 0, "sd0:/dsd/new.dsd");
   CHECK(r.status == FB_OK && D(r)[5] == 3 && rd16(D(r) + 6) == 256 && rd32(D(r) + 8) == 1600,
         "mount 80-track DSD");
   r = disk_read(7, 800, 256);
   CHECK(r.status == FB_OK && !memcmp(D(r) + 11, "BLANK", 5), "side 1 starts at sector 800");
   CHECK(disk_read(7, 1600, 256).status == FB_INVALID_REQUEST, "lba 1600 out of range");

   /* Each logical sector lands in its interleaved place in the file. */
   static const uint32_t lbas[] = { 2, 10, 799, 805, 1599 };
   for (unsigned int i = 0; i < sizeof lbas / sizeof lbas[0]; i++) {
      uint8_t sec[256];
      for (int k = 0; k < 256; k++) sec[k] = (uint8_t)(k + 7 * (int)i + 1);
      CHECK(disk_write(7, lbas[i], sec, 256).status == FB_OK, "write lba %u", lbas[i]);
      read_file_at("/dsd/new.dsd", dsd_offset(lbas[i], 800), got, 256);
      CHECK(!memcmp(got, sec, 256), "lba %u at file offset %ld", lbas[i], dsd_offset(lbas[i], 800));
      r = disk_read(7, lbas[i], 256);
      CHECK(r.status == FB_OK && !memcmp(D(r) + 11, sec, 256), "read back lba %u", lbas[i]);
   }

   /* 40 tracks a side: side 1 from sector 400, at track 0 side 1. */
   r = disk_mount(8, 0, "sd0:/dsd/forty.dsd");
   CHECK(r.status == FB_OK && D(r)[5] == 3 && rd32(D(r) + 8) == 800, "mount 40-track DSD");
   r = disk_read(8, 400, 256);
   CHECK(r.status == FB_OK && !memcmp(D(r) + 11, "BLANK", 5), "40-track side 1 starts at 400");

   /* Truncated: only track 0 of each side is in the file. */
   uint8_t img[5120] = { 0 };
   img[0x106] = 3; img[0x107] = 0x20; img[2560] = 0xEE;
   write_file("/dsd/short.dsd", img, sizeof img);
   r = disk_mount(8, 0, "sd0:/dsd/short.dsd");
   CHECK(r.status == FB_OK && rd32(D(r) + 8) == 1600, "truncated DSD mounts as 1600");
   r = disk_read(8, 800, 256);
   CHECK(r.status == FB_OK && D(r)[11] == 0xEE, "truncated side 1 in-file sector");
   r = disk_read(8, 900, 256);
   CHECK(r.status == FB_OK && rd16(D(r) + 9) == 256 && D(r)[11] == 0 && D(r)[11 + 255] == 0,
         "truncated past EOF reads zeros");
   uint8_t sec[256];
   memset(sec, 0x3C, sizeof sec);
   CHECK(disk_write(8, 900, sec, 256).status == FB_OK, "write past EOF");
   CHECK(file_size("/dsd/short.dsd") == dsd_offset(900, 800) + 256,
         "file grows to the interleaved sector, size %ld", file_size("/dsd/short.dsd"));

   p = payload(); u8(&p, 7);
   call(FB_DEV_DISK, 0x02, &p);
   p = payload(); u8(&p, 8);
   call(FB_DEV_DISK, 0x02, &p);
   CHECK(fn_store_host_open_count() == open_before, "DSD: no handles leaked: %d, was %d",
         fn_store_host_open_count(), open_before);
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
   /* An empty path is the current directory: fn-rom's plain *FLS sends that. */
   CHECK(host_set("sd0:/img").status == FB_OK, "current host sd0:/img");
   p = payload(); lstr(&p, ""); u16(&p, 0); u16(&p, 400); u8(&p, 0x06); u8(&p, 40);
   r = call(FB_DEV_FILE, 0x02, &p);
   CHECK(r.status == FB_OK && rd16(D(r) + 6) == 5, "empty path lists the current directory: status %02X count %u",
         r.status, r.status == FB_OK ? rd16(D(r) + 6) : 0u);
   /* fn-rom's request exactly: no lineWidth byte after the flags. */
   p = payload(); lstr(&p, ""); u16(&p, 0); u16(&p, 220); u8(&p, 0x06);
   r = call(FB_DEV_FILE, 0x02, &p);
   CHECK(r.status == FB_OK && rd16(D(r) + 6) == 5, "formatted list without lineWidth: status %02X", r.status);
}

/* ---- TNFS: the same devices on a server, through waits -------------------- */

static void make_ssd(uint8_t *img, uint32_t size, uint16_t count, const char *title)
{
   memset(img, 0, size);
   memcpy(img, title, strlen(title));
   img[0x106] = (uint8_t)(count >> 8);
   img[0x107] = (uint8_t)count;
}

static void test_tnfs(void)
{
   static uint8_t img[204800];
   fake_tnfs_mkdir("/games");
   make_ssd(img, sizeof img, 800, "ELITE");
   for (int i = 0; i < 256; i++) img[5 * 256 + i] = (uint8_t)(i * 3);
   fake_tnfs_put("/games/elite.ssd", img, sizeof img);
   uint8_t small[1024];
   make_ssd(small, sizeof small, 400, "SPARSE");
   fake_tnfs_put("/games/sparse.ssd", small, sizeof small);
   fake_tnfs_mkdir("/games/more");

   /* Host on the server: resolved, mounted, STAT says it is a directory. */
   reply_t r = host_set("tnfs://tnfs.test/games");
   CHECK(r.status == FB_OK && r.passes > 1, "set a tnfs host (waited %d passes)", r.passes);
   expect_current("tnfs://tnfs.test/games", "/games");
   CHECK(host_set("tnfs://tnfs.test/nothing").status == FB_IO_ERROR, "missing tnfs directory refused");
   CHECK(host_set("tnfs://nowhere.test/").status == FB_IO_ERROR, "unresolvable server refused");

   /* Catalogue and mount, relative to the tnfs host. */
   buf_t p = payload(); u8(&p, 10); u8(&p, 0); lstr(&p, "elite.ssd");
   r = call(FB_DEV_SLOTCAT, 0x02, &p);
   CHECK(r.status == FB_OK && rd16(D(r) + 3) == strlen("tnfs://tnfs.test/games/elite.ssd") &&
         !memcmp(D(r) + 5, "tnfs://tnfs.test/games/elite.ssd", rd16(D(r) + 3)), "tnfs slot is canonical");
   r = disk_mount(1, 0, "elite.ssd");
   CHECK(r.status == FB_OK && D(r)[1] == 1 && rd32(D(r) + 8) == 800, "mount tnfs SSD rw");
   r = disk_read(1, 0, 256);
   CHECK(r.status == FB_OK && !memcmp(D(r) + 11, "ELITE", 5), "tnfs catalogue");
   r = disk_read(1, 5, 256);
   CHECK(r.status == FB_OK && D(r)[11 + 7] == 21 && D(r)[11 + 255] == (uint8_t)(255 * 3), "tnfs sector 5");

   /* Write, and see it on the server. */
   uint8_t sec[256];
   for (int i = 0; i < 256; i++) sec[i] = (uint8_t)(0xA5 ^ i);
   CHECK(disk_write(1, 9, sec, 256).status == FB_OK, "tnfs write sector 9");
   {
      static uint8_t big[257];
      CHECK(disk_write(1, 9, big, 257).status == FB_INVALID_REQUEST, "more than a sector is refused (nio)");
   }
   uint32_t sz = 0;
   const uint8_t *srv = fake_tnfs_get("/games/elite.ssd", &sz);
   CHECK(srv && sz == sizeof img && !memcmp(srv + 9 * 256, sec, 256), "written on the server");
   r = disk_read(1, 9, 256);
   CHECK(!memcmp(D(r) + 11, sec, 256), "tnfs read back");

   /* Lost datagrams are resent; a busy server is waited for. */
   int before = fake_tnfs_stats()->sent;
   fake_tnfs_drop(2);
   r = disk_read(1, 5, 256);
   CHECK(r.status == FB_OK && D(r)[11 + 7] == 21 && fake_tnfs_stats()->sent - before >= 3,
         "read survives two lost datagrams");
   fake_tnfs_eagain(3);
   CHECK(disk_write(1, 10, sec, 256).status == FB_OK, "write survives three EAGAINs");
   srv = fake_tnfs_get("/games/elite.ssd", &sz);
   CHECK(!memcmp(srv + 10 * 256, sec, 256), "EAGAIN'd write landed once");

   /* A sparse image on the server reads zeros past its end, and a write past
      the end leaves zeros in the gap (the server's POSIX file does). */
   r = disk_mount(2, 0, "sparse.ssd");
   CHECK(r.status == FB_OK && rd32(D(r) + 8) == 400, "sparse tnfs SSD");
   r = disk_read(2, 300, 256);
   CHECK(r.status == FB_OK && D(r)[11] == 0, "past the end reads zeros");
   CHECK(disk_write(2, 20, sec, 256).status == FB_OK, "write past the end");
   srv = fake_tnfs_get("/games/sparse.ssd", &sz);
   CHECK(sz == 21 * 256 && srv[10 * 256] == 0 && !memcmp(srv + 20 * 256, sec, 256), "gap zeros on server");

   /* Create on the server. */
   p = payload(); u8(&p, 0); u8(&p, 2); u16(&p, 256); u32(&p, 400); lstr(&p, "new.ssd");
   r = call(FB_DEV_DISK, 0x07, &p);
   srv = fake_tnfs_get("/games/new.ssd", &sz);
   CHECK(r.status == FB_OK && srv && sz == 102400 && !memcmp(srv, "BLANK", 5) &&
         srv[0x107] == 0x90 && srv[0x106] == 1, "create SSD on tnfs");
   CHECK(call(FB_DEV_DISK, 0x07, &p).status == FB_INVALID_REQUEST, "create again refused (exists)");

   /* Reinitialize a tnfs image: its catalogue is rewritten for 800. */
   r = disk_mount(3, 0, "new.ssd");
   p = payload(); u8(&p, 3); u16(&p, 256); u32(&p, 800);
   r = call(FB_DEV_DISK, 0x0C, &p);
   srv = fake_tnfs_get("/games/new.ssd", &sz);
   CHECK(r.status == FB_OK && rd32(D(r) + 8) == 800 && sz == 204800 && srv[0x107] == 0x20, "reinitialize on tnfs: status %u count %u size %u cat %02X passes %d",
         r.status, rd32(D(r) + 8), sz, srv ? srv[0x107] : 0, r.passes);

   /* Listing a tnfs directory: sorted, formatted, sizes and a folder. */
   p = payload(); lstr(&p, "tnfs://tnfs.test/games"); u16(&p, 0); u16(&p, 400); u8(&p, 0x06); u8(&p, 40);
   r = call(FB_DEV_FILE, 0x02, &p);
   char text[1024];
   uint16_t el = rd16(D(r) + 8);
   memcpy(text, D(r) + 10, el);
   text[el] = '\0';
   CHECK(r.status == FB_OK && rd16(D(r) + 6) == 4 && strstr(text, " more/\n") &&
         strstr(text, " 200.0K ") && strstr(text, "Nov 14  2023") &&
         strstr(text, "elite.ssd") < strstr(text, "new.ssd"), "tnfs listing:\n%s", text);
   int sent = fake_tnfs_stats()->sent;
   p = payload(); lstr(&p, "tnfs://tnfs.test/games"); u16(&p, 2); u16(&p, 400); u8(&p, 0x06); u8(&p, 40);
   r = call(FB_DEV_FILE, 0x02, &p);
   CHECK(r.status == FB_OK && rd16(D(r) + 6) == 2 && fake_tnfs_stats()->sent - sent <= 1,
         "the next page comes from the cached listing");

   /* A dead server fails the request; it is not left pending forever. */
   fake_tnfs_drop(1000);
   r = disk_read(1, 5, 256);
   CHECK(r.answered && r.status == FB_IO_ERROR, "dead server: IOError after %d passes", r.passes);
   fake_tnfs_drop(0);
   r = disk_read(1, 5, 256);
   CHECK(r.status == FB_OK && D(r)[11 + 7] == 21, "server back: remounted and reading");

   /* A resolver that takes a moment. */
   fake_tnfs_mkdir("/");
   CHECK(host_set("tnfs://slow.test/").status == FB_OK, "slow DNS waited for");

   /* The Beeb gives up on a waiting request and asks something else. */
   {
      uint8_t pkt[64];
      buf_t q = payload(); u8(&q, 1); u32(&q, 6); u16(&q, 256);
      uint16_t n = (uint16_t)(6 + q.n);
      pkt[0] = FB_DEV_DISK; pkt[1] = 0x03; pkt[2] = (uint8_t)n; pkt[3] = 0; pkt[4] = 0; pkt[5] = 0;
      memcpy(pkt + 6, q.b, q.n);
      pkt[4] = fb_checksum(pkt, n);
      uint8_t reply[600];
      uint16_t rl;
      fake_tnfs_drop(1000);
      CHECK(fujibus_answer(pkt, n, reply, sizeof reply, &rl) == FB_ANSWER_PENDING, "read pending");
      fn_store_request_abort();
      fake_tnfs_drop(0);
      r = disk_read(1, 5, 256);
      CHECK(r.status == FB_OK && D(r)[11 + 7] == 21, "the next request after an abort works");
   }

   /* The Beeb gives up after the server has opened a file for the request
      but before the request finished: the orphan handle must be closed, or
      eight such give-ups use up every TNFS handle. */
   {
      static uint8_t other[204800];
      make_ssd(other, sizeof other, 800, "OTHER");
      fake_tnfs_put("/games/other.ssd", other, sizeof other);
      fake_tnfs_step();
      int base = fake_tnfs_open_fds();
      buf_t q = payload(); u8(&q, 5); u8(&q, 0); u8(&q, 0); u16(&q, 0);
      lstr(&q, "tnfs://tnfs.test/games/other.ssd");                  /* a mount opens the image */
      uint8_t pkt[96];
      uint16_t n = (uint16_t)(6 + q.n);
      pkt[0] = FB_DEV_DISK; pkt[1] = 0x01; pkt[2] = (uint8_t)n; pkt[3] = 0; pkt[4] = 0; pkt[5] = 0;
      memcpy(pkt + 6, q.b, q.n);
      pkt[4] = fb_checksum(pkt, n);
      uint8_t reply[600];
      uint16_t rl;
      fb_answer a = FB_ANSWER_PENDING;
      for (int i = 0; i < 1000 && fake_tnfs_open_fds() == base; i++) {
         a = fujibus_answer(pkt, n, reply, sizeof reply, &rl);
         if (a != FB_ANSWER_PENDING) break;
         fake_tnfs_step();
         fake_tnfs_advance(1);
         fn_store_poll();
      }
      CHECK(fake_tnfs_open_fds() == base + 1, "the mount's OPEN happened on the server");
      fake_tnfs_drop(1000);                         /* the rest of the mount stalls */
      a = fujibus_answer(pkt, n, reply, sizeof reply, &rl);
      CHECK(a == FB_ANSWER_PENDING, "the mount is still waiting");
      fn_store_request_abort();
      fake_tnfs_drop(0);
      fake_tnfs_step();
      CHECK(fake_tnfs_open_fds() == base, "abort closed the orphan handle (%d open, %d before)",
            fake_tnfs_open_fds(), base);
   }

   /* Unmount closes the file on the server. */
   p = payload(); u8(&p, 1);
   call(FB_DEV_DISK, 0x02, &p);
   p = payload(); u8(&p, 2);
   call(FB_DEV_DISK, 0x02, &p);
   p = payload(); u8(&p, 3);
   call(FB_DEV_DISK, 0x02, &p);
   p = payload(); u8(&p, 5);
   call(FB_DEV_DISK, 0x02, &p);
   fake_tnfs_step();
   CHECK(fake_tnfs_open_fds() == 0, "no fds left open on the server: %d", fake_tnfs_open_fds());
   CHECK(host_set("sd0:/img").status == FB_OK, "back to the SD card");
}

/* ---- fuzz: random requests with valid framing must never crash ------------ */

/* ---- NetworkDevice ($FD) ----------------------------------------------------
   Layouts from fujinet-nio src/lib/network_device.cpp. */

#define NET_H(r) rd16(D(r) + 4)

static buf_t net_open_req(uint8_t method, uint8_t flags, const char *url, uint32_t body)
{
   buf_t p = payload();
   u8(&p, method); u8(&p, flags); lstr(&p, url);
   u16(&p, 0);                 /* request headers */
   u32(&p, body);              /* body length hint */
   u16(&p, 0);                 /* response header names */
   return p;
}

static reply_t net_open(uint8_t method, uint8_t flags, const char *url, uint32_t body)
{
   buf_t p = net_open_req(method, flags, url, body);
   return call(FB_DEV_NETWORK, 0x01, &p);
}

static reply_t net_read(uint16_t h, uint32_t off, uint16_t max)
{
   buf_t p = payload(); u16(&p, h); u32(&p, off); u16(&p, max);
   return call(FB_DEV_NETWORK, 0x02, &p);
}

static reply_t net_write(uint16_t h, uint32_t off, const void *d, uint16_t n)
{
   buf_t p = payload(); u16(&p, h); u32(&p, off); u16(&p, n); put(&p, d, n);
   return call(FB_DEV_NETWORK, 0x03, &p);
}

static uint8_t net_close(uint16_t h)
{
   buf_t p = payload(); u16(&p, h);
   reply_t r = call(FB_DEV_NETWORK, 0x04, &p);
   if (r.status == FB_OK)
      CHECK(r.dlen == 4 && D(r)[0] == 1, "close reply is the 4-byte prefix");
   return r.status;
}

static void net_poll(int n) { while (n--) fn_network_poll(); }

static void test_network(void)
{
   fake_net_reset();
   static const char body[] = "hello world";
   fake_net_body(body, 11);

   /* Open: accepted, a non-zero handle, HTTP proto flags 0. */
   reply_t r = net_open(1, 0, "http://ok.test/a", 0);
   CHECK(r.status == FB_OK && r.dlen == 7, "open GET: %u len %u", r.status, r.dlen);
   CHECK(D(r)[0] == 1 && D(r)[1] == 0x01 && rd16(D(r) + 2) == 0, "open flags: accepted");
   uint16_t h = NET_H(r);
   CHECK(h != 0 && D(r)[6] == 0x00, "handle %04X, proto flags %02X", h, D(r)[6]);
   CHECK(strcmp(fake_net_last_open()->url, "http://ok.test/a") == 0 &&
         fake_net_last_open()->method == 1 && fake_net_last_open()->mode == 4,
         "open reached the stack as GET, read mode");

   /* Read: sequential chunks, truncated while more follows, EOF on the last. */
   r = net_read(h, 0, 4);
   CHECK(r.status == FB_OK && r.dlen == 12 + 4 && memcmp(D(r) + 12, "hell", 4) == 0,
         "read 1: %u len %u", r.status, r.dlen);
   CHECK(D(r)[1] == 0x02 && NET_H(r) == h && rd32(D(r) + 6) == 0 && rd16(D(r) + 10) == 4,
         "read 1 flags %02X: truncated, echoes", D(r)[1]);
   CHECK(net_read(h, 0, 4).status == FB_INVALID_REQUEST, "a read behind the cursor");
   CHECK(net_read(h, 9, 4).status == FB_INVALID_REQUEST, "a read ahead of the cursor");
   fake_net_stall(1);
   CHECK(net_read(h, 4, 4).status == FB_NOT_READY, "no data yet is NotReady, not Ok+0");
   r = net_read(h, 4, 4);
   CHECK(r.status == FB_OK && memcmp(D(r) + 12, "o wo", 4) == 0 && D(r)[1] == 0x02, "read 2");
   r = net_read(h, 8, 4);
   CHECK(r.status == FB_OK && rd16(D(r) + 10) == 3 && memcmp(D(r) + 12, "rld", 3) == 0,
         "read 3: %u", r.status);
   CHECK(D(r)[1] == 0x01, "the last chunk carries EOF (flags %02X)", D(r)[1]);
   r = net_read(h, 11, 4);
   CHECK(r.status == FB_OK && D(r)[1] == 0x01 && rd16(D(r) + 10) == 0, "read at the end: Ok, EOF, 0");
   CHECK(net_close(h) == FB_OK, "close");
   CHECK(net_close(h) == FB_INVALID_REQUEST, "close again");
   CHECK(net_read(h, 11, 4).status == FB_INVALID_REQUEST, "read a closed handle");
   CHECK(fake_net_handles_taken() == 0, "close released the stack's handle");

   /* A slow open: NotReady until it connects; the poll moves it on. */
   fake_net_open_delay(3);
   h = NET_H(net_open(1, 0, "http://ok.test/b", 0));
   CHECK(net_read(h, 0, 64).status == FB_NOT_READY, "read while connecting");
   net_poll(3);
   r = net_read(h, 0, 64);
   CHECK(r.status == FB_OK && rd16(D(r) + 10) == 11 && D(r)[1] == 0x01, "read after connecting");
   uint16_t old = h;
   net_close(h);
   fake_net_open_delay(0);
   h = NET_H(net_open(1, 0, "http://ok.test/c", 0));
   CHECK(h != old && (h & 0xFF) == (old & 0xFF), "a reused slot gets a new generation");
   CHECK(net_read(old, 0, 4).status == FB_INVALID_REQUEST, "the old generation is dead");
   net_close(h);

   /* Sessions: five, then DeviceBusy, or evict the least recently used. */
   uint16_t hs[5];
   for (int i = 0; i < 5; i++)
      hs[i] = NET_H(net_open(1, 0, "http://ok.test/x", 0));
   CHECK(net_open(1, 0, "http://ok.test/y", 0).status == FB_DEVICE_BUSY, "sixth open: busy");
   net_read(hs[0], 0, 1);                            /* hs[1] is now the oldest */
   r = net_open(1, 0x08, "http://ok.test/y", 0);
   CHECK(r.status == FB_OK, "allow_evict opens anyway");
   CHECK(net_read(hs[1], 0, 1).status == FB_INVALID_REQUEST, "the LRU session was evicted");
   CHECK(net_read(hs[0], 1, 1).status == FB_OK, "the recently used one was not");
   CHECK(fake_net_handles_taken() == 5, "no stack handle leaked: %d", fake_net_handles_taken());

   /* BBC reset closes them all on the next poll. */
   fn_network_reset();
   net_poll(1);
   CHECK(fake_net_handles_taken() == 0, "reset closed every session");
   CHECK(net_read(hs[0], 1, 1).status == FB_INVALID_REQUEST, "and their handles are dead");

   /* POST: needs_body_write; the reply waits for the body; Write pends
      until the stack has taken the whole chunk. */
   r = net_open(2, 0, "http://ok.test/post", 10);
   CHECK(r.status == FB_OK && D(r)[1] == 0x03, "open POST: accepted + needs body (%02X)", D(r)[1]);
   h = NET_H(r);
   CHECK(fake_net_last_open()->method == 2 && fake_net_last_open()->body_len == 10 &&
         fake_net_last_open()->mode == 12, "POST reached the stack with its length");
   CHECK(net_read(h, 0, 4).status == FB_NOT_READY, "no response before the body");
   fake_net_accept(2);
   r = net_write(h, 0, "01234", 5);
   CHECK(r.status == FB_OK && r.passes > 1, "write 1 waited for the stack (%d passes)", r.passes);
   CHECK(r.dlen == 12 && NET_H(r) == h && rd32(D(r) + 6) == 0 && rd16(D(r) + 10) == 5,
         "write reply: handle, offset, written");
   r = net_write(h, 0, "01234", 5);
   CHECK(r.status == FB_OK && r.passes == 1 && rd16(D(r) + 10) == 5, "a repeated chunk is answered, not resent");
   CHECK(net_write(h, 7, "x", 1).status == FB_INVALID_REQUEST, "a gap in the body");
   CHECK(net_write(h, 5, "56789X", 6).status == FB_INVALID_REQUEST, "past the declared length");
   fake_net_accept(0);
   CHECK(net_write(h, 5, "56789", 5).status == FB_OK, "write 2");
   uint32_t sunk;
   const uint8_t *sink = fake_net_sink(&sunk);
   CHECK(sunk == 10 && memcmp(sink, "0123456789", 10) == 0, "the stack got the body once (%u)", sunk);
   CHECK(net_read(h, 0, 64).status == FB_OK, "the response follows the body");
   net_close(h);

   /* A Write the network never takes times out after 30 s. */
   h = NET_H(net_open(3, 0, "http://ok.test/put", 4));
   fake_net_write_block(true);
   r = net_write(h, 0, "abcd", 4);
   CHECK(r.status == FB_TIMEOUT && r.passes >= 3000 && r.passes < 4000,
         "stalled write: %u after %d ms", r.status, r.passes);
   fake_net_write_block(false);
   CHECK(net_write(h, 0, "abcd", 4).status == FB_OK, "and can be tried again");
   net_close(h);

   /* The Beeb gives up on a pending Write (fn-rom's ~4 s) and sends something
      else: the next request - here the app store - must not inherit the
      Write's "still waiting" and pend for ever. */
   h = NET_H(net_open(3, 0, "http://ok.test/put", 4));
   fake_net_write_block(true);
   {
      buf_t w = payload(); u16(&w, h); u32(&w, 0); u16(&w, 4); put(&w, "abcd", 4);
      uint8_t pkt[64];
      uint16_t n = (uint16_t)(6 + w.n);
      pkt[0] = FB_DEV_NETWORK; pkt[1] = 0x03; pkt[2] = (uint8_t)n; pkt[3] = 0; pkt[4] = 0; pkt[5] = 0;
      memcpy(pkt + 6, w.b, w.n);
      pkt[4] = fb_checksum(pkt, n);
      uint8_t rep[64];
      uint16_t rl;
      CHECK(fujibus_answer(pkt, n, rep, sizeof rep, &rl) == FB_ANSWER_PENDING, "the write waits");
      fn_store_request_abort();                 /* as the service does when replaced */
      buf_t a = payload(); lstr(&a, "ns"); lstr(&a, "key"); u32(&a, 0); u16(&a, 16);
      reply_t ar = call(FB_DEV_APPSTORE, 0x02, &a);
      CHECK(ar.answered && ar.passes == 1, "the next request answers at once (%d passes)", ar.passes);
   }
   fake_net_write_block(false);
   net_close(h);
   CHECK(net_write(NET_H(net_open(1, 0, "http://ok.test/g", 0)), 0, "a", 1).status == FB_INVALID_REQUEST,
         "a write to a GET");
   fn_network_reset();
   net_poll(1);

   /* tcp: streaming, sequential both ways, no declared body. */
   r = net_open(1, 0, "tcp://ok.test:23", 0);
   CHECK(r.status == FB_OK && D(r)[6] == 0x07 && D(r)[1] == 0x01, "tcp open: proto %02X", D(r)[6]);
   h = NET_H(r);
   sink = fake_net_sink(&sunk);
   uint32_t before = sunk;
   CHECK(net_write(h, 0, "hi", 2).status == FB_OK && fake_net_sink(&sunk) && sunk == before + 2, "tcp write");
   net_close(h);

   /* Refusals. */
   reply_t rs = net_open(1, 0, "https://ok.test/s", 0);
   CHECK(rs.status == FB_OK && D(rs)[6] == 0x00 &&
         strcmp(fake_net_last_open()->url, "https://ok.test/s") == 0, "https: an HTTP session");
   CHECK(net_close(NET_H(rs)) == FB_OK, "close it");
   CHECK(net_open(1, 0, "ftp://ok.test/", 0).status == FB_UNSUPPORTED, "unknown scheme");
   CHECK(net_open(1, 0, "http:///", 0).status == FB_INVALID_REQUEST, "a URL with no host");
   CHECK(net_open(1, 0, "http://ok.test/", 5).status == FB_INVALID_REQUEST, "GET with a body");
   CHECK(net_open(2, 0x04, "http://ok.test/", 0).status == FB_UNSUPPORTED, "unknown body length");
   CHECK(net_open(9, 0, "http://ok.test/", 0).status == FB_INVALID_REQUEST, "unknown method");
   CHECK(fake_net_handles_taken() == 0, "refused opens hold nothing");
   buf_t p = net_open_req(1, 0, "http://ok.test/", 0);
   u8(&p, 0xEE);
   CHECK(call(FB_DEV_NETWORK, 0x01, &p).status == FB_INVALID_REQUEST, "trailing bytes");
   p = net_open_req(1, 0, "http://ok.test/", 0);
   u32(&p, 1); u8(&p, 2); u8(&p, 0); lstr(&p, "/a");
   CHECK(call(FB_DEV_NETWORK, 0x01, &p).status == FB_UNSUPPORTED, "XML translation (as nio)");
   p = net_open_req(1, 0, "http://ok.test/", 0);
   u32(&p, 1); u8(&p, 9); u8(&p, 0); lstr(&p, "/a");
   CHECK(call(FB_DEV_NETWORK, 0x01, &p).status == FB_INVALID_REQUEST, "unknown translation type");
   CHECK(fake_net_handles_taken() == 0, "refused translations hold nothing");
   p = net_open_req(2, 0, "http://ok.test/", 3);
   u32(&p, 2); u8(&p, 1);
   CHECK(call(FB_DEV_NETWORK, 0x01, &p).status == FB_OK &&
         strcmp(fake_net_last_open()->ctype, "application/json") == 0, "content profile JSON");
   p = payload(); u8(&p, 2); u8(&p, 0); lstr(&p, "http://ok.test/");
   u16(&p, 1); lstr(&p, "Content-TYPE"); lstr(&p, "text/csv"); u32(&p, 1); u16(&p, 0);
   CHECK(call(FB_DEV_NETWORK, 0x01, &p).status == FB_OK &&
         strcmp(fake_net_last_open()->ctype, "text/csv") == 0, "a Content-Type header");
   p = payload(); u8(&p, 1); u8(&p, 0); lstr(&p, "http://ok.test/");
   u16(&p, 1); lstr(&p, "Accept"); lstr(&p, "*/*"); u32(&p, 0); u16(&p, 0);
   CHECK(call(FB_DEV_NETWORK, 0x01, &p).status == FB_UNSUPPORTED, "other request headers");
   fn_network_reset();
   net_poll(1);
   p = payload(); u16(&p, 0);
   for (uint8_t c = 5; c <= 6; c++)
      CHECK(call(FB_DEV_NETWORK, c, &p).status == FB_UNSUPPORTED, "command %u (not yet)", c);
   p = net_open_req(1, 0, "http://ok.test/", 0);
   p.b[0] = 2;
   CHECK(call(FB_DEV_NETWORK, 0x01, &p).status == FB_INVALID_REQUEST, "protocol version 2");

   /* A host that does not resolve: accepted, then the read fails. */
   h = NET_H(net_open(1, 0, "http://dns.fail/", 0));
   net_poll(1);
   CHECK(net_read(h, 0, 4).status == FB_IO_ERROR, "unresolvable host: IOError");
   CHECK(net_close(h) == FB_OK, "close it");
   fake_net_open_delay(1);
   h = NET_H(net_open(1, 0, "http://dns.fail/", 0));
   CHECK(net_read(h, 0, 4).status == FB_IO_ERROR, "failing during the read itself: IOError");
   CHECK(net_close(h) == FB_OK && fake_net_handles_taken() == 0, "a failed session closes");

   fake_net_enable(false);
   CHECK(net_open(1, 0, "http://ok.test/", 0).status == FB_DEVICE_NOT_FOUND, "net_enable=0");
   fake_net_reset();
}

static reply_t net_translate(uint16_t h, uint8_t type, const char *sel)
{
   buf_t p = payload(); u16(&p, h); u8(&p, type); u8(&p, 0); lstr(&p, sel);
   return call(FB_DEV_NETWORK, 0x07, &p);
}

/* JSON translation: the flattened text is fn_json.c's (checked against nio's
   own translator separately); here, the device's handling of it. */
static void test_network_json(void)
{
   fake_net_reset();
   static const char body[] =
      "{\"current\":{\"temperature_2m\":12.5,\"is_day\":true},\"name\":\"Leeds\","
      "\"list\":[1,2,3]}";
   fake_net_body(body, (uint32_t)strlen(body));
   fake_net_chunk(10);                               /* the body arrives in pieces */

   /* TranslateConfigure on an open session. */
   uint16_t h = NET_H(net_open(1, 0, "http://ok.test/w", 0));
   fake_net_stall(1);
   CHECK(net_translate(h, 1, "/name").status == FB_NOT_READY, "configure while the body is still coming");
   reply_t r = net_translate(h, 1, "/name");
   CHECK(r.status == FB_OK && r.dlen == 10, "configure: %u len %u", r.status, r.dlen);
   CHECK(D(r)[1] == 0x01 && NET_H(r) == h && rd32(D(r) + 6) == 5, "ready, 5 bytes (%02X %u)",
         D(r)[1], rd32(D(r) + 6));
   r = net_read(h, 0, 64);
   CHECK(r.status == FB_OK && rd16(D(r) + 10) == 5 && memcmp(D(r) + 12, "Leeds", 5) == 0 &&
         D(r)[1] == 0x01, "translated read: Leeds, EOF");
   r = net_read(h, 1, 3);
   CHECK(r.status == FB_OK && memcmp(D(r) + 12, "eed", 3) == 0 && D(r)[1] == 0x06,
         "any offset; truncated + more (%02X)", D(r)[1]);
   CHECK(net_read(h, 6, 3).status == FB_INVALID_REQUEST, "past the end");
   r = net_read(h, 5, 3);
   CHECK(r.status == FB_OK && rd16(D(r) + 10) == 0 && D(r)[1] == 0x01, "at the end: Ok, EOF, 0");

   /* A second selector reuses the buffered body. */
   r = net_translate(h, 1, "/CURRENT/temperature_2m");
   CHECK(r.status == FB_OK && rd32(D(r) + 6) == 4, "second query on the same body");
   r = net_read(h, 0, 64);
   CHECK(memcmp(D(r) + 12, "12.5", 4) == 0, "keys match without case, as cJSON");
   r = net_translate(h, 1, "/list");
   r = net_read(h, 0, 64);
   CHECK(rd16(D(r) + 10) == 5 && memcmp(D(r) + 12, "1\n2\n3", 5) == 0, "an array, one per line");
   r = net_translate(h, 1, "/missing");
   CHECK(r.status == FB_OK && D(r)[1] == 0x01 && rd32(D(r) + 6) == 0, "nothing selected: ready, empty");
   CHECK(net_translate(h, 2, "/a").status == FB_UNSUPPORTED, "XML: Unsupported");
   CHECK(net_translate(h, 7, "/a").status == FB_INVALID_REQUEST, "unknown type");
   r = net_translate(h, 0, "");
   CHECK(r.status == FB_OK && D(r)[1] == 0x00 && rd32(D(r) + 6) == 0, "type None: off");
   CHECK(net_translate(0x7F00, 1, "/a").status == FB_INVALID_REQUEST, "unknown handle");
   net_close(h);

   /* The Open extension: translated from the first Read. */
   buf_t p = net_open_req(1, 0, "http://ok.test/w", 0);
   u32(&p, 1); u8(&p, 1); u8(&p, 0); lstr(&p, "/current/is_day");
   r = call(FB_DEV_NETWORK, 0x01, &p);
   CHECK(r.status == FB_OK, "open with a translation");
   h = NET_H(r);
   r = net_read(h, 0, 64);
   CHECK(r.status == FB_OK && rd16(D(r) + 10) == 4 && memcmp(D(r) + 12, "TRUE", 4) == 0,
         "translated from the open: TRUE");
   net_close(h);

   /* A POST's response is translated only after its body has gone. */
   h = NET_H(net_open(2, 0, "http://ok.test/p", 2));
   CHECK(net_translate(h, 1, "/name").status == FB_NOT_READY, "configure before the request body");
   CHECK(net_write(h, 0, "{}", 2).status == FB_OK, "the request body");
   CHECK(net_translate(h, 1, "/name").status == FB_OK, "then it translates");
   net_close(h);

   /* Not JSON: an empty result, still Ok (nio). */
   fake_net_body("<html>", 6);
   h = NET_H(net_open(1, 0, "http://ok.test/h", 0));
   r = net_translate(h, 1, "/a");
   CHECK(r.status == FB_OK && rd32(D(r) + 6) == 0, "a body that is not JSON");
   net_close(h);
   CHECK(fake_net_handles_taken() == 0, "all closed");
   fake_net_reset();
}

/* fn_json.c's number formatting: printf's %.10g done without printf.  The
   expected text is fujinet-nio's own translator's output for this array (its
   json_content_translator.cpp built on the host with the same cJSON): exact
   ties round to even, subnormals, the largest double, %g's layout edges. */
static void test_json_numbers(void)
{
   static const char body[] = "[1234567890.5,1234567891.5,-1234567890.5,9999999999.5,99999999995,0.00012345678905,0.0001,0.00001,1e-5,9.9999999995e-5,1e10,9999999999,12345678901,5e-324,2.2250738585072014e-308,1.7976931348623157e308,0.1,0.3,1e15,1e16,123456789012345678,4.35,2.675,1.0000000005,1.00000000050000001,8.5,0.5,-0.5,1e100,1.5e-7]";
   static const char want[] = "1234567890\n1234567892\n-1234567890\n1e+10\n99999999995\n0.0001234567891\n0.0001\n1e-05\n1e-05\n0.0001\n10000000000\n9999999999\n12345678901\n4.940656458e-324\n2.225073859e-308\n1.797693135e+308\n0.1\n0.3\n1000000000000000\n1e+16\n1.23456789e+17\n4.35\n2.675\n1.000000001\n1.000000001\n8.5\n0.5\n-0.5\n1e+100\n1.5e-07";
   char *out;
   uint32_t len;
   CHECK(fn_json_translate(body, "", &out, &len), "translate the number array");
   CHECK(out && len == strlen(want) && memcmp(out, want, len) == 0,
         "numbers as nio prints them:\n  got  %.*s\n  want %s", (int)len, out ? out : "", want);
   free(out);
   CHECK(fn_json_translate("[{\"a\":{\"b\":[1,{\"c\":\"d\"}]}},{}]", "", &out, &len) &&
         len == 10 && memcmp(out, "a\nb\n1\nc\nd\n", 10) == 0, "nested object/array layout (nio)");
   free(out);
}

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
      fb_answer a;
      int passes = 0;
      while ((a = fujibus_answer(pkt, n, reply, sizeof reply, &rl)) == FB_ANSWER_PENDING &&
             ++passes < 200000) {
         fake_tnfs_step();
         fake_tnfs_advance(1);
         fn_store_poll();
      }
      bool ok = a == FB_ANSWER_REPLY;
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
   fake_tnfs_reset();

   test_packet_layer();
   test_host();
   test_appstore();
   test_appstore_limits();
   test_slotcat();
   test_disk();
   test_dsd();
   test_file();
   test_tnfs();
   test_network();
   test_network_json();
   test_json_numbers();
   test_fuzz();

   char cmd[300];
   snprintf(cmd, sizeof cmd, "rm -rf %s", s_root);
   if (system(cmd)) {}
   printf("%d passed, %d failed\n", s_pass, s_fail);
   return s_fail ? 1 : 0;
}
