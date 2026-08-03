/* interop.c - the REAL AUN engine against a REAL PiEconetBridge.
 *
 * The other test layers validate the engine against a peer we wrote
 * ourselves, so they can only prove we are self-consistent: if our
 * reading of the wire convention is wrong, the unit tests, the fuzzers
 * and the lockstep peer are all wrong in exactly the same way. This
 * layer removes that blind spot by talking to Chris Royle's
 * PiEconetBridge over loopback and letting IT judge our datagrams.
 *
 * What is checked here:
 *   - our own emitted bytes, asserted in-process (a regression in the
 *     encoding fails immediately, with the offending datagram printed);
 *   - the bridge's interpretation, asserted by run.sh, which greps the
 *     bridge's debug log for the reassembled NOTIFY string.
 *
 * Known quirk, deliberately NOT treated as a failure: the bridge does
 * not acknowledge port-0 traffic to a local emulator station - see its
 * own TODO at econet-hpbridge.c ("Received traffic to port &00 which is
 * not listening"). It decodes and acts on the NOTIFY, then stays quiet,
 * so our transmit legitimately ends NOT_LISTENING. A real Beeb behind
 * the bridge completes the four-way on the wire and the bridge relays a
 * proper ACK.
 *
 * We are station 2.42 at 127.0.0.1:40042; the bridge's local emulator
 * is 1.254, listening on 127.0.0.1:32768 (see peb-test.json).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <poll.h>
#include "aun.h"

#define BRIDGE_IP   "127.0.0.1"
#define BRIDGE_PORT 32768
#define OUR_PORT    40042

static int sock;
static int failures;

/* the last datagram we put on the wire, for assertions */
static uint8_t  last[2048];
static size_t   last_len;

static void check(int cond, const char *what)
{
   printf("  %s: %s\n", cond ? "ok  " : "FAIL", what);
   if (!cond)
      failures++;
}

static bool tx(void *u, uint32_t ip_be, uint16_t port,
               const uint8_t *b, size_t n)
{
   (void)u;
   struct sockaddr_in d = {0};
   d.sin_family      = AF_INET;
   d.sin_addr.s_addr = ip_be;
   d.sin_port        = htons(port);
   memcpy(last, b, n < sizeof last ? n : sizeof last);
   last_len = n;
   printf("    -> type=%u port=&%02X ctrl=&%02X len=%zu [", b[0], b[1],
          b[2], n - AUN_HDR_SIZE);
   for (size_t i = AUN_HDR_SIZE; i < n && i < AUN_HDR_SIZE + 20; i++)
      printf("%02X ", b[i]);
   printf("]\n");
   return sendto(sock, b, n, 0, (struct sockaddr *)&d, sizeof d) == (ssize_t)n;
}

