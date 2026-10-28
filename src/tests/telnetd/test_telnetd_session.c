/*
   Host tests for telnetd.c, the session: the whole file, against a fake TCP (session/lwip/, one
   pcb per connection, recording what the client is sent), a fake serial redirect (the Beeb's
   output to read, the keys it is given, the hook reports) and a fake Pi1MHz.cfg.  Each scenario
   drives the callbacks lwIP and the main loop would: connect, client data, client close, and the
   poll, a millisecond at a time.

   With TRACE set it prints every event in order - what each client is sent, every key given to
   the Beeb, every redirect request, every close - so two versions of telnetd.c can be compared
   step for step (run_tests.sh: TELNETD_TRACE_BASE=<rev>).  A refactor must leave it identical.
*/
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Pi1MHz.h"
#include "config.h"
#include "serial_redirect.h"
#include "telnetd.h"
#include "telnetd_proto.h"
#include "lwip/tcp.h"
#include "lwip/altcp.h"
#include "lwip/priv/tcp_priv.h"
#include "wifi/wifi.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
   printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bool trace;
static void step(const char *what) { if (trace) printf("-- %s\n", what); }

static void tr_bytes(const char *tag, int id, const uint8_t *p, size_t n)
{
   if (!trace)
      return;
   printf("  %s%d ", tag, id);
   for (size_t i = 0u; i < n; i++) {
      if (p[i] >= 32u && p[i] < 127u && p[i] != '\\')
         putchar(p[i]);
      else
         printf("\\%02x", p[i]);
   }
   putchar('\n');
}

/* telnetd.c's calls into telnetd_proto.c that leave nothing in the output by themselves, made
   visible in the trace (run_tests.sh links with --wrap). */
void __real_tnd_out_scan_merge(tnd_out_t *o);
void __wrap_tnd_out_scan_merge(tnd_out_t *o)
{
   if (trace) printf("  scan merged\n");
   __real_tnd_out_scan_merge(o);
}

/* ---- Pi1MHz and config --------------------------------------------------- */
uint32_t Pi1MHz_now_us;
static func_ptr poll_fn;

void Pi1MHz_Register_Poll(func_ptr fn, const char *name)
{
   (void)name;
   poll_fn = fn;
}

#define CFG_MAX 16
static const char *cfg_key[CFG_MAX], *cfg_val[CFG_MAX];

static void cfg_set(const char *k, const char *v)
{
   for (int i = 0; i < CFG_MAX; i++)
      if (cfg_key[i] == NULL || strcmp(cfg_key[i], k) == 0) {
         cfg_key[i] = k;
         cfg_val[i] = v;
         return;
      }
}

const char *config_get(const char *key)
{
   for (int i = 0; i < CFG_MAX && cfg_key[i] != NULL; i++)
      if (strcmp(cfg_key[i], key) == 0)
         return cfg_val[i];
   return NULL;
}

bool config_get_flag(const char *key, bool dflt)
{
   const char *v = config_get(key);
   if (v == NULL || v[0] == '\0')
      return dflt;
   return v[0] != '0';
}

/* The network config: the greeting names the Pi by its hostname. */
static wifi_config_t wcfg;
const wifi_config_t *wifi_get_config(void) { return &wcfg; }

int config_emulator_override(const char *name, uint8_t *addr)
{
   (void)name;
   (void)addr;
   return 0;
}

/* ---- the serial redirect: the Beeb's side -------------------------------- */
static ser_beeb_report_t rep;
volatile const ser_beeb_report_t *const serial_redirect_beeb = &rep;
static bool    attached;
static uint8_t beeb_out[8192];                 /* what the Beeb printed, not read yet */
static size_t  bo_head, bo_tail;
static uint8_t keys[8192];                     /* what the Beeb was given */
static size_t  nkeys;
static size_t  room = 64u;
static unsigned n_escape, n_break, n_shift, n_drop, n_refresh;

static void beeb_prints(const char *s)
{
   size_t n = strlen(s);
   memcpy(beeb_out + bo_tail, s, n);
   bo_tail += n;
}

uint8_t serial_redirect_address(void) { return 0xCCu; }
bool    serial_redirect_telnet_attached(void) { return attached; }
bool    serial_redirect_pending(void) { return bo_head != bo_tail; }
size_t  serial_redirect_room(void) { return room; }
uint32_t serial_redirect_rx_count(void) { return (uint32_t)nkeys; }

size_t serial_redirect_read(uint8_t *dst, size_t max)
{
   size_t n = 0u;
   while (n < max && bo_head != bo_tail)
      dst[n++] = beeb_out[bo_head++];
   return n;
}

