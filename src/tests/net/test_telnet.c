/* Host tests for the TELNET IAC filter (net_telnet.c) - option negotiation,
 * IAC-IAC unescaping, subnegotiation dropping, CR-NUL, respond-once, and
 * sequences split across calls. */

#include "net_telnet.h"
#include <stdio.h>
#include <string.h>

static int checks, failures;
#define CHECK(cond, msg) do { checks++; \
   if (cond) printf("  ok: %s\n", msg); \
   else { printf("  FAIL: %s\n", msg); failures++; } } while (0)

/* Run one buffer through a fresh context. */
static telnet_ctx_t C;
static uint8_t OUT[256], REP[64];
static size_t  OL, RL;
static void run(const void *in, size_t n) {
   telnet_filter(&C, (const uint8_t *)in, n, OUT, sizeof OUT, &OL, REP, sizeof REP, &RL);
}

int main(void)
{
   printf("== TELNET filter ==\n");

   /* plain text passes through untouched */
   telnet_reset(&C);
   run("hello world", 11);
   CHECK(OL == 11 && memcmp(OUT, "hello world", 11) == 0 && RL == 0, "plain text passes through");

   /* IAC IAC -> one literal 0xFF byte */
   telnet_reset(&C);
   { uint8_t in[] = { 'a', TN_IAC, TN_IAC, 'b' };
     run(in, sizeof in);
     CHECK(OL == 3 && OUT[0]=='a' && OUT[1]==0xFF && OUT[2]=='b' && RL==0, "IAC IAC -> literal 0xFF"); }

   /* IAC WILL ECHO -> reply IAC DO ECHO, nothing to output */
   telnet_reset(&C);
   { uint8_t in[] = { TN_IAC, TN_WILL, TN_OPT_ECHO };
     run(in, sizeof in);
     CHECK(OL == 0 && RL == 3 && REP[0]==TN_IAC && REP[1]==TN_DO && REP[2]==TN_OPT_ECHO,
           "WILL ECHO -> DO ECHO"); }

   /* IAC WILL SGA -> DO SGA */
   telnet_reset(&C);
   { uint8_t in[] = { TN_IAC, TN_WILL, TN_OPT_SGA };
     run(in, sizeof in);
     CHECK(RL == 3 && REP[1]==TN_DO && REP[2]==TN_OPT_SGA, "WILL SGA -> DO SGA"); }

   /* IAC WILL <other> -> DONT <other> (refuse) */
   telnet_reset(&C);
   { uint8_t in[] = { TN_IAC, TN_WILL, 99 };
     run(in, sizeof in);
     CHECK(RL == 3 && REP[1]==TN_DONT && REP[2]==99, "WILL <other> -> DONT"); }

   /* IAC DO <opt> -> WONT <opt> (we enable nothing) */
   telnet_reset(&C);
   { uint8_t in[] = { TN_IAC, TN_DO, 24 };
     run(in, sizeof in);
     CHECK(RL == 3 && REP[1]==TN_WONT && REP[2]==24, "DO <opt> -> WONT"); }

   /* IAC WONT / IAC DONT -> no reply */
   telnet_reset(&C);
   { uint8_t in[] = { TN_IAC, TN_WONT, 1, TN_IAC, TN_DONT, 3 };
     run(in, sizeof in);
     CHECK(RL == 0 && OL == 0, "WONT/DONT -> no reply"); }

   /* subnegotiation IAC SB ... IAC SE is dropped entirely */
   telnet_reset(&C);
   { uint8_t in[] = { 'x', TN_IAC, TN_SB, 24, 1, 'A','N','S','I', TN_IAC, TN_SE, 'y' };
     run(in, sizeof in);
     CHECK(OL == 2 && OUT[0]=='x' && OUT[1]=='y' && RL==0, "IAC SB..IAC SE dropped"); }

   /* CR NUL -> CR ; CR LF stays CR LF */
   telnet_reset(&C);
   { uint8_t in[] = { 'a', 0x0D, 0x00, 'b', 0x0D, 0x0A, 'c' };
     run(in, sizeof in);
     CHECK(OL == 6 && OUT[0]=='a' && OUT[1]==0x0D && OUT[2]=='b'
           && OUT[3]==0x0D && OUT[4]==0x0A && OUT[5]=='c', "CR NUL -> CR, CR LF kept"); }

   /* respond-once: WILL ECHO twice -> only one DO ECHO */
   telnet_reset(&C);
   { uint8_t in[] = { TN_IAC, TN_WILL, TN_OPT_ECHO, TN_IAC, TN_WILL, TN_OPT_ECHO };
     run(in, sizeof in);
     CHECK(RL == 3, "respond-once: WILL ECHO twice -> single reply"); }

   /* a sequence split across two calls carries state via ctx */
   telnet_reset(&C);
   { uint8_t a[] = { 'h','i', TN_IAC };            /* IAC at end of chunk 1 */
     uint8_t b[] = { TN_WILL, TN_OPT_ECHO, '!' };  /* completes in chunk 2 */
     run(a, sizeof a);
     CHECK(OL == 2 && RL == 0, "chunk1: 'hi', IAC pending");
     run(b, sizeof b);
     CHECK(OL == 1 && OUT[0]=='!' && RL==3 && REP[1]==TN_DO, "chunk2: completes WILL ECHO -> DO ECHO + '!'"); }

   /* CR at a chunk boundary then NUL -> single CR */
   telnet_reset(&C);
   { uint8_t a[] = { 'z', 0x0D }; uint8_t b[] = { 0x00, 'w' };
     run(a, sizeof a); CHECK(OL == 2 && OUT[1]==0x0D, "chunk1 ends with CR");
     run(b, sizeof b); CHECK(OL == 1 && OUT[0]=='w', "chunk2: NUL swallowed, then 'w'"); }

   /* --- outbound escaping --- */
   { uint8_t in[] = { 'a', 0xFF, 'b', 0xFF }; uint8_t out[16]; size_t cons, n;
     n = telnet_escape(in, sizeof in, out, sizeof out, &cons);
     CHECK(n == 6 && cons == 4 && out[0]=='a' && out[1]==0xFF && out[2]==0xFF
           && out[3]=='b' && out[4]==0xFF && out[5]==0xFF, "escape: 0xFF -> IAC IAC"); }
   { uint8_t in[] = "hi"; uint8_t out[8]; size_t cons, n;
     n = telnet_escape(in, 2, out, sizeof out, &cons);
     CHECK(n == 2 && cons == 2 && memcmp(out, "hi", 2) == 0, "escape: plain text unchanged"); }
   { uint8_t in[] = { 0xFF, 0xFF }; uint8_t out[3]; size_t cons, n;
     n = telnet_escape(in, sizeof in, out, sizeof out, &cons);
     CHECK(n == 2 && cons == 1, "escape: stops when the IAC pair won't fit (partial consume)"); }

   printf("\n%d checks, %d failures\n", checks, failures);
   if (failures) { printf("TELNET FILTER TESTS FAILED\n"); return 1; }
   printf("TELNET FILTER TESTS PASSED\n");
   return 0;
}
