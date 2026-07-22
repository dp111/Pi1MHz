/* aun.h - AUN (Acorn Universal Networking) protocol engine.
 *
 * Pure C, no bare-metal or lwIP dependencies: the UDP transport and the
 * millisecond clock are injected via function pointers, so this module
 * compiles and runs unmodified on a PC host for unit testing (see
 * tests/aun/).  All platform glue lives in aun_emulator.c.
 *
 * Wire format (one UDP datagram per AUN transaction leg, default UDP
 * port 32768):
 *
 *   +0  type      1=broadcast 2=data 3=ack 4=nak 5=immediate 6=imm-reply
 *   +1  port      Econet port byte
 *   +2  control   Econet control byte with bit 7 CLEARED on the wire
 *   +3  pad       0
 *   +4  sequence  32-bit little-endian; +4 per transaction, ACK/NAK echo it
 *   +8  data...
 *
 * The Econet four-way handshake collapses to data -> ack/nak over IP.
 * Source station identity is NOT carried in the datagram: it is derived
 * by reverse-looking-up the sender's IP:port in the station map, exactly
 * as AUN/PiEconetBridge/BeebEm do.
 */

#ifndef AUN_H
#define AUN_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ---- wire constants ---------------------------------------------------- */

#define AUN_DEFAULT_UDP_PORT  32768u
#define AUN_HDR_SIZE          8u

/* lwIP has IP_FRAG/IP_REASSEMBLY enabled, so AUN datagrams may exceed
 * one Ethernet frame; 8K covers every fileserver bulk size in use. */
#define AUN_MAX_DATA          8192u

#define AUN_TYPE_BROADCAST    1u
#define AUN_TYPE_DATA         2u
#define AUN_TYPE_ACK          3u
#define AUN_TYPE_NAK          4u
#define AUN_TYPE_IMMEDIATE    5u
#define AUN_TYPE_IMM_REPLY    6u

/* Econet immediate control byte for machine peek, as seen on the wire
 * (bit 7 stripped: &88 -> &08). */
#define AUN_CTRL_MACHINE_PEEK 0x08u

/* ---- status codes (also what the Beeb sees) ----------------------------- */

#define AUN_OK                0u
#define AUN_TX_NOT_LISTENING  1u   /* NAK received, or all retries timed out */
#define AUN_TX_NET_ERROR      2u   /* transport send failed                  */
#define AUN_TX_NO_ROUTE       3u   /* destination not in station map         */
#define AUN_TX_BUSY           4u   /* a transmit is already in flight        */
#define AUN_ERR_PARAM         5u   /* bad length/handle/argument             */
#define AUN_ERR_NOT_READY     6u   /* engine not initialised / no network    */
#define AUN_ERR_NO_BLOCK      7u   /* rx handle not open                     */
#define AUN_STATUS_PENDING    0x80u /* tx in flight / rx block still waiting */

/* ---- limits ------------------------------------------------------------- */

#define AUN_MAP_MAX           32u
/* ANFS opens exactly one receive block (handle 0, the wildcard funnel - see
 * eco_library.asm), through which ALL inbound frames pass before the ROM
 * pump demultiplexes them to internal CBs. So the depth that matters is the
 * per-block queue, not the block count. Two blocks cover every user (handle 0
 * in the field/ROM, handle 1 in the host test suite).
 * The funnel depth lives in AUN_RX_QUEUE: handle 0 absorbs a 16-frame burst
 * (data + reply + completion + a few peer retransmits) before it NAKs. 16,
 * not 8: with ACK-on-receipt the engine ACKs into the queue at wire speed
 * while the real-6502 ROM pump drains it at block-copy speed, so a fast
 * fileserver runs a long way ahead - and frames a not-yet-armed CB has
 * rejected now stay IN the queue (see the defer notes below), so the queue
 * alone is the whole absorb budget. Once it fills, fresh frames are NAKed
 * and the sender's own retransmit provides the flow control. */
