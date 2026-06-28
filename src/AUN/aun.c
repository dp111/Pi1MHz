/* aun_aun.c - AUN protocol engine. See aun.h for the wire
 * format and the design notes; platform glue is in aun_emulator.c.
 *
 * Design constraints:
 *  - Single outstanding transmit. The 8-bit NFS performs one transmit
 *    at a time (scout -> ack per transaction), so this loses nothing
 *    and keeps the state machine trivial.
 *  - Non-blocking throughout: aun_tx_start() returns immediately and
 *    the caller polls aun_tx_status(); retries/timeouts are driven by
 *    aun_poll() from the main loop. Nothing here may stall the 1MHz
 *    bus servicing.
 *  - Duplicate suppression: a retransmitted DATA datagram (our ACK was
 *    lost) is re-ACKed but not delivered twice, tracked per map entry
 *    by the last accepted sequence number.
 *
 * Test responder: when enabled, transmits addressed to the configured
 * test station never touch the network - they are ACKed locally and the
 * payload is fed straight back through the receive path as if that
 * station had sent it. Immediates to it answer with our machine id.
 * This closes the whole Beeb -> JIM -> command -> engine -> rx-block ->
 * Beeb loop with no second station, no fileserver and no network.
 */

#include <string.h>

#include "aun.h"

/* ---- small helpers ------------------------------------------------------*/

static uint32_t rd32le(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
          ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32le(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)v;
   p[1] = (uint8_t)(v >> 8);
   p[2] = (uint8_t)(v >> 16);
   p[3] = (uint8_t)(v >> 24);
}

static uint32_t now_ms(aun_engine_t *e)
{
   return e->transport.now_ms(e->transport.user);
}

static bool udp_send(aun_engine_t *e, uint32_t ip_be, uint16_t port,
                     const uint8_t *buf, size_t len)
{
   return e->transport.udp_send(e->transport.user, ip_be, port, buf, len);
}

static aun_map_entry_t *map_find_by_addr(aun_engine_t *e,
                                         uint8_t net, uint8_t stn)
{
   for (uint32_t i = 0; i < e->map_count; i++)
      if (e->map[i].net == net && e->map[i].stn == stn)
         return &e->map[i];
   return NULL;
}

/* Learn-mode outbound resolve: subnet_base | station on the default
 * port. Returns false when learn mode is off or doesn't apply. */
static bool learn_resolve(aun_engine_t *e, uint8_t net, uint8_t stn,
                          uint32_t *ip_be, uint16_t *port)
{
   if (e->learn_net == 0xFF || net != e->learn_net)
      return false;
   /* last octet is the high byte of the network-order u32 */
   *ip_be = (e->subnet_base_be & 0x00FFFFFFu) | ((uint32_t)stn << 24);
   *port  = (uint16_t)AUN_DEFAULT_UDP_PORT;
   return true;
}

/* Learn-mode inbound attribution for an unmapped in-subnet source. */
static bool learn_attribute(aun_engine_t *e, uint32_t ip_be, uint16_t port,
                            uint8_t *net, uint8_t *stn)
{
   if (e->learn_net == 0xFF || port != (uint16_t)AUN_DEFAULT_UDP_PORT)
      return false;
   if ((ip_be & e->subnet_mask_be) != (e->subnet_base_be & e->subnet_mask_be))
      return false;
   *net = e->learn_net;
   *stn = (uint8_t)(ip_be >> 24);
   return *stn != 0 && *stn != 255;
}

static aun_map_entry_t *map_find_by_ip(aun_engine_t *e,
                                       uint32_t ip_be, uint16_t port)
{
   for (uint32_t i = 0; i < e->map_count; i++)
      if (e->map[i].ip_be == ip_be && e->map[i].udp_port == port)
         return &e->map[i];
   return NULL;
}

static void build_header(uint8_t *hdr, uint8_t type, uint8_t port,
                         uint8_t ctrl, uint32_t seq)
{
   hdr[0] = type;
   hdr[1] = port;
   hdr[2] = (uint8_t)(ctrl & 0x7F);  /* bit 7 always cleared on the wire */
   hdr[3] = 0;
   wr32le(&hdr[4], seq);
}

