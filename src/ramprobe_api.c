/* ramprobe_api.c - the RAM probe over HTTP, apart from the web server so that
   the tests can drive it.  See docs/dev/ramprobe.md for the routes.

   A POST starts something; GET /bbc says how it is getting on, so a client
   polls until "busy" is false.  With wait=1 the web server holds the answer
   until the work is done instead. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scripts/gitversion.h"
#include "ramprobe.h"
#include "ramprobe_api.h"
#include "lib/textparse.h"
#include "Pi1MHz.h"
#include <stdint.h>

#define DATA_MAX_DEFAULT 4096u

/* key=value in a query string: its value and length, or NULL if it isn't there (lib/textparse:
   keys in any case, only at a parameter's start).  No percent-decoding, since every value here
   is a number or hex digits; ranges= decodes its own. */
static const char *find(const char *query, const char *key, size_t *length)
{
   return text_query_find(query, key, length);
}

/* A copy of the value; false if it's missing or too long for out (never cut short: a cut hex=
   would poke fewer bytes than asked). */
static bool param(const char *query, const char *key, char *out, size_t n)
{
   size_t len = 0;
   const char *v = find(query, key, &len);
   if (v == NULL || len >= n) return false;
   memcpy(out, v, len);
   out[len] = '\0';
   return true;
}

/* An optional number: 0x2000 or 8192 (lib/textparse: decimal or 0x hex, never octal).  True if
   it's absent (*present false, *value untouched) or good; false if it's malformed or over max. */
static bool number(const char *query, const char *key, uint32_t max, uint32_t *value, bool *present)
{
   size_t len = 0;
   *present = false;
   const char *raw = find(query, key, &len);
   if (raw == NULL) return true;
   if (!text_parse_u32(raw, len, max, value)) return false;
   *present = true;
   return true;
}

/* Hex digits into bytes: how many, or -1 if they aren't whole hex bytes that fit. */
static int unhex(const char *hex, uint8_t *out, size_t max)
{
   size_t len = strlen(hex);
   if (len == 0u || (len & 1u) != 0u || len / 2u > max) return -1;
   for (size_t i = 0; i < len / 2u; i++) {
      int hi = text_hex_nibble(hex[2u * i]), lo = text_hex_nibble(hex[2u * i + 1u]);
      if (hi < 0 || lo < 0) return -1;
      out[i] = (uint8_t)(hi << 4 | lo);
   }
   return (int)(len / 2u);
}

/* ranges=0x300:32,0x1900:256 (':' and ',' may come percent-encoded): how many, 0 if there's no
   ranges=, or -1 if it's malformed.  *total is all their lengths. */
static int ranges_param(const char *query, rp_range_t *r, uint32_t *total)
{
   char raw[RP_MAX_RANGES * 16u * 3u], v[RP_MAX_RANGES * 16u];
   *total = 0;
   if (!param(query, "ranges", raw, sizeof raw)) {
      size_t len = 0;
      return find(query, "ranges", &len) == NULL ? 0 : -1;    /* absent, or too long */
   }
   if (!text_url_decode(raw, v, sizeof v)) return -1;
   int count = 0;
   for (const char *p = v; ; ) {
      const char *colon = strchr(p, ':');
      if (count == (int)RP_MAX_RANGES || colon == NULL) return -1;
      const char *l = colon + 1;
      size_t llen = strcspn(l, ",");
      uint32_t a, n;
      if (!text_parse_u32(p, (size_t)(colon - p), 0xFFFFu, &a)
          || !text_parse_u32(l, llen, RP_MAX_TRANSFER, &n) || n == 0u)
         return -1;
      r[count].address = (uint16_t)a;
      r[count].length = n;
      *total += n;
      count++;
      if (l[llen] == '\0') break;
      p = l + llen + 1;                                /* past the ',' */
   }
   return count;
}

/* Where the services port's pointer is in shared memory, or -1 if it points elsewhere: from the
   address the port publishes in its own registers (&FCA6-&FCA8, bits 0-23 of a services offset),
   which its FIQ updates on every byte read or written, for the BBC to read back.  These are the
   same three bytes the BBC would read, so nothing is asked of the services core.  The FIQ can
   update them between two of these reads, so all three are read twice, and again until two
   reads agree: never a torn value.  RP_SHARED_SIZE once the BBC has read or written the last
   byte.  -1 while the BBC is stopped: the port is the probe's then. */