#define AUN_RX_BLOCKS         2u
#define AUN_RX_QUEUE          16u  /* frames buffered per rx block      */
/* Outbound retransmit. The AUN spec (RISC OS PRM, "AUN") describes a
 * background Net module that, on SILENCE, waits up to 5 s and retransmits
 * while the application's SWI returns. Our ANFS ROM does NOT work that
 * way: eco_tx_begin polls TX_POLL synchronously until the engine
 * completes (no ROM-side timeout), so the engine's timeout IS the Beeb's
 * foreground stall. The ROM is designed around a ~1 s budget. So:
 *  - on an explicit REJECT, retransmit promptly, a bounded number of
 *    times (the spec's fast reject path - cheap, stays responsive);
 *  - on SILENCE, do NOT retransmit at the engine level. Report
 *    NOT_LISTENING after ~1 s and let ANFS's NFS layer retry the whole
 *    transmit, exactly as native Econet does on a missing scout-ack.
 *    This keeps the foreground stall bounded AND means the engine never
 *    emits a silence retransmit that could land mid-transaction on a
 *    strictly single-transaction peer (e.g. BeebEm) and abort its
 *    four-way. (The old 250 ms blanket retry did exactly that; the 5 s
 *    spec value instead froze the Beeb for 5 s per lost ack.) */
#define AUN_REJECT_RETRIES    10u   /* bounded retransmits on receipt of reject */
#define AUN_REJECT_TIMEOUT_MS 10u   /* AUN spec: 1 centisecond between them      */
#define AUN_NORESP_TIMEOUT_MS 1000u /* fail after ~1 s of silence (ROM budget)  */
#define AUN_NORESP_RETRIES    0u    /* no engine silence-retransmit; NFS retries */
/* A held host-immediate is released by aun_himm_reply(); if the host never
 * replies (unimplemented op, aborted 6502 handler, the ROM's own command
 * timeout) it must not wedge the engine - every later inbound immediate would
 * be NAKed and nIRQ bit &40 would stay asserted. aun_poll() reaps a stale held
 * immediate after this bound and NAKs the originator so it fails fast. */
#define AUN_HIMM_TIMEOUT_MS   1000u

/* How long the last-answered-immediate cache (and a reap NAK-marker) stays
 * valid for replay. Bounds the window during which a lost IMM_REPLY can be
 * re-answered from cache; after it, a peer that reboots and reuses the seq
 * gets a fresh execution, not a stale replay. */
#define AUN_HIMM_CACHE_MS     2000u

/* defer-in-place: a DATA frame the host rejected (no RXCB armed yet) STAYS
 * in the rx queue, and its stream - (Econet port, source) - is deferred for
 * AUN_DEFER_DELAY_MS before the frame is presented again. This replaces the
 * old park-and-retry side pool (copy the reject out of the queue, re-inject
 * it later), which had three field-failure modes:
 *
 *  - pool exhaustion: parking vacated a queue slot, so up to
 *    AUN_RX_QUEUE + pool distinct ACKed frames could be in flight and the
 *    next reject was dropped for good (its ACK had already gone out, so the
 *    sender believed it delivered - the classic silent "No reply" hang);
 *  - a ~128 ms retry budget: any window where the CB stayed unarmed longer
 *    (e.g. the whole 1 s TX_POLL silence wait after a lost server ACK, with
 *    the server already streaming data) dropped ACKed frames;
 *  - reordering: a parked frame was invisible for the park delay and then
 *    re-injected at the TAIL, so a newer same-port frame could overtake it
 *    and bulk data blocks arrived at the ROM swapped.
 *
 * Keeping the frame in the queue kills all three by construction: nothing
 * is copied (no pool to exhaust), nothing ACKed is dropped short of the
 * generous budget below, and per-stream order is the queue order. Frames of
 * OTHER streams still bypass a deferred one (aun_rx_poll presents the first
 * frame whose stream is not deferred), so the wildcard funnel has no
 * head-of-line blocking - the reason parking existed at all.
 *
 * Budget: one reject is burned per presentation, and a deferred stream is
 * re-presented at most every AUN_DEFER_DELAY_MS, so 250 x 8 ms ~= 2 s. That
 * covers the ROM's full 1 s tx-silence window (plus foreground stalls) with
 * margin, while a genuinely stray stream - one nobody ever arms a CB for -
 * is still dropped rather than clogging the funnel forever. */
