/* Randomized soak of the AUN engine under ASan/UBSan: random datagrams
 * (valid-ish and malformed) interleaved with random API calls, with
 * invariant checks. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "econet_aun.h"

static uint32_t rng = 0x12345678;
static uint32_t rnd(void) { rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5; return rng; }

static int sent_acks, sent_naks, sent_other;
static bool fail_send;
static bool s(void*u,uint32_t ip,uint16_t p,const uint8_t*b,size_t n){
  (void)u;(void)ip;(void)p;
  assert(n >= 8 && n <= 8 + AUN_HIMM_MAX + 8192);
  if (b[0]==3) sent_acks++; else if (b[0]==4) sent_naks++; else sent_other++;
  return !fail_send;
}
static uint32_t fake_ms;
static uint32_t now(void*u){(void)u;return fake_ms;}

int main(void){
  static aun_engine_t e;
  static uint8_t buf[16384];
  static uint8_t rxbuf[8][8192];
  aun_transport_t T={s,now,NULL};
  aun_init(&e,&T,32,1);
  aun_map_add(&e,1,254,0x0100000A,32768);
  aun_map_add(&e,1,200,0x0200000A,32769);
  aun_set_addressing(&e, 0xff01a8c0, 0x1401a8c0, 0x00ffffff, 2);
  aun_set_host_imm(&e, true);

  for (long i = 0; i < 2000000; i++) {
    uint32_t op = rnd() % 100;
    if (op < 55) {                      /* inbound datagram */
      uint32_t len = rnd() % (sizeof buf);
      if (rnd()%4 == 0) len = rnd() % 16;          /* tiny/truncated */
      for (uint32_t k = 0; k < len && k < 32; k++) buf[k] = (uint8_t)rnd();
      if (rnd()%2) buf[0] = (uint8_t)(1 + rnd()%6);  /* often valid type */
      uint32_t ip = (rnd()%3==0) ? 0x0100000A :
                    (rnd()%3==0) ? ((0x1401a8c0 & 0xffffff) | ((rnd()%256)<<24))
                                 : rnd();
      uint16_t port = (rnd()%2) ? 32768 : (uint16_t)rnd();
      aun_udp_input(&e, ip, port, buf, len);
    } else if (op < 65) {
      uint8_t h = (uint8_t)(rnd()%10);             /* incl. bad handles */
      aun_rx_open(&e, h, (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd(),
                  rxbuf[h & 7], 1 + rnd() % 8192);
    } else if (op < 75) {
      aun_rx_info_t info;
      uint8_t h = (uint8_t)(rnd()%10);
      if (aun_rx_poll(&e, h, &info) == AUN_OK)
        assert(info.len <= 8192);
      if (rnd()%2) aun_rx_collect(&e, h, rnd()%2);
    } else if (op < 85) {
      uint32_t len = rnd() % 9000;                 /* incl. oversize */
      fail_send = (rnd()%16==0);
      aun_tx_start(&e, (uint8_t)(rnd()%3==0?1:rnd()), (uint8_t)rnd(),
                   (uint8_t)rnd(), (uint8_t)rnd(), buf, len % (8192+1));
      fail_send = false;
    } else if (op < 90) {
      aun_broadcast(&e, (uint8_t)rnd(), (uint8_t)rnd(), buf, rnd()%8193);
    } else if (op < 94) {
      aun_immediate(&e, 1, 254, (uint8_t)(0x81+rnd()%8), buf, rnd()%64,
                    rxbuf[0], rnd()%8192);
    } else if (op < 97) {
      aun_himm_reply(&e, buf, rnd()%4096);
    } else {
      fake_ms += rnd() % 1000;
      aun_poll(&e);
      if (rnd()%8==0) aun_rx_close(&e, (uint8_t)(rnd()%10));
      if (rnd()%32==0) { aun_init(&e,&T,32,1); aun_map_add(&e,1,254,0x0100000A,32768); }
    }
    /* invariants */
    for (int b2 = 0; b2 < (int)AUN_RX_BLOCKS; b2++)
      assert(e.rx[b2].count <= AUN_RX_QUEUE && e.rx[b2].head < AUN_RX_QUEUE);
    assert(e.map_count <= AUN_MAP_MAX);
  }
  printf("fuzz: 2M operations, no crashes; acks=%d naks=%d other=%d\n",
         sent_acks, sent_naks, sent_other);
  return 0;
}