#define SERVICES_PORT 0xA6u        /* &FCA6: hard-wired, as the probe's agent and 6502code.asm are */
static uint32_t port_address(void)
{
   return (uint32_t)Pi1MHz_MemoryRead(SERVICES_PORT + 2u) << 16 | (uint32_t)Pi1MHz_MemoryRead(SERVICES_PORT + 1u) << 8
        | (uint32_t)Pi1MHz_MemoryRead(SERVICES_PORT);
}
static long shared_read(void)
{
   if (ramprobe_state() != RP_RUNNING) return -1L;
   uint32_t at = port_address();
   for (int tries = 0; tries < 8; tries++) {
      uint32_t again = port_address();
      if (again == at)
         return (at >= RP_SHARED && at <= RP_SHARED + RP_SHARED_SIZE) ? (long)(at - RP_SHARED) : -1L;
      at = again;
   }
   return -1L;
}

static int status_json(char *out, size_t n, int http)
{
   const rp_regs_t *s = ramprobe_stopped();
   const rp_regs_t *r = ramprobe_result();
   uint32_t peeked = 0;
   (void)ramprobe_data(&peeked);
   rp_state_t st = ramprobe_state();
   /* After rung 0, null: it stopped inside the OS's vsync event, so the registers are the OS's. */
   char stopped[80] = "null";
   if (ramprobe_rung() != RP_RUNG_EVENT)
      snprintf(stopped, sizeof stopped, "{\"a\":%u,\"x\":%u,\"y\":%u,\"p\":%u,\"pc\":%u,\"s\":%u}",
               s->a, s->x, s->y, s->p, s->pc, s->s);
   bool busy = st == RP_HALTING || st == RP_WORKING || st == RP_RESUMING;
   snprintf(out, n,
      "{\"api\":2,\"build\":\"%s\",\"resets\":%lu,\"stops\":%lu,"
      "\"state\":\"%s\",\"busy\":%s,\"installed\":%s,\"rung\":%d,\"error\":\"%s\","
      "\"stopped\":%s,"
      "\"result\":{\"a\":%u,\"x\":%u,\"y\":%u,\"p\":%u},\"peeked\":%lu,"
      "\"report\":%lu,\"shared\":{\"at\":%lu,\"size\":%lu,\"jim\":%lu,\"read\":%ld}}\n",
      GITVERSION, (unsigned long)ramprobe_resets(), (unsigned long)ramprobe_stops(), ramprobe_state_name(), busy ? "true" : "false", ramprobe_installed() ? "true" : "false", ramprobe_rung(), ramprobe_error(),
      stopped, r->a, r->x, r->y, r->p, (unsigned long)peeked,
      (unsigned long)ramprobe_report_at(),
      (unsigned long)RP_SHARED, (unsigned long)RP_SHARED_SIZE, (unsigned long)ramprobe_shared_jim(), shared_read());
   return http;
}

static int bad(char *out, size_t n, const char *why)
{
   snprintf(out, n, "{\"error\":\"%s\"}\n", why);
   return 400;
}

/* A started action: its state, or 409 with why it couldn't start. */
static int started(bool ok, char *out, size_t n)
{
   return status_json(out, n, ok ? 200 : 409);
}

/* rung=0|1|2: absent, the default given.  Rung 2, the NMI, only with nmi=1 as well: the 6502 jumps
   straight to &0D00 on an NMI, and after a disc access Acorn DFS's own NMI code is there, not the
   probe's JMP (rungs 0 and 1 don't use &0D00 at all).  An 8271 or 1770 handler entered then
   moves a byte to wherever its pointer was left (INFERRED, not tried on hardware).  *why is set
   when it's refused. */
static bool rung_param(const char *query, int dflt, int *rung, const char **why)
{
   uint32_t v = 0;
   bool has;
   char nmi[4];
   *why = "bad rung";
   if (!number(query, "rung", RP_RUNG_NMI, &v, &has)) return false;
   *rung = has ? (int)v : dflt;
   if (*rung == RP_RUNG_NMI && !(param(query, "nmi", nmi, sizeof nmi) && strcmp(nmi, "1") == 0)) {
      *why = "rung 2 is the NMI: add nmi=1 (after a disc access it lands in the DFS's NMI code)";
      return false;
   }
   return true;
}

static int data_hex(const char *query, char *out, size_t n)
{
   uint32_t have = 0, offset = 0, len = DATA_MAX_DEFAULT;
   bool given;
   const uint8_t *d = ramprobe_data(&have);
   if (!number(query, "offset", RP_MAX_TRANSFER, &offset, &given) || !number(query, "len", RP_MAX_TRANSFER, &len, &given))
      return bad(out, n, "bad offset or len");
   if (offset > have) offset = have;
   if (len > have - offset) len = have - offset;
   if ((size_t)len * 2u + 2u > n) len = (uint32_t)((n - 2u) / 2u);
   for (uint32_t i = 0; i < len; i++)
      snprintf(out + 2u * i, 3u, "%02X", d[offset + i]);
   out[2u * len] = '\n';
   out[2u * len + 1u] = '\0';
   return 200;
}

