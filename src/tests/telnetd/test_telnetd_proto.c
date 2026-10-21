/* test_telnetd_proto.c - host tests for the telnet daemon's byte translators
   (telnetd_proto.c): what a telnet client's keys become on the Beeb, and what
   the Beeb's VDU stream becomes on a terminal. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net_telnet.h"
#include "telnetd_proto.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
   printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- client -> Beeb ---------------------------------------------------- */

static uint8_t keys[64]; static size_t nkeys;
static uint8_t reply[64]; static size_t nreply;
static uint32_t now;

static void in_reset(void) { nkeys = nreply = 0; now = 1000; }

static void feed(tnd_in_t *s, const void *data, size_t len)
{
   const uint8_t *p = data;
   for (size_t i = 0; i < len; i++) {
      uint8_t k[TND_IN_KEYS], r[TND_IN_REPLY];
      size_t rl, n = tnd_in_byte(s, p[i], now, k, r, &rl);
      CHECK(n <= TND_IN_KEYS, "one byte released %u keys, more than TND_IN_KEYS", (unsigned)n);
      if (n > TND_IN_KEYS || nkeys + n > sizeof keys)
         n = 0;
      memcpy(keys + nkeys, k, n);         nkeys += n;
      memcpy(reply + nreply, r, rl);      nreply += rl;
   }
}
#define FEED(s, lit) feed(s, lit, sizeof(lit) - 1)

static void tick(tnd_in_t *s, uint32_t advance_us)
{
   uint8_t k[TND_IN_KEYS];
   now += advance_us;
   size_t n = tnd_in_tick(s, now, k);
   CHECK(n <= TND_IN_KEYS, "a tick released %u keys, more than TND_IN_KEYS", (unsigned)n);
   if (n > TND_IN_KEYS || nkeys + n > sizeof keys)
      n = 0;
   memcpy(keys + nkeys, k, n);
   nkeys += n;
}

/* The login's rules (PR review): per address, a hold after every failure, longer lockouts, and
   timers that survive the 32-bit microsecond clock wrapping. */
static void test_login(void)
{
   tnd_login_t l;
   const uint32_t A = 0x0A00000Au, B = 0x0B00000Bu;
   uint32_t t = 0xFFFFFF00u;                    /* just before the clock wraps */

   tnd_login_reset(&l);
   tnd_login_tick(&l, t);
   CHECK(!tnd_login_locked(&l, A) && !tnd_login_held(&l), "login: nothing against anybody at the start");
   CHECK(!tnd_login_fail(&l, A) && tnd_login_held(&l), "login: a wrong password holds every prompt ...");
   tnd_login_tick(&l, t += TND_HOLD_US - 1u);
   CHECK(tnd_login_held(&l), "... until TND_HOLD_US has gone ...");
   tnd_login_tick(&l, t += 2u);
   CHECK(!tnd_login_held(&l) && !tnd_login_locked(&l, A), "... and then not, across the clock's wrap");
   CHECK(!tnd_login_fail(&l, A) && tnd_login_fail(&l, A), "login: the third wrong password from an address locks it");
   CHECK(tnd_login_locked(&l, A) && !tnd_login_locked(&l, B), "login: ... that address only: another can still log in");
   tnd_login_tick(&l, t += TND_LOCK_US - 1u);
   CHECK(tnd_login_locked(&l, A), "login: locked for TND_LOCK_US ...");
   tnd_login_tick(&l, t += 2u);
   CHECK(!tnd_login_locked(&l, A), "login: ... then free");
   tnd_login_fail(&l, A); tnd_login_fail(&l, A);
   CHECK(tnd_login_fail(&l, A), "login: three more: locked again ...");
   tnd_login_tick(&l, t += TND_LOCK_US + 1u);
   CHECK(tnd_login_locked(&l, A), "... for twice as long the second time");
   tnd_login_tick(&l, t += TND_LOCK_US);
   CHECK(!tnd_login_locked(&l, A), "... and then free");
   tnd_login_ok(&l, A);
   tnd_login_fail(&l, A);
   tnd_login_fail(&l, A);
   CHECK(!tnd_login_locked(&l, A), "login: a right login clears the count (two after it do not lock)");
   tnd_login_tick(&l, t += TND_FAIL_DECAY_US + 1u);
   CHECK(!tnd_login_fail(&l, A), "login: failures are forgotten after TND_FAIL_DECAY_US (one more does not lock)");
   for (int i = 0; i < 30; i++) {               /* the lockout stops growing at TND_LOCK_MAX_US */
      tnd_login_fail(&l, A); tnd_login_fail(&l, A); tnd_login_fail(&l, A);
   }
   tnd_login_tick(&l, t += TND_LOCK_MAX_US - 1u);
   CHECK(tnd_login_locked(&l, A), "login: a much repeated lockout lasts TND_LOCK_MAX_US ...");
   tnd_login_tick(&l, t += 2u);
   CHECK(!tnd_login_locked(&l, A), "login: ... and no longer");
   tnd_login_reset(&l);                         /* more addresses than the table holds: a lock is kept */
   tnd_login_tick(&l, t);
   tnd_login_fail(&l, A); tnd_login_fail(&l, A); tnd_login_fail(&l, A);
   for (uint32_t i = 1u; i <= 3u * TND_LOGIN_ADDRS; i++)
      tnd_login_fail(&l, 0x0C000000u + i);
   CHECK(tnd_login_locked(&l, A), "login: many other addresses do not push a locked one out of the table");
   CHECK(tnd_login_may_replace(A, A, 0u) && !tnd_login_may_replace(A, B, TND_REPLACE_IDLE_US - 1u)
         && tnd_login_may_replace(A, B, TND_REPLACE_IDLE_US),
         "login: a session at the prompt is replaced by its own address, or by another once idle");
}

/* PR review: one input byte released up to 5 keys into a 3-key array (a lone Latin-1 lead byte, ESC,
   then a high byte; or ESC P after one), before any login.  Every call, on random input and on the
   worst cases found, stays within TND_IN_KEYS, with the array exactly that size (ASan sees past it). */
static void test_input_key_bound(void)
{
   tnd_in_t s;
   uint32_t seed = 12345u, worst = 0u, t = 1000u;
   static const uint8_t worst_cases[][4] = {
      { 0xE9, 0xF5, 0, 0 }, { 0xE9, 0x1B, 0xF5, 0 }, { 0xE9, 0x1B, 'P', 0xF5 }, { 0xC3, 0x1B, 'P', 0xE9 },
      { 0x1B, 'P', 0xC0, 0 }, { 0x1B, 0xFE, 0, 0 }, { 0xF0, 0x9F, 0x1B, 0xFF },
   };
   for (int lit = 0; lit < 2; lit++)
      for (int tele = 0; tele < 2; tele++) {
         for (size_t w = 0; w < sizeof worst_cases / sizeof worst_cases[0]; w++) {
            tnd_in_reset(&s);
            s.lit_ok = lit != 0;
            s.tele = tele != 0;
            for (size_t i = 0; i < 4 && (i == 0 || worst_cases[w][i] != 0); i++) {
               uint8_t k[TND_IN_KEYS], r[TND_IN_REPLY];
               size_t  rl, n = tnd_in_byte(&s, worst_cases[w][i], t, k, r, &rl);
               if (n > worst) worst = (uint32_t)n;
               CHECK(n <= TND_IN_KEYS, "case %u byte %u: %u keys", (unsigned)w, (unsigned)i, (unsigned)n);
            }
            {
               uint8_t k[TND_IN_KEYS];
               size_t  n = tnd_in_tick(&s, t + 1000000u, k);  /* whatever was left waiting, timed out */
               CHECK(n <= TND_IN_KEYS, "case %u tick: %u keys", (unsigned)w, (unsigned)n);
            }
         }
         tnd_in_reset(&s);
         s.lit_ok = lit != 0;
         s.tele = tele != 0;
         for (unsigned i = 0; i < 2000000u; i++) {
            uint8_t k[TND_IN_KEYS], r[TND_IN_REPLY], c;
            size_t  rl, n;
            seed = seed * 1103515245u + 12345u;
            c = (uint8_t)(seed >> 16);
            if ((seed >> 8 & 7u) == 0u) c = 0x1B;              /* plenty of ESCs, Ps and high bytes */
            else if ((seed >> 8 & 7u) == 1u) c = 'P';
            t += (seed >> 24 & 3u) == 0u ? 300000u : 10u;      /* now and then long enough to time out */
            n = tnd_in_byte(&s, c, t, k, r, &rl);
            if (n > worst) worst = (uint32_t)n;
            if (n > TND_IN_KEYS) { CHECK(0, "random input: %u keys from one byte", (unsigned)n); break; }
            n = tnd_in_tick(&s, t, k);
            if (n > TND_IN_KEYS) { CHECK(0, "random input: %u keys from one tick", (unsigned)n); break; }
         }
      }
   CHECK(worst <= TND_IN_KEYS, "the most keys from one byte: %u (TND_IN_KEYS %u)", (unsigned)worst, (unsigned)TND_IN_KEYS);
}

static bool keys_are(const char *expect, size_t len)
{
   return nkeys == len && memcmp(keys, expect, len) == 0;
}
#define KEYS(lit) keys_are(lit, sizeof(lit) - 1)

static void dump(void)
{
   printf("    keys:");
   for (size_t i = 0; i < nkeys; i++) printf(" %02X", keys[i]);
   printf("\n    reply:");
   for (size_t i = 0; i < nreply; i++) printf(" %02X", reply[i]);
   printf("\n");
}

static void test_plain_keys(void)
{
   tnd_in_t s; tnd_in_reset(&s); in_reset();

   FEED(&s, "PRINT 6*7");
   CHECK(KEYS("PRINT 6*7"), "ordinary keys pass through"); if (!KEYS("PRINT 6*7")) dump();

   in_reset();
   FEED(&s, "A\r\0B");
   CHECK(nkeys == 3 && keys[0] == 'A' && keys[1] == 13 && keys[2] == 'B',
         "CR NUL is one RETURN, the NUL swallowed"); if (nkeys != 3) dump();

   in_reset();
   FEED(&s, "A\r\nB");
   CHECK(nkeys == 3 && keys[1] == 13 && keys[2] == 'B', "CR LF is one RETURN");

   in_reset();
   FEED(&s, "A\nB");
   CHECK(nkeys == 3 && keys[1] == 13, "a lone LF is a RETURN (netcat, printf)");

   in_reset();
   FEED(&s, "\r\r");
   CHECK(nkeys == 2 && keys[0] == 13 && keys[1] == 13, "two RETURNs are two");

   in_reset();
   FEED(&s, "x\x08y\x7f");
   CHECK(nkeys == 4 && keys[1] == 127 && keys[3] == 127, "BS and DEL both give the Beeb's DELETE");

   in_reset();
   FEED(&s, "\x01\x1a");
   CHECK(nkeys == 2 && keys[0] == 1 && keys[1] == 26, "control keys pass");

   in_reset();
   FEED(&s, "\x03");
   CHECK(nkeys == 1 && keys[0] == TND_KEY_ESCAPE, "Ctrl-C is the ESCAPE request");
}