size_t serial_redirect_write(const uint8_t *src, size_t len)
{
   if (len > room)
      len = room;
   memcpy(keys + nkeys, src, len);
   nkeys += len;
   tr_bytes("K", 0, src, len);
   return len;
}

void serial_redirect_escape(void) { n_escape++; if (trace) printf("  ESCAPE\n"); }
/* The ESCAPE key: the Beeb's settings are the MOS's defaults, so it is ESCAPE, decided at once. */
static uint8_t esc_result;
void serial_redirect_escape_key(void) { n_escape++; esc_result = SER_ESC_RAISED; if (trace) printf("  ESCAPE\n"); }
uint8_t serial_redirect_escape_result(void) { uint8_t r = esc_result; esc_result = 0u; return r; }
void serial_redirect_break(uint8_t how) { n_break++; if (trace) printf("  BREAK %u\n", how); }
void serial_redirect_shift(void) { n_shift++; if (trace) printf("  SHIFT\n"); }
void serial_redirect_telnetd_refresh(void) { n_refresh++; if (trace) printf("  REFRESH\n"); }
void serial_redirect_drop_keys(uint8_t keep_after)
{
   n_drop++;
   if (trace) printf("  DROP %02x\n", keep_after);
}

static bool keys_have(const char *s)
{
   size_t n = strlen(s);
   for (size_t i = 0u; i + n <= nkeys; i++)
      if (memcmp(keys + i, s, n) == 0)
         return true;
   return false;
}

/* The ROM hooks the Beeb in: a hook report, MODE m with the cursor at col,row. */
static void beeb_hook(uint8_t m, uint8_t col, uint8_t row)
{
   attached = true;
   rep.rom = true;
   rep.mode = m;
   rep.col = col;
   rep.row = row;
   rep.mode_gen++;
}

/* ---- lwIP ---------------------------------------------------------------- */
const ip_addr_t ip_addr_any = { 0u };
#define PCB_MAX 64
static struct tcp_pcb *pcbs[PCB_MAX];
static int npcbs;

char *ip4addr_ntoa(const ip4_addr_t *a)
{
   static char s[16];
   snprintf(s, sizeof s, "%u.%u.%u.%u", (unsigned)(a->addr & 0xFFu), (unsigned)(a->addr >> 8 & 0xFFu),
            (unsigned)(a->addr >> 16 & 0xFFu), (unsigned)(a->addr >> 24));
   return s;
}

struct tcp_pcb *tcp_new_ip_type(u8_t type)
{
   (void)type;
   struct tcp_pcb *p = calloc(1u, sizeof *p);
   p->id = npcbs;
   p->sndbuf = 8192u;
   pcbs[npcbs++] = p;
   return p;
}
err_t tcp_bind(struct tcp_pcb *pcb, const ip_addr_t *ip, u16_t port) { (void)ip; pcb->port = port; return ERR_OK; }
struct tcp_pcb *tcp_listen_with_backlog(struct tcp_pcb *pcb, u8_t b) { (void)b; pcb->state = LISTEN; return pcb; }
void  tcp_arg(struct tcp_pcb *pcb, void *arg) { pcb->arg = arg; }
void  tcp_accept(struct tcp_pcb *pcb, tcp_accept_fn f) { pcb->accept = f; }
void  tcp_recv(struct tcp_pcb *pcb, tcp_recv_fn f) { pcb->recv = f; }
void  tcp_sent(struct tcp_pcb *pcb, tcp_sent_fn f) { pcb->sent = f; }
void  tcp_poll(struct tcp_pcb *pcb, tcp_poll_fn f, u8_t i) { (void)pcb; (void)f; (void)i; }
void  tcp_err(struct tcp_pcb *pcb, tcp_err_fn f) { pcb->errf = f; }
err_t tcp_output(struct tcp_pcb *pcb) { (void)pcb; return ERR_OK; }
void  tcp_recved(struct tcp_pcb *pcb, u16_t len) { pcb->recved += len; }
u16_t tcp_sndbuf(const struct tcp_pcb *pcb) { return pcb->sndbuf; }
void  tcp_nagle_disable(struct tcp_pcb *pcb) { (void)pcb; }
void  tcp_rexmit_rto(struct tcp_pcb *pcb) { (void)pcb; }
u16_t altcp_sndbuf(struct altcp_pcb *pcb) { (void)pcb; return 0u; }
err_t altcp_write(struct altcp_pcb *pcb, const void *d, u16_t len, u8_t f) { (void)pcb; (void)d; (void)len; (void)f; return ERR_VAL; }

