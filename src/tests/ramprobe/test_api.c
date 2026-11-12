/* ramprobe_api.c's request parsing on its own, against a recording stub of ramprobe.c: needs
   no kc-beeb, so it runs everywhere (run_tests.sh, before the end-to-end test). */
#include <stdio.h>
#include <string.h>
#include "ramprobe.h"
#include "ramprobe_api.h"

static int failures;
static void ok(int c, const char *what) { if (!c) { printf("FAIL: %s\n", what); failures++; } }

/* the stub: records what the API asked for */
static rp_range_t got[RP_MAX_RANGES];
static unsigned got_n;
static int got_bank, got_rung, calls;
static uint8_t got_bytes[64];
static rp_regs_t regs;
rp_state_t ramprobe_state(void) { return RP_HALTED; }
const char *ramprobe_state_name(void) { return "halted"; }
bool ramprobe_installed(void) { return true; }
const char *ramprobe_error(void) { return ""; }
uint32_t ramprobe_resets(void) { return 0; }
int ramprobe_rung(void) { return RP_RUNG_IRQ; }
bool ramprobe_halt(int rung) { got_rung = rung; calls++; return true; }
bool ramprobe_resume(const rp_resume_t *how) { (void)how; calls++; return true; }
bool ramprobe_peek_ranges(const rp_range_t *r, unsigned n, int bank, int rung)
{ memcpy(got, r, n * sizeof *r); got_n = n; got_bank = bank; got_rung = rung; calls++; return true; }
bool ramprobe_poke_ranges(const rp_range_t *r, unsigned n, const uint8_t *b, int rung)
{ memcpy(got, r, n * sizeof *r); got_n = n; got_rung = rung; memcpy(got_bytes, b, 4); calls++; return true; }
bool ramprobe_poke(uint16_t a, const uint8_t *b, uint32_t len, int rung)
{ got[0] = (rp_range_t){ a, len }; got_n = 1; got_rung = rung; memcpy(got_bytes, b, len < 64u ? len : 64u); calls++; return true; }
bool ramprobe_call(uint16_t a, uint8_t x, uint8_t y, uint8_t z) { got[0].address = a; (void)x; (void)y; (void)z; calls++; return true; }
bool ramprobe_run(const uint8_t *c, uint32_t n, uint8_t a, uint8_t x, uint8_t y) { (void)c; (void)n; (void)a; (void)x; (void)y; calls++; return true; }
const rp_regs_t *ramprobe_stopped(void) { return &regs; }
const rp_regs_t *ramprobe_result(void) { return &regs; }
const uint8_t *ramprobe_data(uint32_t *len) { *len = 0; return NULL; }
uint32_t ramprobe_report_at(void) { return RP_REPORT; }

static char out[RAMPROBE_API_OUT];
static int post(const char *path, const char *q) { calls = 0; got_n = 0; return ramprobe_api(true, path, q, out, sizeof out); }

int main(void)
{
   ok(post("/bbc/peek", "addr=0x1900&len=256") == 200 && got_n == 1 && got[0].address == 0x1900 && got[0].length == 256,
      "peek: hex addr, decimal len");
   ok(post("/bbc/peek", "addr=010&len=1") == 200 && got[0].address == 10, "peek: 010 is ten, not octal 8");
   ok(post("/bbc/peek", "ADDR=16&Len=2") == 200 && got[0].address == 16 && got[0].length == 2, "keys in any case");
   ok(post("/bbc/peek", "addr=12abc&len=1") == 400 && calls == 0, "trailing text refused");
   ok(post("/bbc/peek", "addr=-1&len=1") == 400 && calls == 0, "a sign refused");
   ok(post("/bbc/peek", "addr=0x10000&len=1") == 400 && calls == 0, "addr over &FFFF refused");
   ok(post("/bbc/peek", "xaddr=1&len=1") == 400 && calls == 0, "a key only at a parameter's start");
   ok(post("/bbc/peek", "ranges=0x300:32,0x1900:256&bank=4&rung=1") == 200 && got_n == 2
      && got[0].address == 0x300 && got[0].length == 32 && got[1].address == 0x1900 && got[1].length == 256
      && got_bank == 4 && got_rung == 1, "ranges, bank and rung");
   ok(post("/bbc/peek", "ranges=0x300%3A32%2C0x1900%3a256") == 200 && got_n == 2 && got[1].length == 256,
      "ranges percent-encoded, either case");
   ok(post("/bbc/peek", "ranges=0x300:32,") == 400, "ranges: a trailing comma");
   ok(post("/bbc/peek", "ranges=") == 400, "ranges: empty");
   ok(post("/bbc/peek", "ranges=0x300") == 400, "ranges: no length");
   ok(post("/bbc/peek", "ranges=0x300:0") == 400, "ranges: zero length");
   ok(post("/bbc/peek", "ranges=1:2:3") == 400, "ranges: two colons");
   ok(post("/bbc/peek", "ranges=0x300:%0A") == 400, "ranges: a decoded control byte");
   {
      char q[1024] = "ranges=";
      for (int i = 0; i < 33; i++) strcat(q, i ? ",1:1" : "1:1");
      ok(post("/bbc/peek", q) == 400 && calls == 0, "ranges: 33 refused");
   }
   ok(post("/bbc/poke", "addr=0x2000&hex=0102aBcD") == 200 && got[0].length == 4
      && got_bytes[0] == 1 && got_bytes[2] == 0xAB && got_bytes[3] == 0xCD, "poke hex, either case");
   ok(post("/bbc/poke", "addr=0x2000&hex=012") == 400 && calls == 0, "poke: odd hex refused");
   ok(post("/bbc/poke", "addr=0x2000&hex=01zz") == 400 && calls == 0, "poke: bad hex refused");
   {
      static char q[2048];
      strcpy(q, "addr=0x2000&hex=");
      for (int i = 0; i < 513; i++) strcat(q, "00");
      ok(post("/bbc/poke", q) == 400 && calls == 0, "poke: hex over 512 bytes refused, not cut short");
   }
   ok(post("/bbc/halt", "rung=2") == 400 && calls == 0, "rung 2 needs nmi=1");
   ok(post("/bbc/halt", "rung=2&nmi=1") == 200 && got_rung == 2, "rung 2 with nmi=1");
   ok(post("/bbc/halt", "rung=3") == 400, "rung 3 refused");
   if (failures) { printf("ramprobe api: %d failure(s)\n", failures); return 1; }
   printf("ramprobe api: all passed\n");
   return 0;
}