static void test_pound_and_high_bytes(void)
{
   tnd_in_t s; tnd_in_reset(&s); in_reset();

   FEED(&s, "\xc2\xa3");
   CHECK(nkeys == 1 && keys[0] == 96, "UTF-8 pound sign is the Beeb's 96");

   in_reset();
   FEED(&s, "\xa3");
   CHECK(nkeys == 1 && keys[0] == 96, "Latin-1 pound sign too");

   in_reset();
   FEED(&s, "a\xc3\xa9z");
   CHECK(KEYS("az"), "another UTF-8 character is dropped whole"); if (!KEYS("az")) dump();

   in_reset();
   FEED(&s, "a\xe2\x82\xacz");
   CHECK(KEYS("az"), "a three-byte character is dropped whole"); if (!KEYS("az")) dump();

   /* T-18: with a ROM that reads bytes as they are (build 13), characters above 127 go through */
   tnd_in_reset(&s); s.lit_ok = true; in_reset();
   FEED(&s, "a\xc3\xa9z");
   CHECK(nkeys == 4 && keys[0] == 'a' && keys[1] == TND_KEY_LIT && keys[2] == 0xE9 && keys[3] == 'z',
         "lit_ok: UTF-8 e-acute is LIT and &E9"); if (nkeys != 4) dump();
   in_reset();
   FEED(&s, "\xc2\x81\xc2\x9d");
   CHECK(nkeys == 4 && keys[0] == TND_KEY_LIT && keys[1] == 0x81 && keys[2] == TND_KEY_LIT && keys[3] == 0x9D,
         "lit_ok: U+0081 and U+009D (teletext red, new background) are their bytes"); if (nkeys != 4) dump();
   in_reset();
   FEED(&s, "\xc2\xa3");
   CHECK(nkeys == 1 && keys[0] == 96, "lit_ok: the pound sign is still the Beeb's 96");
   in_reset();
   FEED(&s, "x\xe9y");
   CHECK(nkeys == 4 && keys[1] == TND_KEY_LIT && keys[2] == 0xE9, "lit_ok: a Latin-1 client's byte &E9 too"); if (nkeys != 4) dump();
   in_reset();
   FEED(&s, "\xe2\x86\x91");
   CHECK(nkeys == 0, "not MODE 7: an up arrow is no Beeb byte");
   s.tele = true; in_reset();
   FEED(&s, "\xe2\x86\x91\xc2\xbd\xc3\xb7\xe2\x80\x95");
   CHECK(KEYS("^\\~`"), "MODE 7: the glyphs the daemon prints come back as their codes (up arrow, 1/2, divide, dash)");
   if (!KEYS("^\\~`")) dump();
   in_reset();
   FEED(&s, "\xc3\xa9");
   CHECK(nkeys == 2 && keys[0] == TND_KEY_LIT && keys[1] == 0xE9, "MODE 7: other characters as their bytes");
   in_reset();
   FEED(&s, "\xf0\x9f\x98\x80" "q");
   CHECK(KEYS("q"), "a four-byte character (emoji) is dropped whole"); if (!KEYS("q")) dump();

   tnd_in_reset(&s); in_reset();
   FEED(&s, "\xc2" "a");
   CHECK(KEYS("a"), "a lead byte then ASCII: the ASCII kept (the lone byte, without lit_ok, dropped)");
}

static void test_cursor_and_function_keys(void)
{
   tnd_in_t s; tnd_in_reset(&s); in_reset();

   FEED(&s, "\x1b[A\x1b[B\x1b[C\x1b[D");
   CHECK(nkeys == 4 && keys[0] == 143 && keys[1] == 142 && keys[2] == 141 && keys[3] == 140,
         "CSI arrows are the buffer's codes: up 143, down 142, right 141, left 140"); if (nkeys != 4) dump();

   in_reset();
   FEED(&s, "\x1bOA\x1bOB\x1bOC\x1bOD");
   CHECK(nkeys == 4 && keys[0] == 143 && keys[3] == 140, "SS3 arrows (application mode) too");

   in_reset();
   FEED(&s, "\x1b[1;5A");
   CHECK(nkeys == 1 && keys[0] == 143, "modifier parameters are ignored: Ctrl-Up is Up");

   in_reset();
   FEED(&s, "\x1b[3~");
   CHECK(nkeys == 1 && keys[0] == 127, "Delete is the Beeb's DELETE");

   in_reset();
   FEED(&s, "\x1b[F");
   CHECK(nkeys == 1 && keys[0] == 139, "End is COPY (139 in the buffer)");
   in_reset();
   FEED(&s, "\x1b[4~");
   CHECK(nkeys == 1 && keys[0] == 139, "End (VT220) is COPY");

   in_reset();
   FEED(&s, "\x1bOP\x1bOQ\x1bOR\x1bOS");
   CHECK(nkeys == 4 && keys[0] == 0x81 && keys[1] == 0x82 && keys[2] == 0x83 && keys[3] == 0x84,
         "F1-F4 are f1-f4");

   in_reset();
   FEED(&s, "\x1b[15~\x1b[17~\x1b[18~\x1b[19~\x1b[20~\x1b[21~");
   CHECK(nkeys == 6 && keys[0] == 0x85 && keys[1] == 0x86 && keys[2] == 0x87 && keys[3] == 0x88
         && keys[4] == 0x89 && keys[5] == 0x80, "F5-F9 are f5-f9, F10 is f0"); if (nkeys != 6) dump();

   in_reset();
   FEED(&s, "\x1b[2~\x1b[5~\x1b[6~\x1b[H\x1b[1~");
   CHECK(nkeys == 1 && keys[0] == TND_KEY_SHIFT, "Insert, PgUp, Home: no Beeb key, nothing leaks; PgDn is SHIFT");
   if (nkeys != 1) dump();

   in_reset();
   FEED(&s, "\x1b[?25h" "k");
   CHECK(KEYS("k"), "a private sequence is swallowed, the next key survives"); if (!KEYS("k")) dump();
}

static void test_escape_key(void)
{
   tnd_in_t s; tnd_in_reset(&s); in_reset();

   FEED(&s, "\x1b");
   CHECK(nkeys == 0, "a lone ESC waits to see if a sequence follows");
   tick(&s, TND_ESC_US - 1);
   CHECK(nkeys == 0, "... not yet");
   tick(&s, 2);
   CHECK(nkeys == 1 && keys[0] == TND_KEY_ESCAPE, "... then it is the ESCAPE request");
   tick(&s, 1000000);
   CHECK(nkeys == 1, "and only once");

   in_reset();
   FEED(&s, "\x1bx");
   CHECK(nkeys == 2 && keys[0] == TND_KEY_ESCAPE && keys[1] == 'x', "ESC then a letter: both, in order");

   in_reset();
   FEED(&s, "\x1b\r");
   CHECK(nkeys == 2 && keys[0] == TND_KEY_ESCAPE && keys[1] == 13, "ESC then RETURN: both");

   in_reset();
   FEED(&s, "\x1b\x1b");
   CHECK(nkeys == 1 && keys[0] == TND_KEY_ESCAPE, "ESC ESC: the first is a request, the second still waiting");
   tick(&s, TND_ESC_US);
   CHECK(nkeys == 2 && keys[1] == TND_KEY_ESCAPE, "... and then it is one too");

   in_reset();
   FEED(&s, "\x1b[");
   tick(&s, 400000);
   FEED(&s, "A");
   CHECK(nkeys == 1 && keys[0] == 143, "a slow sequence still completes");
   in_reset();
   FEED(&s, "\x1b[");
   tick(&s, 600000);
   FEED(&s, "A");
   CHECK(KEYS("A"), "a sequence abandoned for half a second is dropped; the next key is plain");
}

static void test_blank(void)
{
   tnd_out_t o;
   uint8_t   buf[TND_OUT_MAX];
   tnd_out_reset(&o, false);
   tnd_out_assume_mode(&o, 7);
   tnd_out_at(&o, 0, 0);
   CHECK(tnd_out_blank(&o), "a model that has seen nothing is blank");
   (void)tnd_out_byte(&o, 'A', buf);
   CHECK(!tnd_out_blank(&o), "...and one with a character is not");
   (void)tnd_out_byte(&o, 12, buf);
   CHECK(tnd_out_blank(&o), "...until the screen is cleared");
}

static void test_break_key(void)
{
   tnd_in_t s; tnd_in_reset(&s); in_reset();
   FEED(&s, "\x1b[24~");
   CHECK(nkeys == 1 && keys[0] == TND_KEY_BREAK, "F12 is the BREAK request");
   in_reset();
   FEED(&s, "\x1b[21~\x1b[23~");
   CHECK(nkeys == 2 && keys[0] == 0x80 && keys[1] == TND_KEY_REFRESH, "F10 is f0 and F11 is the redraw request");
   in_reset();
   FEED(&s, "\x1b[6~");
   CHECK(nkeys == 1 && keys[0] == TND_KEY_SHIFT, "Page Down is SHIFT, for paged mode");
   in_reset();
   FEED(&s, "\x1b[5~");
   CHECK(nkeys == 0, "Page Up is nothing");
   in_reset();
   FEED(&s, "");
   CHECK(nkeys == 1 && keys[0] == TND_KEY_REFRESH, "Ctrl-L is the redraw request too");
}

static void test_negotiation(void)
{
   tnd_in_t s; tnd_in_reset(&s); in_reset();
   uint8_t g[TND_GREETING_LEN];

   CHECK(tnd_greeting(g) == TND_GREETING_LEN, "greeting length");
   static const uint8_t want[] = { 255, 251, 1, 255, 251, 3, 255, 253, 3, 255, 253, 31, 255, 253, 24 };
   CHECK(sizeof want == TND_GREETING_LEN && memcmp(g, want, sizeof want) == 0, "greeting: WILL ECHO, WILL SGA, DO SGA, DO NAWS, DO TTYPE");

   /* The client's answers to our offer need no reply. */
   FEED(&s, "\xff\xfd\x01\xff\xfd\x03\xff\xfb\x03");
   CHECK(nreply == 0 && nkeys == 0, "DO ECHO, DO SGA, WILL SGA: accepted silently"); if (nreply || nkeys) dump();

   /* Things we do not offer are refused, once. */
   in_reset();
   FEED(&s, "\xff\xfd\x18");
   static const uint8_t wont24[] = { 255, 252, 24 };
   CHECK(nreply == 3 && memcmp(reply, wont24, 3) == 0, "DO TTYPE: WONT TTYPE"); if (nreply != 3) dump();
   FEED(&s, "\xff\xfd\x18");
   CHECK(nreply == 3, "asked again: no second reply (no loop)");

   /* NAWS: asked for in the greeting, so WILL NAWS is accepted silently, and the sizes kept. */
   in_reset();
   FEED(&s, "\xff\xfb\x1f");
   CHECK(nreply == 0 && nkeys == 0 && s.win_gen == 0u, "WILL NAWS: accepted silently");
   FEED(&s, "\xff\xfa\x1f\x00\x64\x00\x1e\xff\xf0" "a");
   CHECK(s.win_cols == 100u && s.win_rows == 30u && s.win_gen == 1u && nkeys == 1 && keys[0] == 'a' && nreply == 0,
         "NAWS 100 x 30 (Windows telnet's answer): kept, nothing leaks, the next key goes through");
   FEED(&s, "\xff\xfa\x1f\x00\xff\xff\x01\x00\xff\xf0");
   CHECK(s.win_cols == 255u && s.win_rows == 256u && s.win_gen == 2u, "a 255 in the size is doubled (IAC IAC)");
   FEED(&s, "\xff\xfa\x1f\x00\x50\xff\xf0");
   CHECK(s.win_cols == 255u && s.win_gen == 2u, "a short NAWS is ignored");
   FEED(&s, "\xff\xfa\x18\x00VT100\xff\xf0");
   CHECK(s.win_gen == 2u, "another option's subnegotiation is not a size");
   tnd_in_reset(&s); in_reset();
   FEED(&s, "\xff\xfc\x1f");
   CHECK(s.naws_no && nreply == 0, "WONT NAWS: noted, no reply");

   /* TTYPE: asked for in the greeting; WILL TTYPE is answered with SEND, once; IS is kept. */
   tnd_in_reset(&s); in_reset();
   FEED(&s, "\xff\xfb\x18");
   static const uint8_t send24[] = { 255, 250, 24, 1, 255, 240 };
   CHECK(nreply == 6 && memcmp(reply, send24, 6) == 0, "WILL TTYPE: SB TTYPE SEND"); if (nreply != 6) dump();
   FEED(&s, "\xff\xfb\x18");
   CHECK(nreply == 6, "... once");
   FEED(&s, "\xff\xfa\x18\x00" "ANSI" "\xff\xf0" "x");
   CHECK(s.ttype_got && strcmp(s.ttype, "ANSI") == 0 && nkeys == 1 && keys[0] == 'x', "TTYPE IS ANSI (Windows telnet): kept, nothing leaks");
   FEED(&s, "\xff\xfa\x18\x00" "XTERM-256COLOR-AND-A-VERY-LONG-NAME-INDEED" "\xff\xf0");
   CHECK(strlen(s.ttype) == sizeof s.ttype - 1u && strncmp(s.ttype, "XTERM-256COLOR", 14) == 0, "a long name is cut, still ended");

   in_reset();
   FEED(&s, "\xff\xfe\x01\xff\xfc\x01");
   CHECK(nreply == 0, "DONT and WONT need no reply");

   /* Subnegotiation is dropped entirely, even with an escaped 0xFF inside. */
   in_reset();
   FEED(&s, "a\xff\xfa\x18\x00VT100\xff\xff\xff\xf0" "b");
   CHECK(KEYS("ab") && nreply == 0, "SB ... IAC SE is dropped, the keys around it survive"); if (!KEYS("ab")) dump();

   in_reset();
   FEED(&s, "a\xff\xff" "b");
   CHECK(KEYS("ab"), "IAC IAC is a literal 0xFF, which is not a Beeb key");

   in_reset();
   FEED(&s, "a\xff\xf1\xff\xf6" "b");
   CHECK(KEYS("ab") && nreply == 0, "NOP and AYT are ignored");

   in_reset();
   FEED(&s, "\xff");
   FEED(&s, "\xfd");
   FEED(&s, "\x2d");
   CHECK(nreply == 3, "a command split across reads still parses");
}

