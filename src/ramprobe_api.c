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
#include <stdint.h>

#define DATA_MAX_DEFAULT 4096u

/* key=value in a query string: its value and length, or NULL if it isn't there.
   No percent-decoding, since every value here is a number or hex digits. */
static const char *find(const char *query, const char *key, size_t *length)
{
   size_t klen = strlen(key);
   for (const char *p = query; p != NULL && *p != '\0'; ) {
      const char *end = strchr(p, '&');
      size_t len = end != NULL ? (size_t)(end - p) : strlen(p);
      if (len > klen && p[klen] == '=' && strncmp(p, key, klen) == 0) {
         *length = len - klen - 1u;
         return p + klen + 1u;
      }
      p = end != NULL ? end + 1 : NULL;
   }
   return NULL;
}

/* A copy of the value; false if it's missing or too long for out. */
static bool param(const char *query, const char *key, char *out, size_t n)
{
   size_t len = 0;
   const char *v = find(query, key, &len);
   if (v == NULL || len >= n) return false;
   memcpy(out, v, len);
   out[len] = '\0';
   return true;
}

/* An optional number: 0x2000 or 8192.  True if it's absent (*present false, *value
   untouched) or good; false if it's malformed or over max. */
static bool number(const char *query, const char *key, uint32_t max, uint32_t *value, bool *present)
{
   char v[16];
   size_t len = 0;
   char *end;
   *present = false;
   const char *raw = find(query, key, &len);
   if (raw == NULL) return true;
   if (len == 0u || len >= sizeof v) return false;
   memcpy(v, raw, len);
   v[len] = '\0';
   if (v[0] == '-' || v[0] == '+' || v[0] == ' ') return false;
   unsigned long n = strtoul(v, &end, 0);
   if (*end != '\0' || n > max) return false;
   *value = (uint32_t)n;
   *present = true;
   return true;
}

static int hexval(char c)
{
   if (c >= '0' && c <= '9') return c - '0';
   if (c >= 'a' && c <= 'f') return c - 'a' + 10;
   if (c >= 'A' && c <= 'F') return c - 'A' + 10;
   return -1;
}

/* Hex digits into bytes: how many, or -1 if they aren't whole hex bytes that fit. */
static int unhex(const char *hex, uint8_t *out, size_t max)
{
   size_t len = strlen(hex);
   if (len == 0u || (len & 1u) != 0u || len / 2u > max) return -1;
   for (size_t i = 0; i < len / 2u; i++) {
      int hi = hexval(hex[2u * i]), lo = hexval(hex[2u * i + 1u]);
      if (hi < 0 || lo < 0) return -1;
      out[i] = (uint8_t)(hi << 4 | lo);
   }
   return (int)(len / 2u);
}

/* ranges=0x300:32,0x1900:256 (':' and ',' may come as %3A and %2C): how many, 0 if there's no
   ranges=, or -1 if it's malformed.  *total is all their lengths. */
static int ranges_param(const char *query, rp_range_t *r, uint32_t *total)
{
   char v[RP_MAX_RANGES * 16u];
   size_t len = 0, k = 0;
   const char *raw = find(query, "ranges", &len);
   *total = 0;
   if (raw == NULL) return 0;
   for (size_t i = 0; i < len; i++) {               /* the two escapes a client may send */
      if (k + 1u >= sizeof v) return -1;
      if (raw[i] == '%') {
         if (i + 2u >= len) return -1;
         int hi = hexval(raw[i + 1u]), lo = hexval(raw[i + 2u]);
         if (hi < 0 || lo < 0) return -1;
         v[k++] = (char)(hi << 4 | lo);
         i += 2u;
      } else {
         v[k++] = raw[i];
      }
   }
   v[k] = '\0';
   int count = 0;
   for (char *p = v; ; ) {
      char *end;
      if (count == (int)RP_MAX_RANGES || *p == '\0' || *p == '-' || *p == '+' || *p == ' ') return -1;
      unsigned long a = strtoul(p, &end, 0);
      if (end == p || *end != ':' || a > 0xFFFFu) return -1;
      p = end + 1;
      if (*p == '\0' || *p == '-' || *p == '+' || *p == ' ') return -1;
      unsigned long l = strtoul(p, &end, 0);
      if (end == p || l == 0u || l > RP_MAX_TRANSFER) return -1;
      r[count].address = (uint16_t)a;
      r[count].length = (uint32_t)l;
      *total += (uint32_t)l;
      count++;
      if (*end == '\0') break;
      if (*end != ',') return -1;
      p = end + 1;
   }
   return count;
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
      "{\"api\":2,\"build\":\"%s\",\"resets\":%lu,"
      "\"state\":\"%s\",\"busy\":%s,\"installed\":%s,\"rung\":%d,\"error\":\"%s\","
      "\"stopped\":%s,"
      "\"result\":{\"a\":%u,\"x\":%u,\"y\":%u,\"p\":%u},\"peeked\":%lu,"
      "\"report\":%lu}\n",
      GITVERSION, (unsigned long)ramprobe_resets(), ramprobe_state_name(), busy ? "true" : "false", ramprobe_installed() ? "true" : "false", ramprobe_rung(), ramprobe_error(),
      stopped, r->a, r->x, r->y, r->p, (unsigned long)peeked,
      (unsigned long)ramprobe_report_at());
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
      return 0;
   }

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
