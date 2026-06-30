/* Host sanity tests for aun_aun.c - stub transport, fake clock. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aun.h"

/* ---- stub transport ---- */
#define SENT_MAX 16
static struct { uint32_t ip; uint16_t port; uint8_t buf[1500]; size_t len; } sent[SENT_MAX];
static int sent_count;
static uint32_t fake_ms;
static bool send_fail;

static bool stub_send(void *u, uint32_t ip, uint16_t port, const uint8_t *b, size_t n)
{
   (void)u;
   if (send_fail) return false;
   assert(sent_count < SENT_MAX);
   sent[sent_count].ip = ip; sent[sent_count].port = port;
   memcpy(sent[sent_count].buf, b, n); sent[sent_count].len = n;
   sent_count++;
   return true;
}
static uint32_t stub_now(void *u) { (void)u; return fake_ms; }

static const aun_transport_t T = { stub_send, stub_now, NULL };
static aun_engine_t e;

static void reset(void)
{
   sent_count = 0; fake_ms = 0; send_fail = false;
   aun_init(&e, &T, 32, 1);                      /* we are 1.32 */
   aun_map_add(&e, 1, 254, 0x0100000A, 32768);   /* 1.254 = 10.0.0.1 */
}

static uint32_t seq_of(int i) { uint32_t s; memcpy(&s, &sent[i].buf[4], 4); return s; }

static void ack(int i, uint8_t type)   /* answer sent[i] with ACK/NAK from its dest */
{
   uint8_t h[8]; memcpy(h, sent[i].buf, 8); h[0] = type;
   aun_udp_input(&e, sent[i].ip, sent[i].port, h, 8);
}