static void test_input_robustness(void)
{
   tnd_in_t s; tnd_in_reset(&s); in_reset();
   uint8_t junk[4096];
   unsigned seed = 12345;
   for (size_t i = 0; i < sizeof junk; i++) {
      seed = seed * 1103515245u + 12345u;
      junk[i] = (uint8_t)(seed >> 16);
   }
   /* Any byte stream: no key beyond what a byte can release, replies bounded. */
   size_t maxkeys = 0;
   for (size_t i = 0; i < sizeof junk; i++) {
      uint8_t k[TND_IN_KEYS], r[TND_IN_REPLY];
      size_t rl, n = tnd_in_byte(&s, junk[i], (uint32_t)i * 50u, k, r, &rl);
      if (n > maxkeys) maxkeys = n;
      CHECK(n <= TND_IN_KEYS && rl <= TND_IN_REPLY, "bounds at byte %zu", i);
      for (size_t j = 0; j < n; j++)
         CHECK(k[j] != 0u || k[j] == TND_KEY_ESCAPE, "a NUL key is only ever the ESCAPE request");
   }
   CHECK(maxkeys <= TND_IN_KEYS, "fuzz: key bound holds");
}

/* ---- Beeb -> terminal ---------------------------------------------------- */

static char outbuf[8192];
static const char *render(const void *data, size_t len, bool utf8, tnd_out_t *keep)
{
   tnd_out_t local;
   tnd_out_t *s = keep ? keep : &local;
   const uint8_t *p = data;
   size_t o = 0;
   if (!keep) tnd_out_reset(s, utf8);
   for (size_t i = 0; i < len; i++) {
      uint8_t d[TND_OUT_MAX];
      size_t n = tnd_out_byte(s, p[i], d);
      if (n > TND_OUT_MAX) { outbuf[0] = '!'; outbuf[1] = 0; return outbuf; }
      memcpy(outbuf + o, d, n);
      o += n;
   }
   outbuf[o] = '\0';
   return outbuf;
}
#define RENDER(lit) render(lit, sizeof(lit) - 1, true, NULL)
#define OUT_IS(lit, expect) (strcmp(RENDER(lit), expect) == 0)

/* A narrow text window: the terminal cannot scroll or clear part of a line, so the model does
   and the rectangle is drawn again, a row at a time, from the model. */
static void test_narrow_window(void)
{
   tnd_out_t o;
   uint8_t   d[TND_REPAINT_MAX];
   tnd_out_reset(&o, true);
   tnd_out_assume_mode(&o, 7);
   (void)render("\x0c" "TOP", 4, true, &o);                     /* a full CLS: the rows are the Beeb's */
   const char *r = render("\x1c\x02\x0a\x14\x03" "\x0c", 6, true, &o);  /* VDU 28,2,10,20,3 then CLS */
   CHECK(strstr(r, "\x1b[2J") == NULL, "a CLS in a narrow window does not clear the whole terminal");
   CHECK(o.dirty && o.dl == 2 && o.dt == 3 && o.dr == 20 && o.db == 10, "... the window is marked to be drawn again");
   CHECK(o.ch[0][0] == 'T' && o.ch[0][1] == 'O', "... and what is outside it is kept");
   CHECK(o.cx == 2 && o.cy == 3, "... the cursor is at the window's corner");
   r = render("ab\r", 3, true, &o);
   CHECK(strstr(r, "\r\x1b[2C") != NULL, "a CR in the window goes to its left edge, not the terminal's");
   unsigned rows = 0;
   size_t   n;
   char     first[TND_REPAINT_MAX + 1];
   first[0] = 0;
   while ((n = tnd_out_dirty_row(&o, d)) != 0u) {
      if (rows == 0u) { memcpy(first, d, n); first[n] = 0; }
      rows++;
      CHECK(n <= TND_REPAINT_MAX, "a drawn row fits");
      if (rows > 40u) break;
   }
   CHECK(rows == 8u, "the window's eight rows are drawn");
   CHECK(strncmp(first, "\x1b[4;3H", 6) == 0, "... from its top left corner");
   CHECK(!o.dirty, "... and then nothing is left to draw");

   /* scrolling inside it: the rows move in the model and are drawn again */
   for (int i = 1; i <= 12; i++) {
      char line[8];
      int  len = snprintf(line, sizeof line, "W%d\r\n", i);
      (void)render(line, (size_t)len, true, &o);
   }
   CHECK(o.dirty, "a scroll in the window marks it");
   CHECK(o.ch[10][2] == ' ' && o.ch[9][2] == 'W' && o.ch[9][3] == '1' && o.ch[9][4] == '2', "the model has scrolled: W12 on the window's last row but one");
   CHECK(o.ch[0][0] == 'T', "the line above the window did not move");
   while (tnd_out_dirty_row(&o, d) != 0u) {}
   CHECK(!o.dirty, "drawn again");

   /* a CLS while the terminal's rows are not yet the Beeb's starts them, and the rest comes back */
   tnd_out_origin_lost(&o);
   r = render("\x0c", 1, true, &o);
   CHECK(strstr(r, "\x1b[2J") != NULL && o.origin_ok, "without an origin the terminal is cleared and gets one");
   CHECK(o.dirty && o.dl == 0 && o.dt == 0 && o.db == o.rows - 1u, "... and the whole screen is drawn again");
}

static void test_output_text(void)
{
   CHECK(OUT_IS("PRINT 42", "PRINT 42"), "text passes");
   CHECK(OUT_IS(">*FX2,1\r\n", ">*FX2,1\r\n"), "CR LF passes");
   CHECK(OUT_IS("a\n\rb", "a\n\rb"), "the Beeb's LF CR passes in its own order");
   CHECK(OUT_IS("ab\x7f", "ab\x08 \x08"), "DELETE (127) is back, blank, back");
   CHECK(OUT_IS("\x08", "\x08"), "cursor left is BS");
   CHECK(OUT_IS("\x09", "\x1b[C"), "cursor right");
   CHECK(OUT_IS("\x0b", "\x1b[A"), "cursor up");
   CHECK(OUT_IS("\x07", "\x07"), "bell");
   CHECK(OUT_IS("\x0c", "\x1b[2J\x1b[H"), "CLS clears and homes");
   CHECK(OUT_IS("\x1e", "\x1b[1;1H"), "VDU 30 homes");
   CHECK(strcmp(render("a`b", 3, true, NULL), "a\xc2\xa3" "b") == 0, "the Beeb's pound (96) is UTF-8");
   CHECK(strcmp(render("a`b", 3, false, NULL), "a\xa3" "b") == 0, "... or Latin-1");
}

static void test_output_params(void)
{
   const char *r;

   r = RENDER("\x1f\x05\x03" "X");
   CHECK(strcmp(r, "\x1b[4;6HX") == 0, "VDU 31,5,3 is row 4, column 6 (1-based, y first)"); if (strcmp(r, "\x1b[4;6HX")) printf("    got [%s]\n", r);
   r = RENDER("\x1f\x00\x00");
   CHECK(strcmp(r, "\x1b[1;1H") == 0, "VDU 31,0,0 is the corner");
   r = RENDER("\x1f\xff\xff");
   CHECK(strcmp(r, "") == 0, "VDU 31 outside the screen is ignored, as the Beeb does");

   r = RENDER("\x16\x07" "OK");
   CHECK(strcmp(r, "\x1b[2J\x1b[HOK") == 0, "MODE n clears the screen and its parameter vanishes"); if (strcmp(r, "\x1b[2J\x1b[HOK")) printf("    got [%s]\n", r);

   r = RENDER("\x11\x01R\x11\x82G");
   CHECK(strcmp(r, "\x1b[31mR\x1b[42mG") == 0, "COLOUR 1 is red text, 130 is green background"); if (strcmp(r, "\x1b[31mR\x1b[42mG")) printf("    got [%s]\n", r);
   r = RENDER("\x11\x0f" "x");
   CHECK(strcmp(r, "\x1b[37mx") == 0, "colour 15 (flashing white) is white");

   /* Parameters nobody shows are swallowed whole, so no junk reaches the terminal. */
   r = RENDER("\x12\x00\x01" "a");                 /* GCOL */
   CHECK(strcmp(r, "a") == 0, "GCOL eats 2");
   r = RENDER("\x13\x01\x02\x00\x00\x00" "a");     /* palette */
   CHECK(strcmp(r, "a") == 0, "palette eats 5");
   r = RENDER("\x17\x01\x00\x00\x00\x00\x00\x00\x00\x00" "a");   /* VDU 23,1: cursor off */
   CHECK(strcmp(r, "\x1b[?25la") == 0, "VDU 23 eats 9, and VDU 23,1,0 hides the terminal's cursor too");
   r = RENDER("\x17\x01\x01\x00\x00\x00\x00\x00\x00\x00" "a");
   CHECK(strcmp(r, "\x1b[?25ha") == 0, "VDU 23,1,1 shows it");
   r = RENDER("\x17\x00\x0a\x00\x00\x00\x00\x00\x00\x00" "a");
   CHECK(strcmp(r, "a") == 0, "other VDU 23 (character definitions) stay silent");
   r = RENDER("\x18\x01\x02\x03\x04\x05\x06\x07\x08" "a");
   CHECK(strcmp(r, "a") == 0, "VDU 24 eats 8");
   r = RENDER("\x19\x04\x00\x01\x00\x02" "a");     /* PLOT */
   CHECK(strcmp(r, "a") == 0, "VDU 25 eats 5");
   r = RENDER("\x1c\x00\x18\x27\x00" "a");         /* text window */
   CHECK(strcmp(r, "\x1b[1;1Ha") == 0, "VDU 28 eats 4 and homes the cursor in the window");
   r = RENDER("\x1d\x00\x01\x02\x03" "a");
   CHECK(strcmp(r, "a") == 0, "VDU 29 eats 4");
   r = RENDER("\x01\x41" "a");                      /* VDU 1: next char to printer */
   CHECK(strcmp(r, "a") == 0, "VDU 1 eats 1");

   /* Control bytes as parameters must not act as codes. */
   r = RENDER("\x11\x0c" "x");                      /* COLOUR 12: the 12 is a colour, not CLS */
   CHECK(strcmp(r, "\x1b[34mx") == 0, "a parameter that looks like CLS is just a number"); if (strcmp(r, "\x1b[34mx")) printf("    got [%s]\n", r);
   r = RENDER("\x16\x0d" "x");
   CHECK(strcmp(r, "\x1b[2J\x1b[Hx") == 0, "MODE 13: the 13 is not a RETURN");

   /* Codes that do nothing on a terminal take no parameters and print nothing. */
   r = RENDER("\x00\x02\x03\x04\x05\x0e\x0f\x10\x14\x1b" "z");
   CHECK(strcmp(r, "z") == 0, "VDU 0,2-5,14-16,20,27: silent"); if (strcmp(r, "z")) printf("    got [%s]\n", r);
}

/* ---- the screen model and the copy cursor ------------------------------- */

/* Windows' telnet client gets three CSI sequences wrong (checked against the client, Oct 2026): CHA
   (ESC [ n G) puts the cursor on the next line; SU and SD (ESC [ n S / T) do nothing and garble what
   follows.  No byte of output may contain them.  (A sequence split between two calls is not seen:
   the translator writes each sequence in one call.) */