#define AUN_DEFER_RETRIES     250u
#define AUN_DEFER_DELAY_MS    8u
/* Deferred-stream table depth. Each entry keys one (Econet port, src ip,
 * src UDP port) stream; a full queue can hold at most AUN_RX_QUEUE distinct
 * streams, so sizing the table to match means it cannot over-subscribe. */
#define AUN_RX_DEFER_SLOTS    AUN_RX_QUEUE

#define AUN_WILDCARD          0xFFu /* in rx-block station/net filters       */

/* ---- injected transport -------------------------------------------------*/

typedef struct {
   /* Send one UDP datagram. ip_be is the IPv4 address in network byte
    * order. Returns true if the datagram was handed to the stack. */
   bool (*udp_send)(void *user, uint32_t ip_be, uint16_t udp_port,
                    const uint8_t *buf, size_t len);
   /* Monotonic milliseconds. Wrap is handled. */
   uint32_t (*now_ms)(void *user);
   void *user;
} aun_transport_t;

/* ---- engine state -------------------------------------------------------*/

typedef struct {
   uint8_t  net, stn;          /* Econet address                  */
   uint32_t ip_be;             /* peer IPv4, network byte order   */
   uint16_t udp_port;
   uint32_t last_rx_seq;       /* duplicate suppression           */
   bool     seq_valid;
} aun_map_entry_t;

typedef struct {
   uint32_t len;
   uint8_t  src_stn, src_net, port, ctrl;
   uint8_t  rejects;           /* host rejections so far (defer budget) */
   uint32_t seq;               /* wire seq (diagnostics)                */
   uint32_t src_ip_be;         /* stream key; 0 = broadcast/local, never
                                * deferred                              */
   uint16_t src_port;
   uint8_t  data[AUN_MAX_DATA];
} aun_rx_frame_t;

/* One deferred stream: frames from (src ip:port) on this Econet port stay
 * queued but are not presented until due_ms - see AUN_DEFER_DELAY_MS. */
typedef struct {
   bool     valid;
   uint8_t  port;              /* Econet port of the rejected frame */
   uint16_t src_port;          /* source UDP port                   */
   uint32_t src_ip_be;
   uint32_t due_ms;
} aun_rx_defer_t;

typedef struct {
   bool     open;
   bool     presented;         /* presented frame copied into buf  */
   uint8_t  pres_off;          /* its offset from head while presented */
   uint8_t  port;              /* filter: 0 = any port             */
   uint8_t  stn, net;          /* filter: AUN_WILDCARD = any       */
   uint8_t *buf;               /* where RX_POLL presents a frame   */
   uint32_t buf_size;
   uint8_t  head, count;       /* FIFO of received frames: frames  */
   aun_rx_frame_t q[AUN_RX_QUEUE];  /* are ACKed and queued; NAK only
                                       when the queue is full       */
   /* streams the host rejected, awaiting re-presentation */
   aun_rx_defer_t defer[AUN_RX_DEFER_SLOTS];
} aun_rx_block_t;