int main(void)
{
   /* 1: tx happy path */
   reset();
   uint8_t pay[] = {1,2,3,4};
   assert(aun_tx_start(&e, 1, 254, 0x80, 0x99, pay, 4) == AUN_OK);
   assert(aun_tx_status(&e) == AUN_STATUS_PENDING);
   assert(sent_count == 1 && sent[0].len == 12);
   assert(sent[0].buf[0] == AUN_TYPE_DATA && sent[0].buf[1] == 0x99);
   assert(sent[0].buf[2] == 0x00);                 /* ctrl bit7 stripped */
   assert(memcmp(&sent[0].buf[8], pay, 4) == 0);
   ack(0, AUN_TYPE_ACK);
   assert(aun_tx_status(&e) == AUN_OK);

   /* 2: busy while pending; an explicit reject (NAK) prompts a same-seq
    * retransmit rather than an immediate failure (AUN spec), and only a
    * sustained run of rejects exhausts the budget to NOT_LISTENING. */
   reset();
   assert(aun_tx_start(&e, 1, 254, 0x80, 1, pay, 4) == AUN_OK);
   assert(aun_tx_start(&e, 1, 254, 0x80, 1, pay, 4) == AUN_TX_BUSY);
   ack(0, AUN_TYPE_NAK);
   assert(aun_tx_status(&e) == AUN_STATUS_PENDING);   /* reject -> scheduled, not failed */
   assert(sent_count == 1);                           /* retransmit deferred ~1 cs */
   fake_ms += AUN_REJECT_TIMEOUT_MS + 1;
   aun_poll(&e);
   assert(sent_count == 2 && seq_of(1) == seq_of(0)); /* now the same-seq retransmit */
   while (aun_tx_status(&e) == AUN_STATUS_PENDING) {  /* reject each retransmit */
      ack(sent_count - 1, AUN_TYPE_NAK);
      fake_ms += AUN_REJECT_TIMEOUT_MS + 1;
      aun_poll(&e);
   }
   assert(aun_tx_status(&e) == AUN_TX_NOT_LISTENING);
   assert(sent_count == AUN_REJECT_RETRIES + 1);      /* original + reject retries */

   /* 3: silence -> fail after ~1 s with NO engine retransmit. The ANFS NFS
    * layer retries the whole transmit (as native Econet does on a missing
    * scout-ack); the engine must not emit a silence retransmit that could
    * land mid-transaction on a single-transaction peer and abort it. */
   reset();
   assert(aun_tx_start(&e, 1, 254, 0x80, 1, pay, 4) == AUN_OK);
   assert(sent_count == 1);
   fake_ms += AUN_NORESP_TIMEOUT_MS + 1;
   aun_poll(&e);
   assert(aun_tx_status(&e) == AUN_TX_NOT_LISTENING);
   assert(sent_count == 1);                            /* no retransmit on silence */

   /* 4: no route */
   reset();
   assert(aun_tx_start(&e, 9, 9, 0x80, 1, pay, 4) == AUN_TX_NO_ROUTE);

   /* 5: rx deliver -> ACK at receipt (fast enough to beat the fileserver's
    * reply-ACK timeout), ctrl bit7 restored. */
   reset();
   uint8_t rbuf[64];
   assert(aun_rx_open(&e, 0, 0x99, AUN_WILDCARD, AUN_WILDCARD, rbuf, 64) == AUN_OK);
   assert(aun_rx_poll(&e, 0, NULL) == AUN_STATUS_PENDING);
   uint8_t dg[12] = { AUN_TYPE_DATA, 0x99, 0x00, 0, 40,0,0,0, 9,8,7,6 };
   aun_udp_input(&e, 0x0100000A, 32768, dg, 12);
   assert(sent_count == 1 && sent[0].buf[0] == AUN_TYPE_ACK && seq_of(0) == 40);
   aun_rx_info_t info;
   assert(aun_rx_poll(&e, 0, &info) == AUN_OK);
   assert(info.src_stn == 254 && info.src_net == 1);
   assert(info.port == 0x99 && info.ctrl == 0x80 && info.len == 4);
   assert(memcmp(rbuf, &dg[8], 4) == 0);
   /* same-seq retransmission (our ACK was lost): re-ACK, do not re-deliver */
   sent_count = 0;
   aun_udp_input(&e, 0x0100000A, 32768, dg, 12);
   assert(sent_count == 1 && sent[0].buf[0] == AUN_TYPE_ACK && e.counters.rx_dup == 1);
   assert(aun_rx_poll(&e, 0, NULL) == AUN_OK);               /* still the one frame */
   /* a new-seq second frame is ACKed at receipt and queued behind the head */
   uint8_t dgB[10] = { AUN_TYPE_DATA, 0x99, 0x00, 0, 48,0,0,0, 1,2 };
   sent_count = 0;
   aun_udp_input(&e, 0x0100000A, 32768, dgB, 10);
   assert(sent_count == 1 && sent[0].buf[0] == AUN_TYPE_ACK && seq_of(0) == 48);
   assert(aun_rx_poll(&e, 0, &info) == AUN_OK && info.len == 4);  /* still A (head) */
   sent_count = 0;
   assert(aun_rx_collect(&e, 0, true) == AUN_OK);            /* pop A: silent */
   assert(sent_count == 0);
   assert(aun_rx_poll(&e, 0, &info) == AUN_OK && info.len == 2);  /* now B */
   assert(memcmp(rbuf, &dgB[8], 2) == 0);
   assert(aun_rx_collect(&e, 0, true) == AUN_OK);
   assert(aun_rx_poll(&e, 0, NULL) == AUN_STATUS_PENDING);
   /* collect with accept=false is silent (frame was already ACKed at receipt) */
   dgB[4] = 56;
   sent_count = 0;
   aun_udp_input(&e, 0x0100000A, 32768, dgB, 10);
   assert(sent_count == 1 && sent[0].buf[0] == AUN_TYPE_ACK && seq_of(0) == 56);
   assert(aun_rx_poll(&e, 0, NULL) == AUN_OK);
   sent_count = 0;
   assert(aun_rx_collect(&e, 0, false) == AUN_OK);
   assert(sent_count == 0);                                  /* collect is silent */
   /* fill the queue: each accepted frame ACKs at receipt; a full queue NAKs.
    * Payloads must be DISTINCT, else the dup-in-queue drop (see test 19)
    * legitimately absorbs the repeats and the queue never fills. */
   for (uint8_t i = 0; i < AUN_RX_QUEUE; i++) {
      dgB[4] = (uint8_t)(60 + 4*i);
      dgB[8] = (uint8_t)(0xA0 + i);                 /* distinct payload */
      aun_udp_input(&e, 0x0100000A, 32768, dgB, 10);
   }
   dgB[4] = 90;
   dgB[8] = 0xBE;                                    /* distinct again */
   sent_count = 0;
   aun_udp_input(&e, 0x0100000A, 32768, dgB, 10);
   assert(sent_count == 1 && sent[0].buf[0] == AUN_TYPE_NAK);   /* queue full */
   for (uint8_t i = 0; i < AUN_RX_QUEUE; i++) {
      assert(aun_rx_poll(&e, 0, NULL) == AUN_OK);
      assert(aun_rx_collect(&e, 0, true) == AUN_OK);
   }
   assert(aun_rx_poll(&e, 0, NULL) == AUN_STATUS_PENDING);

   /* 6: duplicate (last accepted seq) -> re-ACK, no re-deliver */
   dg[4] = 92;                                               /* fresh seq */
   aun_udp_input(&e, 0x0100000A, 32768, dg, 12);
   aun_rx_info_t i2;
   assert(aun_rx_poll(&e, 0, &i2) == AUN_OK);
   assert(aun_rx_collect(&e, 0, true) == AUN_OK);
   sent_count = 0;
   aun_udp_input(&e, 0x0100000A, 32768, dg, 12);             /* dup seq 92 */
   assert(aun_rx_poll(&e, 0, NULL) == AUN_STATUS_PENDING);   /* not re-delivered */
   assert(sent_count == 1 && sent[0].buf[0] == AUN_TYPE_ACK);

   /* 7: no listener -> NAK */
   reset();
   uint8_t dg2[9] = { AUN_TYPE_DATA, 0x55, 0, 0, 8,0,0,0, 1 };
   aun_udp_input(&e, 0x0100000A, 32768, dg2, 9);
   assert(sent_count == 1 && sent[0].buf[0] == AUN_TYPE_NAK);

   /* 8: unknown source dropped silently */
   reset();
   aun_udp_input(&e, 0xDEADBEEF, 32768, dg2, 9);
   assert(sent_count == 0 && e.counters.rx_unknown_source == 1);

   /* 9: broadcast goes to every mapped peer, no ack needed */
   reset();
   aun_map_add(&e, 1, 200, 0x0200000A, 32768);
   assert(aun_broadcast(&e, 0x80, 0x9C, pay, 4) == AUN_OK);
   assert(sent_count == 2 && sent[0].buf[0] == AUN_TYPE_BROADCAST);
   assert(aun_tx_status(&e) == AUN_OK);

   /* 10: inbound machine peek answered */
   reset();
   uint8_t imm[8] = { AUN_TYPE_IMMEDIATE, 0, AUN_CTRL_MACHINE_PEEK, 0, 4,0,0,0 };
   aun_udp_input(&e, 0x0100000A, 32768, imm, 8);
   assert(sent_count == 1 && sent[0].buf[0] == AUN_TYPE_IMM_REPLY);
   assert(sent[0].len == 12 && sent[0].buf[8] == 0x01);
   assert(seq_of(0) == 4);

   /* 11: outbound immediate + reply */
   reset();
   uint8_t reply[8];
   assert(aun_immediate(&e, 1, 254, AUN_CTRL_MACHINE_PEEK, NULL, 0, reply, 8) == AUN_OK);
   assert(sent_count == 1 && sent[0].buf[0] == AUN_TYPE_IMMEDIATE);
   uint8_t ir[12] = { AUN_TYPE_IMM_REPLY, 0, AUN_CTRL_MACHINE_PEEK, 0, 0,0,0,0, 0xAA,0xBB,0xCC,0xDD };
   memcpy(&ir[4], &sent[0].buf[4], 4);
   aun_udp_input(&e, sent[0].ip, sent[0].port, ir, 12);
   assert(aun_tx_status(&e) == AUN_OK && aun_tx_reply_len(&e) == 4);
   assert(reply[0] == 0xAA && reply[3] == 0xDD);

   /* 12: loopback test responder, no network involved */
   reset();
   aun_test_responder(&e, true, 99, 0);
   assert(aun_rx_open(&e, 1, 0x77, AUN_WILDCARD, AUN_WILDCARD, rbuf, 64) == AUN_OK);
   assert(aun_tx_start(&e, 0, 99, 0x85, 0x77, pay, 4) == AUN_OK);
   assert(aun_tx_status(&e) == AUN_OK);
   assert(sent_count == 0);                        /* nothing hit the wire */
   assert(aun_rx_poll(&e, 1, &info) == AUN_OK);
   assert(info.src_stn == 99 && info.ctrl == 0x85 && info.len == 4);

   /* 13: send failure surfaces as net error */
   reset();
   send_fail = true;
   assert(aun_tx_start(&e, 1, 254, 0x80, 1, pay, 4) == AUN_OK);
   assert(aun_tx_status(&e) == AUN_TX_NET_ERROR);

   /* 14: learn mode - outbound resolve + inbound auto-map */
   reset();
   aun_set_addressing(&e, 0xff01a8c0, 0x1401a8c0, 0x00ffffff, 2);
   assert(aun_tx_start(&e, 2, 70, 0x80, 0x99, pay, 4) == AUN_OK);
   assert(sent_count == 1 && sent[0].ip == ((0x1401a8c0 & 0x00ffffff) | (70u<<24)));
   ack(0, AUN_TYPE_ACK);
   assert(aun_tx_status(&e) == AUN_OK);
   uint8_t rb2[32];
   assert(aun_rx_open(&e, 1, 0, AUN_WILDCARD, AUN_WILDCARD, rb2, 32) == AUN_OK);
   uint8_t dgl[9] = { AUN_TYPE_DATA, 0x55, 0, 0, 8,0,0,0, 7 };
   aun_udp_input(&e, (0x1401a8c0 & 0x00ffffff) | (44u<<24), 32768, dgl, 9);
   aun_rx_info_t li;
   assert(aun_rx_poll(&e, 1, &li) == AUN_OK);
   assert(li.src_stn == 44 && li.src_net == 2);   /* auto-attributed */
   assert(aun_rx_collect(&e, 1, true) == AUN_OK);

   /* 15: subnet broadcast goes out alongside mapped peers */
   reset();
   aun_set_addressing(&e, 0xff01a8c0, 0x1401a8c0, 0x00ffffff, 0xFF);
   assert(aun_broadcast(&e, 0x80, 0x9c, pay, 4) == AUN_OK);
   assert(sent_count == 2 && sent[1].ip == 0xff01a8c0);

   /* 16: park-and-retry. A DATA reply the host rejects (no CB armed yet)
    * is ACKed at receipt, then held out of the queue and re-presented
    * after a short delay - so it is delivered once the CB is armed, not
    * lost (the AUN arm-race fix). */
   reset();
   uint8_t pbuf[64];
   assert(aun_rx_open(&e, 0, 0x92, AUN_WILDCARD, AUN_WILDCARD, pbuf, 64) == AUN_OK);
   uint8_t pf[12] = { AUN_TYPE_DATA, 0x92, 0x00, 0, 200,0,0,0, 1,2,3,4 };
   aun_udp_input(&e, 0x0100000A, 32768, pf, 12);
   assert(sent_count == 1 && sent[0].buf[0] == AUN_TYPE_ACK);   /* acked at receipt */
   assert(aun_rx_poll(&e, 0, &info) == AUN_OK && info.len == 4);
   sent_count = 0;
   assert(aun_rx_collect(&e, 0, false) == AUN_OK);              /* host: not listening */
   assert(sent_count == 0 && e.parked_valid);                  /* parked, no NAK */
   assert(aun_rx_poll(&e, 0, NULL) == AUN_STATUS_PENDING);      /* out of the queue */
   aun_poll(&e);                                                /* before delay: held */
   assert(aun_rx_poll(&e, 0, NULL) == AUN_STATUS_PENDING);
   fake_ms += AUN_PARK_DELAY_MS;
   aun_poll(&e);                                                /* re-presented */
   assert(aun_rx_poll(&e, 0, &info) == AUN_OK && info.len == 4);
   assert(memcmp(pbuf, &pf[8], 4) == 0);
   assert(aun_rx_collect(&e, 0, true) == AUN_OK);               /* now delivered */
   assert(!e.parked_valid);
   assert(aun_rx_poll(&e, 0, NULL) == AUN_STATUS_PENDING);

   /* 17: a frame nobody ever listens for is dropped after the retry budget */
   reset();
   assert(aun_rx_open(&e, 0, 0x92, AUN_WILDCARD, AUN_WILDCARD, pbuf, 64) == AUN_OK);
   pf[4] = 220;
   aun_udp_input(&e, 0x0100000A, 32768, pf, 12);
   assert(aun_rx_poll(&e, 0, NULL) == AUN_OK);
   assert(aun_rx_collect(&e, 0, false) == AUN_OK);              /* park */
   for (uint32_t i = 0; i < AUN_PARK_RETRIES + 2u && e.parked_valid; i++) {
      fake_ms += AUN_PARK_DELAY_MS;
      aun_poll(&e);                                             /* re-inject */
      if (aun_rx_poll(&e, 0, NULL) == AUN_OK)
         assert(aun_rx_collect(&e, 0, false) == AUN_OK);        /* reject again */
   }
   assert(!e.parked_valid);                                     /* eventually dropped */
   assert(aun_rx_poll(&e, 0, NULL) == AUN_STATUS_PENDING);

   /* 18: a NEW-sequence frame that happens to be byte-identical to its
    * predecessor is a legitimate distinct block, NOT a retransmit (a real
    * retransmit reuses the wire seq and is suppressed before delivery). So on
    * an arm-race reject it must be PARKED like any other new frame - dropping
    * it on content equality would silently lose data the peer already ACKed.
    * (M4) */
   reset();
   assert(aun_rx_open(&e, 0, 0x92, AUN_WILDCARD, AUN_WILDCARD, pbuf, 64) == AUN_OK);
   uint8_t d1[12] = { AUN_TYPE_DATA, 0x92, 0x00, 0, 40,0,0,0, 5,6,7,8 };
   aun_udp_input(&e, 0x0100000A, 32768, d1, 12);               /* first copy */
   assert(aun_rx_poll(&e, 0, NULL) == AUN_OK);
   assert(aun_rx_collect(&e, 0, true) == AUN_OK);              /* delivered */
   uint8_t d2[12] = { AUN_TYPE_DATA, 0x92, 0x00, 0, 44,0,0,0, 5,6,7,8 };  /* same bytes, NEW seq */
   aun_udp_input(&e, 0x0100000A, 32768, d2, 12);               /* distinct new block */
   assert(aun_rx_poll(&e, 0, NULL) == AUN_OK);
   assert(aun_rx_collect(&e, 0, false) == AUN_OK);             /* host rejects (CB not armed) */
   assert(e.parked_valid);                                     /* parked, not lost */
   fake_ms += AUN_PARK_DELAY_MS;
   aun_poll(&e);                                               /* re-presented */
   assert(aun_rx_poll(&e, 0, &info) == AUN_OK && info.len == 4);
   assert(memcmp(pbuf, &d2[8], 4) == 0);                       /* delivered once CB armed */
   assert(aun_rx_collect(&e, 0, true) == AUN_OK);
   assert(!e.parked_valid);

   /* 19: content identity does NOT suppress delivery (spec: the application
    * detects duplicates). Two byte-identical frames under different sequence
    * numbers are BOTH delivered and ACKed, so a legitimate identical data
    * block - e.g. the same file loaded twice - is never silently dropped. */
   reset();
   assert(aun_rx_open(&e, 0, 0x90, AUN_WILDCARD, AUN_WILDCARD, pbuf, 64) == AUN_OK);
   uint8_t q1[12] = { AUN_TYPE_DATA, 0x90, 0x00, 0, 40,0,0,0, 9,9,9,9 };
   aun_udp_input(&e, 0x0100000A, 32768, q1, 12);               /* queued + ACK */
   uint8_t q2[12] = { AUN_TYPE_DATA, 0x90, 0x00, 0, 44,0,0,0, 9,9,9,9 };  /* same bytes, new seq */
   sent_count = 0;
   aun_udp_input(&e, 0x0100000A, 32768, q2, 12);               /* also delivered + ACK */
   assert(sent_count == 1 && sent[0].buf[0] == AUN_TYPE_ACK && seq_of(0) == 44);
   assert(aun_rx_poll(&e, 0, &info) == AUN_OK && info.len == 4);   /* q1 (head) */
   assert(aun_rx_collect(&e, 0, true) == AUN_OK);
   assert(aun_rx_poll(&e, 0, &info) == AUN_OK && info.len == 4);   /* q2 also present */
   assert(aun_rx_collect(&e, 0, true) == AUN_OK);
   assert(aun_rx_poll(&e, 0, NULL) == AUN_STATUS_PENDING);

   /* 20: closing a receive block while a frame is parked-and-re-injected
    * must clear the park slot, else park-and-retry is wedged for the life of
    * the engine (parked_in_queue stuck true). */
   reset();
   assert(aun_rx_open(&e, 0, 0x92, AUN_WILDCARD, AUN_WILDCARD, pbuf, 64) == AUN_OK);
   pf[4] = 230; pf[8] = 0x11;
   aun_udp_input(&e, 0x0100000A, 32768, pf, 12);
   assert(aun_rx_poll(&e, 0, NULL) == AUN_OK);
   assert(aun_rx_collect(&e, 0, false) == AUN_OK);            /* park */
   fake_ms += AUN_PARK_DELAY_MS;
   aun_poll(&e);                                              /* re-inject */
   assert(e.parked_valid && e.parked_in_queue);
   assert(aun_rx_close(&e, 0) == AUN_OK);                     /* close while parked */
   assert(!e.parked_valid && e.counters.rx_parked_drop == 1); /* slot cleared */
   /* park-and-retry still works afterwards: a fresh reject parks again */
   assert(aun_rx_open(&e, 0, 0x92, AUN_WILDCARD, AUN_WILDCARD, pbuf, 64) == AUN_OK);
   pf[4] = 234; pf[8] = 0x22;                                 /* distinct -> not a dup */
   aun_udp_input(&e, 0x0100000A, 32768, pf, 12);
   assert(aun_rx_poll(&e, 0, NULL) == AUN_OK);
   assert(aun_rx_collect(&e, 0, false) == AUN_OK);
   assert(e.parked_valid);                                    /* not wedged */

   /* 21: a payload larger than AUN_MAX_DATA is refused (NAK) and never
    * overflows the fixed frame slot, even when the block's buffer is big
    * enough for it. */
   reset();
   static uint8_t bigbuf[AUN_MAX_DATA + 64];
   assert(aun_rx_open(&e, 0, 0x93, AUN_WILDCARD, AUN_WILDCARD,
                      bigbuf, sizeof bigbuf) == AUN_OK);
   static uint8_t bigdg[AUN_HDR_SIZE + AUN_MAX_DATA + 16];
   memset(bigdg, 0, sizeof bigdg);
   bigdg[0] = AUN_TYPE_DATA; bigdg[1] = 0x93; bigdg[4] = 60;  /* dlen > AUN_MAX_DATA */
   sent_count = 0;
   aun_udp_input(&e, 0x0100000A, 32768, bigdg, sizeof bigdg);
   assert(sent_count == 1 && sent[0].buf[0] == AUN_TYPE_NAK);  /* refused */
   assert(e.counters.rx_too_big == 1);
   assert(aun_rx_poll(&e, 0, NULL) == AUN_STATUS_PENDING);     /* nothing queued */

   /* 22: a re-injected parked frame goes to the TAIL, behind a frame that
    * queued while it was parked - so a parked (e.g. undeliverable) frame can
    * never block or jump ahead of a fresh frame. (Confirms park is not a
    * head-of-line hazard.) */
   reset();
   assert(aun_rx_open(&e, 0, 0x92, AUN_WILDCARD, AUN_WILDCARD, pbuf, 64) == AUN_OK);
   uint8_t fa[12] = { AUN_TYPE_DATA, 0x92, 0x00, 0, 40,0,0,0, 0xA1,0xA2,0xA3,0xA4 };
   aun_udp_input(&e, 0x0100000A, 32768, fa, 12);                 /* frame A */
   assert(aun_rx_poll(&e, 0, NULL) == AUN_OK);
   assert(aun_rx_collect(&e, 0, false) == AUN_OK);               /* A parked, out of queue */
   assert(e.parked_valid);
   uint8_t fb[12] = { AUN_TYPE_DATA, 0x92, 0x00, 0, 44,0,0,0, 0xB1,0xB2,0xB3,0xB4 };
   aun_udp_input(&e, 0x0100000A, 32768, fb, 12);                 /* frame B queued */
   fake_ms += AUN_PARK_DELAY_MS;
   aun_poll(&e);                                                 /* A re-injected at tail */
   assert(aun_rx_poll(&e, 0, &info) == AUN_OK && info.len == 4);
   assert(memcmp(pbuf, &fb[8], 4) == 0);                         /* B (queued first) delivered first */
   assert(aun_rx_collect(&e, 0, true) == AUN_OK);
   assert(aun_rx_poll(&e, 0, &info) == AUN_OK && info.len == 4);
   assert(memcmp(pbuf, &fa[8], 4) == 0);                         /* then A, behind B */
   assert(aun_rx_collect(&e, 0, true) == AUN_OK);
   assert(!e.parked_valid && aun_rx_poll(&e, 0, NULL) == AUN_STATUS_PENDING);

   /* 23: re-opening a receive handle while a frame is parked must DROP the
    * parked frame. Its len was validated against the old (larger) buffer, so
    * re-opening with a smaller buffer would otherwise let rx_park_poll
    * re-inject an oversize frame that aun_rx_poll then memcpy()s past the new
    * buffer. (H1) */
   reset();
   static uint8_t big_rx[600];
   assert(aun_rx_open(&e, 0, 0x92, AUN_WILDCARD, AUN_WILDCARD,
                      big_rx, sizeof big_rx) == AUN_OK);
   static uint8_t dg100[AUN_HDR_SIZE + 100];
   memset(dg100, 0, sizeof dg100);
   dg100[0] = AUN_TYPE_DATA; dg100[1] = 0x92; dg100[4] = 0x10; /* seq */
   for (unsigned i = 0; i < 100; i++) dg100[AUN_HDR_SIZE + i] = (uint8_t)(i + 1);
   aun_udp_input(&e, 0x0100000A, 32768, dg100, sizeof dg100);  /* delivered + ACKed */
   assert(aun_rx_poll(&e, 0, &info) == AUN_OK && info.len == 100);
   assert(aun_rx_collect(&e, 0, false) == AUN_OK);             /* host rejects -> park */
   assert(e.parked_valid && e.parked.len == 100);
   {
      uint32_t drops = e.counters.rx_parked_drop;
      static uint8_t small_rx[64];                             /* < parked len */
      assert(aun_rx_open(&e, 0, 0x92, AUN_WILDCARD, AUN_WILDCARD,
                         small_rx, sizeof small_rx) == AUN_OK);
      assert(!e.parked_valid);                                 /* dropped on re-open */
      assert(e.counters.rx_parked_drop == drops + 1);
      fake_ms += AUN_PARK_DELAY_MS;
      aun_poll(&e);                                            /* must not re-inject */
      assert(aun_rx_poll(&e, 0, NULL) == AUN_STATUS_PENDING);
   }

   /* 24: a held host-immediate the host never answers is reaped after
    * AUN_HIMM_TIMEOUT_MS - NAKed and the slot freed - so later inbound
    * immediates are not NAKed forever and nIRQ bit &40 is not pinned. (M3) */
   reset();
   aun_set_host_imm(&e, true);
   {
      uint8_t imm[AUN_HDR_SIZE + 4] =
         { AUN_TYPE_IMMEDIATE, 0, 0x01, 0, 0x20,0,0,0, 1,2,3,4 };
      sent_count = 0;
      aun_udp_input(&e, 0x0100000A, 32768, imm, sizeof imm);   /* held for the host */
      assert(e.himm.active && sent_count == 0);                /* captured, no reply */
      fake_ms += AUN_HIMM_TIMEOUT_MS;                          /* host never replies */
      aun_poll(&e);
      assert(!e.himm.active);                                  /* slot released */
      assert(e.counters.himm_timeout == 1);
      assert(sent_count == 1 && sent[0].buf[0] == AUN_TYPE_NAK); /* originator NAKed */
      uint8_t imm2[AUN_HDR_SIZE + 4] =
         { AUN_TYPE_IMMEDIATE, 0, 0x01, 0, 0x24,0,0,0, 5,6,7,8 };
      aun_udp_input(&e, 0x0100000A, 32768, imm2, sizeof imm2); /* fresh: accepted */
      assert(e.himm.active && e.himm.seq == 0x24);             /* not wedged */
   }

   /* ---- network-delay variance (jitter / reordering / boundary sweep) ----
    * The transmits above always advance the clock to a single fixed point.
    * Real UDP latency varies: an ACK can land at any time within the silence
    * window, the host polls at irregular intervals, frames can reorder, and a
    * reply can arrive a hair under the reap deadline. These cases sweep that
    * variance so a future change that turns a "<" into a "<=", drops the
    * deadline re-arm, or tightens the dup filter is caught. */

   /* 25: ACK-latency sweep across the silence boundary. The engine's success
    * is purely deadline-based, so an ACK at ANY latency below
    * AUN_NORESP_TIMEOUT_MS must complete the transfer, and one at or beyond it
    * must be ignored - a late ACK cannot revive a transmit already failed. */
   {
      static const uint32_t good[] = { 0, 1, 37, 250, AUN_NORESP_TIMEOUT_MS - 1 };
      for (unsigned k = 0; k < sizeof good / sizeof good[0]; k++) {
         reset();
         assert(aun_tx_start(&e, 1, 254, 0x80, 1, pay, 4) == AUN_OK);
         fake_ms += good[k];
         aun_poll(&e);                                /* still inside window */
         assert(aun_tx_status(&e) == AUN_STATUS_PENDING);
         ack(0, AUN_TYPE_ACK);                         /* ACK at latency good[k] */
         assert(aun_tx_status(&e) == AUN_OK);
      }
      static const uint32_t late[] = { AUN_NORESP_TIMEOUT_MS,
                                       AUN_NORESP_TIMEOUT_MS + 500 };
      for (unsigned k = 0; k < sizeof late / sizeof late[0]; k++) {
         reset();
         assert(aun_tx_start(&e, 1, 254, 0x80, 1, pay, 4) == AUN_OK);
         fake_ms += late[k];
         aun_poll(&e);                                /* deadline passed -> fail */
         assert(aun_tx_status(&e) == AUN_TX_NOT_LISTENING);
         ack(0, AUN_TYPE_ACK);                         /* late ACK must not revive */
         assert(aun_tx_status(&e) == AUN_TX_NOT_LISTENING);
      }
   }

   /* 26: jitter - the host polls at irregular gaps while the peer is silent but
    * still within the window. No gap pattern may fail the transmit early or
    * emit a spurious silence retransmit, and an ACK on a late poll still wins. */
   reset();
   assert(aun_tx_start(&e, 1, 254, 0x80, 1, pay, 4) == AUN_OK);
   {
      static const uint32_t gaps[] = { 37, 113, 7, 200, 311, 90 };  /* sum < 1 s */
      uint32_t acc = 0;
      for (unsigned k = 0; k < sizeof gaps / sizeof gaps[0]; k++) {
         fake_ms += gaps[k]; acc += gaps[k];
         aun_poll(&e);
         assert(aun_tx_status(&e) == AUN_STATUS_PENDING);
         assert(sent_count == 1);                      /* no silence retransmit */
      }
      assert(acc < AUN_NORESP_TIMEOUT_MS);
   }
   ack(0, AUN_TYPE_ACK);
   assert(aun_tx_status(&e) == AUN_OK);

   /* 27: a reject schedules a same-seq retransmit and re-arms a fresh silence
    * window; the ACK for that retransmit may itself arrive at any latency under
    * the new window. Verify the deadline is re-armed so a delayed-but-valid ACK
    * still completes (a missed re-arm would fail it as silence). */
   reset();
   assert(aun_tx_start(&e, 1, 254, 0x80, 1, pay, 4) == AUN_OK);
   ack(0, AUN_TYPE_NAK);                               /* peer rejects */
   assert(aun_tx_status(&e) == AUN_STATUS_PENDING);
   fake_ms += AUN_REJECT_TIMEOUT_MS + 1;
   aun_poll(&e);                                       /* retransmit fires */
   assert(sent_count == 2 && seq_of(1) == seq_of(0));
   fake_ms += AUN_NORESP_TIMEOUT_MS - 1;               /* ACK late, but in window */
   aun_poll(&e);
   assert(aun_tx_status(&e) == AUN_STATUS_PENDING);
   ack(1, AUN_TYPE_ACK);
   assert(aun_tx_status(&e) == AUN_OK);

   /* 28: reordering - distinct frames that arrive out of sequence order (a
    * higher seq before a lower one, as variable path delay can cause) are EACH
    * new transmissions and must both be delivered. The same-seq dup filter
    * keys only on the immediately-previous seq, so it must not drop a
    * legitimately reordered frame. */
   reset();
   assert(aun_rx_open(&e, 0, 0x99, AUN_WILDCARD, AUN_WILDCARD, rbuf, 64) == AUN_OK);
   {
      uint8_t hi[10] = { AUN_TYPE_DATA, 0x99, 0x00, 0, 0x50,0,0,0, 0xAA,0xBB };
      uint8_t lo[10] = { AUN_TYPE_DATA, 0x99, 0x00, 0, 0x40,0,0,0, 0xCC,0xDD };
      sent_count = 0;
      aun_udp_input(&e, 0x0100000A, 32768, hi, 10);    /* higher seq first */
      aun_udp_input(&e, 0x0100000A, 32768, lo, 10);    /* lower seq after */
      assert(sent_count == 2);                         /* both ACKed */
      assert(e.counters.rx_dup == 0);                  /* neither suppressed */
      assert(aun_rx_poll(&e, 0, &info) == AUN_OK && info.len == 2);
      assert(aun_rx_collect(&e, 0, true) == AUN_OK);
      assert(aun_rx_poll(&e, 0, &info) == AUN_OK && info.len == 2);
      assert(aun_rx_collect(&e, 0, true) == AUN_OK);
      assert(aun_rx_poll(&e, 0, NULL) == AUN_STATUS_PENDING);
   }

   /* 29: a host-immediate answered just before AUN_HIMM_TIMEOUT_MS is released
    * with an IMM_REPLY and must NOT be counted as a timeout - the reap window
    * is a ceiling, not a fixed delay (complements test 24's never-answered
    * case, proving the reply latency can vary right up to the boundary). */
   reset();
   aun_set_host_imm(&e, true);
   {
      uint8_t imm[AUN_HDR_SIZE + 4] =
         { AUN_TYPE_IMMEDIATE, 0, 0x01, 0, 0x28,0,0,0, 1,2,3,4 };
      sent_count = 0;
      aun_udp_input(&e, 0x0100000A, 32768, imm, sizeof imm);
      assert(e.himm.active && sent_count == 0);
      fake_ms += AUN_HIMM_TIMEOUT_MS - 1;              /* host answers just in time */
      aun_poll(&e);                                    /* not yet reaped */
      assert(e.himm.active && e.counters.himm_timeout == 0);
      uint8_t rep[4] = { 9, 8, 7, 6 };
      aun_himm_reply(&e, rep, 4);
      assert(!e.himm.active);
      assert(sent_count == 1 && sent[0].buf[0] == AUN_TYPE_IMM_REPLY);
      assert(e.counters.himm_timeout == 0);            /* answered, not reaped */
   }

   /* 30: a late retransmit of an ALREADY-ANSWERED host immediate is replayed
    * from cache, not handed to the host a second time. A re-executed Poke/JSR/
    * OSProcCall would corrupt state; the reply the host gave is cached keyed on
    * (ip,port,seq) and re-sent on a matching retransmit. A genuinely new seq is
    * still delivered. (L7) */
   reset();
   aun_set_host_imm(&e, true);
   {
      uint8_t imm[AUN_HDR_SIZE + 4] =        /* ctrl &02 = Poke (non-idempotent) */
         { AUN_TYPE_IMMEDIATE, 0, 0x02, 0, 0x30,0,0,0, 0xde,0xad,0xbe,0xef };
      sent_count = 0;
      aun_udp_input(&e, 0x0100000A, 32768, imm, sizeof imm);   /* held for host */
      assert(e.himm.active && e.himm.seq == 0x30 && sent_count == 0);
      uint8_t rep[2] = { 0x12, 0x34 };
      aun_himm_reply(&e, rep, 2);                              /* host executes+answers */
      assert(!e.himm.active && e.himm_cache.valid);
      assert(sent_count == 1 && sent[0].buf[0] == AUN_TYPE_IMM_REPLY);
      assert(memcmp(&sent[0].buf[AUN_HDR_SIZE], rep, 2) == 0);
      /* peer missed the reply and retransmits the SAME immediate (same seq) */
      sent_count = 0;
      aun_udp_input(&e, 0x0100000A, 32768, imm, sizeof imm);
      assert(!e.himm.active);                                  /* NOT re-delivered */
      assert(e.counters.himm_replay == 1);
      assert(sent_count == 1 && sent[0].buf[0] == AUN_TYPE_IMM_REPLY); /* replayed */
      assert(memcmp(&sent[0].buf[AUN_HDR_SIZE], rep, 2) == 0); /* identical reply */
      assert(seq_of(0) == 0x30);                               /* same seq echoed */
      /* a genuinely new immediate (new seq) IS still held for the host */
      uint8_t imm2[AUN_HDR_SIZE + 4] =
         { AUN_TYPE_IMMEDIATE, 0, 0x02, 0, 0x34,0,0,0, 1,2,3,4 };
      aun_udp_input(&e, 0x0100000A, 32768, imm2, sizeof imm2);
      assert(e.himm.active && e.himm.seq == 0x34);
   }

   /* 31: the ms clock MUST be derived from the full 64-bit microsecond timer
    * (aun_emulator.c aun_now_ms -> RPI_GetSystemTime64). Deriving it from only
    * the low 32 bits (us32/1000) makes a ramp that resets every ~71.6 min, so
    * a deadline = now + timeout computed just before the wrap lands in a gap of
    * ms values the clock never reaches again, and the wrap-safe compare below
    * never fires (HIGH-1: tx/himm/park timer hangs). This guards the property
    * the production derivation relies on. */
   {
      const uint64_t boundary = (uint64_t)0xFFFFFFFFu + 1u;     /* 2^32 us */
      uint64_t start_us = boundary - 500000u;                  /* 0.5s before */
      uint64_t late_us  = boundary + 1000000u;                 /* 1.0s after  */
      uint32_t start_clean = (uint32_t)(start_us / 1000u);
      uint32_t late_clean  = (uint32_t)(late_us  / 1000u);     /* 64-bit: clean */
      uint32_t late_broken = (uint32_t)late_us / 1000u;        /* low-32: ramp  */
      uint32_t deadline = start_clean + 1000u;                 /* engine timer  */
      assert((int32_t)(late_clean  - deadline) >= 0);  /* clean clock: FIRES */
      assert((int32_t)(late_broken - deadline) <  0);  /* low-32 clock: never */
   }

   /* 32: a retransmit of a REAPED host immediate is re-refused from the cache
    * (NAK), not re-held and re-executed - a non-idempotent Poke/JSR must not
    * run a second time just because the host was slow and the slot was reaped
    * before it answered. (MED-1) */
   reset();
   aun_set_host_imm(&e, true);
   {
      uint8_t imm[AUN_HDR_SIZE + 4] =        /* ctrl &02 = Poke (non-idempotent) */
         { AUN_TYPE_IMMEDIATE, 0, 0x02, 0, 0x40,0,0,0, 1,2,3,4 };
      aun_udp_input(&e, 0x0100000A, 32768, imm, sizeof imm);   /* held */
      assert(e.himm.active);
      fake_ms += AUN_HIMM_TIMEOUT_MS;                          /* host never answers */
      aun_poll(&e);
      assert(!e.himm.active && e.counters.himm_timeout == 1);  /* reaped + NAKed */
      assert(e.himm_cache.valid && e.himm_cache.is_nak);       /* NAK-marker seeded */
      sent_count = 0;
      aun_udp_input(&e, 0x0100000A, 32768, imm, sizeof imm);   /* originator retransmits */
      assert(!e.himm.active);                                  /* NOT re-held/executed */
      assert(e.counters.himm_replay == 1);
      assert(sent_count == 1 && sent[0].buf[0] == AUN_TYPE_NAK && seq_of(0) == 0x40);
   }

   /* 33: the answered-immediate cache expires after AUN_HIMM_CACHE_MS, so a
    * peer that reboots and reuses the sequence number gets a fresh execution,
    * not a stale replay. (MED-2) */
   reset();
   aun_set_host_imm(&e, true);
   {
      uint8_t imm[AUN_HDR_SIZE + 4] =
         { AUN_TYPE_IMMEDIATE, 0, 0x02, 0, 0x44,0,0,0, 1,2,3,4 };
      aun_udp_input(&e, 0x0100000A, 32768, imm, sizeof imm);   /* held */
      uint8_t rep[2] = { 9, 9 };
      aun_himm_reply(&e, rep, 2);                              /* answered + cached */
      assert(e.himm_cache.valid && !e.himm_cache.is_nak);
      sent_count = 0;
      aun_udp_input(&e, 0x0100000A, 32768, imm, sizeof imm);   /* within window */
      assert(!e.himm.active && e.counters.himm_replay == 1);   /* replayed, not re-held */
      fake_ms += AUN_HIMM_CACHE_MS;                            /* window elapses */
      aun_udp_input(&e, 0x0100000A, 32768, imm, sizeof imm);   /* same (ip,port,seq) */
      assert(e.himm.active && e.himm.seq == 0x44);             /* now a FRESH hold */
   }

   printf("all aun tests passed\n");
   return 0;
}
