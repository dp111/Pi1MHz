/* aun.c - AUN protocol engine. See aun.h for the wire
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
 *  - ACK-on-collect: an inbound DATA frame is queued silently and ACKed
 *    only when the host collects it (see the design notes in aun.h).
 *  - Duplicate suppression, two-tier: a retransmitted DATA datagram whose
 *    original is still queued awaiting collect is dropped silently (the
 *    sender is retransmitting into our pre-ACK silence); one whose
 *    original was already collected (our ACK was lost) is re-ACKed but
 *    not delivered twice, tracked per map entry by the last collected
 *    sequence number.
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
   /* default machine-peek reply: machine type &0001 (BBC B), NFS version
    * 3.60. The version bytes are read as hex digit pairs (the ecosystem
    * convention, e.g. &0425 = "4.25"), so 3.60 is &60,&03, not &3C,&03. */
   e->machine_id[0] = 0x01;
   e->machine_id[1] = 0x00;
   e->machine_id[2] = 0x60;
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
   e->himm_cache.valid = false;       /* drop any stale replay on (re)config */
}

/* Send an IMM_REPLY datagram. Shared by aun_himm_reply (the host answering a
 * held immediate) and the replay of an already-answered immediate. */
static void send_imm_reply(aun_engine_t *e, uint32_t ip_be, uint16_t port,
                           uint8_t ctrl, uint32_t seq,
                           const uint8_t *data, uint32_t len)
{
   uint8_t dgram[AUN_HDR_SIZE + AUN_HIMM_MAX];
   if (len > AUN_HIMM_MAX)
      len = AUN_HIMM_MAX;
   build_header(dgram, AUN_TYPE_IMM_REPLY, 0, ctrl, seq);
   if (len != 0)
      memcpy(&dgram[AUN_HDR_SIZE], data, len);
   (void)udp_send(e, ip_be, port, dgram, AUN_HDR_SIZE + len);
}

/* Cache the outcome of the currently-held immediate, keyed on its
 * (ip,port,seq), so a retransmit (lost reply / reaped request) is answered
 * from here instead of being handed to the host a second time - a re-run of a
 * Poke/JSR/OSProcCall is not idempotent. is_nak records a reaped (refused)
 * immediate so its retransmit is re-refused rather than re-held. Bounded by
 * AUN_HIMM_CACHE_MS so a peer reboot that reuses a seq is not replayed stale. */
static void himm_cache_store(aun_engine_t *e, bool is_nak,
                             const uint8_t *data, uint32_t len)
{
   if (len > AUN_HIMM_MAX)
      len = AUN_HIMM_MAX;
   e->himm_cache.valid  = true;
   e->himm_cache.is_nak = is_nak;
   e->himm_cache.ip_be  = e->himm.ip_be;
   e->himm_cache.port   = e->himm.port;
   e->himm_cache.seq    = e->himm.seq;
   e->himm_cache.ctrl   = e->himm.ctrl;
   e->himm_cache.due_ms = now_ms(e) + AUN_HIMM_CACHE_MS;
   e->himm_cache.len    = is_nak ? 0u : len;
   if (!is_nak && len != 0)
      memcpy(e->himm_cache.data, data, len);
}

void aun_himm_reply(aun_engine_t *e, const uint8_t *data, uint32_t len)
{
   if (!e->himm.active)
      return;
   if (len > AUN_HIMM_MAX)
      len = AUN_HIMM_MAX;
   send_imm_reply(e, e->himm.ip_be, e->himm.port, e->himm.ctrl, e->himm.seq,
                  data, len);
   himm_cache_store(e, false, data, len);
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
   /* Re-opening resets the whole block: queued frames (validated against
    * the OLD buf_size - a smaller new buffer must never see them) and the
    * deferred-stream table go together. */
   memset(b, 0, sizeof *b);
   b->open     = true;
   b->port     = port;
   b->stn      = stn;
   b->net      = net;
   b->buf      = buf;
   b->buf_size = buf_size;
   return AUN_OK;
}

/* Find the defer entry for stream (Econet port, source) in block b, or
 * NULL. Entries whose due time has passed are lazily invalidated here, so
 * an expired deferral costs nothing and frees its slot. */