typedef struct {
   uint8_t  state;             /* AUN_STATUS_PENDING or a final AUN_* code */
   uint8_t  wire_type;         /* DATA / BROADCAST / IMMEDIATE    */
   uint32_t seq;
   uint32_t ip_be;
   uint16_t udp_port;
   uint8_t  datagram[AUN_HDR_SIZE + AUN_MAX_DATA];
   uint32_t len;               /* full datagram length, kept for retries */
   uint32_t deadline_ms;        /* when the current (reject/silence) timer fires */
   uint8_t  noresp_left;        /* retransmits remaining for the silent case  */
   uint8_t  reject_left;        /* prompt retransmits remaining after rejects */
   bool     reject_pending;     /* a reject retransmit is scheduled at deadline */
   /* immediate-reply destination (NULL when not an immediate op) */
   uint8_t *reply_buf;
   uint32_t reply_max;
   uint32_t reply_len;
} aun_tx_t;

typedef struct {
   uint32_t tx_ok, tx_fail;
   uint32_t rx_data, rx_broadcast, rx_imm;
   uint32_t rx_dup, rx_no_block, rx_unknown_source, rx_too_big, rx_bad;
   uint32_t ack_sent, nak_sent;
   uint32_t ack_fail;          /* ACK/NAK that the transport failed to send */
   uint32_t rx_parked_drop;    /* deferred frame dropped: reject budget spent */
   uint32_t himm_timeout;      /* held immediate reaped: host never replied  */
   uint32_t himm_replay;       /* answered immediate retransmitted: replayed */
} aun_counters_t;

/* Inbound immediate operation held for the host to execute (remote
 * peek/poke/JSR/halt/continue - everything except machine peek, which
 * the engine answers itself). */
#define AUN_HIMM_MAX 2048u
typedef struct {
   bool     active;
   uint8_t  ctrl;              /* wire ctrl &01-&07 */
   uint32_t seq;
   uint32_t ip_be;
   uint16_t port;
   uint32_t len;
   uint32_t due_ms;            /* reap deadline if the host never replies */
   uint32_t gen;               /* bumped each fresh hold; identifies the slot */
   uint32_t polled_gen;        /* gen the host last took via IMM_POLL         */
   uint8_t  data[AUN_HIMM_MAX];
} aun_host_imm_t;

/* One-slot cache of the last host immediate we answered. A peer that does not
 * receive our IMM_REPLY retransmits the same immediate under the same seq;
 * once the host has already executed and answered it, re-delivering it would
 * run a non-idempotent op (Poke/JSR/OSProcCall) a second time. Keyed on
 * (ip,port,seq); a matching retransmit is re-answered from here instead. */
typedef struct {
   bool     valid;
   bool     is_nak;            /* true: replay a NAK (reaped), not an IMM_REPLY */
   uint8_t  ctrl;
   uint32_t seq;
   uint32_t ip_be;
   uint16_t port;
   uint32_t len;
   uint32_t due_ms;            /* replay only while now < due_ms (expiry)       */
   uint8_t  data[AUN_HIMM_MAX];
} aun_himm_cache_t;

/* Verdict the engine reached for an inbound DATA frame, reported to the
 * optional diagnostic trace hook below. */
#define AUN_RXV_ACK    0u   /* delivered into an open block, ACKed       */
#define AUN_RXV_REDUP  1u   /* same seq as last accepted: re-ACKed       */
#define AUN_RXV_NAK    2u   /* no listener / queue full / oversize: NAK  */
#define AUN_RXV_NOSRC  3u   /* unmapped source: dropped silently         */

/* Optional diagnostic hook, fired once per inbound DATA frame. The
 * firmware points this at its logger; host unit tests leave it NULL.
 * 'same_as_prev' is true when this frame's payload is byte-identical to
 * the previous DATA frame from the same IP:port on the same Econet port -
 * the signal that tells a retransmit apart from a genuinely new block. */
typedef void (*aun_trace_fn)(void *user, uint32_t seq, uint8_t port,
                             uint32_t len, uint8_t verdict, bool same_as_prev,
                             const uint8_t *data);