static void wintelnet_check(const uint8_t *d, size_t n)
{
   for (size_t i = 0; i + 1 < n; i++) {
      if (d[i] != 27u || d[i + 1] != '[')
         continue;
      size_t j = i + 2;
      while (j < n && ((d[j] >= '0' && d[j] <= '9') || d[j] == ';'))
         j++;
      if (j < n && (d[j] == 'G' || d[j] == 'S' || d[j] == 'T'))
         CHECK(false, "no CHA, SU or SD in the output (Windows telnet gets them wrong)");
   }
}

static size_t feed_out(tnd_out_t *s, const char *data, size_t len, char *dst, size_t cap)
{
   size_t o = 0;
   for (size_t i = 0; i < len; i++) {
      uint8_t d[TND_OUT_MAX];
      size_t n = tnd_out_byte(s, (uint8_t)data[i], d);
      CHECK(n <= TND_OUT_MAX, "a byte of output never exceeds TND_OUT_MAX");
      wintelnet_check(d, n);
      if (o + n < cap) { memcpy(dst + o, d, n); o += n; }
   }
   dst[o] = '\0';
   return o;
}
#define FEED_OUT(s, lit, dst) feed_out(s, lit, sizeof(lit) - 1, dst, sizeof dst)

static void test_model_wrap_scroll(void)
{
   tnd_out_t s;
   char out[2048];
   char want[2048];

   /* Not synced: exactly what the translator always did. */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "0123456789012345678901234567890123456789ABC", out);
   CHECK(strcmp(out, "0123456789012345678901234567890123456789ABC") == 0,
         "before the Beeb's cursor is known nothing is added");

   /* MODE 7 is 40 columns: the 41st character starts the next row. */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x07" "0123456789012345678901234567890123456789ABC", out);
   strcpy(want, "\x1b[2J\x1b[H0123456789012345678901234567890123456789\x1b[2;1HABC");
   CHECK(strcmp(out, want) == 0, "a 40-column line wraps where the Beeb wraps"); if (strcmp(out, want)) printf("    got [%s]\n", out);

   /* MODE 3 is 80 columns by 25 rows. */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x03", out);
   {
      char line[100];
      memset(line, 'x', 80); line[80] = '\0';
      strcpy(out, ""); size_t o = 0;
      for (int i = 0; i < 80; i++) {
         uint8_t d[TND_OUT_MAX];
         size_t n = tnd_out_byte(&s, 'x', d);
         memcpy(out + o, d, n); o += n;
      }
      out[o] = '\0';
      CHECK(strstr(out, "\x1b[2;1H") != NULL && out[strlen(out) - 1] == 'H', "MODE 3 wraps after 80");
   }

   /* LF at the bottom scrolls the Beeb's window, and the terminal's. */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x07\x1f\x00\x18x\n", out);
   strcpy(want, "\x1b[2J\x1b[H\x1b[25;1Hx\x1b[1;25r\x1b[25;1H\x1b" "D\x1b[r\x1b[25;2H");
   CHECK(strcmp(out, want) == 0, "LF on the last row scrolls the terminal too"); if (strcmp(out, want)) printf("    got [%s]\n", out);

   /* A text window: TAB is relative to it and it scrolls on its own. */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x03\x1c\x0a\x14\x1d\x05\x1f\x00\x00y", out);
   strcpy(want, "\x1b[2J\x1b[H\x1b[6;11H\x1b[6;11Hy");
   CHECK(strcmp(out, want) == 0, "VDU 28 sets the window and TAB(0,0) is its corner"); if (strcmp(out, want)) printf("    got [%s]\n", out);

   /* Writing in the last cell of the screen scrolls. */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x07\x1f\x27\x18z", out);
   strcpy(want, "\x1b[2J\x1b[H\x1b[25;40Hz\x1b[1;25r\x1b[25;1H\x1b" "D\x1b[r\x1b[25;1H");
   CHECK(strcmp(out, want) == 0, "a character in the bottom right cell scrolls"); if (strcmp(out, want)) printf("    got [%s]\n", out);

   /* Teletext starts each row white. */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x07\x1f\x00\x00\x81" "R\x1f\x00\x01" "W", out);
   CHECK(strstr(out, "\x1b[0;31mR") != NULL && strstr(out, "\x1b[2;1H\x1b[0mW") != NULL,
         "a colour does not carry to the next row in MODE 7");
   if (strstr(out, "\x1b[2;1H\x1b[0mW") == NULL) printf("    got [%s]\n", out);
}

static void test_copy_cursor(void)
{
   tnd_out_t s;
   char out[2048];
   uint8_t d[TND_COPY_MAX];
   size_t n;
   uint32_t t = 1000000;

   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x0c" ">HELLO\r\n>", out);            /* CLS, >HELLO, and BASIC's prompt on row 2 */
   tnd_out_touch(&s, t);
   tnd_out_copykey(&s, 143);                            /* up */
   n = tnd_out_copyflush(&s, t + 1000, d);
   CHECK(n == 0, "the mark waits until the Beeb has been quiet");
   n = tnd_out_copyflush(&s, t + 50000, d);
   CHECK(n > 0 && n <= TND_COPY_MAX, "... then it is drawn");
   d[n] = 0;
   CHECK(strstr((char *)d, "\x1b[1;2H") != NULL && strstr((char *)d, "\x1b[7mH") != NULL,
         "up from the cursor marks the H above it, reversed"); if (!strstr((char *)d, "\x1b[7mH")) printf("    got [%s]\n", d);
   CHECK(n >= 6 && strstr((char *)d + n - 12, "\x1b[2;2H") != NULL, "and the cursor goes back to where typing goes");

   tnd_out_copykey(&s, 139);                            /* COPY: the mark moves on to the E */
   n = tnd_out_copyflush(&s, t + 100000, d);
   d[n] = 0;
   CHECK(strstr((char *)d, "\x1b[1;2H\x1b[0mH") != NULL && strstr((char *)d, "\x1b[7mE") != NULL,
         "COPY puts the H back and marks the E"); if (!strstr((char *)d, "\x1b[7mE")) printf("    got [%s]\n", d);

   tnd_out_copykey(&s, 13);                             /* RETURN ends it */
   n = tnd_out_copyflush(&s, t + 150000, d);
   d[n] = 0;
   CHECK(strstr((char *)d, "\x1b[1;3H\x1b[0mE") != NULL && strstr((char *)d, "\x1b[7m") == NULL,
         "RETURN removes the mark"); if (strstr((char *)d, "\x1b[7m")) printf("    got [%s]\n", d);
   CHECK(tnd_out_copyflush(&s, t + 200000, d) == 0, "and nothing more is sent");

   /* The mark moves with the text when the screen scrolls, and never stays on a cell the model has not seen. */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x07" ">AB\r\n>", out);
   tnd_out_touch(&s, t);
   tnd_out_copykey(&s, 143);
   n = tnd_out_copyflush(&s, t + 50000, d);
   d[n] = 0;
   CHECK(strstr((char *)d, "\x1b[7mA") != NULL, "MODE 7: the mark is on the A");
   FEED_OUT(&s, "\x1f\x00\x18\n", out);                 /* TAB to the bottom row and a LF: the screen scrolls */
   tnd_out_touch(&s, t + 60000);
   tnd_out_copykey(&s, 141);
   n = tnd_out_copyflush(&s, t + 120000, d);
   d[n] = 0;
   CHECK(strstr((char *)d, "\x1b[7m") == NULL || strstr((char *)d, "\x1b[7mB") == NULL,
         "after a scroll the mark is not left on the old row");

   /* T-13: a listing waiting in paged mode (the cursor at the start of its row, nothing typed
      there): COPY or a cursor key just waits in the Beeb's buffer, so no mark is started. */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x07\x0e" "  10PRINT\r\n  20END\r\n", out);
   tnd_out_touch(&s, t);
   tnd_out_copykey(&s, 139);
   n = tnd_out_copyflush(&s, t + 50000, d);
   CHECK(n == 0 && !s.cc_on && !s.hl_on, "T-13: COPY with the cursor at the start of a row (a paged listing): no mark");
   tnd_out_copykey(&s, 143);
   n = tnd_out_copyflush(&s, t + 100000, d);
   CHECK(n == 0 && !s.cc_on, "T-13: ... nor for a cursor key");
   FEED_OUT(&s, ">", out);                              /* the listing ends: BASIC's prompt */
   tnd_out_touch(&s, t + 150000);
   tnd_out_copykey(&s, 143);
   n = tnd_out_copyflush(&s, t + 250000, d);
   d[n] = 0;
   CHECK(s.cc_on && strstr((char *)d, "\x1b[7m") != NULL, "T-13: at the prompt the copy cursor starts as before");

   /* T-13 on the Beeb: in MODE 7 a paged listing also waits where a long line wraps, its cursor at
      column 39: in paged mode the copy cursor starts only on a row that begins with ">" or "?". */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x07\x0e" "  10PRINT\r\n" "  20PRINT\"ABCDEFGHIJKLMNOPQRSTUVWXYZ012", out);   /* cursor at column 39 */
   tnd_out_touch(&s, t);
   tnd_out_copykey(&s, 139);
   n = tnd_out_copyflush(&s, t + 50000, d);
   CHECK(n == 0 && !s.cc_on && s.paged && s.cx > 30u, "T-13: paged, waiting mid-row at a wrap: COPY starts no mark");
   FEED_OUT(&s, "\r\n?", out);                          /* an INPUT prompt */
   tnd_out_touch(&s, t + 60000);
   tnd_out_copykey(&s, 143);
   n = tnd_out_copyflush(&s, t + 150000, d);
   CHECK(s.cc_on, "T-13: paged, at INPUT's ? the copy cursor starts");
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x07\x0e\x0f" "  20PRINT\"ABCDEFGHIJKLMNOPQRSTUVWXYZ012", out);   /* VDU 15: paged off */
   tnd_out_touch(&s, t);
   tnd_out_copykey(&s, 140);
   n = tnd_out_copyflush(&s, t + 50000, d);
   CHECK(!s.paged && s.cc_on, "T-13: VDU 15 ends paged mode: the plain rule again");
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x07\x0e", out);
   tnd_out_assume_mode(&s, 7u);
   CHECK(!s.paged, "T-13: a reset ends paged mode");

   /* With ROM build 11 the keys come only when the Beeb has taken them (cc_exact): no guessing
      from the screen, so a program's own prompt in paged mode ("Option (y/n)") works too. */
   tnd_out_reset(&s, true);
   s.cc_exact = true;
   FEED_OUT(&s, "\x16\x07\x0e" "  10PRINT\r\n" "Option (y/n)", out);
   tnd_out_touch(&s, t);
   tnd_out_copykey(&s, 143);
   n = tnd_out_copyflush(&s, t + 50000, d);
   CHECK(s.cc_on && s.ccy == 0u, "cc_exact: paged, a program's own prompt: the copy cursor starts (the Beeb took the key)");
   tnd_out_reset(&s, true);
   s.cc_exact = true;
   FEED_OUT(&s, "\x16\x07" "\r\n", out);               /* INPUT with no prompt: the cursor at column 0 */
   tnd_out_touch(&s, t);
   tnd_out_copykey(&s, 143);
   n = tnd_out_copyflush(&s, t + 50000, d);
   CHECK(s.cc_on, "cc_exact: at column 0 too");
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x07" ">", out);
   tnd_out_touch(&s, t);
   tnd_out_copykey(&s, 139);
   n = tnd_out_copyflush(&s, t + 50000, d);
   CHECK(!s.cc_on && n == 0, "COPY with no copy cursor: nothing (the MOS only beeps)");

   /* Not synced: no mark at all. */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "abc", out);
   tnd_out_copykey(&s, 143);
   CHECK(tnd_out_copyflush(&s, t + 500000, d) == 0, "with the cursor position unknown there is no mark");

   /* A cell the model has not seen is never marked (it could be anything). */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x1f\x05\x05" "x", out);               /* TAB syncs; the rest of the screen is unknown */
   tnd_out_touch(&s, t);
   tnd_out_copykey(&s, 143);
   tnd_out_copykey(&s, 143);
   n = tnd_out_copyflush(&s, t + 60000, d);
   CHECK(n == 0, "an unseen cell is not marked");
}