/* Send an 8-byte ACK/NAK echoing the sequence number of the datagram it
 * answers. */
static void send_ack_nak(aun_engine_t *e, uint32_t ip_be, uint16_t port,
                         uint8_t type, uint8_t aun_port, uint8_t ctrl,
                         uint32_t seq)
{
   uint8_t hdr[AUN_HDR_SIZE];
   build_header(hdr, type, aun_port, ctrl, seq);
   /* Honour the transport result: an ACK that fails to go on the wire
    * (pbuf exhaustion, udp_sendto error) leaves the far end waiting and
    * retransmitting. Count those separately so a silent drop is visible
    * instead of being miscounted as a successful ack. */
   bool ok = udp_send(e, ip_be, port, hdr, sizeof hdr);
   if (!ok)
      e->counters.ack_fail++;
   else if (type == AUN_TYPE_ACK)
      e->counters.ack_sent++;
   else
      e->counters.nak_sent++;
}

/* ---- init / configuration ----------------------------------------------*/

void aun_init(aun_engine_t *e, const aun_transport_t *transport,
              uint8_t station, uint8_t net)
{
   memset(e, 0, sizeof *e);
   e->transport = *transport;
   e->station   = station;
   e->net       = net;
   e->next_seq  = 4;
   e->learn_net = 0xFF;            /* learn mode off */
   e->tx.state  = AUN_OK;          /* idle reads as complete */
   /* default machine-peek reply: machine type &0001 (BBC B),
    * NFS version 3.60 */
   e->machine_id[0] = 0x01;
   e->machine_id[1] = 0x00;
   e->machine_id[2] = 0x3C;
   e->machine_id[3] = 0x03;
   e->initialised = true;
}

bool aun_map_add(aun_engine_t *e, uint8_t net, uint8_t stn,
                 uint32_t ip_be, uint16_t udp_port)
{
   if (udp_port == 0)
      udp_port = (uint16_t)AUN_DEFAULT_UDP_PORT;

   aun_map_entry_t *m = map_find_by_addr(e, net, stn);
   if (m == NULL) {
      if (e->map_count >= AUN_MAP_MAX)
         return false;
      m = &e->map[e->map_count++];
      m->net = net;
      m->stn = stn;
   }
   m->ip_be     = ip_be;
   m->udp_port  = udp_port;
   m->seq_valid = false;
   return true;
}

void aun_set_addressing(aun_engine_t *e, uint32_t broadcast_ip_be,
                        uint32_t subnet_base_be, uint32_t mask_be,
                        uint8_t learn_net)
{
   e->broadcast_ip_be = broadcast_ip_be;
   e->subnet_base_be  = subnet_base_be;
   e->subnet_mask_be  = mask_be;
   e->learn_net       = learn_net;
}

void aun_test_responder(aun_engine_t *e, bool enable,
                        uint8_t stn, uint8_t net)
{
   e->test_enabled = enable;
   e->test_stn     = stn;
   e->test_net     = net;
}

void aun_set_machine_id(aun_engine_t *e, const uint8_t id[4])
{
   memcpy(e->machine_id, id, 4);
}

void aun_set_trace(aun_engine_t *e, aun_trace_fn fn, void *user)
{
   e->trace_fn   = fn;
   e->trace_user = user;
}

void aun_set_host_imm(aun_engine_t *e, bool enable)
{
   e->host_imm_enabled = enable;
   e->himm.active = false;
}

void aun_himm_reply(aun_engine_t *e, const uint8_t *data, uint32_t len)
{
   if (!e->himm.active)
      return;
   uint8_t dgram[AUN_HDR_SIZE + AUN_HIMM_MAX];
   if (len > AUN_HIMM_MAX)
      len = AUN_HIMM_MAX;
   build_header(dgram, AUN_TYPE_IMM_REPLY, 0, e->himm.ctrl, e->himm.seq);
   if (len != 0)
      memcpy(&dgram[AUN_HDR_SIZE], data, len);
   (void)udp_send(e, e->himm.ip_be, e->himm.port, dgram,
                  AUN_HDR_SIZE + len);
   e->himm.active = false;
}

