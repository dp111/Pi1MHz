/* teletext_emulator.c - Acorn Teletext Adapter (ATS) for Pi1MHz.
 *
 * A port of BeebEm's Teletext.cpp to the Pi1MHz bus interface. The BBC
 * sees the adapter at FRED &FC10-&FC13:
 *
 *   Offset   Read              Write
 *   +0       Status register   Control latch
 *   +1       (undefined)       Load row counter
 *   +2       Next data byte    Next data byte   (column auto-increments)
 *   +3       Clear status      Clear status
 *
 * Status bits: 0-3 link settings (float high, 0x0f), 4 FSYNC, 5 DEW
 * (high during the teletext portion of the field), 6 DOR (data overrun:
 * status not cleared before DEW), 7 INT (trailing edge of DEW).
 *
 * Control latch: 0-1 channel select, 2 teletext enable, 3 interrupt
 * enable, 4 AFC enable, 5 spare.
 *
 * The broadcast stream is sourced over TCP as raw "t42" data: 16 lines
 * of 42 bytes per field, ~50 fields/s, exactly the wire format BeebEm
 * consumes, so existing teletext servers work unchanged. Up to four
 * channels (CEEFAX 1-4) are configured from cmdline.txt. A faithful
 * 50 Hz field state machine (FSYNC -> DEW -> INT) is driven from the
 * main-loop poll and raises the teletext interrupt on nIRQ.
 */

#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "Pi1MHz.h"
#include "rpi/asm-helpers.h"
#include "rpi/info.h"
#include "rpi/systimer.h"
#include "teletext_emulator.h"
#include "config.h"

#include "wifi/wifi.h"
#include "wifi/wifi_lwip.h"

#include "lwip/tcp.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"

#define TTX_CHANNELS        4u
#define TTX_ROWS            16u
#define TTX_ROW_BYTES       42u
#define TTX_ROW_STRIDE      64u   /* per-row storage the BBC reads from   */
#define TTX_FIELD_BYTES     (TTX_ROWS * TTX_ROW_BYTES)   /* 672 bytes      */
#define TTX_RING_FIELDS     6u
#define TTX_RING_BYTES      (TTX_FIELD_BYTES * TTX_RING_FIELDS)

#define TELETEXT_DEFAULT_PORT 19761u

/* status register bits */
#define TTX_ST_LINKS   0x0fu
#define TTX_ST_FSYNC   0x10u
#define TTX_ST_DEW     0x20u
#define TTX_ST_DOR     0x40u
#define TTX_ST_INT     0x80u
#define TTX_ST_CLEARABLE (TTX_ST_INT | TTX_ST_DOR | TTX_ST_FSYNC)  /* 0xD0 */

/* control latch bits */
#define TTX_CTL_CHAN   0x03u
#define TTX_CTL_ENABLE 0x04u
#define TTX_CTL_INTEN  0x08u

/* field timing, microseconds (BeebEm 2 MHz cycle counts / 2) */
#define TTX_US_FSYNC_EVEN  290u
#define TTX_US_FSYNC_ODD   320u
#define TTX_US_DEW         1088u
#define TTX_US_FIELD_EVEN  18592u
#define TTX_US_FIELD_ODD   18622u
#define TTX_US_RECONNECT   2000000u

typedef enum { TTX_FIELD = 0, TTX_FSYNC = 1, TTX_DEW = 2 } ttx_phase_t;

typedef struct {
   uint32_t        ip_be;          /* 0 = channel unconfigured           */
   uint16_t        port;
   struct tcp_pcb *pcb;            /* NULL = not connected/connecting    */
   bool            connected;
   uint32_t        reconnect_at_us;
   uint8_t         buf[TTX_RING_BYTES];
   uint32_t        head;           /* write index                        */
   uint32_t        tail;           /* read index                         */
   uint32_t        count;          /* bytes held                         */
   bool            rx_seen;        /* logged "receiving data" once        */
} ttx_chan_t;

static uint8_t  TTX_ADDR;
static uint8_t  IRQ_NUM;

// Cleared from the FIQ callback (ttx_clear) while the poll updates it:
// volatile so every load/store is real, and the poll's FSYNC/DOR updates
// run with FIQ masked. The DEW bit lives in ttx_status_DEW, owned by the
// poll alone (DEW is not a clearable bit), and is merged at write-out.
static volatile uint8_t ttx_status = TTX_ST_LINKS;
static uint8_t  ttx_status_DEW = 0u;
static bool     ttx_ints_enabled;
static bool     ttx_enable;
static uint8_t  ttx_channel;
static uint8_t  ttx_row_ptr;
static uint8_t  ttx_col_ptr;
static uint8_t  ttx_row[TTX_ROWS][TTX_ROW_STRIDE];

