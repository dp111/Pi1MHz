/* Functional test for teletext_emulator.c, built on the host against the
 * stub headers in run.sh. Drives the adapter via its registered FRED
 * callbacks and a simulated TCP feed, checking the t42 decode, the
 * 50 Hz field state machine, the data-register auto-increment and the
 * teletext interrupt. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "Pi1MHz.h"
#include "lwip/tcp.h"
#include "wifi/wifi_lwip.h"

/* ---- captured state / stubs ---- */
static uint8_t  mem[256];
static uint32_t now_us;
static uint32_t irq_mask;          /* mirrors the production uint32 mask */
static callback_func_ptr wr_cb[256], rd_cb[256];
static func_ptr poll_fn;
static wifi_lwip_context_t ctx = { .address_ready = true };

static void *tcp_arg_saved;
static tcp_recv_fn recv_cb;
static tcp_connected_fn conn_cb;
static struct tcp_pcb { int x; } the_pcb;

void Pi1MHz_Register_Memory(int access, uint8_t addr, callback_func_ptr fn)
{ if (access & READ_FRED) rd_cb[addr]=fn; else wr_cb[addr]=fn; }
void Pi1MHz_Register_Poll(func_ptr fn){ poll_fn=fn; }
void Pi1MHz_MemoryWrite(uint32_t addr, uint8_t data){ mem[addr&0xff]=data; }
void Pi1MHz_nIRQ_ASSERT(uint8_t src){ irq_mask |= (1u<<src); }
void Pi1MHz_nIRQ_CLEAR(uint8_t src){ irq_mask &= ~(1u<<src); }
uint32_t RPI_GetSystemTime(void){ return now_us; }
void wifi_debug_printf(const char *fmt, ...){ (void)fmt; }
const wifi_lwip_context_t *wifi_lwip_get_context(void){ return &ctx; }
char *get_cmdline_prop(const char *p){
   static char r[32];
   if (!strcmp(p,"teletext_server1")){ strcpy(r,"1.2.3.4:1234"); return r; }
   return NULL;
}
struct tcp_pcb *tcp_new(void){ return &the_pcb; }
void tcp_arg(struct tcp_pcb*p, void*a){ (void)p; tcp_arg_saved=a; }
void tcp_recv(struct tcp_pcb*p, tcp_recv_fn cb){ (void)p; recv_cb=cb; }
void tcp_err(struct tcp_pcb*p, tcp_err_fn cb){ (void)p; (void)cb; }
err_t tcp_connect(struct tcp_pcb*p, const ip_addr_t*ip, uint16_t port, tcp_connected_fn cb){
   (void)ip;(void)port; conn_cb=cb;
   return cb(tcp_arg_saved, p, ERR_OK);   /* simulate immediate connect */
}
void tcp_recved(struct tcp_pcb*p, uint16_t n){ (void)p;(void)n; }
err_t tcp_close(struct tcp_pcb*p){ (void)p; return ERR_OK; }
void tcp_abort(struct tcp_pcb*p){ (void)p; }
uint8_t pbuf_free(struct pbuf*p){ (void)p; return 1; }

#include "teletext_emulator.c"

#define IRQ_SLOT 12u
static int irq_line(void){ return (irq_mask & (1u<<IRQ_SLOT)) != 0; }
static void step(void){ now_us += 100000u; poll_fn(); }   /* force phase deadline */

int main(void)
{
   now_us = 0;
   teletext_emulator_init(IRQ_SLOT, 0x10);

   step();                                  /* connect channel 0 */
   assert(recv_cb != NULL && "recv callback registered");

   wr_cb[0x10](0x04u | 0x08u);              /* enable + interrupt enable, ch0 */

   static uint8_t field[16*42];
   for (int i=0;i<16;i++)
      for (int j=0;j<42;j++)
         field[i*42+j] = (uint8_t)(0x31 + i + j);
   struct pbuf pb = { .next=NULL, .payload=field, .len=sizeof field, .tot_len=sizeof field };
   recv_cb(tcp_arg_saved, &the_pcb, &pb, ERR_OK);

   bool saw_fsync=false, saw_dew=false, saw_int=false;
   for (int k=0;k<8;k++){
      step();
      if (mem[0x10] & 0x10u) saw_fsync=true;
      if (mem[0x10] & 0x20u) saw_dew=true;
      if (mem[0x10] & 0x80u) saw_int=true;
   }
   assert(saw_fsync && "FSYNC bit asserted");
   assert(saw_dew   && "DEW bit asserted");
   assert(saw_int   && "INT bit asserted at end of DEW");
   assert(irq_line() && "nIRQ asserted (ints enabled + INT)");

   wr_cb[0x11](0x00);                       /* load row counter 0 */
   uint8_t first = mem[0x12]; rd_cb[0x12](0);
   assert(first == 0x27u && "row marker byte");
   for (int j=0;j<42;j++){
      uint8_t b = mem[0x12]; rd_cb[0x12](0);
      assert(b == (uint8_t)(0x31 + 0 + j) && "row 0 data byte matches t42");
   }

   rd_cb[0x13](0);                          /* clear status (+03) */
   assert((mem[0x10] & 0x80u) == 0u && "INT cleared by +03");
   assert(!irq_line() && "nIRQ released after clear");

   char buf[512]; teletext_status_text(buf, sizeof buf);
   assert(strstr(buf,"ch0") && strstr(buf,"connected"));

   printf("all teletext tests passed\n");
   return 0;
}
