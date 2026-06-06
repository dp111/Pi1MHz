/* aun_emulator.c - glue between the discaccess command interface and
 * the AUN protocol engine (aun_aun.c), plus the lwIP UDP transport.
 *
 * The Beeb drives this exactly like the disc commands: build a command
 * block in the top 64K of disc RAM, write the block's page number to
 * FRED &FCAA, then poll &FCAA for the result byte. Opcodes 30..41 are
 * routed here from discaccess_emulator_command()'s default case.
 *
 * Execution context: the FRED write callback runs inside the FIQ
 * handler, so it must not touch lwIP or the SDIO bus (the main loop may
 * be mid-transaction). aun_emulator_command() therefore only records
 * the request in a one-slot mailbox; aun_emulator_poll() executes it from the
 * main loop and writes the result byte. The Beeb-side protocol: command
 * block page numbers are >= &E0, every result is < &E0, and the FIQ has
 * already echoed the page number to &FCAA - so the Beeb writes the page
 * and polls &FCAA until the value drops below &E0.
 *
 * Command block layouts (offsets within the 256-byte block; multi-byte
 * fields little-endian; 32-bit fields 4-byte aligned; buffer offsets are
 * relative to DISC_RAM_BASE, the same convention as the disc commands):
 *
 *  30 INIT        +1 station  +2 net  +4 u32 listen UDP port (0=32768)
 *  31 STATUS      fills +4 stn  +5 net  +6 network-ready  +8 ip[4]
 *                 +12 u32 counters x11 (tx_ok, tx_fail, rx_data, rx_bcast,
 *                 rx_imm, rx_dup, rx_no_block, rx_unknown_src, rx_too_big,
 *                 ack_sent, nak_sent)
 *  32 TX          +2 ctrl  +3 port  +4 dest stn  +5 dest net
 *                 +8 u32 data offset  +12 u32 length
 *  33 TX_POLL     result = &80 pending / 0 ok / error; on a completed
 *                 immediate also fills +8 u32 reply length
 *  34 RX_OPEN     +1 handle(0-7)  +2 port(0=any)  +4 stn  +5 net
 *                 (&FF=any)  +8 u32 buffer offset  +12 u32 buffer size
 *  35 RX_POLL     +1 handle; when ready fills +2 ctrl  +3 port
 *                 +4 src stn  +5 src net  +12 u32 length
 *  36 RX_CLOSE    +1 handle
 *  37 BCAST       +2 ctrl  +3 port  +8 u32 data offset  +12 u32 length
 *  38 IMMEDIATE   +2 ctrl  +4 dest stn  +5 dest net  +8 u32 tx offset
 *                 +12 u32 tx length  +16 u32 reply offset  +20 u32 reply max
 *  39 MAP_ADD     +1 stn  +2 net  +4 ip[4] (a.b.c.d)  +8 u32 UDP port
 *  40 TEST        +1 enable  +2 station  +3 net   (loopback responder)
 *  41 SET_MACHINE +4 machine id [4] (machine-peek reply bytes)
 *
 * Result codes are the AUN_* values from aun.h (0 = OK).
 */

#include <string.h>

#include "../Pi1MHz.h"
#include "aun.h"
#include "aun_config.h"
#include "aun_emulator.h"
#include "../rpi/info.h"
#include <stdio.h>
#include "../wifi/wifi.h"
#include "../rpi/systimer.h"

#include "../wifi/wifi_lwip.h"

#include "lwip/udp.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"

static aun_engine_t aun;
static struct udp_pcb *aun_pcb;

/* One-slot command mailbox: written by the FIQ-context FRED callback,
 * consumed by aun_emulator_poll() in the main loop. 'pending' is the
 * handshake flag; the FIQ only ever sets it when it is clear (the Beeb
 * waits for each result before issuing the next command). */
static volatile bool     aun_pending;
static volatile uint32_t aun_pending_cp;
static volatile uint32_t aun_pending_addr;

/* Station used when the Beeb's INIT block carries station 0 ("use the
 * Pi-side configuration") and cmdline.txt has no aun_station. */