static uint32_t now_ms(void *u)
{
   (void)u;
   struct timeval tv;
   gettimeofday(&tv, NULL);
   return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static aun_engine_t e;

/* run the engine for ms milliseconds, feeding it anything that arrives */
static void pump(uint32_t ms)
{
   uint32_t end = now_ms(NULL) + ms;
   while ((int32_t)(now_ms(NULL) - end) < 0) {
      struct pollfd p = { sock, POLLIN, 0 };
      if (poll(&p, 1, 10) > 0) {
         uint8_t buf[2048];
         struct sockaddr_in src;
         socklen_t sl = sizeof src;
         ssize_t n = recvfrom(sock, buf, sizeof buf, 0,
                              (struct sockaddr *)&src, &sl);
         if (n >= (ssize_t)AUN_HDR_SIZE) {
            static const char *tn[] = { "?", "BCAST", "DATA", "ACK",
                                        "NAK", "IMM", "IMMREP" };
            printf("    <- %s port=&%02X ctrl=&%02X len=%zd\n",
                   buf[0] <= 6 ? tn[buf[0]] : "?", buf[1], buf[2],
                   n - AUN_HDR_SIZE);
            aun_udp_input(&e, src.sin_addr.s_addr, ntohs(src.sin_port),
                          buf, (uint32_t)n);
         }
      }
      aun_poll(&e);
   }
}

/* One transmit may be in flight at a time, so each step starts fresh. */
static void reset_engine(void)
{
   static const aun_transport_t T = { tx, now_ms, NULL };
   aun_init(&e, &T, 42, 2);                       /* we are 2.42 */
   aun_set_host_imm(&e, true);
   aun_map_add(&e, 1, 254, inet_addr(BRIDGE_IP), BRIDGE_PORT);
   last_len = 0;
}

int main(void)
{
   sock = socket(AF_INET, SOCK_DGRAM, 0);
   if (sock < 0) { perror("socket"); return 2; }
   struct sockaddr_in me = {0};
   me.sin_family      = AF_INET;
   me.sin_addr.s_addr = inet_addr(BRIDGE_IP);
   me.sin_port        = htons(OUR_PORT);
   if (bind(sock, (struct sockaddr *)&me, sizeof me) != 0) {
      perror("bind"); return 2;
   }

   printf("== 1: machine peek &88 stays a TWO-WAY immediate (type 5) ==\n");
   /* The control case. Peek/Halt/Continue/MachinePeek must NOT have been
    * swept into the DATA-to-port-0 encoding along with ctrl 2-5. */
   reset_engine();
   {
      uint8_t reply[8];
      aun_immediate(&e, 1, 254, 0x88, NULL, 0, reply, sizeof reply);
      check(last_len >= AUN_HDR_SIZE && last[0] == AUN_TYPE_IMMEDIATE,
            "machine peek goes out as AUN type 5 (IMMEDIATE)");
      check(last[2] == AUN_CTRL_MACHINE_PEEK, "ctrl &88 -> wire &08");
      pump(300);
   }

   printf("\n== 2: *NOTIFY \"PI\" - OSProc &85 as DATA to port 0 ==\n");
   /* The fix itself. One 4-way immediate per character: the ROM hands us
    * [4 args][data] and we re-type it as DATA to port 0. */
   reset_engine();
   {
      const char *msg = "PI";
      int sent_ok = 1;
      for (const char *c = msg; *c; c++) {
         uint8_t pl[5] = { 0, 0, (uint8_t)*c, 0x0F, (uint8_t)*c };
         if (aun_immediate(&e, 1, 254, 0x85, pl, sizeof pl, NULL, 0) != AUN_OK)
            sent_ok = 0;
         if (last[0] != AUN_TYPE_DATA || last[1] != 0x00 || last[2] != 0x05)
            sent_ok = 0;
         if (last_len != AUN_HDR_SIZE + 5 ||
             memcmp(&last[AUN_HDR_SIZE], pl, 5) != 0)
            sent_ok = 0;
         pump(600);            /* the bridge never ACKs port 0 - see above */
         reset_engine();
      }
      check(sent_ok, "each char is DATA, port 0, ctrl &05, payload intact");
      printf("  (run.sh checks the bridge reassembled \"%s\")\n", msg);
   }

   printf("\n== 3: remote POKE &82 splices the byte count ==\n");
   /* The ROM supplies [start][data]; the wire wants [start][count][data],
    * the count being what the real ROM's calc_peek_poke_size emits. */
   reset_engine();
   {
      uint8_t pk[7] = { 0x00, 0x7C, 0x00, 0x00, 'P', 'i', '!' };
      aun_immediate(&e, 1, 254, 0x82, pk, sizeof pk, NULL, 0);
      check(last[0] == AUN_TYPE_DATA && last[1] == 0x00 && last[2] == 0x02,
            "POKE goes out as DATA, port 0, ctrl &02");
      check(last_len == AUN_HDR_SIZE + 11,
            "payload grew by the 4 spliced count bytes");
      check(memcmp(&last[AUN_HDR_SIZE], pk, 4) == 0,
            "start address preserved");
      static const uint8_t cnt[4] = { 3, 0, 0, 0 };
      check(memcmp(&last[AUN_HDR_SIZE + 4], cnt, 4) == 0,
            "scout extras 4-7 = BYTE COUNT (3), not an end address");
      check(memcmp(&last[AUN_HDR_SIZE + 8], &pk[4], 3) == 0,
            "data phase follows the extras");
      pump(300);
   }

   printf("\n== 4: inbound machine peek is auto-answered ==\n");
   /* The engine answers a machine peek itself, without troubling the host. */
   reset_engine();
   {
      uint8_t imm[AUN_HDR_SIZE] = { AUN_TYPE_IMMEDIATE, 0,
                                    AUN_CTRL_MACHINE_PEEK, 0, 0x10, 0, 0, 0 };
      aun_udp_input(&e, inet_addr(BRIDGE_IP), BRIDGE_PORT, imm, sizeof imm);
      check(last[0] == AUN_TYPE_IMM_REPLY && last_len == AUN_HDR_SIZE + 4,
            "answered with a 4-byte IMM_REPLY");
   }

   printf("\n%s (%d failure%s)\n", failures ? "INTEROP FAILED" : "interop ok",
          failures, failures == 1 ? "" : "s");
   return failures ? 1 : 0;
}
