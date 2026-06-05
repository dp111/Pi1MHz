/* Host sanity tests for econet_aun.c - stub transport, fake clock. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "econet_aun.h"

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

   /* 2: busy while pending, then NAK -> not listening */
   reset();
   assert(aun_tx_start(&e, 1, 254, 0x80, 1, pay, 4) == AUN_OK);
   assert(aun_tx_start(&e, 1, 254, 0x80, 1, pay, 4) == AUN_TX_BUSY);
   ack(0, AUN_TYPE_NAK);
   assert(aun_tx_status(&e) == AUN_TX_NOT_LISTENING);

   /* 3: retry then timeout */
   reset();
   assert(aun_tx_start(&e, 1, 254, 0x80, 1, pay, 4) == AUN_OK);
   for (int i = 0; i < 10 && aun_tx_status(&e) == AUN_STATUS_PENDING; i++) {
      fake_ms += AUN_ACK_TIMEOUT_MS + 1;
      aun_poll(&e);
   }
   assert(aun_tx_status(&e) == AUN_TX_NOT_LISTENING);
   assert(sent_count == AUN_RETRIES);              /* original + retries */
   assert(seq_of(0) == seq_of(AUN_RETRIES - 1));   /* same seq throughout */

   /* 4: no route */
   reset();
   assert(aun_tx_start(&e, 9, 9, 0x80, 1, pay, 4) == AUN_TX_NO_ROUTE);

   /* 5: rx deliver + ack, ctrl bit7 restored */
   reset();
   uint8_t rbuf[64];
   assert(aun_rx_open(&e, 0, 0x99, AUN_WILDCARD, AUN_WILDCARD, rbuf, 64) == AUN_OK);
   assert(aun_rx_poll(&e, 0, NULL) == AUN_STATUS_PENDING);
   uint8_t dg[12] = { AUN_TYPE_DATA, 0x99, 0x00, 0, 40,0,0,0, 9,8,7,6 };
   aun_udp_input(&e, 0x0100000A, 32768, dg, 12);
   aun_rx_info_t info;
   assert(aun_rx_poll(&e, 0, &info) == AUN_OK);
   assert(info.src_stn == 254 && info.src_net == 1);
   assert(info.port == 0x99 && info.ctrl == 0x80 && info.len == 4);
   assert(memcmp(rbuf, &dg[8], 4) == 0);
   assert(sent_count == 1 && sent[0].buf[0] == AUN_TYPE_ACK && seq_of(0) == 40);
   assert(aun_rx_poll(&e, 0, NULL) == AUN_OK);               /* held until collect */
   /* a second frame is ACKed and queued behind the held head */
   uint8_t dgB[10] = { AUN_TYPE_DATA, 0x99, 0x00, 0, 48,0,0,0, 1,2 };
   aun_udp_input(&e, 0x0100000A, 32768, dgB, 10);
   assert(sent[sent_count-1].buf[0] == AUN_TYPE_ACK);
   assert(aun_rx_poll(&e, 0, &info) == AUN_OK && info.len == 4);  /* still A */
   assert(aun_rx_collect(&e, 0) == AUN_OK);
   assert(aun_rx_poll(&e, 0, &info) == AUN_OK && info.len == 2);  /* now B */
   assert(memcmp(rbuf, &dgB[8], 2) == 0);
   assert(aun_rx_collect(&e, 0) == AUN_OK);
   assert(aun_rx_poll(&e, 0, NULL) == AUN_STATUS_PENDING);
   /* fill the queue: AUN_RX_QUEUE frames accepted, the next is NAKed */
   for (uint8_t i = 0; i < AUN_RX_QUEUE; i++) {
      dgB[4] = (uint8_t)(60 + 4*i);
      aun_udp_input(&e, 0x0100000A, 32768, dgB, 10);
      assert(sent[sent_count-1].buf[0] == AUN_TYPE_ACK);
   }
   dgB[4] = 90;
   aun_udp_input(&e, 0x0100000A, 32768, dgB, 10);
   assert(sent[sent_count-1].buf[0] == AUN_TYPE_NAK);        /* queue full */
   for (uint8_t i = 0; i < AUN_RX_QUEUE; i++) {
      assert(aun_rx_poll(&e, 0, NULL) == AUN_OK);
      assert(aun_rx_collect(&e, 0) == AUN_OK);
   }
   assert(aun_rx_poll(&e, 0, NULL) == AUN_STATUS_PENDING);

   /* 6: duplicate -> re-ACK, no re-deliver */
   dg[4] = 52;                                               /* fresh seq */
   aun_udp_input(&e, 0x0100000A, 32768, dg, 12);
   aun_rx_info_t i2;
   assert(aun_rx_poll(&e, 0, &i2) == AUN_OK);
   assert(aun_rx_collect(&e, 0) == AUN_OK);
   aun_udp_input(&e, 0x0100000A, 32768, dg, 12);             /* dup seq 52 */
   assert(aun_rx_poll(&e, 0, NULL) == AUN_STATUS_PENDING);
   assert(e.counters.rx_dup == 1);
   assert(sent[sent_count-1].buf[0] == AUN_TYPE_ACK);

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

   printf("all econet_aun tests passed\n");
   return 0;
}