/* ---- receive blocks ------------------------------------------------------*/

uint8_t aun_rx_open(aun_engine_t *e, uint8_t handle, uint8_t port,
                    uint8_t stn, uint8_t net, uint8_t *buf, uint32_t buf_size)
{
   if (!e->initialised)
      return AUN_ERR_NOT_READY;
   if (handle >= AUN_RX_BLOCKS || buf == NULL || buf_size == 0)
      return AUN_ERR_PARAM;

   aun_rx_block_t *b = &e->rx[handle];
   memset(b, 0, sizeof *b);
   b->open     = true;
   b->port     = port;
   b->stn      = stn;
   b->net      = net;
   b->buf      = buf;
   b->buf_size = buf_size;
   return AUN_OK;
}

uint8_t aun_rx_poll(aun_engine_t *e, uint8_t handle, aun_rx_info_t *out)
{
   if (handle >= AUN_RX_BLOCKS || !e->rx[handle].open)
      return AUN_ERR_NO_BLOCK;

   aun_rx_block_t *b = &e->rx[handle];
   if (b->count == 0)
      return AUN_STATUS_PENDING;

   aun_rx_frame_t *f = &b->q[b->head];
   if (!b->presented) {
      /* copy the head frame into the host-visible buffer once; it
       * stays there untouched until collected */
      memcpy(b->buf, f->data, f->len);
      b->presented = true;
   }
   if (out != NULL) {
      out->src_stn = f->src_stn;
      out->src_net = f->src_net;
      out->port    = f->port;
      out->ctrl    = f->ctrl;
      out->len     = f->len;
   }
   return AUN_OK;
}

uint8_t aun_rx_collect(aun_engine_t *e, uint8_t handle, bool accept)
{
   /* The ACK was already sent when the frame arrived (ACK-on-receipt:
    * the engine acks into the queue immediately, like the real ADLC/NMI
    * acks into the RXCB - this is fast enough to beat the fileserver's
    * reply-ACK timeout, which a deferred collect-time ack is not). So a
    * normal collect just pops the head.
    *
    * accept=false means the host's RXCB scan found no listener for this
    * frame. Rather than discard it (and lose a reply that arrived a beat
    * before its control block was armed - the ACK already went out, so the
    * sender will not retransmit), the frame is "parked" out of the queue
    * and re-presented a few times by aun_poll(). It is delivered the moment
    * the host arms the matching CB, and dropped only if no one ever does. */
   if (handle >= AUN_RX_BLOCKS || !e->rx[handle].open)
      return AUN_ERR_NO_BLOCK;
   aun_rx_block_t *b = &e->rx[handle];
   if (b->count != 0) {
      aun_rx_frame_t *f = &b->q[b->head];
      bool is_parked = e->parked_valid && e->parked_in_queue &&
                       f->src_ip_be != 0 &&
                       e->parked.seq      == f->seq &&
                       e->parked.src_ip_be == f->src_ip_be &&
                       e->parked.src_port == f->src_port;
      if (accept) {
         if (is_parked)
            e->parked_valid = false;        /* re-injected frame delivered */
      } else if (is_parked) {
         /* the re-injected frame was rejected again: retry or give up */
         if (e->parked_retries != 0) {
            e->parked_retries--;
            e->parked_in_queue = false;
            e->parked_due_ms   = now_ms(e) + AUN_PARK_DELAY_MS;
         } else {
            e->parked_valid = false;        /* exhausted: drop */
         }
      } else if (!e->parked_valid && f->src_ip_be != 0 && !f->dup) {
         /* fresh reject of a NEW DATA reply (not a retransmit): park it for
          * re-presentation, in case its RXCB is armed a beat later. A 'dup'
          * frame is a retransmit of something already handled - drop it. */
         e->parked          = *f;
         e->parked_valid    = true;
         e->parked_in_queue = false;
         e->parked_handle   = handle;
         e->parked_retries  = AUN_PARK_RETRIES;
         e->parked_due_ms   = now_ms(e) + AUN_PARK_DELAY_MS;
      }
      /* else: park slot busy with another frame, or non-DATA -> just drop */
      b->head = (uint8_t)((b->head + 1u) % AUN_RX_QUEUE);
      b->count--;
   }
   b->presented = false;
   return AUN_OK;
}