#define AUN_DEFAULT_STATION 32u
#define AUN_DEFAULT_NET     0u

#define AUN_IRQ_STATUS_REG 0xABu
static bool    aun_irq_enabled;
static uint8_t aun_irq_state;
static uint8_t IRQ_NUM;
/* terse event log on the shared wifi debug channel, enabled by
 * aun_debug=1 in cmdline.txt */
static bool aun_debug;
#define AUN_LOG(...) do { if (aun_debug) wifi_debug_printf(__VA_ARGS__); } while (0)

static void aun_irq_update(void)
{
   uint8_t st = 0;
   if (aun_irq_enabled && aun.rx[0].open && aun.rx[0].count != 0)
      st = (uint8_t)(0x80u | aun.rx[0].count);
   if (aun_irq_enabled && aun.himm.active)
      st |= 0x40u;              /* immediate operation awaiting the host */
   if (st != aun_irq_state) {
      aun_irq_state = st;
      Pi1MHz_MemoryWrite(AUN_IRQ_STATUS_REG, st);
      if (st != 0)
         Pi1MHz_nIRQ_ASSERT(IRQ_NUM);
      else
         Pi1MHz_nIRQ_CLEAR(IRQ_NUM);

   }
}

/* aun_map_add() adapter for the aun_map cmdline parser. */
static bool aun_map_add_cb(void *user, uint8_t net, uint8_t stn,
                           uint32_t ip_be, uint16_t port)
{
   return aun_map_add((aun_engine_t *)user, net, stn, ip_be, port);
}

/* ---- JIM helpers (same pattern as discaccess_emulator.c) ---------------- */

static uint32_t jim_read32(uint32_t off)
{
   uint32_t v;
   memcpy(&v, __builtin_assume_aligned(&Pi1MHz->JIM_ram[off], 4), sizeof v);
   return v;
}

static void jim_write32(uint32_t off, uint32_t v)
{
   memcpy(__builtin_assume_aligned(&Pi1MHz->JIM_ram[off], 4), &v, sizeof v);
}

/* Every buffer offset/length pair from the Beeb is untrusted; keep all
 * accesses inside the disc RAM region. */
static bool aun_buffer_ok(uint32_t offset, uint32_t length)
{
   if (offset > DISC_RAM_SIZE)
      return false;
   return length <= (DISC_RAM_SIZE - offset);
}

/* ---- transport: lwIP UDP -------------------------------------------------*/

static bool aun_udp_send(void *user, uint32_t ip_be, uint16_t port,
                         const uint8_t *buf, size_t len)
{
   (void)user;
   if (aun_pcb == NULL)
      return false;

   struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
   if (p == NULL)
      return false;
   if (pbuf_take(p, buf, (u16_t)len) != ERR_OK) {
      pbuf_free(p);
      return false;
   }
   ip_addr_t dest;
   ip_addr_set_ip4_u32(&dest, ip_be);
   err_t r = udp_sendto(aun_pcb, p, &dest, port);
   pbuf_free(p);
   return r == ERR_OK;
}

static uint32_t aun_now_ms(void *user)
{
   (void)user;
   return RPI_GetSystemTime() / 1000u;   /* us -> ms */
}

static void aun_udp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                         const ip_addr_t *addr, u16_t port)
{
   /* static: poll-path buffer, function never re-entered (NO_SYS=1) */
   static uint8_t frame[AUN_HDR_SIZE + AUN_MAX_DATA];
   (void)arg; (void)pcb;

   if (p == NULL)
      return;
   if (p->tot_len <= sizeof frame) {
      u16_t n = pbuf_copy_partial(p, frame, p->tot_len, 0);
      aun_udp_input(&aun, ip_addr_get_ip4_u32(addr), port, frame, n);
   }
   pbuf_free(p);
}

/* ---- poll hook ------------------------------------------------------------*/

static void aun_execute(uint32_t cp, uint32_t addr);

