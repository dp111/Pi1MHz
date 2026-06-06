/* Fuzz the Beeb-facing command dispatch: random command blocks with
 * hostile offsets/lengths, under ASan/UBSan. Uses the real
 * aun_emulator.c via the lockstep stubs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Pi1MHz.h"
#include "lwip/udp.h"
#include "wifi/wifi_lwip.h"
#include "aun_emulator.h"

static Pi1MHz_t pi;
Pi1MHz_t *const Pi1MHz = &pi;
const ip_addr_t ip_addr_any = {0};
static wifi_lwip_context_t wctx;
static func_ptr poll_fn;
static struct udp_pcb the_pcb; static int pcb_used;
const wifi_lwip_context_t *wifi_lwip_get_context(void){ return &wctx; }
void Pi1MHz_Register_Poll(func_ptr f){ poll_fn = f; }
void Pi1MHz_SetnIRQ(bool irq){ (void)irq; }
void Pi1MHz_SetnIRQ_src(uint8_t s, bool a){ (void)s; (void)a; }
bool wifi_debug_enabled(void){ return false; }
void wifi_debug_printf(const char *format, ...){ (void)format; }
void Pi1MHz_MemoryWrite(uint32_t a, uint8_t d){ pi.Memory[a & 0x1ff] = d; }
uint32_t RPI_GetSystemTime(void){ static uint32_t t; return t += 997; }
char *get_cmdline_prop(const char *p){
   static char r[64];
   if (!strcmp(p,"aun_station")) { strcpy(r,"1.32"); return r; }
   if (!strcmp(p,"aun_map")) { strcpy(r,"1.254=10.0.0.1"); return r; }
   if (!strcmp(p,"aun_learn")) { strcpy(r,"2"); return r; }
   return NULL;
}
struct pbuf *pbuf_alloc(int l, u16_t n, int t){ (void)l;(void)t;
   struct pbuf *p = malloc(sizeof *p + n); p->tot_len=n; p->payload=p+1; p->next=NULL; return p; }
err_t pbuf_take(struct pbuf *p, const void *d, u16_t n){ memcpy(p->payload,d,n); return 0; }
uint8_t pbuf_free(struct pbuf *p){ free(p); return 1; }
u16_t pbuf_copy_partial(const struct pbuf *p, void *d, u16_t n, u16_t o){ memcpy(d,(uint8_t*)p->payload+o,n); return n; }
struct udp_pcb *udp_new(void){ if(pcb_used) return NULL; pcb_used=1; memset(&the_pcb,0,sizeof the_pcb); return &the_pcb; }
err_t udp_bind(struct udp_pcb *p,const ip_addr_t *i,uint16_t n){(void)p;(void)i;(void)n;return 0;}
void udp_recv(struct udp_pcb *p, udp_recv_fn cb, void *a){ p->cb=cb; p->arg=a; }
void udp_remove(struct udp_pcb *p){ (void)p; pcb_used=0; }
err_t udp_sendto(struct udp_pcb *p, struct pbuf *b, const ip_addr_t *i, uint16_t n){ (void)p;(void)b;(void)i;(void)n; return 0; }

static uint32_t rng = 0xdeadbeef;
static uint32_t rnd(void){ rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; }

int main(void){
   pi.JIM_ram = calloc(1, 32u*1024u*1024u);
   pi.JIM_ram_size = 2;
   wctx.address_ready = true; wctx.netif_added = true;
   wctx.netif.ip = 0x1401a8c0; wctx.netif.mask = 0x00ffffff;
   aun_emulator_init();
   for (long i = 0; i < 400000; i++) {
      uint32_t page = 0xE0 + rnd()%8;
      uint32_t cp = 0xFF0000u | (page<<8);
      /* random block: opcode in range or wild; hostile 32-bit fields */
      pi.JIM_ram[cp] = (uint8_t)(30 + rnd()%15);          /* 30..44 */
      for (int k = 1; k < 32; k++) {
         uint32_t r = rnd();
         pi.JIM_ram[cp+(uint32_t)k] = (uint8_t)r;
         if (r%5==0) pi.JIM_ram[cp+(uint32_t)k] = 0xff;   /* big offsets */
         if (r%7==0) pi.JIM_ram[cp+(uint32_t)k] = 0;
      }
      aun_emulator_command(cp, 0xaa);
      poll_fn();
      /* feed the engine some inbound traffic too */
      if (rnd()%4==0 && the_pcb.cb) {
         uint8_t raw[64]; u16_t n = (u16_t)(rnd()%64);
         for (u16_t k=0;k<n;k++) raw[k]=(uint8_t)rnd();
         struct pbuf *p = pbuf_alloc(0,n,0); memcpy(p->payload,raw,n);
         ip_addr_t a; a.addr = (rnd()%2)?0x0100000A:rnd();
         the_pcb.cb(the_pcb.arg,&the_pcb,p,&a,(u16_t)((rnd()%2)?32768:rnd()));
      }
   }
   printf("cmd fuzz: 400k hostile command blocks, no crashes\n");
   return 0;
}