/* A copy cursor key, applied at once (the model only; whatever is drawn is not looked at). */
static void cc_key(tnd_out_t *s, uint8_t k)
{
   uint8_t cb[TND_COPY_MAX];
   tnd_out_copykey(s, k);
   (void)tnd_out_copyflush(s, 100000000u, cb);
}

static void test_repaint(void)
{
   tnd_out_t s;
   char out[2048];
   uint8_t d[TND_REPAINT_MAX];
   size_t n;

   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x01" "HI\r\n\x11\x01YO", out);      /* MODE 1, HI, a new line, then YO in red */
   n = tnd_out_repaint(&s, 0u, d);
   d[n] = 0;
   CHECK(strcmp((char *)d, "\x1b[0mHI\x1b[0m\r\n") == 0, "repaint draws row 0 as a plain line, with no clear and no positioning"); if (strcmp((char *)d, "\x1b[0mHI\x1b[0m\r\n")) printf("    got [%s]\n", d);
   n = tnd_out_repaint(&s, 1u, d);
   d[n] = 0;
   CHECK(strstr((char *)d, "\x1b[31mYO") != NULL && strstr((char *)d, "\r\n") == NULL,
         "row 1, the last, comes back in its colour with no line end"); if (!strstr((char *)d, "\x1b[31mYO")) printf("    got [%s]\n", d);
   CHECK(tnd_out_repaint(&s, 5u, d) == 0u, "a row with nothing on it needs nothing");
   n = tnd_out_repaint(&s, s.rows, d);
   d[n] = 0;
   CHECK(strstr((char *)d, "\r\x1b[2C") != NULL && strstr((char *)d, "\x1b[1A") == NULL, "after the last row the cursor goes to the Beeb's column"); if (!strstr((char *)d, "\r\x1b[2C")) printf("    got [%s]\n", d);

   /* A screen with something below the cursor: the cursor is moved back up to its row. */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x0c" "A\r\nB\r\nC\x1f\x01\x00", out);   /* the cursor is on row 0 again, at column 1 */
   n = tnd_out_repaint(&s, s.rows, d);
   d[n] = 0;
   CHECK(strstr((char *)d, "\x1b[2A") != NULL && strstr((char *)d, "\r\x1b[1C") != NULL, "the cursor is moved up to its row and across"); if (!strstr((char *)d, "\x1b[2A")) printf("    got [%s]\n", d);

   /* Blank rows above the first thing on the screen are not drawn. */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x0c\x1f\x00\x0a" "Z", out);
   CHECK(tnd_out_repaint(&s, 3u, d) == 0u && tnd_out_repaint(&s, 10u, d) > 0u, "the empty rows at the top are skipped");

   /* Is a line on the screen? */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x0c Telnet\r\n>", out);
   CHECK(tnd_out_has_line(&s, " Telnet", 7u) && !tnd_out_has_line(&s, "Telnet", 6u) && !tnd_out_has_line(&s, "Acorn", 5u),
         "a row is found only when it is exactly that text");

   /* The rows are the Beeb's no longer: relative moves only, and a clear before the first absolute one. */
   tnd_out_reset(&s, true);
   tnd_out_assume_mode(&s, 7u);
   FEED_OUT(&s, "\x0c", out);
   tnd_out_origin_lost(&s);
   FEED_OUT(&s, "0123456789012345678901234567890123456789ab", out);
   CHECK(strstr(out, "\x1b[") == NULL && strstr(out, "0123456789012345678901234567890123456789\r\nab") != NULL,
         "a 40-column line is wrapped by hand: CR LF, no absolute position"); if (strstr(out, "\x1b[")) printf("    got [%s]\n", out);
   FEED_OUT(&s, "\x1f\x05\x03" "T", out);
   CHECK(strcmp(out, "\x1b[2J\x1b[4;6HT") == 0, "the first TAB clears once and positions"); if (strcmp(out, "\x1b[2J\x1b[4;6HT")) printf("    got [%s]\n", out);
   FEED_OUT(&s, "\x1f\x01\x01" "U", out);
   CHECK(strcmp(out, "\x1b[2;2HU") == 0, "... and not again");
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x0c", out);
   tnd_out_origin_lost(&s);
   FEED_OUT(&s, "\x0c", out);
   FEED_OUT(&s, "\x1f\x02\x02" "V", out);
   CHECK(strcmp(out, "\x1b[3;3HV") == 0, "a CLS puts the rows right: no extra clear");
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x0c", out);
   tnd_out_origin_lost(&s);
   FEED_OUT(&s, "\x1f\x00\x02" "AB\r\n" "CD", out);
   tnd_out_origin_lost(&s);
   tnd_out_copykey(&s, 143);
   {
      uint8_t cb[TND_COPY_MAX + 1];
      size_t cn = tnd_out_copyflush(&s, 100000000u, cb);
      cb[cn] = 0;
      CHECK(strncmp((char *)cb, "\x1b[0m\x1b[2J", 8) == 0 && strstr((char *)cb, "\x1b[7m") == NULL && s.origin_ok && s.dirty,
            "T-11: a copy key under the greeting: the terminal is cleared and the rows put to be drawn again, no mark yet");
      if (strncmp((char *)cb, "\x1b[0m\x1b[2J", 8)) printf("    got [%s]\n", cb);
      CHECK(s.npend == 1u && tnd_out_copyflush(&s, 100000000u, cb) == 0u, "... the key kept while the rows are drawn");
      out[0] = 0;
      for (int i = 0; i < 64 && s.dirty; i++) {
         size_t dn = tnd_out_dirty_row(&s, d);
         d[dn] = 0;
         strcat(out, (char *)d);
      }
      CHECK(!s.dirty && strstr(out, "AB") != NULL && strstr(out, "CD") != NULL, "... the screen drawn again from the model");
      cn = tnd_out_copyflush(&s, 100000000u, cb);
      cb[cn] = 0;
      CHECK(strstr((char *)cb, "\x1b[3;3H") != NULL && strstr((char *)cb, "\x1b[7m") != NULL && s.hl_on && s.hly == 2u,
            "... then the mark, one row up from the cursor"); if (!strstr((char *)cb, "\x1b[3;3H")) printf("    got [%s]\n", cb);
   }

   /* The copy cursor wraps as the MOS's does (OS 1.20 &D8F0, &CD3F): inside the text window,
      off the top to the bottom row, off the bottom to the top, off a side to the other side of
      the row above or below. */
   tnd_out_reset(&s, true);
   tnd_out_assume_mode(&s, 7u);
   FEED_OUT(&s, "\x0c" "TOP\x1f\x00\x18" "BOTTOM\x1f\x00\x00>", out);   /* the prompt on row 0 */
   cc_key(&s, 143u);
   CHECK(s.ccx == 1u && s.ccy == 24u, "up off the top: the bottom row");
   cc_key(&s, 142u);
   CHECK(s.ccy == 0u, "down off the bottom: the top row");
   cc_key(&s, 140u);
   cc_key(&s, 140u);
   CHECK(s.ccx == 39u && s.ccy == 24u, "left off row 0's first column: the last column of the bottom row");
   cc_key(&s, 141u);
   CHECK(s.ccx == 0u && s.ccy == 0u, "right off the last cell: the first of the top row");
   cc_key(&s, 139u);
   CHECK(s.ccx == 1u && s.ccy == 0u, "COPY moves on one");
   tnd_out_reset(&s, true);
   tnd_out_assume_mode(&s, 7u);
   FEED_OUT(&s, "\x0c\x1c\x05\x14\x1e\x0a" "x", out);     /* VDU 28,5,20,30,10: rows 10-20, columns 5-30 */
   cc_key(&s, 143u);
   CHECK(s.ccy == 20u, "inside a text window: up off its top row goes to its bottom row");
   s.ccx = 5u;
   cc_key(&s, 140u);
   CHECK(s.ccx == 30u && s.ccy == 19u, "... and left off its left edge to its right edge, a row up");

   /* A mark belongs to a terminal: a new one starts without. */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x0c" ">HI\r\n>", out);
   tnd_out_touch(&s, 100u);
   tnd_out_copykey(&s, 143);
   n = tnd_out_copyflush(&s, 100000u, d);
   CHECK(s.hl_on, "the copy cursor is marked");
   tnd_out_session(&s);
   CHECK(!s.hl_on && !s.cc_on && s.npend == 0, "a new session starts with no mark");

   /* ... but the Beeb's colours go on, and the new terminal is told them. */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x01\x11\x03x", out);
   tnd_out_session(&s);
   {
      uint8_t cb[16];
      size_t cn = tnd_out_colours(&s, cb);
      cb[cn] = 0;
      CHECK(strcmp((char *)cb, "\x1b[0m\x1b[37m") == 0, "a new terminal is brought to COLOUR 3, which is white in MODE 1's own palette");
   }
   n = tnd_out_repaint(&s, s.rows, d);
   d[n] = 0;
   CHECK(strstr((char *)d, "\x1b[37m") != NULL, "and so is the last step of a repaint"); if (!strstr((char *)d, "\x1b[37m")) printf("    got [%s]\n", d);

   /* The palette: logical colours are not the terminal's colours outside MODE 7. */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x01\x11\x02x", out);
   CHECK(strstr(out, "\x1b[33mx") != NULL, "MODE 1: COLOUR 2 is yellow");
   FEED_OUT(&s, "\x13\x02\x02\x00\x00\x00y", out);     /* VDU 19,2,2: logical 2 is now green */
   CHECK(strstr(out, "\x1b[32m") != NULL && out[strlen(out) - 1] == 'y', "VDU 19 changes what a logical colour shows as, and the colour already set follows"); if (!strstr(out, "\x1b[32m")) printf("    got [%s]\n", out);
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x00\x11\x01x", out);
   CHECK(strstr(out, "\x1b[37mx") != NULL, "a two-colour mode: COLOUR 1 is white");
   FEED_OUT(&s, "\x11\x81z", out);
   CHECK(strstr(out, "\x1b[47mz") != NULL, "... and 129 is a white background");
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x07\x11\x03x", out);
   CHECK(strstr(out, "\x1b[33m") == NULL && out[strlen(out) - 1] == 'x', "MODE 7: COLOUR does nothing, as on the Beeb"); if (strstr(out, "\x1b[33m")) printf("    got [%s]\n", out);

   /* The Beeb's cursor read from the MOS: followed from there, but nothing is known to be on the screen. */
   tnd_out_reset(&s, true);
   tnd_out_assume_mode(&s, 3u);
   tnd_out_at(&s, 5u, 24u);
   FEED_OUT(&s, "x\n", out);
   CHECK(strstr(out, "\x1b[1;25r\x1b[25;1H\x1b" "D\x1b[r") != NULL, "from a known cursor on the last row an LF scrolls the terminal too"); if (!strstr(out, "\x1b" "D")) printf("    got [%s]\n", out);

   /* Not synced: nothing is known, so nothing is drawn. */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "abc", out);
   CHECK(tnd_out_repaint(&s, 0u, d) == 0u, "an unsynced model draws nothing");
}

/* Full review 2026-10-20: teletext colour codes only in MODE 7, and a reset ends a VDU 21 or a
   VDU sequence half collected. */
static void test_review_fixes_full(void)
{
   tnd_out_t s;
   char out[2048];
   const char *r = RENDER("\x16\x04\x81Hi");
   CHECK(strstr(r, "\x1b[31m") == NULL && strstr(r, " Hi") != NULL, "MODE 4: 0x81 is a space, not red"); if (strstr(r, "\x1b[31m")) printf("    got [%s]\n", r);

   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x15", out);                    /* VDU 21, then a BREAK */
   tnd_out_assume_mode(&s, 7u);
   FEED_OUT(&s, "ok", out);
   CHECK(strstr(out, "ok") != NULL, "a reset ends VDU 21: what is printed after it shows"); if (!strstr(out, "ok")) printf("    got [%s]\n", out);

   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x1f\x05", out);                /* VDU 31 with one of its two parameters, then a BREAK */
   tnd_out_assume_mode(&s, 7u);
   FEED_OUT(&s, "ok", out);
   CHECK(strstr(out, "ok") != NULL, "a reset ends a VDU sequence half collected"); if (!strstr(out, "ok")) printf("    got [%s]\n", out);
}