static ttx_phase_t ttx_phase = TTX_FIELD;
static uint32_t    ttx_next_us;
static uint8_t     ttx_field_parity;

static ttx_chan_t  ttx_chan[TTX_CHANNELS];
static bool        ttx_configured;
static bool        ttx_debug;
static bool        ttx_net_waiting_logged;

#define TTX_LOG(...) do { if (ttx_debug) {LOG_DEBUG(__VA_ARGS__); } } while (0)

/* ---- ring buffer (per channel) -------------------------------------------*/

static void ring_push(ttx_chan_t *c, const uint8_t *d, uint32_t len)
{
   if (len >= TTX_RING_BYTES) {        /* keep only the newest bytes */
      d  += len - TTX_RING_BYTES;
      len = TTX_RING_BYTES;
   }
   if (c->count + len > TTX_RING_BYTES) {   /* drop oldest to make room */
      uint32_t drop = c->count + len - TTX_RING_BYTES;
      c->tail   = (c->tail + drop) % TTX_RING_BYTES;
      c->count -= drop;
   }
   for (uint32_t i = 0u; i < len; i++) {
      c->buf[c->head] = d[i];
      c->head = (c->head + 1u) % TTX_RING_BYTES;
   }
   c->count += len;
}

static bool ring_pop_field(ttx_chan_t *c, uint8_t out[TTX_FIELD_BYTES])
{
   if (c->count < TTX_FIELD_BYTES)
      return false;
   for (uint32_t i = 0u; i < TTX_FIELD_BYTES; i++) {
      out[i]  = c->buf[c->tail];
      c->tail = (c->tail + 1u) % TTX_RING_BYTES;
   }
   c->count -= TTX_FIELD_BYTES;
   return true;
}

/* ---- adapter register helpers --------------------------------------------*/

static bool is_channel_open(uint8_t ch)
{
   return ch < TTX_CHANNELS && ttx_chan[ch].connected;
}

static void ttx_status_write(void)
{
   Pi1MHz_MemoryWrite((uint32_t)TTX_ADDR, ttx_status | ttx_status_DEW);
}

static void ttx_preload_data(void)
{
   uint8_t b = (ttx_row_ptr < TTX_ROWS) ? ttx_row[ttx_row_ptr][ttx_col_ptr & 0x3fu] : 0u;
   Pi1MHz_MemoryWrite((uint32_t)(TTX_ADDR + 2u), b);
}

static void ttx_update_irq(void)
{
   if (ttx_ints_enabled && (ttx_status & TTX_ST_INT) != 0u)
      Pi1MHz_nIRQ_ASSERT(IRQ_NUM);
   else
      Pi1MHz_nIRQ_CLEAR(IRQ_NUM);
}

/* ---- FRED callbacks (run in FIQ context) ---------------------------------*/

static void ttx_w_control(unsigned int gpio)
{
   uint8_t v = (uint8_t)GET_DATA(gpio);
   ttx_ints_enabled = (v & TTX_CTL_INTEN) != 0u;
   ttx_enable       = (v & TTX_CTL_ENABLE) != 0u;
   ttx_channel      = (uint8_t)(v & TTX_CTL_CHAN);
   ttx_update_irq();
}

static void ttx_w_row(unsigned int gpio)
{
   ttx_row_ptr = (uint8_t)GET_DATA(gpio);
   ttx_col_ptr = 0u;
   ttx_preload_data();
}

static void ttx_r_data(unsigned int gpio)
{
   (void)gpio;                                  /* value already on the bus */
   ttx_col_ptr = (uint8_t)((ttx_col_ptr + 1u) & 0x3fu);
   ttx_preload_data();
}

static void ttx_w_data(unsigned int gpio)
{
   uint8_t v = (uint8_t)GET_DATA(gpio);
   if (ttx_row_ptr < TTX_ROWS)
      ttx_row[ttx_row_ptr][ttx_col_ptr & 0x3fu] = v;
   ttx_col_ptr = (uint8_t)((ttx_col_ptr + 1u) & 0x3fu);
   ttx_preload_data();
}

static void ttx_clear(unsigned int gpio)
{
   (void)gpio;
   ttx_status &= (uint8_t)~TTX_ST_CLEARABLE;
   ttx_status_write();
   Pi1MHz_nIRQ_CLEAR(IRQ_NUM);
}