static aun_rx_defer_t *defer_find(aun_engine_t *e, aun_rx_block_t *b,
                                  uint8_t port, uint32_t src_ip_be,
                                  uint16_t src_port)
{
   for (uint32_t i = 0; i < AUN_RX_DEFER_SLOTS; i++) {
      aun_rx_defer_t *d = &b->defer[i];
      if (d->valid && d->port == port && d->src_ip_be == src_ip_be &&
          d->src_port == src_port) {
         if ((int32_t)(now_ms(e) - d->due_ms) >= 0) {
            d->valid = false;               /* expired */
            return NULL;
         }
         return d;
      }
   }
   return NULL;
}

/* Defer frame f's stream until 'due_ms'. Reuses the stream's entry, else a
 * free one. The table cannot over-subscribe (AUN_RX_DEFER_SLOTS ==
 * AUN_RX_QUEUE >= distinct streams in a full queue), but if it ever were
 * full the earliest-due entry is stolen: that stream is merely re-presented
 * early and re-deferred - nothing is lost. */
static void defer_set(aun_engine_t *e, aun_rx_block_t *b,
                      const aun_rx_frame_t *f, uint32_t due_ms)
{
   aun_rx_defer_t *victim = NULL;
   for (uint32_t i = 0; i < AUN_RX_DEFER_SLOTS; i++) {
      aun_rx_defer_t *d = &b->defer[i];
      if (d->valid && d->port == f->port && d->src_ip_be == f->src_ip_be &&
          d->src_port == f->src_port) {
         victim = d;                        /* this stream's own entry */
         break;
      }
   }
   if (victim == NULL) {
      victim = &b->defer[0];
      for (uint32_t i = 0; i < AUN_RX_DEFER_SLOTS; i++) {
         aun_rx_defer_t *d = &b->defer[i];
         if (!d->valid || (int32_t)(now_ms(e) - d->due_ms) >= 0) {
            victim = d;                     /* free or expired slot */
            break;
         }
         if ((int32_t)(victim->due_ms - d->due_ms) > 0)
            victim = d;                     /* earliest-due fallback */
      }
   }
   victim->valid     = true;
   victim->port      = f->port;
   victim->src_ip_be = f->src_ip_be;
   victim->src_port  = f->src_port;
   victim->due_ms    = due_ms;
}

/* Is frame f presently deliverable (its stream not deferred)?
 * Broadcast/local frames (src_ip_be == 0) are never deferred. */
static bool rx_frame_eligible(aun_engine_t *e, aun_rx_block_t *b,
                              const aun_rx_frame_t *f)
{
   if (f->src_ip_be == 0)
      return true;
   return defer_find(e, b, f->port, f->src_ip_be, f->src_port) == NULL;
}

/* Offset from head of the first (oldest) eligible frame, or -1 if every
 * queued frame's stream is deferred. Scanning oldest-first is what keeps
 * per-stream FIFO order: a stream's earliest queued frame is always the
 * one presented. */
static int32_t rx_first_eligible(aun_engine_t *e, aun_rx_block_t *b)
{
   for (uint32_t i = 0; i < b->count; i++) {
      aun_rx_frame_t *f = &b->q[((uint32_t)b->head + i) % AUN_RX_QUEUE];
      if (rx_frame_eligible(e, b, f))
         return (int32_t)i;
   }
   return -1;
}

/* Remove the frame at logical offset 'off' from head, closing the gap.
 * off == 0 (the common case - no stream was deferred) is a plain pop; a
 * mid-queue removal shifts the later frames down one slot. */
static void rx_remove_at(aun_rx_block_t *b, uint32_t off)
{
   if (off == 0) {
      b->head = (uint8_t)((b->head + 1u) % AUN_RX_QUEUE);
   } else {
      for (uint32_t j = off; j + 1u < b->count; j++)
         b->q[((uint32_t)b->head + j) % AUN_RX_QUEUE] =
            b->q[((uint32_t)b->head + j + 1u) % AUN_RX_QUEUE];
   }
   b->count--;
}