/* The second review's fixes (2026-10-21): each check failed on 4d96b12's telnetd_proto.c. */
static void test_review_fixes_2(void)
{
   tnd_in_t in;
   tnd_out_t s;
   char out[4096];
   size_t n;
   const char *r;

   /* A telnet command inside an escape sequence is read as one, and the sequence carries on. */
   tnd_in_reset(&in); in_reset();
   FEED(&in, "\x1bO\xff\xfa\x1f\x00\x50\x00\x18\xff\xf0" "A");
   CHECK(nkeys == 1 && keys[0] == 143u, "ESC O, a NAWS, then A: the up arrow and nothing else"); if (nkeys != 1 || keys[0] != 143u) dump();
   CHECK(in.win_cols == 80u && in.win_rows == 24u, "... and the window size is taken");
   tnd_in_reset(&in); in_reset();
   FEED(&in, "\x1b[1\xff\xf1" "5~");
   CHECK(nkeys == 1 && keys[0] == 0x85u, "an IAC NOP inside ESC [ 15 ~: still f5"); if (nkeys != 1) dump();

   /* IAC SB with no IAC SE: dropped after a while, and the keys after it are keys. */
   tnd_in_reset(&in); in_reset();
   FEED(&in, "\xff\xfa\x1f\x01\x02");
   tick(&in, 3000000u);
   FEED(&in, "x");
   CHECK(nkeys == 1 && keys[0] == 'x', "an unended subnegotiation does not swallow every later key"); if (nkeys != 1) dump();

   /* A Latin-1 client's lone byte (&E9) is given once nothing follows it. */
   tnd_in_reset(&in); in_reset(); in.lit_ok = true;
   FEED(&in, "\xe9");
   tick(&in, 200000u);
   CHECK(nkeys == 2 && keys[0] == TND_KEY_LIT && keys[1] == 0xE9u, "a lone &E9 reaches the Beeb without another key"); if (nkeys != 2) dump();

   /* An agreed option taken back is acknowledged; a refusal of the offer is not answered. */
   tnd_in_reset(&in); in_reset();
   FEED(&in, "\xff\xfe\x01");
   CHECK(nreply == 0, "DONT ECHO as the answer to WILL ECHO: no reply");
   FEED(&in, "\xff\xfd\x01");
   CHECK(nreply == 0, "DO ECHO: agreed, no reply");
   FEED(&in, "\xff\xfe\x01");
   CHECK(nreply == 3 && memcmp(reply, "\xff\xfc\x01", 3) == 0, "DONT ECHO after DO ECHO: WONT ECHO");
   in_reset();
   FEED(&in, "\xff\xfb\x03\xff\xfc\x03");
   CHECK(nreply == 3 && memcmp(reply, "\xff\xfe\x03", 3) == 0, "WONT SGA after WILL SGA: DONT SGA");

   /* VDU 21 still counts parameters: a 6 among them does not end it. */
   r = RENDER("\x15\x1f\x06\x0a" "hi" "\x06" "b");
   CHECK(strcmp(r, "b") == 0, "VDU 21,31,6,10 hides the TAB's parameters and the text after it"); if (strcmp(r, "b")) printf("    got [%s]\n", r);

   /* VDU 20: the default colours. */
   r = RENDER("\x16\x01\x11\x01" "x" "\x14" "y");
   CHECK(strstr(r, "x\x1b[0my") != NULL, "VDU 20 puts the terminal's colours back"); if (!strstr(r, "x\x1b[0my")) printf("    got [%s]\n", r);
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x04", out);
   CHECK(s.fg == 1u, "MODE 4: the text colour is logical 1, the highest there"); if (s.fg != 1u) printf("    got %u\n", s.fg);

   /* A read that agrees with the model but has another cursor: the cursor is taken. */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x01" "HI\r\n\x11\x01YO", out);
   tnd_out_scan_merge(&s);
   n = FEED_OUT(&s, "\x1a\x0c" "\x1f\x00\x00" "H" "\x1f\x01\x00" "I" "\x1f\x00\x01" "Y" "\x1f\x01\x01" "O"
                    "\x1c\x00\x1f\x27\x00" "\x1f\x05\x03", out);
   CHECK(n != 0u && s.cx == 5u && s.cy == 3u, "a read that agrees but moved the cursor: the cursor is the Beeb's"); if (s.cx != 5u || s.cy != 3u) printf("    got %u,%u\n", s.cx, s.cy);
   CHECK(s.ch[1][0] == 'Y' && (s.at[1][0] & 0x40u) == 0u, "... and the red YO keeps its colour");

   /* VDU 8 in the window's top left corner: the window scrolls down, the cursor to the row's end. */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x01" "A\r\x08", out);
   CHECK(s.cx == 39u && s.cy == 0u && s.ch[1][0] == 'A', "VDU 8 at the top left scrolls down"); if (s.cx != 39u) printf("    got %u,%u [%c]\n", s.cx, s.cy, s.ch[1][0]);

   /* A redraw in MODE 7 sends line attributes for the 25 rows only. */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x07", out);
   {
      uint8_t d[TND_COPY_MAX];
      bool clean = true;
      tnd_out_redraw(&s, d);
      for (unsigned y = 25u; y < TND_ROWS_MAX; y++)
         if (s.lattr[y] != 0u) clean = false;
      CHECK(clean, "a MODE 7 redraw leaves no line attribute owed below row 25");
   }

   /* A Beeb reset ends a screen read half sent. */
   tnd_out_reset(&s, true);
   tnd_out_scan_merge(&s);
   FEED_OUT(&s, "\x1a\x0c", out);
   tnd_out_beeb_reset(&s);
   CHECK(!s.smerge, "a reset ends a screen read");
}

static void test_output_vdu21(void)
{
   tnd_out_t s; tnd_out_reset(&s, true);
   static const char text[] = "a\x15" "hidden\x1f\x01\x02\x0c" "more\x06" "b";
   const char *r = render(text, sizeof text - 1, true, &s);
   CHECK(strcmp(r, "ab") == 0, "VDU 21 hides everything, including codes, until VDU 6"); if (strcmp(r, "ab")) printf("    got [%s]\n", r);
   r = render("c", 1, true, &s);
   CHECK(strcmp(r, "c") == 0, "and output resumes");
}

/* MODE 7 once the cursor is known: every cell drawn as the SAA5050 draws it. */
static void ttx_feed(tnd_out_t *s, bool utf8, const char *data, size_t len, char *out, size_t cap)
{
   tnd_out_reset(s, utf8);
   feed_out(s, "\x16\x07", 2, out, cap);
   feed_out(s, data, len, out, cap);
}
#define TTX(lit) ttx_feed(&s, true, lit, sizeof(lit) - 1, out, sizeof out)
#define TTX_ASCII(lit) ttx_feed(&s, false, lit, sizeof(lit) - 1, out, sizeof out)

static void test_output_teletext_chip(void)
{
   tnd_out_t s;
   char out[4096];
   uint8_t d[TND_REPAINT_MAX];
   size_t n;

   TTX("\x97\xff");
   CHECK(strstr(out, "\xe2\x96\x88") != NULL, "MODE 7 graphics: &FF (all six cells) is a full block"); if (!strstr(out, "\xe2\x96\x88")) printf("    got [%s]\n", out);
   TTX("\x94" "5");
   CHECK(strstr(out, "\x1b[0;34m\xe2\x96\x8c") != NULL, "graphics blue '5' (the left column) is a blue left half block"); if (!strstr(out, "\x1b[0;34m\xe2\x96\x8c")) printf("    got [%s]\n", out);
   TTX("\x97" "j");
   CHECK(strstr(out, "\xe2\x96\x90") != NULL, "'j' (the right column) is the right half block");
   TTX("\x97!");
   CHECK(strstr(out, "\xf0\x9f\xac\x80") != NULL, "'!' (the top left cell) is the first sextant, U+1FB00"); if (!strstr(out, "\xf0\x9f\xac\x80")) printf("    got [%s]\n", out);
   TTX("\x97" "A");
   CHECK(out[strlen(out) - 1] == 'A', "capitals blast through in graphics");

   TTX("`#_{}~[]^\\|");
   CHECK(strstr(out, "\xc2\xa3#\xe2\x80\x95\xc2\xbc\xc2\xbe\xc3\xb7\xe2\x86\x90\xe2\x86\x92\xe2\x86\x91\xc2\xbd\xe2\x80\x96") != NULL,
         "the SAA5050's own characters: pound # dash 1/4 3/4 divide left right up 1/2 double bar"); if (!strstr(out, "\xc2\xa3#")) printf("    got [%s]\n", out);

   TTX("\x81\x9d\x87X");
   CHECK(strstr(out, "\x1b[0;37;41m") != NULL && out[strlen(out) - 1] == 'X', "new background: white X on red (the row after the code is red)"); if (!strstr(out, "\x1b[0;37;41m")) printf("    got [%s]\n", out);
   TTX("\x88X");
   CHECK(strstr(out, "\x1b[0;37;5mX") != NULL, "flash is the terminal's blink"); if (!strstr(out, "\x1b[0;37;5mX")) printf("    got [%s]\n", out);
   TTX("\x98X");
   CHECK(strchr(out, 'X') == NULL, "conceal hides the text");

   TTX(" ABC\x1f\x00\x00\x82");
   CHECK(strstr(out, "\x1b[0;32mABC") != NULL, "a colour code put in front of text colours the text after it"); if (!strstr(out, "\x1b[0;32mABC")) printf("    got [%s]\n", out);
   CHECK(strstr(out, "\r\x1b[1C") != NULL, "... and the cursor goes back to the cell after the code");

   TTX("\x97\x9e\xff\x93X");
   {
      unsigned blocks = 0;
      for (const char *p = out; (p = strstr(p, "\xe2\x96\x88")) != NULL; p += 3)
         blocks++;
      CHECK(blocks == 2, "hold graphics: the control code after a mosaic shows the mosaic again (%u blocks)", blocks);
   }

   TTX("\x8d" "AB");
   CHECK(strstr(out, "\x1b#3\x1b[1;1H AB") != NULL, "the double height code's own cell stays a space: the line attribute goes first"); if (!strstr(out, "\x1b#3\x1b[1;1H AB")) printf("    got [%s]\n", out);
   TTX("\x8d" "AB\r\n\x8d" "AB");
   CHECK(strstr(out, "\x1b[1;1H\x1b#3") != NULL && strstr(out, "\x1b[2;1H\x1b#4") != NULL,
         "double height: the first row is the top half, the next the bottom"); if (!strstr(out, "\x1b#4")) printf("    got [%s]\n", out);
   feed_out(&s, "\x0c", 1, out, sizeof out);
   CHECK(strstr(out, "\x1b#5") != NULL && strstr(out, "\x1b[2J") != NULL, "a CLS puts the rows back to single height");
   TTX("\x8d" "AB\x1f\x00\x00 ");
   CHECK(strstr(out, "\x1b#5") != NULL, "a double height code overwritten: the row goes back to single height"); if (!strstr(out, "\x1b#5")) printf("    got [%s]\n", out);

   TTX("AB\x7f");
   CHECK(strstr(out, "\b \r\x1b[1C") != NULL, "DELETE draws the cell as a space and the cursor stays on it"); if (!strstr(out, "\b \r\x1b[1C")) printf("    got [%s]\n", out);

   TTX("\x81R\x8dZ");
   n = tnd_out_repaint(&s, 0u, d);
   d[n] = 0;
   CHECK(strstr((char *)d, "\x1b#3") != NULL && strstr((char *)d, "\x1b[0;31mR") != NULL,
         "a repainted MODE 7 row has its line attribute and colours"); if (!strstr((char *)d, "\x1b[0;31mR")) printf("    got [%s]\n", d);

   TTX_ASCII("`");
   CHECK(strchr(out, '\xa3') != NULL, "no UTF-8: the pound is Latin-1");
   TTX_ASCII("\x97!");
   CHECK(out[strlen(out) - 1] == '#', "no UTF-8: a mosaic is a #");

   /* Anything at all, in MODE 7 with the cursor known: the output bound holds. */
   tnd_out_reset(&s, true);
   feed_out(&s, "\x16\x07", 2, out, sizeof out);
   {
      uint32_t x = 12345u;
      size_t   worst = 0u;
      for (unsigned i = 0; i < 200000u; i++) {
         uint8_t b[TND_OUT_MAX];
         x = x * 1103515245u + 12345u;
         uint8_t c = (uint8_t)(x >> 16);
         if (c == 22u || c == 12u) c = 0x8D;      /* fewer clears: the rows fill up */
         n = tnd_out_byte(&s, c, b);
         if (n > worst) worst = n;
         if (n > TND_OUT_MAX) break;
         if ((i & 4095u) == 0u) {
            uint8_t r[TND_REPAINT_MAX];
            for (unsigned y = 0; y <= s.rows; y++)
               if (tnd_out_repaint(&s, y, r) > TND_REPAINT_MAX) worst = 99999u;
         }
      }
      CHECK(worst <= TND_OUT_MAX, "MODE 7 noise: no byte's output exceeds TND_OUT_MAX (worst %u)", (unsigned)worst);
   }
}