/* ---- network: lwIP raw TCP client ----------------------------------------*/

static void ttx_disconnect(ttx_chan_t *c, bool from_err)
{
   if (!from_err && c->pcb != NULL) {
      tcp_arg(c->pcb, NULL);
      tcp_recv(c->pcb, NULL);
      tcp_err(c->pcb, NULL);
      if (tcp_close(c->pcb) != ERR_OK)
         tcp_abort(c->pcb);
   }
   c->pcb             = NULL;
   c->connected       = false;
   c->rx_seen         = false;
   c->head = c->tail  = 0u;
   c->count           = 0u;
   c->reconnect_at_us = RPI_GetSystemTime() + TTX_US_RECONNECT;
}

static void ttx_tcp_err(void *arg, err_t err)
{
   if (arg != NULL) {
      TTX_LOG("TELETEXT: connection error %d, will retry\r\n", (int)err);
      ttx_disconnect((ttx_chan_t *)arg, true);   /* pcb already freed by lwIP */
   }
}

static err_t ttx_tcp_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
   ttx_chan_t *c = (ttx_chan_t *)arg;
   if (c == NULL)
      return ERR_OK;
   if (p == NULL || err != ERR_OK) {            /* remote closed / error */
      if (p != NULL)
         pbuf_free(p);
      ttx_disconnect(c, false);
      return ERR_OK;
   }
   for (struct pbuf *q = p; q != NULL; q = q->next)
      ring_push(c, (const uint8_t *)q->payload, (uint32_t)q->len);
   if (!c->rx_seen) {
      c->rx_seen = true;
      TTX_LOG("TELETEXT: receiving data (%u bytes)\r\n", (unsigned)p->tot_len);
   }
   tcp_recved(tpcb, p->tot_len);
   pbuf_free(p);
   return ERR_OK;
}

static err_t ttx_tcp_connected(void *arg, struct tcp_pcb *tpcb, err_t err)
{
   (void)tpcb;
   ttx_chan_t *c = (ttx_chan_t *)arg;
   if (c == NULL)
      return ERR_OK;
   if (err != ERR_OK)
      return err;                               /* err callback will fire */
   c->connected = true;
   TTX_LOG("TELETEXT: channel connected\r\n");
   return ERR_OK;
}

static void ttx_connect(ttx_chan_t *c)
{
   c->pcb = tcp_new();
   if (c->pcb == NULL) {
      TTX_LOG("TELETEXT: tcp_new() failed\r\n");
      c->reconnect_at_us = RPI_GetSystemTime() + TTX_US_RECONNECT;
      return;
   }
   tcp_arg(c->pcb, c);
   tcp_recv(c->pcb, ttx_tcp_recv);
   tcp_err(c->pcb, ttx_tcp_err);

   ip_addr_t ip;
   ip_addr_set_ip4_u32(&ip, c->ip_be);
   TTX_LOG("TELETEXT: connecting to %u.%u.%u.%u:%u\r\n",
           (unsigned)(c->ip_be & 0xffu), (unsigned)((c->ip_be >> 8) & 0xffu),
           (unsigned)((c->ip_be >> 16) & 0xffu), (unsigned)((c->ip_be >> 24) & 0xffu),
           (unsigned)c->port);
   err_t rc = tcp_connect(c->pcb, &ip, c->port, ttx_tcp_connected);
   if (rc != ERR_OK) {
      TTX_LOG("TELETEXT: tcp_connect() failed rc=%d\r\n", (int)rc);
      tcp_abort(c->pcb);                         /* err callback cleans up */
   }
}

static void ttx_net_poll(void)
{
   const wifi_lwip_context_t *net = wifi_lwip_get_context();
   if (net == NULL || !net->address_ready) {
      if (!ttx_net_waiting_logged) {
         ttx_net_waiting_logged = true;
         TTX_LOG("TELETEXT: waiting for network address\r\n");
      }
      return;
   }
   ttx_net_waiting_logged = false;

   uint32_t now = RPI_GetSystemTime();
   for (uint32_t i = 0u; i < TTX_CHANNELS; i++) {
      ttx_chan_t *c = &ttx_chan[i];
      if (c->ip_be == 0u || c->pcb != NULL)
         continue;
      if ((int32_t)(now - c->reconnect_at_us) < 0)
         continue;
      ttx_connect(c);
   }
}

/* ---- field state machine + data delivery ---------------------------------*/

