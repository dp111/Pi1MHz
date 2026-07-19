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
 * in the field/ROM, handle 1 in the host test suite); each rx block is large
 * (AUN_RX_QUEUE x AUN_MAX_DATA ~= 64 KB), so 2 rather than 4 reclaims ~131 KB.
 * The funnel depth lives in AUN_RX_QUEUE: handle 0 absorbs an 8-frame burst
 * (data + reply + completion + a few peer retransmits) before it NAKs. */
#define AUN_RX_BLOCKS         2u
#define AUN_RX_QUEUE          8u   /* frames buffered per rx block      */
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

/* park-and-retry: a DATA frame the host rejected (no RXCB armed yet) is
 * held out of the rx queue and re-presented a few times, so a reply that
 * arrives a beat before its control block is armed is retried rather than
 * lost. ~16 x 8ms ~= 128ms covers ANFS arming its CB; a true stray drops. */
#define AUN_PARK_RETRIES      16u
#define AUN_PARK_DELAY_MS     8u
/* Pool depth for the above: a single slot loses the SECOND frame that needs
 * parking in the same beat (its ACK already went out, so it cannot be
 * recovered - see aun_rx_collect). This is not rare: a fileserver's closing
 * reply on the FS control port routinely lands one poll cycle behind the
 * last data block on the transfer port, and if that last block also needed
 * parking (CB armed a beat late), a lone slot silently drops the close
 * reply, which is exactly what leaves the Beeb hanging with no reply ever
 * arriving.
 *
 * A fixed small pool (originally 4) is not enough either: the fileserver
 * paces its sends on the engine's ACK-on-receipt, not on the (much slower,
 * real 6502) ROM pump actually re-arming the next CB, so a fast fileserver
 * can have several distinct blocks rejected-and-awaiting-retry at once
 * before the ROM catches up. AUN_RX_QUEUE is the hard ceiling on how many
 * distinct frames can ever be in flight for one handle (a fuller queue is
 * NAKed at the network layer, so the sender retransmits instead of piling
 * up more), so sizing the pool to match guarantees a reject is never lost
 * to pool exhaustion - only genuine abandonment (AUN_PARK_RETRIES exhausted
 * with no listener ever arming) still drops a frame. */
#define AUN_PARK_SLOTS        AUN_RX_QUEUE

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
   uint32_t seq;               /* wire seq, for park-and-retry identity */
   uint32_t src_ip_be;
   uint16_t src_port;
   uint8_t  data[AUN_MAX_DATA];
} aun_rx_frame_t;

/* One held rejected frame, part of the AUN_PARK_SLOTS pool below. */
typedef struct {
   aun_rx_frame_t frame;
   bool           valid;
   bool           in_queue;   /* re-injected, awaiting verdict */
   uint8_t        handle;
   uint8_t        retries;
   uint32_t       due_ms;
} aun_park_t;

typedef struct {
   bool     open;
   bool     presented;         /* head frame copied into buf       */
   uint8_t  port;              /* filter: 0 = any port             */
   uint8_t  stn, net;          /* filter: AUN_WILDCARD = any       */
   uint8_t *buf;               /* where RX_POLL presents the head  */
   uint32_t buf_size;
   uint8_t  head, count;       /* FIFO of received frames: frames  */
   aun_rx_frame_t q[AUN_RX_QUEUE];  /* are ACKed and queued; NAK only
                                       when the queue is full       */
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
   uint32_t rx_parked_drop;    /* parked reply dropped (budget/closed/busy)  */
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
   /* park-and-retry: a small pool of rejected DATA frames held for
    * re-presentation - see AUN_PARK_SLOTS. */
   aun_park_t      parked[AUN_PARK_SLOTS];
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

/* AUN_STATUS_PENDING while the queue is empty; AUN_OK once a frame is
 * available - the head frame is copied into the block's buffer and
 * described in *out (may be NULL). Polling is idempotent: the head
 * stays presented (and cannot be overwritten) until aun_rx_collect()
 * pops it. Frames beyond the head are ACKed and queued (up to
 * AUN_RX_QUEUE); only a full queue NAKs, making the sender retry. */
typedef struct {
   uint8_t  src_stn, src_net, port, ctrl;
   uint32_t len;
} aun_rx_info_t;
uint8_t aun_rx_poll(aun_engine_t *e, uint8_t handle, aun_rx_info_t *out);

/* Release the held packet and re-arm the block. The ACK was already sent
 * on receipt, so collect is silent on the wire: accept=true simply pops
 * the head; accept=false (no listener took it) parks the frame for a few
 * re-presentations in case its control block is armed a beat later. */
uint8_t aun_rx_collect(aun_engine_t *e, uint8_t handle, bool accept);

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