static void aun_emulator_poll(void)
{
   if (aun_pending) {
      uint32_t cp   = aun_pending_cp;
      uint32_t addr = aun_pending_addr;
      aun_execute(cp, addr);
      aun_pending = false;
   }
   aun_poll(&aun);
   aun_irq_update();
}


void aun_emulator_init(uint8_t instance, uint8_t address)
{
   IRQ_NUM = instance;
   /* The AUN engine itself comes up on the Beeb's INIT command (the
    * network stack may not be ready yet at RST); the poll hook is
    * registered once here - Pi1MHz_Register_Poll dedupes. */
   aun_pending     = false;
   aun_irq_enabled = false;
   aun_irq_state   = 0;
   Pi1MHz_Register_Poll(aun_emulator_poll);
}

/* Plain-text status block for the web UI (webserver.c /aun). */
void aun_status_text(char *buf, size_t size)
{
   size_t n = 0;
   #define APPEND(...) do { if (n < size) \
      n += (size_t)snprintf(buf + n, size - n, __VA_ARGS__); } while (0)
   APPEND("station      %u.%u\n", aun.net, aun.station);
   APPEND("initialised  %s\n", aun.initialised ? "yes" : "no");
   APPEND("irq          enabled=%u status=&%02X\n",
          aun_irq_enabled ? 1u : 0u, aun_irq_state);
   APPEND("rx queue     %u frame(s) held\n", aun.rx[0].count);
   APPEND("imm pending  %s\n", aun.himm.active ? "yes" : "no");
   APPEND("learn net    %u\n", aun.learn_net);
   APPEND("map          %lu entries\n", (unsigned long)aun.map_count);
   for (uint32_t i = 0; i < aun.map_count && i < 16; i++)
      APPEND("  %u.%u -> %u.%u.%u.%u:%u\n",
             aun.map[i].net, aun.map[i].stn,
             (unsigned)(aun.map[i].ip_be & 0xff),
             (unsigned)((aun.map[i].ip_be >> 8) & 0xff),
             (unsigned)((aun.map[i].ip_be >> 16) & 0xff),
             (unsigned)((aun.map[i].ip_be >> 24) & 0xff),
             aun.map[i].udp_port);
   APPEND("tx ok/fail   %lu/%lu\n", (unsigned long)aun.counters.tx_ok,
          (unsigned long)aun.counters.tx_fail);
   APPEND("rx data/bcast/imm  %lu/%lu/%lu\n",
          (unsigned long)aun.counters.rx_data,
          (unsigned long)aun.counters.rx_broadcast,
          (unsigned long)aun.counters.rx_imm);
   APPEND("rx dup/noblk/unkn/big  %lu/%lu/%lu/%lu\n",
          (unsigned long)aun.counters.rx_dup,
          (unsigned long)aun.counters.rx_no_block,
          (unsigned long)aun.counters.rx_unknown_source,
          (unsigned long)aun.counters.rx_too_big);
   APPEND("ack/nak sent %lu/%lu\n", (unsigned long)aun.counters.ack_sent,
          (unsigned long)aun.counters.nak_sent);
   #undef APPEND
}

/* ---- command dispatch ------------------------------------------------------*/

/* FIQ context: queue only. The discaccess dispatcher has already echoed
 * the command page number to the FRED result register; the real result
 * (always < &E0) is written by aun_execute() from the main loop. */
void aun_emulator_command(uint32_t cp, uint32_t addr)
{
   aun_pending_cp   = cp;
   aun_pending_addr = addr;
   aun_pending      = true;
}