static void ttx_load_field(void)
{
   if (!is_channel_open(ttx_channel))
      return;

   uint8_t field[TTX_FIELD_BYTES];
   if (!ring_pop_field(&ttx_chan[ttx_channel], field))
      return;                                    /* underrun: keep last page */
   if (!ttx_enable)
      return;

   for (uint32_t i = 0u; i < TTX_ROWS; i++) {
      const uint8_t *line = &field[i * TTX_ROW_BYTES];
      if (line[0] != 0u) {                       /* non-empty line */
         ttx_row[i][0] = 0x27u;                  /* row marker the ROM expects */
         memcpy(&ttx_row[i][1], line, TTX_ROW_BYTES);
      }
   }
   ttx_row_ptr = 0u;
   ttx_col_ptr = 0u;
   ttx_preload_data();
}

static void teletext_poll(void)
{
   ttx_net_poll();

   uint32_t now = RPI_GetSystemTime();
   if ((int32_t)(now - ttx_next_us) < 0)
      return;

   switch (ttx_phase) {
   default:
   case TTX_FIELD:                               /* -> FSYNC */
      ttx_phase = TTX_FSYNC;
      if (is_channel_open(ttx_channel)) {
         /* A ttx_clear() FIQ landing inside this RMW would be undone,
            leaving INT stale-set and falsely latching DOR at DEW */
         unsigned int cpsr = _disable_interrupts_cspr();
         ttx_status |= TTX_ST_FSYNC;
         _restore_cpsr(cpsr);
      }
      ttx_next_us = now + ((ttx_field_parity & 1u) ? TTX_US_FSYNC_ODD : TTX_US_FSYNC_EVEN);
      break;

   case TTX_FSYNC:                               /* -> DEW */
      ttx_phase = TTX_DEW;
      if (is_channel_open(ttx_channel)) {
         /* DOR latches the INT state that was not cleared before DEW.
            Masked so a ttx_clear() FIQ cannot be undone by the
            store-back (the Beeb would see INT+DOR it just cleared).
            The DEW->FIELD |= INT needs no mask: a clear lost there is
            superseded by the new INT firing the handler again. */
         unsigned int cpsr = _disable_interrupts_cspr();
         ttx_status = (uint8_t)((ttx_status & ~TTX_ST_DOR) |
                                ((ttx_status & TTX_ST_INT) >> 1));
         _restore_cpsr(cpsr);
         ttx_status_DEW = TTX_ST_DEW;
      }
      ttx_load_field();
      ttx_next_us = now + TTX_US_DEW;
      break;

   case TTX_DEW:                                 /* -> FIELD */
      ttx_phase = TTX_FIELD;
      ttx_status_DEW = 0u;
      if (is_channel_open(ttx_channel)) {
         ttx_status |= TTX_ST_INT;               /* trailing edge of DEW */
         if (ttx_ints_enabled)
            Pi1MHz_nIRQ_ASSERT(IRQ_NUM);
      }
      ttx_field_parity ^= 1u;
      ttx_next_us = now + ((ttx_field_parity & 1u) ? TTX_US_FIELD_ODD : TTX_US_FIELD_EVEN);
      break;
   }

   ttx_status_write();
}

/* ---- configuration -------------------------------------------------------*/

static bool ttx_parse_endpoint(const char *s, uint32_t *ip_be, uint16_t *port)
{
   if (s == NULL)
      return false;

   uint32_t oct[4] = { 0u, 0u, 0u, 0u };
   uint32_t idx = 0u, val = 0u;
   bool any = false;
   const char *p = s;

   for (;;) {
      char ch = *p;
      if (ch >= '0' && ch <= '9') {
         val = val * 10u + (uint32_t)(ch - '0');
         if (val > 255u)
            return false;
         any = true;
         p++;
      } else if (ch == '.') {
         if (!any || idx >= 3u)
            return false;
         oct[idx++] = val;
         val = 0u;
         any = false;
         p++;
      } else {
         break;
      }
   }
   if (idx != 3u || !any)
      return false;
   oct[3] = val;

   uint16_t pt = (uint16_t)TELETEXT_DEFAULT_PORT;
   if (*p == ':') {
      uint32_t pv = 0u;
      bool pany = false;
      p++;
      while (*p >= '0' && *p <= '9') {
         pv = pv * 10u + (uint32_t)(*p - '0');
         if (pv > 65535u)
            return false;
         pany = true;
         p++;
      }
      if (!pany)
         return false;
      pt = (uint16_t)pv;
   }
   if (*p != '\0')
      return false;

   *ip_be = oct[0] | (oct[1] << 8) | (oct[2] << 16) | (oct[3] << 24);
   *port  = pt;
   return true;
}