uint8_t aun_rx_poll(aun_engine_t *e, uint8_t handle, aun_rx_info_t *out)
{
   if (handle >= AUN_RX_BLOCKS || !e->rx[handle].open)
      return AUN_ERR_NO_BLOCK;

   aun_rx_block_t *b = &e->rx[handle];
   if (b->count == 0)
      return AUN_STATUS_PENDING;

   if (!b->presented) {
      int32_t off = rx_first_eligible(e, b);
      if (off < 0)
         return AUN_STATUS_PENDING;   /* all queued streams deferred */
      b->pres_off = (uint8_t)off;
      /* copy the frame into the host-visible buffer once; it stays there
       * untouched until collected */
      aun_rx_frame_t *pf =
         &b->q[((uint32_t)b->head + (uint32_t)off) % AUN_RX_QUEUE];
      memcpy(b->buf, pf->data, pf->len);
      b->presented = true;
   }
   aun_rx_frame_t *f = &b->q[((uint32_t)b->head + b->pres_off) % AUN_RX_QUEUE];
   if (out != NULL) {
      out->src_stn = f->src_stn;
      out->src_net = f->src_net;
      out->port    = f->port;
      out->ctrl    = f->ctrl;
      out->len     = f->len;
   }
   return AUN_OK;
}

uint32_t aun_rx_ready(aun_engine_t *e, uint8_t handle)
{
   if (handle >= AUN_RX_BLOCKS || !e->rx[handle].open)
      return 0;
   aun_rx_block_t *b = &e->rx[handle];
   uint32_t n = 0;
   for (uint32_t i = 0; i < b->count; i++)
      if (rx_frame_eligible(e, b, &b->q[((uint32_t)b->head + i) % AUN_RX_QUEUE]))
         n++;
   return n;
}