err_t tcp_write(struct tcp_pcb *pcb, const void *d, u16_t len, u8_t flags)
{
   (void)flags;
   if (pcb->closed || pcb->aborted || pcb->got_len + len > sizeof pcb->got)
      return ERR_VAL;
   memcpy(pcb->got + pcb->got_len, d, len);
   pcb->got_len += len;
   tr_bytes(">c", pcb->id, d, len);
   return ERR_OK;
}
err_t tcp_close(struct tcp_pcb *pcb)
{
   pcb->closed = 1;
   if (trace) printf("  close c%d\n", pcb->id);
   return ERR_OK;
}
void tcp_abort(struct tcp_pcb *pcb)
{
   pcb->aborted = 1;
   if (trace) printf("  abort c%d\n", pcb->id);
}

static struct pbuf *pbuf_make(const void *d, size_t n)
{
   struct pbuf *p = malloc(sizeof *p + n);
   p->next = NULL;
   p->payload = p + 1;
   p->len = p->tot_len = (u16_t)n;
   memcpy(p->payload, d, n);
   return p;
}
u8_t pbuf_free(struct pbuf *p)
{
   u8_t n = 0u;
   while (p != NULL) {
      struct pbuf *next = p->next;
      free(p);
      p = next;
      n++;
   }
   return n;
}
void pbuf_cat(struct pbuf *h, struct pbuf *t)
{
   struct pbuf *q = h;
   for (; q->next != NULL; q = q->next)
      q->tot_len = (u16_t)(q->tot_len + t->tot_len);
   q->tot_len = (u16_t)(q->tot_len + t->tot_len);
   q->next = t;
}
struct pbuf *pbuf_free_header(struct pbuf *q, u16_t size)
{
   while (q != NULL && size >= q->len) {
      struct pbuf *next = q->next;
      size = (u16_t)(size - q->len);
      free(q);
      q = next;
   }
   return q;
}

/* ---- the client ------------------------------------------------------------ */
static struct tcp_pcb *listener(u16_t port)
{
   for (int i = 0; i < npcbs; i++)
      if (pcbs[i]->state == LISTEN && pcbs[i]->port == port)
         return pcbs[i];
   return NULL;
}

static struct tcp_pcb *connect_from(u16_t port, uint32_t ip)
{
   struct tcp_pcb *l = listener(port), *c = tcp_new_ip_type(0u);
   c->remote_ip.addr = ip;
   c->state = ESTABLISHED;
   if (trace) printf("  connect c%d from %s to %u\n", c->id, ip4addr_ntoa(&c->remote_ip), (unsigned)port);
   if (l != NULL && l->accept != NULL)
      (void)l->accept(l->arg, c, ERR_OK);
   return c;
}

static bool live(const struct tcp_pcb *c) { return !c->closed && !c->aborted; }

static void csend_n(struct tcp_pcb *c, const void *d, size_t n)
{
   tr_bytes("<c", c->id, d, n);
   if (c->recv != NULL && live(c))
      (void)c->recv(c->arg, c, pbuf_make(d, n), ERR_OK);
}
static void csend(struct tcp_pcb *c, const char *s) { csend_n(c, s, strlen(s)); }

static void cclose(struct tcp_pcb *c)
{
   if (trace) printf("  client c%d closes\n", c->id);
   if (c->recv != NULL && live(c))
      (void)c->recv(c->arg, c, NULL, ERR_OK);
}

static void tick(uint32_t us)                   /* the main loop, a pass a millisecond */
{
   for (uint32_t t = 0u; t < us; t += 1000u) {
      Pi1MHz_now_us += 1000u;
      if (poll_fn != NULL)
         poll_fn();
   }
}

/* What the client has been sent since mark (set with mark_at). */
static size_t mark;
static void mark_at(const struct tcp_pcb *c) { mark = c->got_len; }
static bool got_has(const struct tcp_pcb *c, const char *s)
{
   size_t n = strlen(s);
   for (size_t i = mark; i + n <= c->got_len; i++)
      if (memcmp(c->got + i, s, n) == 0)
         return true;
   return false;
}

static const uint8_t NAWS_80x25[] = { 255, 250, 31, 0, 80, 0, 25, 255, 240 };
static const uint8_t NAWS_40x20[] = { 255, 250, 31, 0, 40, 0, 20, 255, 240 };

#define IP(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

static struct tcp_pcb *log_in(uint32_t ip)
{
   struct tcp_pcb *c = connect_from(23u, ip);
   tick(10000u);
   csend(c, "beeb\r");
   tick(10000u);
   csend(c, "bench43\r");
   tick(10000u);
   return c;
}