/* Diagnostics: the previous DATA frame seen on a given (source, Econet
 * port), kept so a new frame can be compared byte-for-byte against the
 * last one on the *same* port - intervening frames on other ports (e.g.
 * the fileserver's &90 replies between two &92 data blocks) must not
 * clobber the comparison. A few slots cover the ports in play. */
#define AUN_DBG_PREV_SLOTS 6u
typedef struct {
   bool     valid;
   uint32_t ip_be;
   uint16_t udp_port;
   uint8_t  aun_port;
   uint32_t len;
   uint8_t  data[AUN_MAX_DATA];
} aun_dbg_prev_t;

typedef struct {
   bool            initialised;
   uint8_t         station, net;
   aun_transport_t transport;
   uint32_t        next_seq;
   aun_map_entry_t map[AUN_MAP_MAX];
   uint32_t        map_count;
   aun_rx_block_t  rx[AUN_RX_BLOCKS];
   aun_tx_t        tx;
   aun_counters_t  counters;
   uint32_t        broadcast_ip_be;   /* 0 = none */
   uint32_t        subnet_base_be, subnet_mask_be;
   uint8_t         learn_net;          /* 0xFF = learn mode off */
   /* 4-byte machine-peek reply: type lo, type hi, NFS ver minor, major */
   uint8_t         machine_id[4];
   bool            host_imm_enabled;
   aun_host_imm_t  himm;
   aun_himm_cache_t himm_cache;       /* last-answered immediate, for replay */
   /* loopback test responder: frames sent to this station are ACKed
    * locally and echoed back through the rx path as if from it. */
   bool            test_enabled;
   uint8_t         test_stn, test_net;
   /* diagnostics: optional per-DATA-frame trace + the previous DATA
    * frame's bytes, kept so each new frame can be compared against it. */
   aun_trace_fn    trace_fn;
   void           *trace_user;
   aun_dbg_prev_t  dbg_prev[AUN_DBG_PREV_SLOTS];
   uint32_t        dbg_prev_next;   /* round-robin victim slot */
} aun_engine_t;

/* ---- API ----------------------------------------------------------------*/

/* Reset the engine and bind it to a transport. station/net is our own
 * Econet address. */
void aun_init(aun_engine_t *e, const aun_transport_t *transport,
              uint8_t station, uint8_t net);

/* Optional address conventions (set after aun_init):
 *  - broadcast_ip_be != 0: aun_broadcast() also sends one datagram to
 *    this address (the local subnet broadcast).
 *  - learn_net != 0xFF: stations without a map entry resolve by the
 *    classic AUN convention - IP = subnet_base | station, and inbound
 *    datagrams from unmapped in-subnet IPs on the default port are
 *    attributed as (learn_net, last octet). */
void aun_set_addressing(aun_engine_t *e, uint32_t broadcast_ip_be,
                        uint32_t subnet_base_be, uint32_t mask_be,
                        uint8_t learn_net);

/* Add (or update) a station -> IP:port mapping. udp_port 0 means
 * AUN_DEFAULT_UDP_PORT. Returns false when the table is full. */
bool aun_map_add(aun_engine_t *e, uint8_t net, uint8_t stn,
                 uint32_t ip_be, uint16_t udp_port);

/* Start a transmit. Exactly one transmit may be in flight; poll
 * aun_tx_status() for completion. Returns AUN_OK when accepted, else an
 * error code. 'data' is copied. */
uint8_t aun_tx_start(aun_engine_t *e, uint8_t dest_net, uint8_t dest_stn,
                     uint8_t ctrl, uint8_t port,
                     const uint8_t *data, uint32_t len);

/* Broadcast: sent once to every mapped peer, no acknowledgement,
 * completes immediately. */
uint8_t aun_broadcast(aun_engine_t *e, uint8_t ctrl, uint8_t port,
                      const uint8_t *data, uint32_t len);

