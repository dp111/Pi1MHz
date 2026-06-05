/* Lockstep harness: drives the REAL econet_emulator.c/econet_aun.c/
 * econet_config.c on the host via a line protocol on stdin/stdout.
 *   S <cmdline>      set the fake cmdline.txt contents
 *   R <0|1>          set wifi address_ready
 *   P <addr> <val>   poke JIM byte (hex)
 *   G <addr>         peek JIM byte -> "V <val>"
 *   C <page>         FRED &FCAA write: command + one poll -> "K <result>"
 *   U <ip> <port> <hex>  inject inbound UDP datagram
 *   L                run econet_poll once (retries/timeouts)
 *   Q                quit
 * Outbound datagrams are reported as: O <ip> <port> <hex>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "Pi1MHz.h"
#include "lwip/udp.h"
#include "wifi/wifi_lwip.h"
#include "econet_emulator.h"

/* ---- stub state ---- */
static Pi1MHz_t pi;
Pi1MHz_t *const Pi1MHz = &pi;
const ip_addr_t ip_addr_any = {0};
static wifi_lwip_context_t wctx;
static char cmdline[1024];
static func_ptr poll_fn;
static struct udp_pcb the_pcb;
static int pcb_in_use;
static uint8_t last_result;
static int irq_line;
static uint8_t tube_r3_in[8704];   /* bytes the parasite would feed (tx) */
static int     tube_r3_in_n, tube_r3_in_pos;
static uint8_t tube_r3_out[8704];  /* bytes streamed to the parasite (rx) */
static int     tube_r3_out_n;
static uint8_t tube_r4[64]; static int tube_r4_n;
static int have_result;

const wifi_lwip_context_t *wifi_lwip_get_context(void) { return &wctx; }
void Pi1MHz_Register_Poll(func_ptr f) { poll_fn = f; }
void Pi1MHz_SetnIRQ(bool irq) { irq_line = irq; }
void Pi1MHz_SetnIRQ_src(uint8_t src, bool a)
{ static uint8_t mask; if (a) mask |= (uint8_t)(1u<<src); else mask &= (uint8_t)~(1u<<src);
  irq_line = mask != 0; }
bool wifi_debug_enabled(void) { return false; }
void wifi_debug_printf(const char *format, ...) { (void)format; }
void Pi1MHz_MemoryWrite(uint32_t addr, uint8_t data)
{ pi.Memory[addr & 0x1ff] = data; if ((addr & 0xff) == 0xaa) { last_result = data; have_result = 1; } }

uint32_t RPI_GetSystemTime(void)
{ struct timeval tv; gettimeofday(&tv, NULL); return (uint32_t)(tv.tv_sec*1000000ull + tv.tv_usec); }

char *get_cmdline_prop(const char *prop)
{
   static char ret[256];
   char *p = strstr(cmdline, prop);
   if (!p) return NULL;
   p += strlen(prop);
   if (*p != '=') return NULL;
   p++;
   size_t i = 0;
   while (*p && *p != ' ' && i < sizeof ret - 1) ret[i++] = *p++;
   ret[i] = 0;
   return ret;
}

/* ---- pbuf/udp stubs ---- */
struct pbuf *pbuf_alloc(int layer, u16_t len, int type)
{ (void)layer; (void)type;
  struct pbuf *p = malloc(sizeof *p + len);
  p->tot_len = len; p->payload = p + 1; p->next = NULL; return p; }
err_t pbuf_take(struct pbuf *p, const void *d, u16_t len)
{ memcpy(p->payload, d, len); return ERR_OK; }
uint8_t pbuf_free(struct pbuf *p) { free(p); return 1; }
u16_t pbuf_copy_partial(const struct pbuf *p, void *dst, u16_t len, u16_t off)
{ memcpy(dst, (const uint8_t*)p->payload + off, len); return len; }