/* GET /bbc/shared?offset=&len=: shared memory as hex, straight from the Pi's memory with no stop.
   Both are needed, len is 1 to 4 KB, and the range must lie inside shared memory. */
static int shared_hex(const char *query, char *out, size_t n)
{
   uint32_t offset = 0, len = 0;
   bool has_offset, has_len;
   if (!number(query, "offset", RP_SHARED_SIZE, &offset, &has_offset) || !has_offset)
      return bad(out, n, "bad offset");
   if (!number(query, "len", DATA_MAX_DEFAULT, &len, &has_len) || !has_len || len == 0u)
      return bad(out, n, "bad len: 1 to 4096");
   if (len > RP_SHARED_SIZE - offset) return bad(out, n, "past the end of shared memory (GET /bbc's shared.size)");
   const uint8_t *d = ramprobe_shared() + offset;
   if ((size_t)len * 2u + 2u > n) len = (uint32_t)((n - 2u) / 2u);
   for (uint32_t i = 0; i < len; i++)
      snprintf(out + 2u * i, 3u, "%02X", d[i]);
   out[2u * len] = '\n';
   out[2u * len + 1u] = '\0';
   return 200;
}

int ramprobe_api_shared(const char *query, size_t len, uint8_t **dest, char *out, size_t n)
{
   uint32_t off = 0;
   bool has;
   if (ramprobe_state() == RP_OFF) return 0;
   if (!number(query, "offset", RP_SHARED_SIZE, &off, &has) || !has) return bad(out, n, "bad offset");
   if (len == 0u || len > RP_SHARED_SIZE - off) return bad(out, n, "bad length: none, or past the end of shared memory (GET /bbc's shared.size)");
   *dest = ramprobe_shared() + off;
   return 200;
}

int ramprobe_api_body(const char *path, const char *query, const uint8_t *body, size_t len, char *out, size_t n)
{
   uint32_t v = 0;
   bool has;
   int rung;
   if (ramprobe_state() == RP_OFF) return 0;
   if (strcmp(path, "/bbc/poke") != 0) return bad(out, n, "only /bbc/poke takes a body");
   if (len == 0u || len > RAMPROBE_API_BODY_MAX) return bad(out, n, "bad body length");
   const char *why;
   if (!rung_param(query, RP_RUNG_NONE, &rung, &why)) return bad(out, n, why);
   rp_range_t r[RP_MAX_RANGES];
   uint32_t total;
   int count = ranges_param(query, r, &total);
   if (count < 0) return bad(out, n, "bad ranges");
   if (count > 0) {
      if (total != len) return bad(out, n, "the body isn't the ranges' length");
      return started(ramprobe_poke_ranges(r, (unsigned)count, body, rung), out, n);
   }
   if (!number(query, "addr", 0xFFFFu, &v, &has) || !has) return bad(out, n, "bad addr");
   return started(ramprobe_poke((uint16_t)v, body, (uint32_t)len, rung), out, n);
}

bool ramprobe_api_hold(bool post, const char *query, int status)
{
   char w[4];
   return post && status == 200 && param(query, "wait", w, sizeof w) && w[0] == '1' && !ramprobe_api_done();
}

bool ramprobe_api_done(void)
{
   rp_state_t st = ramprobe_state();
   return !(st == RP_HALTING || st == RP_WORKING || st == RP_RESUMING);
}

int ramprobe_api_status(char *out, size_t n)
{
   return status_json(out, n, 200);
}