uint8_t aun_rx_close(aun_engine_t *e, uint8_t handle)
{
   if (handle >= AUN_RX_BLOCKS || !e->rx[handle].open)
      return AUN_ERR_NO_BLOCK;
   memset(&e->rx[handle], 0, sizeof e->rx[handle]);
   return AUN_OK;
}

/* Find the first open receive block with queue space matching
 * (port, src). */
static aun_rx_block_t *rx_match(aun_engine_t *e, uint8_t port,
                                uint8_t src_stn, uint8_t src_net)
{
   for (uint32_t i = 0; i < AUN_RX_BLOCKS; i++) {
      aun_rx_block_t *b = &e->rx[i];
      if (!b->open || b->count >= AUN_RX_QUEUE)
         continue;
      if (b->port != 0 && b->port != port)
         continue;
      if (b->stn != AUN_WILDCARD && b->stn != src_stn)
         continue;
      if (b->net != AUN_WILDCARD && b->net != src_net)
         continue;
      return b;
   }
   return NULL;
}

/* Deliver a payload into a receive block; returns false if no matching
 * block could take it (caller decides whether to NAK). For DATA frames
 * the ACK is NOT sent here: the frame carries its verdict context and
 * the ACK/NAK goes out at aun_rx_collect(), so a sender only sees
 * success once a listener has truly taken the frame. */
static aun_rx_frame_t *rx_deliver(aun_engine_t *e, uint8_t port, uint8_t ctrl,
                                  uint8_t src_stn, uint8_t src_net,
                                  const uint8_t *data, uint32_t len)
{
   aun_rx_block_t *b = rx_match(e, port, src_stn, src_net);
   if (b == NULL) {
      e->counters.rx_no_block++;
      return NULL;
   }
   if (len > b->buf_size) {
      e->counters.rx_too_big++;
      return NULL;
   }
   aun_rx_frame_t *f = &b->q[((uint32_t)b->head + b->count) % AUN_RX_QUEUE];
   memcpy(f->data, data, len);
   f->len     = len;
   f->src_stn = src_stn;
   f->src_net = src_net;
   f->port    = port;
   /* present the control byte with bit 7 set again, as a real Econet
    * receive block would */
   f->ctrl    = (uint8_t)(ctrl | 0x80);
   f->needs_verdict = false;
   f->dup       = false;
   /* identity, used by park-and-retry; the DATA path overwrites these with
    * the real source/seq so a parked frame can be recognised. A zero
    * src_ip_be marks a frame that must never be parked (broadcast/local). */
   f->seq       = 0;
   f->src_ip_be = 0;
   f->src_port  = 0;
   b->count++;
   return f;
}

/* ---- transmit ------------------------------------------------------------*/