static void ttx_configure(void)
{
   static const char *const names[TTX_CHANNELS] = {
      "teletext_server1", "teletext_server2",
      "teletext_server3", "teletext_server4"
   };
   ttx_debug = config_get("teletext_debug") != NULL;

   for (uint32_t i = 0u; i < TTX_CHANNELS; i++) {
      uint32_t ip_be = 0u;
      uint16_t port  = 0u;
      if (ttx_parse_endpoint(config_get(names[i]), &ip_be, &port)) {
         ttx_chan[i].ip_be = ip_be;
         ttx_chan[i].port  = port;
         TTX_LOG("TELETEXT: channel %u -> %u.%u.%u.%u:%u\r\n",
                 (unsigned)i,
                 (unsigned)(ip_be & 0xffu), (unsigned)((ip_be >> 8) & 0xffu),
                 (unsigned)((ip_be >> 16) & 0xffu), (unsigned)((ip_be >> 24) & 0xffu),
                 (unsigned)port);
      }
   }
}

/* ---- diagnostics ---------------------------------------------------------*/

void teletext_status_text(char *buf, size_t size)
{
   size_t n = 0u;
   #define TTX_APPEND(...) do { if (n < size) \
      n += (size_t)snprintf(buf + n, size - n, __VA_ARGS__); } while (0)

   TTX_APPEND("enable       %s\n", ttx_enable ? "yes" : "no");
   TTX_APPEND("channel      %u\n", (unsigned)ttx_channel);
   TTX_APPEND("ints         %s  status=&%02X\n",
              ttx_ints_enabled ? "on" : "off", (unsigned)ttx_status);
   for (uint32_t i = 0u; i < TTX_CHANNELS; i++) {
      const ttx_chan_t *c = &ttx_chan[i];
      if (c->ip_be == 0u) {
         TTX_APPEND("ch%u          unconfigured\n", (unsigned)i);
      } else {
         TTX_APPEND("ch%u          %u.%u.%u.%u:%u %s  %u/%u bytes\n",
                    (unsigned)i,
                    (unsigned)(c->ip_be & 0xffu), (unsigned)((c->ip_be >> 8) & 0xffu),
                    (unsigned)((c->ip_be >> 16) & 0xffu), (unsigned)((c->ip_be >> 24) & 0xffu),
                    (unsigned)c->port, c->connected ? "connected" : "down",
                    (unsigned)c->count, (unsigned)TTX_RING_BYTES);
      }
   }
   #undef TTX_APPEND
}

/* ---- init ----------------------------------------------------------------*/

void teletext_emulator_init(uint8_t instance, uint8_t address)
{
   TTX_ADDR = address;
   IRQ_NUM  = instance;

   /* adapter register/field state resets on every BREAK; the TCP
    * connections and parsed config persist (re-connecting on each
    * BREAK would storm the server). */
   ttx_status       = TTX_ST_LINKS;
   ttx_ints_enabled = false;
   ttx_enable       = false;
   ttx_channel      = 0u;
   ttx_row_ptr      = 0u;
   ttx_col_ptr      = 0u;
   ttx_phase        = TTX_FIELD;
   ttx_field_parity = 0u;
   ttx_next_us      = RPI_GetSystemTime() + 64u;

   if (!ttx_configured) {
      ttx_configure();
      ttx_configured = true;
   }

   ttx_status_write();
   Pi1MHz_MemoryWrite((uint32_t)(TTX_ADDR + 2u), 0u);
   Pi1MHz_nIRQ_CLEAR(IRQ_NUM);

   /* +0 status read is served from the readback memory the poll keeps
    * current; the rest need callbacks. */
   Pi1MHz_Register_Memory(WRITE_FRED, TTX_ADDR,             ttx_w_control);
   Pi1MHz_Register_Memory(WRITE_FRED, (uint8_t)(TTX_ADDR + 1u), ttx_w_row);
   Pi1MHz_Register_Memory(READ_FRED,  (uint8_t)(TTX_ADDR + 2u), ttx_r_data);
   Pi1MHz_Register_Memory(WRITE_FRED, (uint8_t)(TTX_ADDR + 2u), ttx_w_data);
   Pi1MHz_Register_Memory(READ_FRED,  (uint8_t)(TTX_ADDR + 3u), ttx_clear);
   Pi1MHz_Register_Memory(WRITE_FRED, (uint8_t)(TTX_ADDR + 3u), ttx_clear);

   Pi1MHz_Register_Poll(teletext_poll);
}