struct udp_pcb *udp_new(void)
{ if (pcb_in_use) return NULL; pcb_in_use = 1; memset(&the_pcb,0,sizeof the_pcb); return &the_pcb; }
err_t udp_bind(struct udp_pcb *pcb, const ip_addr_t *ip, uint16_t port)
{ (void)ip; pcb->port = port; return ERR_OK; }
void udp_recv(struct udp_pcb *pcb, udp_recv_fn cb, void *arg) { pcb->cb = cb; pcb->arg = arg; }
void udp_remove(struct udp_pcb *pcb) { (void)pcb; pcb_in_use = 0; }
err_t udp_sendto(struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *ip, uint16_t port)
{
   (void)pcb;
   printf("O %08x %u ", ip->addr, port);
   for (u16_t i = 0; i < p->tot_len; i++) printf("%02x", ((uint8_t*)p->payload)[i]);
   printf("\n"); fflush(stdout);
   return ERR_OK;
}

int main(void)
{
   static char line[20000];
   pi.JIM_ram = calloc(1, 32u*1024u*1024u);
   pi.JIM_ram_size = 2;            /* DISC_RAM_BASE == 0 */
   econet_emulator_init();

   while (fgets(line, sizeof line, stdin)) {
      char *nl = strchr(line, '\n'); if (nl) *nl = 0;
      switch (line[0]) {
      case 'S': snprintf(cmdline, sizeof cmdline, "%s", line + 2); puts("K 0"); break;
      case 'R': wctx.address_ready = line[2] == '1'; wctx.netif_added = wctx.address_ready;
                wctx.netif.ip = 0x1401a8c0; wctx.netif.mask = 0x00ffffff; puts("K 0"); break;
      case 'P': { unsigned a, v; sscanf(line+2, "%x %x", &a, &v); pi.JIM_ram[a] = (uint8_t)v; puts("K 0"); break; }
      case 'G': { unsigned a; sscanf(line+2, "%x", &a); printf("V %02x\n", pi.JIM_ram[a]); break; }
      case 'C': { unsigned pg; sscanf(line+2, "%x", &pg);
                  uint32_t cp = DISC_RAM_BASE | 0xFF0000u | (uint32_t)(pg << 8);
                  have_result = 0;
                  econet_emulator_command(cp, 0xaa);
                  poll_fn();                       /* main-loop turn executes it */
                  printf("K %02x\n", have_result ? last_result : 0xee); break; }
      case 'U': { unsigned ip, port; static char hex[18000]; static uint8_t buf[8500]; uint32_t n = 0;
                  sscanf(line+2, "%x %u %s", &ip, &port, hex);
                  for (char *h = hex; h[0] && h[1]; h += 2) { unsigned b; sscanf(h, "%2x", &b); buf[n++] = (uint8_t)b; }
                  if (the_pcb.cb) { struct pbuf *p = pbuf_alloc(0, (u16_t)n, 0);
                     memcpy(p->payload, buf, n);
                     ip_addr_t a; a.addr = ip;
                     the_pcb.cb(the_pcb.arg, &the_pcb, p, &a, (u16_t)port); }
                  puts("K 0"); break; }
      case 'L': poll_fn(); puts("K 0"); break;
      case 'I': printf("K %02x\n", irq_line); break;
      case '3': /* '3 <hex>' : preload parasite tx bytes for R3 reads */
                { char hx2[18000]; sscanf(line+2,"%s",hx2); tube_r3_in_n=0;
                  for(char*h=hx2;h[0]&&h[1];h+=2){unsigned b;sscanf(h,"%2x",&b);
                  tube_r3_in[tube_r3_in_n++]=(uint8_t)b;} tube_r3_in_pos=0;
                  tube_r3_out_n=0; tube_r4_n=0; puts("K 0"); break; }
      case 'o': /* 'o' : dump R3 bytes streamed out (rx Tube) */
                printf("K ");
                for(int i=0;i<tube_r3_out_n;i++) printf("%02x",tube_r3_out[i]);
                printf("\n"); break;
      case '4': /* '4' : dump R4 claim stream */
                printf("K ");
                for(int i=0;i<tube_r4_n;i++) printf("%02x",tube_r4[i]);
                printf("\n"); break;
      case 'F': printf("K %02x\n", pi.Memory[0x88]); break;
      case 'Q': return 0;
      default: puts("K ff"); break;
      }
      fflush(stdout);
   }
   return 0;
}