static uint8_t tx_begin(aun_engine_t *e, uint8_t wire_type,
                        uint8_t dest_net, uint8_t dest_stn,
                        uint8_t ctrl, uint8_t port,
                        const uint8_t *data, uint32_t len,
                        uint8_t *reply_buf, uint32_t reply_max)
{
   if (!e->initialised)
      return AUN_ERR_NOT_READY;
   if (len > AUN_MAX_DATA || (len != 0 && data == NULL))
      return AUN_ERR_PARAM;
   if (e->tx.state == AUN_STATUS_PENDING)
      return AUN_TX_BUSY;

   /* Loopback test responder: never touches the network. */
   if (e->test_enabled &&
       dest_stn == e->test_stn && dest_net == e->test_net) {
      e->tx.state     = AUN_OK;
      e->tx.reply_buf = NULL;
      e->tx.reply_len = 0;
      if (wire_type == AUN_TYPE_IMMEDIATE) {
         uint32_t n = reply_max < 4 ? reply_max : 4;
         if (reply_buf != NULL)
            memcpy(reply_buf, e->machine_id, n);
         e->tx.reply_len = n;
      } else {
         if (rx_deliver(e, port, ctrl, e->test_stn, e->test_net,
                        data, len) == NULL)
            e->tx.state = AUN_TX_NOT_LISTENING;
      }
      if (e->tx.state == AUN_OK)
         e->counters.tx_ok++;
      else
         e->counters.tx_fail++;
      return AUN_OK;
   }

   uint32_t dest_ip;
   uint16_t dest_port;
   aun_map_entry_t *m = map_find_by_addr(e, dest_net, dest_stn);
   if (m != NULL) {
      dest_ip   = m->ip_be;
      dest_port = m->udp_port;
   } else if (!learn_resolve(e, dest_net, dest_stn, &dest_ip, &dest_port)) {
      return AUN_TX_NO_ROUTE;
   }

   aun_tx_t *t = &e->tx;
   t->wire_type = wire_type;
   t->seq       = e->next_seq;
   e->next_seq += 4;
   t->ip_be     = dest_ip;
   t->udp_port  = dest_port;
   build_header(t->datagram, wire_type, port, ctrl, t->seq);
   if (len != 0)
      memcpy(&t->datagram[AUN_HDR_SIZE], data, len);
   t->len           = AUN_HDR_SIZE + len;
   /* AUN retransmit budget. On an explicit reject a DATA transmit is
    * retransmitted promptly a bounded number of times; an immediate is not.
    * Neither retransmits on silence at the engine level - AUN_NORESP_RETRIES
    * is 0 and the ANFS NFS layer retries instead (see aun.h). */
   t->noresp_left    = AUN_NORESP_RETRIES;
   t->reject_left    = (wire_type == AUN_TYPE_DATA) ? AUN_REJECT_RETRIES : 0u;
   t->reject_pending = false;
   t->reply_buf     = reply_buf;
   t->reply_max     = reply_max;
   t->reply_len     = 0;
   t->state         = AUN_STATUS_PENDING;

   if (!udp_send(e, t->ip_be, t->udp_port, t->datagram, t->len)) {
      t->state = AUN_TX_NET_ERROR;
      e->counters.tx_fail++;
      return AUN_OK;             /* accepted; failure reported via status */
   }
   t->deadline_ms = now_ms(e) + AUN_NORESP_TIMEOUT_MS;
   return AUN_OK;
}

uint8_t aun_tx_start(aun_engine_t *e, uint8_t dest_net, uint8_t dest_stn,
                     uint8_t ctrl, uint8_t port,
                     const uint8_t *data, uint32_t len)
{
   return tx_begin(e, AUN_TYPE_DATA, dest_net, dest_stn, ctrl, port,
                   data, len, NULL, 0);
}

uint8_t aun_immediate(aun_engine_t *e, uint8_t dest_net, uint8_t dest_stn,
                      uint8_t ctrl, const uint8_t *data, uint32_t len,
                      uint8_t *reply_buf, uint32_t reply_max)
{
   return tx_begin(e, AUN_TYPE_IMMEDIATE, dest_net, dest_stn, ctrl, 0,
                   data, len, reply_buf, reply_max);
}

uint8_t aun_broadcast(aun_engine_t *e, uint8_t ctrl, uint8_t port,
                      const uint8_t *data, uint32_t len)
{
   if (!e->initialised)
      return AUN_ERR_NOT_READY;
   if (len > AUN_MAX_DATA || (len != 0 && data == NULL))
      return AUN_ERR_PARAM;

   uint8_t dgram[AUN_HDR_SIZE + AUN_MAX_DATA];
   build_header(dgram, AUN_TYPE_BROADCAST, port, ctrl, e->next_seq);
   e->next_seq += 4;
   if (len != 0)
      memcpy(&dgram[AUN_HDR_SIZE], data, len);

   /* Directed to every mapped peer; broadcasts are fire-and-forget. */
   for (uint32_t i = 0; i < e->map_count; i++)
      (void)udp_send(e, e->map[i].ip_be, e->map[i].udp_port,
                     dgram, AUN_HDR_SIZE + len);
   /* ...and the local subnet broadcast, so unmapped/learn-mode
    * stations hear it too. */
   if (e->broadcast_ip_be != 0)
      (void)udp_send(e, e->broadcast_ip_be, (uint16_t)AUN_DEFAULT_UDP_PORT,
                     dgram, AUN_HDR_SIZE + len);

   /* A broadcast is also receivable locally (a station hears its own
    * broadcast on a real wire it does not, but local delivery costs
    * nothing and the test responder relies on it being absent, so:
    * do NOT self-deliver). */
   return AUN_OK;
}