static void aun_execute(uint32_t cp, uint32_t addr)
{
   uint32_t base_addr = DISC_RAM_BASE;
   uint8_t  result    = AUN_ERR_PARAM;

   switch (Pi1MHz->JIM_ram[cp]) {

   case AUN_CMD_INIT:
   {
      const wifi_lwip_context_t *net = wifi_lwip_get_context();
      if (!net->address_ready) {
         result = AUN_ERR_NOT_READY;
         break;
      }
      uint16_t listen = (uint16_t)jim_read32(cp + 4);
      if (listen == 0 &&
          !aun_parse_port(get_cmdline_prop("aun_port"), &listen))
         listen = (uint16_t)AUN_DEFAULT_UDP_PORT;

      if (aun_pcb != NULL) {
         udp_remove(aun_pcb);
         aun_pcb = NULL;
      }
      aun_pcb = udp_new();
      if (aun_pcb == NULL || udp_bind(aun_pcb, IP_ADDR_ANY, listen) != ERR_OK) {
         if (aun_pcb != NULL) {
            udp_remove(aun_pcb);
            aun_pcb = NULL;
         }
         result = AUN_TX_NET_ERROR;
         break;
      }
      udp_recv(aun_pcb, aun_udp_recv, NULL);

      static const aun_transport_t transport = {
         .udp_send = aun_udp_send,
         .now_ms   = aun_now_ms,
         .user     = NULL,
      };
      uint8_t aun_stn = Pi1MHz->JIM_ram[cp + 1];
      uint8_t aun_net = Pi1MHz->JIM_ram[cp + 2];
      if (aun_stn == 0) {        /* 0 = "use the Pi-side configuration" */
         const char *stn_cfg = get_cmdline_prop("aun_station");
         uint8_t cfg_net = AUN_DEFAULT_NET;
         bool    net_from_ip = false;
         if (aun_station_is_ip(stn_cfg, &cfg_net, &net_from_ip)) {
            uint32_t ip = ip4_addr_get_u32(netif_ip4_addr(&net->netif));
            aun_stn = (uint8_t)(ip >> 24);
            aun_net = net_from_ip ? (uint8_t)(ip >> 16) : cfg_net;
         } else if (!aun_parse_station(stn_cfg, &aun_net, &aun_stn)) {
            aun_stn = AUN_DEFAULT_STATION;
            aun_net = AUN_DEFAULT_NET;
         }
         if (aun_stn == 0 || aun_stn == 0xFFu) {
            aun_stn = AUN_DEFAULT_STATION;
            aun_net = AUN_DEFAULT_NET;
         }
      }
      aun_irq_enabled = (Pi1MHz->JIM_ram[cp + 8] & 1u) != 0;
      bool host_imm   = (Pi1MHz->JIM_ram[cp + 8] & 2u) != 0;
      aun_init(&aun, &transport, aun_stn, aun_net);
      aun_set_host_imm(&aun, host_imm);
      aun_debug = get_cmdline_prop("aun_debug") != NULL;
      {
         uint8_t mid[4];
         if (aun_parse_machine(get_cmdline_prop("aun_machine"), mid))
            aun_set_machine_id(&aun, mid);
      }
      AUN_LOG("ECONET: init stn %u.%u irq=%u imm=%u\r\n",
              aun_net, aun_stn, aun_irq_enabled ? 1u : 0u,
              host_imm ? 1u : 0u);
      /* peer map from cmdline.txt (entries are also addable later via
       * AUN_CMD_MAP_ADD; a parse error keeps whatever was added before
       * the bad entry) */
      (void)aun_parse_map(get_cmdline_prop("aun_map"),
                          aun_map_add_cb, &aun);
      /* subnet broadcast + optional learn mode (aun_learn=<net>) */
      if (net->netif_added) {
         uint32_t ip   = ip4_addr_get_u32(netif_ip4_addr(&net->netif));
         uint32_t mask = ip4_addr_get_u32(netif_ip4_netmask(&net->netif));
         uint8_t  lnet;
         bool     learn = aun_parse_net(get_cmdline_prop("aun_learn"),
                                        &lnet);
         if (ip != 0 && mask != 0)
            aun_set_addressing(&aun, (ip & mask) | ~mask, ip, mask,
                               learn ? lnet : 0xFF);
      }
      result = AUN_OK;
      break;
   }

   case AUN_CMD_STATUS:
   {
      const wifi_lwip_context_t *net = wifi_lwip_get_context();
      Pi1MHz->JIM_ram[cp + 4] = aun.station;
      Pi1MHz->JIM_ram[cp + 5] = aun.net;
      Pi1MHz->JIM_ram[cp + 6] = (uint8_t)((net->address_ready ? 1u : 0u) |
                                          (aun.initialised ? 2u : 0u));
      /* our IPv4 as a.b.c.d */
      uint32_t ip = net->netif_added ?
                    ip4_addr_get_u32(netif_ip4_addr(&net->netif)) : 0;
      memcpy(&Pi1MHz->JIM_ram[cp + 8], &ip, 4);
      const uint32_t c[11] = {
         aun.counters.tx_ok, aun.counters.tx_fail,
         aun.counters.rx_data, aun.counters.rx_broadcast,
         aun.counters.rx_imm, aun.counters.rx_dup,
         aun.counters.rx_no_block, aun.counters.rx_unknown_source,
         aun.counters.rx_too_big, aun.counters.ack_sent,
         aun.counters.nak_sent
      };
      for (uint32_t i = 0; i < 11; i++)
         jim_write32(cp + 12 + (i * 4), c[i]);
      result = AUN_OK;
      break;
   }

   case AUN_CMD_TX:
   {
      uint32_t off = jim_read32(cp + 8);
      uint32_t len = jim_read32(cp + 12);
      if (!aun_buffer_ok(off, len))
         break;                                   /* AUN_ERR_PARAM */
      AUN_LOG("ECONET: tx %u.%u port %02X len %lu\r\n",
              Pi1MHz->JIM_ram[cp + 5], Pi1MHz->JIM_ram[cp + 4],
              Pi1MHz->JIM_ram[cp + 3], (unsigned long)len);
      result = aun_tx_start(&aun,
                            Pi1MHz->JIM_ram[cp + 5],   /* dest net */
                            Pi1MHz->JIM_ram[cp + 4],   /* dest stn */
                            Pi1MHz->JIM_ram[cp + 2],   /* ctrl     */
                            Pi1MHz->JIM_ram[cp + 3],   /* port     */
                            &Pi1MHz->JIM_ram[base_addr + off], len);
      break;
   }

   case AUN_CMD_TX_POLL:
      result = aun_tx_status(&aun);
      if (result == AUN_OK)
         jim_write32(cp + 8, aun_tx_reply_len(&aun));
      break;

   case AUN_CMD_RX_OPEN:
   {
      uint32_t off  = jim_read32(cp + 8);
      uint32_t size = jim_read32(cp + 12);
      if (!aun_buffer_ok(off, size))
         break;                                   /* AUN_ERR_PARAM */
      result = aun_rx_open(&aun,
                           Pi1MHz->JIM_ram[cp + 1],    /* handle */
                           Pi1MHz->JIM_ram[cp + 2],    /* port   */
                           Pi1MHz->JIM_ram[cp + 4],    /* stn    */
                           Pi1MHz->JIM_ram[cp + 5],    /* net    */
                           &Pi1MHz->JIM_ram[base_addr + off], size);

      break;
   }

   case AUN_CMD_RX_POLL:
   {
      aun_rx_info_t info;
      result = aun_rx_poll(&aun, Pi1MHz->JIM_ram[cp + 1], &info);
      if (result == AUN_OK) {
         Pi1MHz->JIM_ram[cp + 2] = info.ctrl;
         Pi1MHz->JIM_ram[cp + 3] = info.port;
         Pi1MHz->JIM_ram[cp + 4] = info.src_stn;
         Pi1MHz->JIM_ram[cp + 5] = info.src_net;
         jim_write32(cp + 12, info.len);
      }
      break;
   }

   case AUN_CMD_RX_CLOSE:
      result = aun_rx_close(&aun, Pi1MHz->JIM_ram[cp + 1]);
      break;

   case AUN_CMD_IMM_POLL:     /* held immediate -> block + JIM &FEA000 */
      if (!aun.himm.active) {
         result = AUN_STATUS_PENDING;
         break;
      }
      Pi1MHz->JIM_ram[cp + 2] = aun.himm.ctrl;
      jim_write32(cp + 12, aun.himm.len);
      if (aun.himm.len != 0)
         memcpy(&Pi1MHz->JIM_ram[base_addr + 0xFEA000u], aun.himm.data,
                aun.himm.len);
      result = AUN_OK;
      break;

   case AUN_CMD_IMM_REPLY:    /* +8 u32 reply offset, +12 u32 length */
   {
      uint32_t off = jim_read32(cp + 8);
      uint32_t len = jim_read32(cp + 12);
      if (!aun_buffer_ok(off, len) || len > AUN_HIMM_MAX)
         break;                                   /* AUN_ERR_PARAM */
      aun_himm_reply(&aun, &Pi1MHz->JIM_ram[base_addr + off], len);
      result = AUN_OK;
      break;
   }

   case AUN_CMD_RX_DONE:
      AUN_LOG("ECONET: rx_done h%u %s\r\n", Pi1MHz->JIM_ram[cp + 1],
              Pi1MHz->JIM_ram[cp + 2] == 0 ? "ack" : "nak");      /* host has copied the frame: re-arm.
                                 * +2 verdict: 0 = delivered (ACK the
                                 * sender), 1 = no listener (NAK) */
      result = aun_rx_collect(&aun, Pi1MHz->JIM_ram[cp + 1],
                              Pi1MHz->JIM_ram[cp + 2] == 0);
      break;

   case AUN_CMD_BCAST:
   {
      uint32_t off = jim_read32(cp + 8);
      uint32_t len = jim_read32(cp + 12);
      if (!aun_buffer_ok(off, len))
         break;                                   /* AUN_ERR_PARAM */
      result = aun_broadcast(&aun,
                             Pi1MHz->JIM_ram[cp + 2],  /* ctrl */
                             Pi1MHz->JIM_ram[cp + 3],  /* port */
                             &Pi1MHz->JIM_ram[base_addr + off], len);
      break;
   }

   case AUN_CMD_IMMEDIATE:
   {
      uint32_t off       = jim_read32(cp + 8);
      uint32_t len       = jim_read32(cp + 12);
      uint32_t reply_off = jim_read32(cp + 16);
      uint32_t reply_max = jim_read32(cp + 20);
      if (!aun_buffer_ok(off, len) || !aun_buffer_ok(reply_off, reply_max))
         break;                                   /* AUN_ERR_PARAM */
      result = aun_immediate(&aun,
                             Pi1MHz->JIM_ram[cp + 5],  /* dest net */
                             Pi1MHz->JIM_ram[cp + 4],  /* dest stn */
                             Pi1MHz->JIM_ram[cp + 2],  /* ctrl     */
                             &Pi1MHz->JIM_ram[base_addr + off], len,
                             &Pi1MHz->JIM_ram[base_addr + reply_off],
                             reply_max);
      break;
   }

   case AUN_CMD_MAP_ADD:
   {
      uint32_t ip_be;
      memcpy(&ip_be, &Pi1MHz->JIM_ram[cp + 4], 4);   /* a.b.c.d as stored */
      result = aun_map_add(&aun,
                           Pi1MHz->JIM_ram[cp + 2],   /* net */
                           Pi1MHz->JIM_ram[cp + 1],   /* stn */
                           ip_be,
                           (uint16_t)jim_read32(cp + 8))
               ? AUN_OK : AUN_ERR_PARAM;
      break;
   }

   case AUN_CMD_TEST:
      aun_test_responder(&aun,
                         Pi1MHz->JIM_ram[cp + 1] != 0,
                         Pi1MHz->JIM_ram[cp + 2],
                         Pi1MHz->JIM_ram[cp + 3]);
      result = AUN_OK;
      break;

   case AUN_CMD_SET_MACHINE:
      aun_set_machine_id(&aun, &Pi1MHz->JIM_ram[cp + 4]);
      result = AUN_OK;
      break;

   default:
      break;
   }

   Pi1MHz_MemoryWrite(addr, result);
}