/* Graphics in xterm's Tektronix 4014 window, once the terminal has said it is xterm. */
static void test_tek(void)
{
   tnd_out_t s;
   char out[4096];
   tnd_in_t in;
   uint8_t keys[TND_IN_KEYS], rep[TND_IN_REPLY];
   size_t replen, nk = 0;

   /* MOVE 0,0 : DRAW 1279,1023 : then text */
   static const char draw[] = "\x16\x04" "\x19\x04\x00\x00\x00\x00" "\x19\x05\xff\x04\xff\x03" "A";
   tnd_out_reset(&s, true);
   feed_out(&s, draw, sizeof draw - 1, out, sizeof out);
   CHECK(strstr(out, "\x1b[?38h") == NULL && strchr(out, 0x1d) == NULL, "graphics are swallowed when the terminal is not xterm");
   tnd_out_reset(&s, true);
   tnd_out_tek(&s, true);
   feed_out(&s, draw, sizeof draw - 1, out, sizeof out);
   CHECK(strstr(out, "\x1b[?38h\x1d" "\x20\x60\x20\x40" "\x38\x6b\x3f\x5f") != NULL,
         "MOVE 0,0 DRAW 1279,1023: the Tek window, one vector corner to corner (0,0 to 1023,779)");
   if (!strstr(out, "\x1b[?38h\x1d")) printf("    got [%s]\n", out);
   CHECK(strstr(out, "\x1b\x03" "A") != NULL, "text afterwards goes back to the text screen");
   CHECK(strstr(out, "\x3f\x5f" "\x1f\x1b\x03") != NULL,
         "... with US before ESC ETX, or xterm drops the last vector (T-19, seen in a real xterm)");

   /* PLOT 21 (dotted): the line in the tube's dotted style, and solid again after */
   static const char dot[] = "\x16\x04" "\x19\x04\x00\x00\x00\x00" "\x19\x15\xff\x04\x00\x00";
   tnd_out_reset(&s, true);
   tnd_out_tek(&s, true);
   feed_out(&s, dot, sizeof dot - 1, out, sizeof out);
   CHECK(strstr(out, "\x1b[?38h\x1b" "a\x1d" "\x20\x60\x20\x40" "\x20\x60\x3f\x5f" "\x1b`") != NULL,
         "PLOT 21: ESC a (dotted), the vector, ESC ` (solid)");

   /* relative moves follow the graphics cursor; the origin applies to absolute ones */
   static const char rel[] = "\x16\x04" "\x1d\x80\x02\x00\x02" "\x19\x04\x00\x00\x00\x00" "\x19\x01\x00\x00\x80\x00";
   tnd_out_reset(&s, true);
   tnd_out_tek(&s, true);
   feed_out(&s, rel, sizeof rel - 1, out, sizeof out);
   CHECK(s.gx == 640 && s.gy == 640, "VDU 29 sets the origin, PLOT 1 is relative to the cursor (%d,%d)", s.gx, s.gy);

   /* CLG clears the Tek page; a background-colour draw only moves */
   tnd_out_reset(&s, true);
   tnd_out_tek(&s, true);
   feed_out(&s, "\x10", 1, out, sizeof out);
   CHECK(strstr(out, "\x1b[?38h\x1b\x0c") != NULL, "CLG clears the Tek page");
   feed_out(&s, "\x19\x07\x10\x00\x10\x00", 6, out, sizeof out);
   CHECK(strchr(out, 0x1d) == NULL && s.gx == 16, "PLOT 7 (background) moves without drawing");

   /* a MODE clears it too, and the text screen comes back */
   feed_out(&s, "\x16\x07", 2, out, sizeof out);
   CHECK(strstr(out, "\x1b\x0c") != NULL && strstr(out, "\x1b\x03") != NULL && strstr(out, "\x1b[2J") != NULL,
         "MODE clears the Tek page and goes back to the text screen");

   /* the terminal's answer to CSI > q */
   tnd_in_reset(&in);
   {
      static const char reply[] = "\x1bP>|XTerm(379)\x1b\\x";
      for (size_t i = 0; i < sizeof reply - 1; i++)
         nk += tnd_in_byte(&in, (uint8_t)reply[i], 0, keys, rep, &replen);
   }
   CHECK(in.xterm && nk == 1 && keys[0] == 'x', "XTVERSION's answer from xterm is taken, not typed, and what follows is a key");
   tnd_in_reset(&in);
   {
      static const char reply[] = "\x1bP>|WezTerm 2024\x1b\\";
      for (size_t i = 0; i < sizeof reply - 1; i++)
         tnd_in_byte(&in, (uint8_t)reply[i], 0, keys, rep, &replen);
   }
   CHECK(!in.xterm, "another terminal's answer: no Tek");
   tnd_in_reset(&in);
   nk = 0;
   {
      uint8_t all[8]; size_t na = 0;
      static const char altp[] = "\x1bPa";
      for (size_t i = 0; i < sizeof altp - 1; i++) {
         size_t k = tnd_in_byte(&in, (uint8_t)altp[i], 0, keys, rep, &replen);
         for (size_t j = 0; j < k && na < sizeof all; j++) all[na++] = keys[j];
      }
      CHECK(na == 3 && all[0] == TND_KEY_ESCAPE && all[1] == 'P' && all[2] == 'a', "Alt-P is ESCAPE and P, not a DCS (%u keys)", (unsigned)na);
   }
}

static void test_output_teletext(void)
{
   const char *r;

   r = RENDER("\x81Hi");
   CHECK(strcmp(r, "\x1b[31m Hi") == 0, "0x81 (alpha red) is a red space"); if (strcmp(r, "\x1b[31m Hi")) printf("    got [%s]\n", r);
   r = RENDER("\x97X");
   CHECK(strcmp(r, "\x1b[37m X") == 0, "0x97 (graphics white) is a white space");
   r = RENDER("\x88\x89\x8c\x8d\x9f" "x");
   CHECK(strcmp(r, "     x") == 0, "other teletext controls are plain spaces");
   r = RENDER("\x82ok\r\n");
   CHECK(strcmp(r, "\x1b[32m ok\x1b[0m\r\n") == 0, "colour is reset at the end of the line"); if (strcmp(r, "\x1b[32m ok\x1b[0m\r\n")) printf("    got [%s]\n", r);
   r = RENDER("\x16\x01\x11\x01x\r\ny");
   CHECK(strcmp(r, "\x1b[2J\x1b[H\x1b[31mx\r\ny") == 0, "outside MODE 7 COLOUR carries over to the next line"); if (strcmp(r, "\x1b[2J\x1b[H\x1b[31mx\r\ny")) printf("    got [%s]\n", r);
   r = RENDER("\x11\x01x\x16\x01");
   CHECK(strcmp(r, "\x1b[31mx\x1b[0m\x1b[2J\x1b[H") == 0, "a MODE puts the colours back"); if (strcmp(r, "\x1b[31mx\x1b[0m\x1b[2J\x1b[H")) printf("    got [%s]\n", r);
   r = RENDER("\xc1\xc2");
   CHECK(strcmp(r, "AB") == 0, "from 0xA0 the chip ignores bit 7: 0xC1 shows A");
   r = RENDER("plain\r\n");
   CHECK(strcmp(r, "plain\r\n") == 0, "no colour used, no reset sent");
}

static void test_output_robustness(void)
{
   tnd_out_t s; tnd_out_reset(&s, true);
   unsigned seed = 777;
   for (unsigned i = 0; i < 200000; i++) {
      uint8_t d[TND_OUT_MAX];
      seed = seed * 1103515245u + 12345u;
      uint8_t c = (uint8_t)(seed >> 16);
      size_t n = tnd_out_byte(&s, c, d);
      CHECK(n <= TND_OUT_MAX, "output bound at %u", i);
      if (n > TND_OUT_MAX) return;
      for (size_t j = 0; j < n; j++)
         if (d[j] == 0xFF) { CHECK(0, "0xFF in the output at %u", i); return; }
   }
   CHECK(true, "fuzz ran");
}

static void test_stalled(void)
{
   CHECK(!tnd_stalled(false, 100000000u, 0u, 10000000u), "an idle session is never stalled");
   CHECK(!tnd_stalled(true, 5000000u, 0u, 10000000u), "waiting, but not for long enough");
   CHECK(tnd_stalled(true, 10000000u, 0u, 10000000u), "waiting for the limit: stalled");
   CHECK(tnd_stalled(true, 50000000u, 1000u, 10000000u), "long overdue");
   CHECK(tnd_stalled(true, 5000u, 0xFFFFF000u, 9000u), "the counter wrapped, 9.1 ms later");
   CHECK(!tnd_stalled(true, 4000u, 0xFFFFF000u, 9000u), "wrapped, and not yet");
}

/* Teletype mode joins with the Beeb's current line (its prompt) from the model. */
static void test_cursor_line(void)
{
   tnd_out_t s;
   char out[4096], cl[TND_COLS_MAX + 1];

   tnd_out_reset(&s, true);
   CHECK(tnd_out_cursor_line(&s, cl, sizeof cl) == 0u, "nothing known: no line");
   FEED_OUT(&s, "\x16\x07" "BASIC\r\n\r\n>", out);
   CHECK(tnd_out_cursor_line(&s, cl, sizeof cl) == 1u && strcmp(cl, ">") == 0, "MODE 7, the prompt: >");
   FEED_OUT(&s, "\r\nName? ", out);
   CHECK(tnd_out_cursor_line(&s, cl, sizeof cl) == 6u && strcmp(cl, "Name? ") == 0, "an INPUT prompt, up to the cursor");
   FEED_OUT(&s, "\r\n", out);
   CHECK(tnd_out_cursor_line(&s, cl, sizeof cl) == 0u, "the start of a line: nothing to show");
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x01" "\x81" ">", out);
   CHECK(tnd_out_cursor_line(&s, cl, sizeof cl) == 2u && cl[0] == ' ' && cl[1] == '>', "MODE 1: a byte above 127 shows as a space");
}