uint8_t aun_tx_status(const aun_engine_t *e)
{
   return e->tx.state;
}

uint32_t aun_tx_reply_len(const aun_engine_t *e)
{
   return e->tx.reply_len;
}

/* ---- inbound -------------------------------------------------------------*/

void aun_udp_input(aun_engine_t *e, uint32_t src_ip_be, uint16_t src_port,
                   const uint8_t *buf, uint32_t len)
{
   if (!e->initialised || len < AUN_HDR_SIZE) {
      e->counters.rx_bad++;
      return;
   }

   uint8_t  type = buf[0];
   uint8_t  port = buf[1];
   uint8_t  ctrl = buf[2];
   uint32_t seq  = rd32le(&buf[4]);
   const uint8_t *data = &buf[AUN_HDR_SIZE];
   uint32_t dlen = len - AUN_HDR_SIZE;

   aun_tx_t *t = &e->tx;

   switch (type) {

   case AUN_TYPE_ACK:
   case AUN_TYPE_NAK:
      if (t->state == AUN_STATUS_PENDING && seq == t->seq &&
          src_ip_be == t->ip_be && src_port == t->udp_port) {
         /* An ACK completes a DATA transaction (an immediate completes on
          * its IMM_REPLY, not an ACK). A NAK is an explicit "not listening
          * right now": per the AUN spec the sender retransmits promptly a
          * bounded number of times before giving up, rather than failing on
          * the first reject. */
         if (type == AUN_TYPE_NAK) {
            if (t->reject_left > 0) {
               /* Spec: on reject, retransmit after a 1-centisecond timeout
                * (not instantly), a bounded number of times. Schedule it on
                * the poll timer rather than hammering the wire on receipt. */
               t->reject_left--;
               t->reject_pending = true;
               t->deadline_ms    = now_ms(e) + AUN_REJECT_TIMEOUT_MS;
            } else {
               t->state = AUN_TX_NOT_LISTENING;
               e->counters.tx_fail++;
            }
         } else if (t->wire_type == AUN_TYPE_DATA) {
            t->state = AUN_OK;
            e->counters.tx_ok++;
         }
      }
      break;

   case AUN_TYPE_IMM_REPLY:
      if (t->state == AUN_STATUS_PENDING && seq == t->seq &&
          src_ip_be == t->ip_be && src_port == t->udp_port &&
          t->wire_type == AUN_TYPE_IMMEDIATE) {
         uint32_t n = dlen < t->reply_max ? dlen : t->reply_max;
         if (t->reply_buf != NULL && n != 0)
            memcpy(t->reply_buf, data, n);
         t->reply_len = n;
         t->state     = AUN_OK;
         e->counters.tx_ok++;
      }
      break;

   case AUN_TYPE_DATA: {
      /* Diagnostic only: is this frame byte-identical to the previous
       * DATA frame from the same source on the same Econet port? This is
       * what distinguishes a retransmit (server missed our ACK, resent
       * the same block under a new seq) from a genuinely new block. It
       * never affects the verdict - the engine still decides purely on
       * whether a listener is open. Tracked per (source, port) so the
       * fileserver's &90 replies between two &92 blocks don't clobber it. */
      aun_dbg_prev_t *dp = NULL;
      for (uint32_t i = 0; i < AUN_DBG_PREV_SLOTS; i++) {
         aun_dbg_prev_t *s = &e->dbg_prev[i];
         if (s->valid && s->ip_be == src_ip_be && s->udp_port == src_port &&
             s->aun_port == port) {
            dp = s;
            break;
         }
      }
      bool same_as_prev = (dp != NULL && dlen != 0 && dlen == dp->len &&
                           memcmp(data, dp->data, dlen) == 0);
      uint8_t verdict;

      aun_map_entry_t *m = map_find_by_ip(e, src_ip_be, src_port);
      if (m == NULL) {
         /* learn mode can attribute (and auto-map) in-subnet sources */
         uint8_t lnet, lstn;
         if (learn_attribute(e, src_ip_be, src_port, &lnet, &lstn)) {
            (void)aun_map_add(e, lnet, lstn, src_ip_be, src_port);
            m = map_find_by_ip(e, src_ip_be, src_port);
         }
      }
      if (m == NULL) {
         /* AUN carries no source station; an unmapped IP cannot be
          * attributed, so it cannot be delivered. */
         e->counters.rx_unknown_source++;
         verdict = AUN_RXV_NOSRC;
      } else if (m->seq_valid && seq == m->last_rx_seq) {
         /* Retransmission of an already-ACKed frame under the SAME sequence
          * number (our ACK was lost): re-ACK, but do not deliver it to the
          * host a second time. This is the only duplicate suppression the
          * AUN spec sanctions at the transport - a frame under a *new*
          * sequence is, by definition, a new transmission, and detecting
          * application-level duplicates is the application's job (ANFS's
          * fileserver protocol carries its own sequencing). Content-based
          * suppression was removed: with the same file loaded repeatedly
          * every block is byte-identical, so it could silently drop a
          * legitimate, distinct data block. */
         e->counters.rx_dup++;
         send_ack_nak(e, src_ip_be, src_port, AUN_TYPE_ACK, port, ctrl, seq);
         verdict = AUN_RXV_REDUP;
      } else {
         aun_rx_frame_t *f = rx_deliver(e, port, ctrl, m->stn, m->net,
                                        data, dlen);
         if (f != NULL) {
            /* ACK the instant the frame lands in an open block - fast enough
             * to beat the fileserver's reply-ACK timeout (a deferred
             * collect-time ack is not, and the server retransmits). */
            send_ack_nak(e, src_ip_be, src_port, AUN_TYPE_ACK, port, ctrl, seq);
            /* stamp identity so park-and-retry can recognise this frame if
             * the host rejects it (no RXCB armed yet) and it is re-presented.
             * 'dup' marks a retransmit (identical to the previous frame on
             * this port) so it is dropped on reject rather than parked - a
             * duplicate is not an arm-race and re-presenting it just storms. */
            f->seq       = seq;
            f->src_ip_be = src_ip_be;
            f->src_port  = src_port;
            f->dup       = same_as_prev;
            m->last_rx_seq = seq;
            m->seq_valid   = true;
            e->counters.rx_data++;
            verdict = AUN_RXV_ACK;
         } else {
            /* no open block, queue full, or oversize: NAK = not listening */
            send_ack_nak(e, src_ip_be, src_port, AUN_TYPE_NAK, port, ctrl, seq);
            verdict = AUN_RXV_NAK;
         }
      }

      if (e->trace_fn != NULL)
         e->trace_fn(e->trace_user, seq, port, dlen, verdict, same_as_prev,
                     data);

      /* Remember this frame's bytes for the next comparison on this
       * (source, port). Reuse the matching slot, else take a fresh one. */
      if (dlen != 0 && dlen <= AUN_MAX_DATA) {
         if (dp == NULL) {
            dp = &e->dbg_prev[e->dbg_prev_next];
            e->dbg_prev_next = (e->dbg_prev_next + 1u) % AUN_DBG_PREV_SLOTS;
         }
         dp->valid    = true;
         dp->ip_be    = src_ip_be;
         dp->udp_port = src_port;
         dp->aun_port = port;
         dp->len      = dlen;
         memcpy(dp->data, data, dlen);
      }
      break;
   }

   case AUN_TYPE_BROADCAST: {
      /* Broadcasts are unacknowledged; deliver if anyone is listening.
       * Source may legitimately be unmapped - present as 0.0 then. */
      aun_map_entry_t *m = map_find_by_ip(e, src_ip_be, src_port);
      uint8_t s = (m != NULL) ? m->stn : 0;
      uint8_t n = (m != NULL) ? m->net : 0;
      if (rx_deliver(e, port, ctrl, s, n, data, dlen) != NULL)
         e->counters.rx_broadcast++;
      break;
   }

   case AUN_TYPE_IMMEDIATE:
      e->counters.rx_imm++;
      if (ctrl == AUN_CTRL_MACHINE_PEEK) {
         uint8_t reply[AUN_HDR_SIZE + 4];
         build_header(reply, AUN_TYPE_IMM_REPLY, port, ctrl, seq);
         memcpy(&reply[AUN_HDR_SIZE], e->machine_id, 4);
         (void)udp_send(e, src_ip_be, src_port, reply, sizeof reply);
      } else if (e->host_imm_enabled && dlen <= AUN_HIMM_MAX &&
                 (!e->himm.active ||
                  (e->himm.ip_be == src_ip_be && e->himm.port == src_port &&
                   e->himm.seq == seq))) {
         if (!e->himm.active) {       /* retransmissions are absorbed */
            e->himm.active = true;
            e->himm.ctrl   = ctrl;
            e->himm.seq    = seq;
            e->himm.ip_be  = src_ip_be;
            e->himm.port   = src_port;
            e->himm.len    = dlen;
            if (dlen != 0)
               memcpy(e->himm.data, data, dlen);
         }
      } else {
         /* unsupported / busy: refuse rather than leave the peer to
          * time out. */
         send_ack_nak(e, src_ip_be, src_port, AUN_TYPE_NAK, port, ctrl, seq);
      }
      break;

   default:
      e->counters.rx_bad++;
      break;
   }
}