int ramprobe_api(bool post, const char *path, const char *query, char *out, size_t n)
{
   if (ramprobe_state() == RP_OFF) return 0;      /* not there: the web server's own 404 */

   if (!post) {
      if (strcmp(path, "/bbc") == 0) return status_json(out, n, 200);
      if (strcmp(path, "/bbc/data") == 0) return data_hex(query, out, n);
      if (strcmp(path, "/bbc/shared") == 0) return shared_hex(query, out, n);
      return 0;
   }

   if (strcmp(path, "/bbc/shared") == 0) return bad(out, n, "POST /bbc/shared takes its bytes as the body");

   if (strcmp(path, "/bbc/halt") == 0) {
      int rung;
      const char *why;
      if (!rung_param(query, RP_RUNG_EVENT, &rung, &why)) return bad(out, n, why);   /* the gentlest */
      return started(ramprobe_halt(rung), out, n);
   }

   if (strcmp(path, "/bbc/continue") == 0) {
      rp_resume_t how = {0};
      uint32_t v = 0;
      if (!number(query, "pc", 0xFFFFu, &v, &how.set_pc)) return bad(out, n, "bad pc");
      how.pc = (uint16_t)v;
      if (!number(query, "a", 0xFFu, &v, &how.set_a)) return bad(out, n, "bad a");
      how.a = (uint8_t)v;
      if (!number(query, "x", 0xFFu, &v, &how.set_x)) return bad(out, n, "bad x");
      how.x = (uint8_t)v;
      if (!number(query, "y", 0xFFu, &v, &how.set_y)) return bad(out, n, "bad y");
      how.y = (uint8_t)v;
      if (!number(query, "p", 0xFFu, &v, &how.set_p)) return bad(out, n, "bad p");
      how.p = (uint8_t)v;
      return started(ramprobe_resume(&how), out, n);
   }

   if (strcmp(path, "/bbc/peek") == 0) {
      uint32_t addr = 0, len = 0, bank = 0, total;
      bool has_addr, has_len, has_bank;
      rp_range_t r[RP_MAX_RANGES];
      int count = ranges_param(query, r, &total);
      if (count < 0) return bad(out, n, "bad ranges");
      if (count == 0) {
         if (!number(query, "addr", 0xFFFFu, &addr, &has_addr) || !has_addr) return bad(out, n, "bad addr");
         if (!number(query, "len", RP_MAX_TRANSFER, &len, &has_len) || !has_len) return bad(out, n, "bad len");
         r[0] = (rp_range_t){ (uint16_t)addr, len };
         count = 1;
      }
      if (!number(query, "bank", 15u, &bank, &has_bank)) return bad(out, n, "bad bank");
      int rung;
      const char *why;
      if (!rung_param(query, RP_RUNG_NONE, &rung, &why)) return bad(out, n, why);
      return started(ramprobe_peek_ranges(r, (unsigned)count, has_bank ? (int)bank : RP_NO_BANK, rung), out, n);
   }

   if (strcmp(path, "/bbc/poke") == 0 || strcmp(path, "/bbc/run") == 0) {
      static char hex[2u * RAMPROBE_API_POKE_MAX + 1u];
      static uint8_t bytes[RAMPROBE_API_POKE_MAX];
      uint32_t v = 0;
      bool has;
      if (!param(query, "hex", hex, sizeof hex)) return bad(out, n, "bad hex");
      int count = unhex(hex, bytes, sizeof bytes);
      if (count < 0) return bad(out, n, "bad hex");
      if (path[5] == 'p') {
         int rung;
         const char *why;
         if (!rung_param(query, RP_RUNG_NONE, &rung, &why)) return bad(out, n, why);
         rp_range_t r[RP_MAX_RANGES];
         uint32_t total;
         int ranges = ranges_param(query, r, &total);
         if (ranges < 0) return bad(out, n, "bad ranges");
         if (ranges > 0) {
            if (total != (uint32_t)count) return bad(out, n, "the hex isn't the ranges' length");
            return started(ramprobe_poke_ranges(r, (unsigned)ranges, bytes, rung), out, n);
         }
         if (!number(query, "addr", 0xFFFFu, &v, &has) || !has) return bad(out, n, "bad addr");
         return started(ramprobe_poke((uint16_t)v, bytes, (uint32_t)count, rung), out, n);
      }
      uint32_t a = 0, x = 0, y = 0;
      if (!number(query, "a", 0xFFu, &a, &has) || !number(query, "x", 0xFFu, &x, &has) || !number(query, "y", 0xFFu, &y, &has))
         return bad(out, n, "bad a, x or y");
      return started(ramprobe_run(bytes, (uint32_t)count, (uint8_t)a, (uint8_t)x, (uint8_t)y), out, n);
   }

   if (strcmp(path, "/bbc/jsr") == 0) {
      uint32_t addr = 0, a = 0, x = 0, y = 0;
      bool has;
      if (!number(query, "addr", 0xFFFFu, &addr, &has) || !has) return bad(out, n, "bad addr");
      if (!number(query, "a", 0xFFu, &a, &has) || !number(query, "x", 0xFFu, &x, &has) || !number(query, "y", 0xFFu, &y, &has))
         return bad(out, n, "bad a, x or y");
      return started(ramprobe_call((uint16_t)addr, (uint8_t)a, (uint8_t)x, (uint8_t)y), out, n);
   }
   return 0;
}