int main(int argc, char **argv)
{
   trace = (argc > 1 && strcmp(argv[1], "trace") == 0);
   Pi1MHz_now_us = 1000000u;

   step("init: port 23, raw port 2323, a login");
   cfg_set("telnetd_port", "23");
   cfg_set("telnetd_raw_port", "2323");
   cfg_set("telnetd_user", "beeb");
   cfg_set("telnetd_password", "bench43");
   telnetd_init();
   telnetd_attach();
   CHECK(telnetd_enabled(), "enabled");
   CHECK(listener(23u) != NULL && listener(2323u) != NULL, "listening on both ports");
   CHECK(poll_fn != NULL, "the poll registered");
   tick(100000u);

   step("the Beeb boots with nobody connected");
   beeb_hook(7u, 0u, 0u);
   beeb_prints("Acorn MOS\r\n\r\nBASIC\r\n\r\n>");
   tick(50000u);
   CHECK(bo_head == bo_tail, "followed and dropped with nobody connected");

   step("login: wrong password, then right");
   struct tcp_pcb *c = connect_from(23u, IP(10, 0, 0, 2));
   tick(20000u);
   CHECK(got_has(c, "login: "), "the login prompt");
   CHECK(n_drop == 1u, "the last session's keys dropped");
   csend(c, "beeb\r");
   tick(20000u);
   CHECK(got_has(c, "Password: "), "asked for the password");
   csend(c, "wrong\r");
   tick(20000u);
   CHECK(got_has(c, "Login incorrect"), "a wrong password refused");
   CHECK(live(c), "but the session stays");
   tick(2100000u);                               /* the hold after a wrong password */
   mark_at(c);
   csend(c, "beeb\r");
   tick(20000u);
   csend(c, "bench43\r");
   tick(20000u);
   CHECK(got_has(c, "Pi1MHz telnet - connecting"), "logged in: connecting");
   CHECK(got_has(c, "BASIC"), "the boot banners after the login");
   CHECK(!keys_have("bench43"), "nothing of the login typed into the Beeb");

   step("a second client while the session is live");
   struct tcp_pcb *c2 = connect_from(23u, IP(10, 0, 0, 3));
   CHECK(c2->closed && strstr(c2->got, "in use by another session") != NULL, "busy: refused");

   step("connecting ends: the window size, then the screen");
   beeb_prints("Z");
   tick(20000u);
   CHECK(bo_head != bo_tail, "the Beeb's output waits while connecting");
   csend_n(c, NAWS_80x25, sizeof NAWS_80x25);
   mark_at(c);
   tick(1200000u);
   CHECK(got_has(c, "\x1b[2J") || got_has(c, "\x1b[H"), "the terminal cleared and drawn");
   CHECK(keys_have("\xff"), "the ROM asked for the screen (SCAN_KEY)");

   step("the Beeb prints, the client types");
   mark_at(c);
   beeb_prints("HELLO");
   tick(20000u);
   CHECK(got_has(c, "HELLO"), "the Beeb's output reaches the client");
   csend(c, "PRINT 6*7\r");
   tick(20000u);
   CHECK(keys_have("PRINT 6*7\r"), "the keys reach the Beeb");

   step("line mode (Linux telnet's \"mode line\"): the client echoes each line itself");
   static const uint8_t DONT_ECHO[] = { 255, 254, 1 }, DO_ECHO[] = { 255, 253, 1 };
   csend_n(c, DONT_ECHO, sizeof DONT_ECHO);
   tick(20000u);                                 /* the mode change has its own redraw */
   mark_at(c);
   csend(c, "PRINT 1\r\n");
   tick(20000u);
   CHECK(got_has(c, "\x1b[2J"), "a line sent in line mode: the terminal drawn again, so the Beeb's echo is not a row low");
   mark_at(c);
   csend_n(c, DO_ECHO, sizeof DO_ECHO);
   tick(20000u);
   mark_at(c);
   csend(c, "PRINT 2\r");
   tick(20000u);
   CHECK(!got_has(c, "\x1b[2J"), "character mode again: RETURN draws nothing");

   step("ESCAPE, F11, F12 and Page Down");
   csend(c, "\x1b");
   tick(400000u);
   CHECK(n_escape == 1u, "a lone ESC is ESCAPE");
   size_t k0 = nkeys;
   csend(c, "\x1b[23~");
   tick(20000u);
   CHECK(nkeys == k0 + 1u && keys[k0] == 0xFFu, "F11 asks the ROM for the screen");
   csend(c, "\x1b[24~");
   tick(20000u);
   CHECK(n_break == 1u, "F12 is BREAK");
   csend(c, "\x1b[6~");
   tick(20000u);
   CHECK(n_shift == 1u, "Page Down is SHIFT");

   step("the Beeb resets (the ROM reports it), and comes back");
   mark_at(c);
   rep.reset_gen++;
   tick(20000u);
   CHECK(got_has(c, "\x1b[2J"), "the terminal cleared at a reset");
   beeb_hook(7u, 0u, 0u);
   beeb_prints("Acorn MOS\r\n>");
   tick(50000u);
   CHECK(got_has(c, "Acorn MOS"), "the new banner shown");

   step("a ROM filing system operation: the screen read once it stops");
   {
      size_t k1 = nkeys;
      rep.unseen_gen++;
      tick(50000u);
      rep.unseen_gen++;
      tick(50000u);
      CHECK(nkeys == k1, "not while the reports come");
      tick(100000u);
      CHECK(nkeys == k1 + 1u && keys[k1] == 0xFFu, "then the ROM is asked for the screen");
   }

   step("a resize redraws");
   mark_at(c);
   csend_n(c, NAWS_40x20, sizeof NAWS_40x20);
   tick(20000u);
   CHECK(c->got_len > mark, "drawn again");

   step("the Beeb goes away");
   mark_at(c);
   attached = false;
   tick(20000u);
   CHECK(got_has(c, "BBC not attached"), "said once");
   attached = true;
   tick(20000u);

   step("the client closes with keys still to go in");
   room = 0u;
   csend(c, "RUN\r");
   tick(20000u);
   cclose(c);
   tick(20000u);
   CHECK(live(c), "kept while its keys wait");
   room = 64u;
   tick(1200000u);
   CHECK(keys_have("RUN\r"), "the keys went in");
   CHECK(c->closed, "then the session closed");

   step("login times out");
   struct tcp_pcb *c3 = connect_from(23u, IP(10, 0, 0, 4));
   tick(31000000u);
   CHECK(strstr(c3->got, "Login timed out") != NULL && c3->closed, "closed after 30 s");

   step("three wrong passwords lock the address out");
   struct tcp_pcb *c4 = connect_from(23u, IP(10, 0, 0, 5));
   for (int i = 0; i < 3; i++) {
      tick(2100000u);
      csend(c4, "beeb\r");
      tick(20000u);
      mark_at(c4);
      csend(c4, i < 2 ? "nope\r" : "nope\rbeeb\rnope\r");   /* the third with more typed behind it */
      tick(20000u);
   }
   tick(100000u);
   CHECK(c4->closed, "the third closes the session");
   {
      unsigned n = 0u;
      for (size_t i = mark; i + 15u <= c4->got_len; i++)
         n += memcmp(c4->got + i, "Login incorrect", 15u) == 0;
      CHECK(n == 1u, "what was typed after the third is not taken as another login (%u)", n);
   }
   struct tcp_pcb *c5 = connect_from(23u, IP(10, 0, 0, 5));
   CHECK(c5->closed && strstr(c5->got, "too many wrong passwords") != NULL, "and the address is refused");

   step("the raw port: no telnet, teletype");
   tick(2100000u);
   struct tcp_pcb *r = connect_from(2323u, IP(10, 0, 0, 6));
   tick(20000u);
   CHECK((uint8_t)r->got[0] != 255u, "no telnet negotiation");
   csend(r, "beeb\n");
   tick(20000u);
   csend(r, "bench43\n");
   tick(1200000u);
   mark_at(r);
   beeb_prints("A\xff" "B");
   tick(20000u);
   CHECK(got_has(r, "A\xff" "B"), "bytes as they are, IAC not doubled");
   cclose(r);
   tick(1200000u);
   CHECK(r->closed, "closed");

   step("a session stalled on the client is dropped");
   struct tcp_pcb *s = log_in(IP(10, 0, 0, 7));
   tick(1200000u);
   s->snd_queuelen = 3u;                         /* sent, never acknowledged */
   beeb_prints("X");
   tick(31000000u);
   CHECK(s->aborted, "aborted after 30 s without progress");

   step("status");
   {
      char b[512];
      telnetd_status(b, sizeof b);
      if (trace) printf("  status: %s\n", b);
      CHECK(strstr(b, "port 23") != NULL, "the status line");
   }

   for (int i = 0; i < npcbs; i++)
      free(pcbs[i]);
   printf("%d checks, %d failures\n", checks, failures);
   return failures != 0;
}
