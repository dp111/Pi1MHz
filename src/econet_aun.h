/* econet_aun.h - AUN (Acorn Universal Networking) protocol engine.
 *
 * Pure C, no bare-metal or lwIP dependencies: the UDP transport and the
 * millisecond clock are injected via function pointers, so this module
 * compiles and runs unmodified on a PC host for unit testing (see
 * tests/econet/).  All platform glue lives in econet_emulator.c.
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

#ifndef ECONET_AUN_H
#define ECONET_AUN_H

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
#define AUN_RX_BLOCKS         8u
#define AUN_RX_QUEUE          4u   /* frames buffered per rx block      */
#define AUN_RETRIES           4u    /* total attempts per transmit           */
#define AUN_ACK_TIMEOUT_MS    250u  /* per attempt                           */

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
   bool     needs_verdict;     /* DATA frame: ACK/NAK owed at collect */
   uint32_t seq;               /* wire seq, for the deferred ACK/NAK  */
   uint32_t src_ip_be;
   uint16_t src_port;
   uint8_t  data[AUN_MAX_DATA];
} aun_rx_frame_t;

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
   uint32_t deadline_ms;
   uint8_t  attempts_left;
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
   uint8_t  data[AUN_HIMM_MAX];
} aun_host_imm_t;

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
   /* loopback test responder: frames sent to this station are ACKed
    * locally and echoed back through the rx path as if from it. */
   bool            test_enabled;
   uint8_t         test_stn, test_net;
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

/* Release the held packet and re-arm the block, delivering the
 * deferred verdict: accept=true sends the ACK the sender is waiting
 * for; accept=false (no listener took it) sends a NAK, so the sender
 * sees a true "not listening" exactly as on a real wire. */
uint8_t aun_rx_collect(aun_engine_t *e, uint8_t handle, bool accept);

uint8_t aun_rx_close(aun_engine_t *e, uint8_t handle);

/* Feed an inbound UDP datagram into the engine (call from the
 * transport's receive path). */
void aun_udp_input(aun_engine_t *e, uint32_t src_ip_be, uint16_t src_port,
                   const uint8_t *buf, uint32_t len);

/* Drive retries/timeouts. Call frequently (every main-loop poll). */
void aun_poll(aun_engine_t *e);

/* Configure the loopback test responder (see header comment in
 * econet_aun.c). enable=false turns it off. */
void aun_test_responder(aun_engine_t *e, bool enable,
                        uint8_t stn, uint8_t net);

void aun_set_machine_id(aun_engine_t *e, const uint8_t id[4]);

/* Host-executed immediates: when enabled, inbound immediate ops other
 * than machine peek are held (e->himm) for the host to execute;
 * aun_himm_reply() sends the IMM_REPLY and releases. While one is
 * held, further immediates are NAKed. */
void aun_set_host_imm(aun_engine_t *e, bool enable);
void aun_himm_reply(aun_engine_t *e, const uint8_t *data, uint32_t len);

#endif