/* ---- timeouts / retries ---------------------------------------------------*/

/* Re-present a parked frame once its retry timer has fired: a reply that
 * the host rejected (no RXCB armed yet) is put back into its rx block, so
 * the next pump delivers it if the CB is now armed. Runs every poll,
 * independent of any transmit in flight. */
static void rx_park_poll(aun_engine_t *e)
{
   if (!e->parked_valid || e->parked_in_queue)
      return;
   if ((int32_t)(now_ms(e) - e->parked_due_ms) < 0)
      return;

   aun_rx_block_t *b = &e->rx[e->parked_handle];
   if (!b->open) {
      e->parked_valid = false;        /* nowhere left to deliver: drop */
      return;
   }
   if (b->count >= AUN_RX_QUEUE) {
      e->parked_due_ms = now_ms(e) + AUN_PARK_DELAY_MS;   /* full: later */
      return;
   }
   aun_rx_frame_t *f = &b->q[((uint32_t)b->head + b->count) % AUN_RX_QUEUE];
   *f = e->parked;
   b->count++;
   e->parked_in_queue = true;
}

void aun_poll(aun_engine_t *e)
{
   aun_tx_t *t = &e->tx;

   if (!e->initialised)
      return;

   rx_park_poll(e);

   if (t->state != AUN_STATUS_PENDING)
      return;

   if ((int32_t)(now_ms(e) - t->deadline_ms) < 0)
      return;

   /* A reject retransmit was scheduled one centisecond ago: send it now and
    * wait out the silence window for the response. */
   if (t->reject_pending) {
      t->reject_pending = false;
      if (!udp_send(e, t->ip_be, t->udp_port, t->datagram, t->len)) {
         t->state = AUN_TX_NET_ERROR;
         e->counters.tx_fail++;
         return;
      }
      t->deadline_ms = now_ms(e) + AUN_NORESP_TIMEOUT_MS;
      return;
   }

   /* Silence: neither ACK nor reject arrived within the window. The engine
    * does not retransmit here (AUN_NORESP_RETRIES = 0) - it reports
    * NOT_LISTENING within the ROM's synchronous poll budget and lets ANFS's
    * NFS layer retry, as native Econet does on a missing scout-ack. */
   if (t->noresp_left == 0) {
      t->state = AUN_TX_NOT_LISTENING;
      e->counters.tx_fail++;
      return;
   }

   t->noresp_left--;
   if (!udp_send(e, t->ip_be, t->udp_port, t->datagram, t->len)) {
      t->state = AUN_TX_NET_ERROR;
      e->counters.tx_fail++;
      return;
   }
   t->deadline_ms = now_ms(e) + AUN_NORESP_TIMEOUT_MS;
}