uint8_t aun_rx_collect(aun_engine_t *e, uint8_t handle, bool accept)
{
   /* accept=true: the host has taken the frame, so send the ACK NOW -
    * ACK-on-collect, the BeebEm model (see aun.h). The ACK is a true
    * delivery confirmation; the collect latency (IRQ pump + copy over the
    * 1 MHz bus, tens of ms) is far inside the sender's retransmit timer
    * (PiEconetBridge: 1 s), so in the common case the sender never even
    * retransmits. Should the ACK itself fail to send (counted in
    * ack_fail), the sender retransmits the same seq and the dup filter
    * re-ACKs it from last_rx_seq - self-healing. last_rx_seq is stamped
    * here, at ACK time, which is what makes the dup filter two-tier: a
    * same-seq retransmit BEFORE collect finds its original still queued
    * and is dropped silently instead.
    *
    * accept=false means the host's RXCB scan found no listener for this
    * frame. Rather than discard it (and pay a full sender-retransmit
    * period per CB arm race), the frame STAYS in the queue - still
    * un-ACKed - and its stream is deferred for AUN_DEFER_DELAY_MS; frames
    * of other streams are presented meanwhile (no head-of-line blocking),
    * and per-stream arrival order is preserved because nothing ever
    * leaves the queue until collected or abandoned. */
   if (handle >= AUN_RX_BLOCKS || !e->rx[handle].open)
      return AUN_ERR_NO_BLOCK;
   aun_rx_block_t *b = &e->rx[handle];
   if (b->count != 0) {
      int32_t off = b->presented ? (int32_t)b->pres_off
                                 : rx_first_eligible(e, b);
      if (off >= 0) {
         aun_rx_frame_t *f =
            &b->q[((uint32_t)b->head + (uint32_t)off) % AUN_RX_QUEUE];
         if (accept) {
            if (f->src_ip_be != 0) {
               /* DATA from the wire: confirm delivery. build_header
                * strips the bit 7 that rx_deliver set for the host. */
               send_ack_nak(e, f->src_ip_be, f->src_port, AUN_TYPE_ACK,
                            f->port, f->ctrl, f->seq);
               aun_map_entry_t *m =
                  map_find_by_ip(e, f->src_ip_be, f->src_port);
               if (m != NULL) {
                  m->last_rx_seq = f->seq;
                  m->seq_valid   = true;
               }
            }
            rx_remove_at(b, (uint32_t)off);          /* delivered */
         } else if (f->src_ip_be == 0) {
            /* non-DATA (broadcast/local): reject = intentional drop */
            rx_remove_at(b, (uint32_t)off);
         } else if (f->rejects >= AUN_DEFER_RETRIES) {
            /* nobody armed a CB for this stream across the whole budget
             * (~2 s of active rejection): a genuine stray - abandon it,
             * silently and un-ACKed, rather than clog the funnel; the
             * sender retransmits until its own budget expires */
            rx_remove_at(b, (uint32_t)off);
            e->counters.rx_parked_drop++;
         } else {
            f->rejects++;
            defer_set(e, b, f, now_ms(e) + AUN_DEFER_DELAY_MS);
         }
      }
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

/* Is a frame with this (source, wire seq) already queued awaiting collect?
 * With ACK-on-collect a sender retransmits into our silence while the pump
 * works through the queue; such a retransmit must be dropped silently - its
 * original will be ACKed at collect. (A frame already collected no longer
 * appears here; its retransmit is instead re-ACKed via last_rx_seq.) At
 * most AUN_RX_BLOCKS x AUN_RX_QUEUE comparisons - cheap next to the UDP
 * receive path that leads here. */
static bool rx_seq_pending(const aun_engine_t *e, uint32_t src_ip_be,
                           uint16_t src_port, uint32_t seq)
{
   for (uint32_t i = 0; i < AUN_RX_BLOCKS; i++) {
      const aun_rx_block_t *b = &e->rx[i];
      if (!b->open)
         continue;
      for (uint32_t j = 0; j < b->count; j++) {
         const aun_rx_frame_t *f =
            &b->q[((uint32_t)b->head + j) % AUN_RX_QUEUE];
         if (f->src_ip_be == src_ip_be && f->src_port == src_port &&
             f->seq == seq)
            return true;
      }
   }
   return false;
}

/* Find the first open receive block with queue space matching (port, src).
 * *saw_full is set when a block matched the filters but its queue was full:
 * the caller must then stay SILENT (the sender's retransmit is the flow
 * control) rather than NAK "not listening" - a NAK is near-fatal on
 * PiEconetBridge (2 NAKs dump the packet and flush its queue for us). */
static aun_rx_block_t *rx_match(aun_engine_t *e, uint8_t port,
                                uint8_t src_stn, uint8_t src_net,
                                bool *saw_full)
{
   *saw_full = false;
   for (uint32_t i = 0; i < AUN_RX_BLOCKS; i++) {
      aun_rx_block_t *b = &e->rx[i];
      if (!b->open)
         continue;
      if (b->port != 0 && b->port != port)
         continue;
      if (b->stn != AUN_WILDCARD && b->stn != src_stn)
         continue;
      if (b->net != AUN_WILDCARD && b->net != src_net)
         continue;
      if (b->count >= AUN_RX_QUEUE) {
         *saw_full = true;
         continue;
      }
      return b;
   }
   return NULL;
}

/* Stage a payload into a receive block's queue; no ACK is sent here - the
 * ACK follows when the host collects the frame (aun_rx_collect). Returns
 * NULL if no matching block could take it, with *why saying how to answer:
 * AUN_RXV_FULL  - a listener exists but its queue is full: stay silent,
 *                 the sender's retransmit delivers it once room appears;
 * AUN_RXV_NAK   - genuinely nobody listening (no block matches the port/
 *                 source, or the payload cannot ever fit): fail fast. */
static aun_rx_frame_t *rx_deliver(aun_engine_t *e, uint8_t port, uint8_t ctrl,
                                  uint8_t src_stn, uint8_t src_net,
                                  const uint8_t *data, uint32_t len,
                                  uint8_t *why)
{
   bool saw_full;
   aun_rx_block_t *b = rx_match(e, port, src_stn, src_net, &saw_full);
   if (b == NULL) {
      if (saw_full) {
         e->counters.rx_full++;
         *why = AUN_RXV_FULL;
      } else {
         e->counters.rx_no_block++;
         *why = AUN_RXV_NAK;
      }
      return NULL;
   }
   /* Bound by both the caller's buffer AND the fixed frame slot: f->data is
    * uint8_t[AUN_MAX_DATA], so a block opened with buf_size > AUN_MAX_DATA
    * must not let an oversize payload overflow the slot. */
   if (len > b->buf_size || len > AUN_MAX_DATA) {
      e->counters.rx_too_big++;
      *why = AUN_RXV_NAK;
      return NULL;
   }
   *why = AUN_RXV_ACK;
   aun_rx_frame_t *f = &b->q[((uint32_t)b->head + b->count) % AUN_RX_QUEUE];
   memcpy(f->data, data, len);
   f->len     = len;
   f->src_stn = src_stn;
   f->src_net = src_net;
   f->port    = port;
   /* present the control byte with bit 7 set again, as a real Econet
    * receive block would */
   f->ctrl    = (uint8_t)(ctrl | 0x80);
   f->rejects = 0;
   /* stream identity, keying defer-in-place, the pending-dup filter and
    * the collect-time ACK; the DATA path overwrites these with the real
    * source/seq. A zero src_ip_be marks a frame that is never deferred
    * and never ACKed (broadcast/local). */
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
         uint8_t why;
         if (rx_deliver(e, port, ctrl, e->test_stn, e->test_net,
                        data, len, &why) == NULL)
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
      /* Diagnostic only: is this frame byte-identical to the previous DATA
       * frame from the same source on the same Econet port? This feeds ONLY
       * the optional trace hook - since M4 removed content-based dedup it no
       * longer affects the verdict or park-and-retry (the engine decides
       * purely on whether a listener is open, and same-seq retransmits are
       * caught by last_rx_seq below). So the whole previous-bytes machine -
       * including the up-to-8 KB memcmp and memcpy on this hot lwIP receive
       * path - is skipped entirely when no trace hook is installed (the
       * field case). Tracked per (source, port) so the fileserver's &90
       * replies between two &92 blocks don't clobber it. */
      bool same_as_prev = false;
      aun_dbg_prev_t *dp = NULL;
      if (e->trace_fn != NULL) {
         for (uint32_t i = 0; i < AUN_DBG_PREV_SLOTS; i++) {
            aun_dbg_prev_t *s = &e->dbg_prev[i];
            if (s->valid && s->ip_be == src_ip_be && s->udp_port == src_port &&
                s->aun_port == port) {
               dp = s;
               break;
            }
         }
         same_as_prev = (dp != NULL && dlen != 0 && dlen == dp->len &&
                         memcmp(data, dp->data, dlen) == 0);
      }
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
      } else if (rx_seq_pending(e, src_ip_be, src_port, seq)) {
         /* Retransmission of a frame still queued awaiting collect: the
          * sender retransmitting into our pre-ACK silence while the pump
          * works. Drop it silently - the queued original is ACKed when the
          * host collects it. */
         e->counters.rx_dup++;
         verdict = AUN_RXV_REDUP;
      } else if (m->seq_valid && seq == m->last_rx_seq) {
         /* Retransmission of an already-COLLECTED frame under the SAME
          * sequence number (our collect-time ACK was lost): re-ACK, but do
          * not deliver it to the host a second time. Same-seq suppression
          * is the only duplicate suppression the AUN spec sanctions at the
          * transport - a frame under a *new* sequence is, by definition, a
          * new transmission, and detecting application-level duplicates is
          * the application's job (ANFS's fileserver protocol carries its
          * own sequencing). Content-based suppression was removed: with
          * the same file loaded repeatedly every block is byte-identical,
          * so it could silently drop a legitimate, distinct data block. */
         e->counters.rx_dup++;
         send_ack_nak(e, src_ip_be, src_port, AUN_TYPE_ACK, port, ctrl, seq);
         verdict = AUN_RXV_REDUP;
      } else {
         uint8_t why;
         aun_rx_frame_t *f = rx_deliver(e, port, ctrl, m->stn, m->net,
                                        data, dlen, &why);
         if (f != NULL) {
            /* Queued, NO ack yet - the ACK goes out when the host collects
             * (ACK-on-collect, see aun.h). Stamp the wire identity: it keys
             * defer-in-place, the pending-dup filter above, and the
             * collect-time ACK itself. */
            f->seq       = seq;
            f->src_ip_be = src_ip_be;
            f->src_port  = src_port;
            e->counters.rx_data++;
            verdict = AUN_RXV_ACK;
         } else if (why == AUN_RXV_FULL) {
            /* Queue full: SILENT. The sender's retransmit-into-silence is
             * the flow control (PiEconetBridge: ~1 s x 5; BeebEm behaves
             * identically when its single buffer is busy). Never NAK for
             * flow control - 2 NAKs make the bridge dump the packet and
             * flush its queue for us. */
            verdict = AUN_RXV_FULL;
         } else {
            /* no open block for this port/source, or oversize: genuinely
             * not listening - NAK so the sender fails fast */
            send_ack_nak(e, src_ip_be, src_port, AUN_TYPE_NAK, port, ctrl, seq);
            verdict = AUN_RXV_NAK;
         }
      }

      if (e->trace_fn != NULL) {
         e->trace_fn(e->trace_user, seq, port, dlen, verdict, same_as_prev,
                     data);
         /* Remember this frame's bytes for the next comparison on this
          * (source, port). Reuse the matching slot, else take a fresh one.
          * Inside the trace guard: with no hook this copy is pure waste. */
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
      }
      break;
   }

   case AUN_TYPE_BROADCAST: {
      /* Broadcasts are unacknowledged; deliver if anyone is listening.
       * Source may legitimately be unmapped - present as 0.0 then. */
      aun_map_entry_t *m = map_find_by_ip(e, src_ip_be, src_port);
      uint8_t s = (m != NULL) ? m->stn : 0;
      uint8_t n = (m != NULL) ? m->net : 0;
      uint8_t why;
      if (rx_deliver(e, port, ctrl, s, n, data, dlen, &why) != NULL)
         e->counters.rx_broadcast++;
      break;
   }

   case AUN_TYPE_IMMEDIATE:
      e->counters.rx_imm++;
      if (ctrl == AUN_CTRL_MACHINE_PEEK) {
         /* the engine answers a machine peek itself: 4-byte machine id */
         send_imm_reply(e, src_ip_be, src_port, ctrl, seq, e->machine_id, 4);
      } else if (e->host_imm_enabled && dlen <= AUN_HIMM_MAX) {
         if (e->himm.active) {
            /* one held at a time: absorb a retransmit of the one in flight
             * (the host is still working on it), refuse anything else. */
            if (!(e->himm.ip_be == src_ip_be && e->himm.port == src_port &&
                  e->himm.seq == seq))
               send_ack_nak(e, src_ip_be, src_port, AUN_TYPE_NAK,
                            port, ctrl, seq);
         } else if (e->himm_cache.valid &&
                    (int32_t)(now_ms(e) - e->himm_cache.due_ms) < 0 &&
                    e->himm_cache.ip_be == src_ip_be &&
                    e->himm_cache.port == src_port &&
                    e->himm_cache.seq == seq) {
            /* Retransmit of an immediate we already resolved (and not yet
             * expired): replay the recorded outcome - the positive reply, or
             * the NAK if it was reaped - do NOT hand it to the host again
             * (Poke/JSR/OSProcCall are not idempotent). */
            if (e->himm_cache.is_nak)
               send_ack_nak(e, src_ip_be, src_port, AUN_TYPE_NAK,
                            port, ctrl, seq);
            else
               send_imm_reply(e, e->himm_cache.ip_be, e->himm_cache.port,
                              e->himm_cache.ctrl, e->himm_cache.seq,
                              e->himm_cache.data, e->himm_cache.len);
            e->counters.himm_replay++;
         } else {                     /* fresh immediate: hold it for the host */
            e->himm.gen++;            /* new slot generation (gen guard) */
            e->himm.active = true;
            e->himm.ctrl   = ctrl;
            e->himm.seq    = seq;
            e->himm.ip_be  = src_ip_be;
            e->himm.port   = src_port;
            e->himm.len    = dlen;
            e->himm.due_ms = now_ms(e) + AUN_HIMM_TIMEOUT_MS;
            if (dlen != 0)
               memcpy(e->himm.data, data, dlen);
         }
      } else {
         /* unsupported / too big: refuse rather than leave the peer to
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

/* (Deferred streams need no poll-time work: a deferral expires by itself -
 * rx_first_eligible/aun_rx_ready simply start seeing the frame again once
 * now_ms passes the entry's due time.) */

void aun_poll(aun_engine_t *e)
{
   aun_tx_t *t = &e->tx;

   if (!e->initialised)
      return;

   /* Reap a held host-immediate the host never answered: release the slot
    * (re-enabling inbound immediates and clearing nIRQ bit &40) and NAK the
    * originator so it fails fast instead of waiting out its own timeout. */
   if (e->himm.active && (int32_t)(now_ms(e) - e->himm.due_ms) >= 0) {
      send_ack_nak(e, e->himm.ip_be, e->himm.port, AUN_TYPE_NAK,
                   0, e->himm.ctrl, e->himm.seq);
      /* Seed the cache as a NAK-marker: a retransmit of this reaped immediate
       * must be re-refused, not re-held and re-executed (the host's eventual
       * late reply is separately dropped by the gen guard in aun_himm_reply). */
      himm_cache_store(e, true, NULL, 0);
      e->himm.active = false;
      e->counters.himm_timeout++;
   }

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