/* A new session's screen read, compared with the model (tnd_out_scan_merge). */
static void test_scan_merge(void)
{
   tnd_out_t s;
   char out[4096];
   uint8_t d[TND_REPAINT_MAX];
   size_t n;
   uint8_t at_h;

   /* The read agrees: nothing is drawn, the colours stay. */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x01" "HI\r\n\x11\x01YO", out);      /* MODE 1: HI, then YO in red */
   at_h = s.at[0][0];
   tnd_out_scan_merge(&s);
   n = FEED_OUT(&s, "\x1a\x0c" "\x1f\x00\x00" "H" "\x1f\x01\x00" "I" "\x1f\x00\x01" "Y" "\x1f\x01\x01" "O"
                    "\x1c\x00\x1f\x27\x00" "\x1f\x02\x01", out);
   CHECK(n == 0u, "a read that agrees with the model draws nothing"); if (n) printf("    got %u bytes\n", (unsigned)n);
   CHECK(!s.smerge && !s.rescan && !s.dirty, "... and is finished, with nothing to draw again");
   CHECK(s.ch[1][0] == 'Y' && (s.at[1][0] & 0x40u) == 0u, "... and the red YO keeps its colour");

   /* The read differs (a program changed the screen where the Pi could not see). */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x01" "HI\r\n\x11\x01YO", out);
   at_h = s.at[0][0];
   tnd_out_scan_merge(&s);
   n = FEED_OUT(&s, "\x1a\x0c" "\x1f\x00\x00" "H" "\x1f\x01\x00" "I" "\x1f\x05\x03" "X"
                    "\x1c\x00\x1f\x27\x00" "\x1f\x06\x03", out);
   CHECK(strstr(out, "\x1b[2J") != NULL, "a read that differs clears the terminal");
   CHECK(s.ch[1][0] == ' ' && s.ch[1][1] == ' ' && s.ch[3][5] == 'X', "... the model takes the Beeb's characters");
   CHECK(s.ch[0][0] == 'H' && s.at[0][0] == at_h, "... the cells that agree keep what they had");
   CHECK(s.cx == 6u && s.cy == 3u, "... the cursor is the Beeb's");
   CHECK(s.dirty, "... and the screen is to be drawn again from the model");
   {
      char all[8192] = "";
      size_t k = 0u;
      while ((n = tnd_out_dirty_row(&s, d)) != 0u && k + n < sizeof all) {
         memcpy(all + k, d, n);
         k += n;
      }
      all[k] = 0;
      CHECK(strstr(all, "HI") != NULL && strstr(all, "X") != NULL && strstr(all, "YO") == NULL,
            "... which shows HI and X and no longer YO");
   }

   /* An inverse heading (COLOUR 0 on COLOUR 129): OSBYTE 135 cannot read text on a background, so
      the read has blanks there; the model's heading stands (such headings went blank). */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x00" "\x1f\x03\x02" "\x11\x00\x11\x81" "HEAD" "\x11\x01\x11\x80" "\x1f\x00\x05", out);
   tnd_out_scan_merge(&s);
   n = FEED_OUT(&s, "\x1a\x0c" "\x1f\x05\x08" "X" "\x1c\x00\x1f\x4f\x00" "\x1f\x06\x08", out);
   CHECK(s.ch[2][3] == 'H' && s.ch[2][6] == 'D' && (s.at[2][3] & 0x40u) == 0u,
         "a heading on a background, read as blanks, stays in the model with its colours");
   CHECK(s.ch[8][5] == 'X', "... while what the read found elsewhere is taken");

   /* Not a read at all: the program printed first. */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x01" "HI", out);
   tnd_out_scan_merge(&s);
   n = FEED_OUT(&s, "A", out);
   CHECK(n >= 1u && strchr(out, 'A') != NULL && !s.smerge && !s.rescan, "output that is not a read is shown as usual, and no read is asked for again");

   /* A read cut short: an ordinary one is asked for. */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x01" "HI", out);
   tnd_out_scan_merge(&s);
   FEED_OUT(&s, "\x1a\x0c\x1f\x05" "Z", out);                 /* a row number of 90 */
   CHECK(!s.smerge && s.rescan, "a read cut short asks for an ordinary read");

   /* MODE 7: the read gives each character's code as printed (OSBYTE 135); # _ and the pound are
      stored swapped and from &A0 without bit 7, as printing them stores them.  Connecting showed
      a REM with e-acute (233) as a space where the Beeb shows i. */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x07" "#_`\xe9" "A", out);
   tnd_out_scan_merge(&s);
   n = FEED_OUT(&s, "\x1a\x0c" "\x1f\x00\x00" "#" "\x1f\x01\x00" "_" "\x1f\x02\x00" "`" "\x1f\x03\x00" "\xe9"
                    "\x1f\x04\x00" "A" "\x1c\x00\x18\x27\x00" "\x1f\x05\x00", out);
   CHECK(n == 0u, "MODE 7: a read of # _ pound and &E9 agrees with the model: nothing drawn");
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x07", out);
   tnd_out_scan_merge(&s);
   n = FEED_OUT(&s, "\x1a\x0c" "\x1f\x00\x00" "\xe9" "\x1f\x01\x00" "#" "\x1c\x00\x18\x27\x00" "\x1f\x02\x00", out);
   CHECK(s.ch[0][0] == 'i' && s.ch[0][1] == 0x5Fu, "MODE 7: a read's &E9 is stored as i, its # as printing stores it");

   /* MODE 7: colour codes compared as they are stored. */
   tnd_out_reset(&s, true);
   FEED_OUT(&s, "\x16\x07" "\x81" "RED", out);
   tnd_out_scan_merge(&s);
   n = FEED_OUT(&s, "\x1a\x0c" "\x1f\x00\x00" "\x81" "\x1f\x01\x00" "R" "\x1f\x02\x00" "E" "\x1f\x03\x00" "D"
                    "\x1c\x00\x18\x27\x00" "\x1f\x04\x00", out);
   CHECK(n == 0u, "MODE 7: a read with the same colour code and text draws nothing"); if (n) printf("    got %u bytes\n", (unsigned)n);
}

/* The boot banners (tnd_banner_byte). */
static bool banner_feed(tnd_banner_t *b, const char *data, size_t len, bool open)
{
   bool prompt = false;
   for (size_t i = 0; i < len; i++)
      prompt |= tnd_banner_byte(b, (uint8_t)data[i], open);
   return prompt;
}
#define BANNER(b, lit, open) banner_feed(b, lit, sizeof(lit) - 1, open)

static void test_banner(void)
{
   tnd_banner_t b;
   bool prompt;

   tnd_banner_reset(&b);
   prompt = BANNER(&b, "BBC Computer 32K\r\n\r\nAcorn DFS\r\n\r\nBASIC\r\n\r\n>", true);
   b.text[b.len] = 0;
   CHECK(strcmp(b.text, "BBC Computer 32K\nAcorn DFS\nBASIC\n") == 0, "the start-up's banners are kept, a line each"); if (strcmp(b.text, "BBC Computer 32K\nAcorn DFS\nBASIC\n")) printf("    got [%s]\n", b.text);
   CHECK(prompt, "... and the prompt ends the start-up");

   tnd_banner_reset(&b);
   BANNER(&b, "LATER TEXT\r\n", false);
   CHECK(b.len == 0u, "text after the start-up is not a banner");

   tnd_banner_reset(&b);
   BANNER(&b, "\x15" "HIDDEN BANNER\r\n" "\x06", true);
   b.text[b.len] = 0;
   CHECK(strcmp(b.text, "HIDDEN BANNER\n") == 0, "a VDU 21 ... VDU 6 block just after a start is kept");
   tnd_banner_reset(&b);
   BANNER(&b, "\x15" "Acorn File Server Level 3\r\nTesting Memory\r\n" "\x06", false);
   CHECK(b.len == 0u, "a VDU 21 block later (a program's, e.g. a file server starting) is not a banner");

   /* A screen read after the start-up: VDU 31,x,y,c for cells in row 21 and column 6.  The 21 and 6
      are parameters, not VDU 21 and VDU 6: no block, nothing kept. */
   tnd_banner_reset(&b);
   BANNER(&b, "\x1a\x0c" "\x1f\x05\x15" "A" "\x1f\x06\x02" "B" "\x1f\x00\x15" "C" "TEXT\r\n" "\x1f\x06\x06" "D"
              "\x1c\x00\x1f\x27\x00" "\x1f\x00\x15" "MORE\r\n", false);
   CHECK(b.len == 0u && !b.blk, "a screen read's row 21 and column 6 are not a banner block"); if (b.len) { b.text[b.len] = 0; printf("    got [%s]\n", b.text); }

   /* VDU 23 and VDU 19 carry 21s and 6s in their parameters too. */
   tnd_banner_reset(&b);
   BANNER(&b, "\x17\x80\x15\x06\x15\x06\x00\x00\x00\x00" "\x13\x01\x15\x06\x00\x00" "AFTER\r\n", false);
   CHECK(b.len == 0u && !b.blk, "VDU 23 and VDU 19 parameters do not open a banner block");

   /* ... and a real block after them, while the banners are read, still counts. */
   BANNER(&b, "\x15" "KEPT\r\n" "\x06", true);
   b.text[b.len] = 0;
   CHECK(strcmp(b.text, "KEPT\n") == 0, "a real VDU 21 block after them is still kept");
}

/* Teletype mode (raw): the client asks with telnet BINARY; then every byte is a key as it is. */
static void test_teletype(void)
{
   tnd_in_t s;

   tnd_in_reset(&s); in_reset();
   FEED(&s, "\xff\xfd\x00");                    /* DO BINARY */
   CHECK(s.bin_out && nreply == 3 && memcmp(reply, "\xff\xfb\x00", 3) == 0, "teletype: DO BINARY is agreed (WILL BINARY)");
   in_reset();
   FEED(&s, "\xff\xfb\x00");                    /* WILL BINARY */
   CHECK(s.bin_in && nreply == 3 && memcmp(reply, "\xff\xfd\x00", 3) == 0, "teletype: WILL BINARY is agreed (DO BINARY)");
   in_reset();
   FEED(&s, "\xff\xfb\x00");
   CHECK(nreply == 0, "teletype: asked again, no answer (no negotiation loop)");

   tnd_in_reset(&s); in_reset();
   s.raw = true; s.lit_ok = true;
   FEED(&s, "A\x1b[A\x01");                    /* a cursor-key sequence is just bytes: ESC, [, A */
   CHECK(KEYS("A\x00[A\x01"), "teletype: bytes are keys as they are, ESC is the ESCAPE key"); if (!KEYS("A\x00[A\x01")) dump();
   in_reset();
   FEED(&s, "\xe9\xff\xff");                    /* e-acute (Latin-1), and IAC IAC: the byte 255 */
   CHECK(KEYS("\xfa\xe9\xfa\xff"), "teletype: bytes above 127 go behind TND_KEY_LIT, IAC IAC is 255"); if (!KEYS("\xfa\xe9\xfa\xff")) dump();
   in_reset();
   {
      static const uint8_t nul[1] = { 0 };
      feed(&s, nul, 1);
   }
   CHECK(nkeys == 2 && keys[0] == 0xFA && keys[1] == 0, "teletype: NUL is a byte (behind LIT), not ESCAPE");
   in_reset();
   FEED(&s, "\r\n\r");
   {
      static const uint8_t z[1] = { 0 };
      feed(&s, z, 1);
   }
   CHECK(KEYS("\r\r"), "teletype: telnet's CR LF and CR NUL are one RETURN each"); if (!KEYS("\r\r")) dump();
   in_reset();
   s.bin_in = true;
   FEED(&s, "\r\n");
   CHECK(KEYS("\r\n"), "teletype, binary: CR and LF are both bytes"); if (!KEYS("\r\n")) dump();
   in_reset();
   s.lit_ok = false;
   FEED(&s, "x\xe9y");
   CHECK(KEYS("xy"), "teletype: without the ROM's LITKEY a byte above 127 is dropped"); if (!KEYS("xy")) dump();

   tnd_in_reset(&s); in_reset();                /* a raw TCP port: not even IAC */
   s.raw = true; s.notelnet = true; s.lit_ok = true;
   FEED(&s, "\xff\xfd\x00\r\n");
   CHECK(nreply == 0 && nkeys == 8, "teletype, raw port: IAC is a byte like any other, no answer (%u keys)", (unsigned)nkeys);
   CHECK(keys[0] == 0xFA && keys[1] == 0xFF && keys[2] == 0xFA && keys[3] == 0xFD && keys[4] == 0xFA && keys[5] == 0x00
         && keys[6] == '\r' && keys[7] == '\n', "teletype, raw port: ... behind LIT, NUL too, CR and LF as they are");
   if (nkeys != 8) dump();
}

int main(void)
{
   test_plain_keys();
   test_pound_and_high_bytes();
   test_cursor_and_function_keys();
   test_escape_key();
   test_negotiation();
   test_break_key();
   test_blank();
   test_narrow_window();
   test_input_robustness();
   test_input_key_bound();
   test_login();
   test_teletype();
   test_output_text();
   test_output_params();
   test_output_vdu21();
   test_output_teletext();
   test_review_fixes_full();
   test_review_fixes_2();
   test_output_teletext_chip();
   test_tek();
   test_output_robustness();
   test_model_wrap_scroll();
   test_copy_cursor();
   test_repaint();
   test_scan_merge();
   test_cursor_line();
   test_banner();
   test_stalled();

   printf("%d checks, %d failed\n", checks, failures);
   return failures ? 1 : 0;
}