/* Immediate operation (e.g. machine peek): the reply payload lands in
 * reply_buf. Completion via aun_tx_status(); reply length via
 * aun_tx_reply_len(). */
uint8_t aun_immediate(aun_engine_t *e, uint8_t dest_net, uint8_t dest_stn,
                      uint8_t ctrl, const uint8_t *data, uint32_t len,
                      uint8_t *reply_buf, uint32_t reply_max);

/* AUN_STATUS_PENDING while in flight, then AUN_OK or an error code.
 * Idle (nothing ever sent) reads as AUN_OK. */
uint8_t  aun_tx_status(const aun_engine_t *e);
uint32_t aun_tx_reply_len(const aun_engine_t *e);

/* Open receive block 'handle' (0..AUN_RX_BLOCKS-1). port 0 matches any
 * port; stn/net AUN_WILDCARD match any source. 'buf' must stay valid
 * until the block is closed. */
uint8_t aun_rx_open(aun_engine_t *e, uint8_t handle, uint8_t port,
                    uint8_t stn, uint8_t net, uint8_t *buf, uint32_t buf_size);

/* AUN_STATUS_PENDING while nothing is deliverable; AUN_OK once a frame is
 * available - the oldest frame whose stream is not deferred is copied into
 * the block's buffer and described in *out (may be NULL). Polling is
 * idempotent: the presented frame stays presented (and cannot be
 * overwritten) until aun_rx_collect() releases it. Frames are ACKed and
 * queued (up to AUN_RX_QUEUE); only a full queue NAKs, making the sender
 * retry. */
typedef struct {
   uint8_t  src_stn, src_net, port, ctrl;
   uint32_t len;
} aun_rx_info_t;
uint8_t aun_rx_poll(aun_engine_t *e, uint8_t handle, aun_rx_info_t *out);

/* Release the presented packet and re-arm the block. The ACK was already
 * sent on receipt, so collect is silent on the wire: accept=true pops the
 * presented frame; accept=false (no listener took it) leaves it queued and
 * defers its stream for AUN_DEFER_DELAY_MS, in case its control block is
 * armed a beat later - frames of other streams are presented meanwhile. */
uint8_t aun_rx_collect(aun_engine_t *e, uint8_t handle, bool accept);

/* Number of frames presently deliverable on 'handle' (queued frames whose
 * stream is not deferred). Feed this - not the raw queue count - to the
 * host interrupt line: a queue holding only deferred frames must not
 * assert nIRQ, or the host pump spins polling AUN_STATUS_PENDING until
 * the defer expires. */
uint32_t aun_rx_ready(aun_engine_t *e, uint8_t handle);

uint8_t aun_rx_close(aun_engine_t *e, uint8_t handle);

/* Feed an inbound UDP datagram into the engine (call from the
 * transport's receive path). */
void aun_udp_input(aun_engine_t *e, uint32_t src_ip_be, uint16_t src_port,
                   const uint8_t *buf, uint32_t len);

/* Drive retries/timeouts. Call frequently (every main-loop poll). */
void aun_poll(aun_engine_t *e);

/* Configure the loopback test responder (see header comment in
 * aun.c). enable=false turns it off. */
void aun_test_responder(aun_engine_t *e, bool enable,
                        uint8_t stn, uint8_t net);

void aun_set_machine_id(aun_engine_t *e, const uint8_t id[4]);

/* Install (or clear, fn=NULL) the inbound-DATA diagnostic trace hook. */
void aun_set_trace(aun_engine_t *e, aun_trace_fn fn, void *user);

/* Host-executed immediates: when enabled, inbound immediate ops other
 * than machine peek are held (e->himm) for the host to execute;
 * aun_himm_reply() sends the IMM_REPLY and releases. While one is
 * held, further immediates are NAKed. */
void aun_set_host_imm(aun_engine_t *e, bool enable);
void aun_himm_reply(aun_engine_t *e, const uint8_t *data, uint32_t len);

#endif
