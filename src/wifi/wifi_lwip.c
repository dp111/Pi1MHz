#include "wifi_lwip.h"

#include "netname.h"
#include "sdio.h"
#include "cyw43.h"
#include "wifi.h"

#include "../Pi1MHz.h"
#include "../rpi/rpi.h"
#include "../rpi/systimer.h"

#include "lwip/dhcp.h"
#include "lwip/prot/dhcp.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/init.h"
#include "lwip/ip.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Doubling backoff for full re-init attempts from WIFI_STATE_ERROR;
   reset once the stack is healthy again. */
static uint32_t s_err_backoff_us;

static wifi_lwip_context_t g_wifi_lwip_context;
/* True once lwip_init() + netif_add() have run.  Kept OUTSIDE the context
   (which wifi_lwip_prepare memsets) so it survives a BBC RST re-init: the
   lwIP core and the netif are set up exactly once per Pi uptime, and a
   reset-retry reuses them instead of re-initialising - see the comments in
   wifi_lwip_prepare / wifi_lwip_init_stack. */
static bool s_lwip_core_up;
static bool g_wifi_lwip_link_logged;
static bool g_wifi_lwip_last_link_up;
static bool g_wifi_lwip_address_logged;
static bool g_wifi_lwip_last_address_ready;
static u8_t g_wifi_lwip_last_dhcp_state = 0xFFu;   /* 0xFF = not seen yet */
static u8_t g_wifi_lwip_last_dhcp_tries = 0xFFu;
static void wifi_lwip_debug_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#define WIFI_LWIP_RX_FRAME_MAX_LEN 1600u
/* Frames per drain slice.  The service pass repeats drain+flush until the
   FIFO is empty or the 1.2 ms wall-clock budget runs out, so this is not
   a per-pass RX cap - the time budget is.  It IS the flush cadence: the
   TX hold queue flushes after each slice, so with the coalescing feed a
   slice's worth of ACKs (x2 segments each) is what one superframe can
   carry.  8 capped that at ~16 segments in principle but ~4-7 in
   practice (ACKs arrive interleaved); 16 lets a full glom-16 batch form
   from one slice while the wall clock still bounds the pass. */
#define WIFI_LWIP_RX_FRAME_BUDGET 16u

/* Idle RX-poll throttle.  When frames are flowing the drain runs on every
   main-loop poll for full throughput and minimal latency; once the chip's
   fn2 FIFO comes up empty the drain backs off to this interval so an idle
   WiFi link stops issuing ~3 SDIO commands (wake_bus + INT_STATUS + fn2
   header read, each a busy-polled CMD52/CMD53) on every single iteration
   of the 1 MHz poll loop.  Inbound frames sit in the chip FIFO until the
   next check, so the only cost is up to this much added latency on the
   first frame after an idle gap - negligible for the file-server workload,
   and active transfers never see it because the drain stays at full rate
   while frames keep arriving. */
#define WIFI_LWIP_RX_IDLE_INTERVAL_US 1000u

/* After the host sends a datagram it is usually about to receive a reply
   (e.g. an Econet/AUN fileserver answer). The 1ms idle backoff above would
   leave that first reply sitting in the chip FIFO for up to a millisecond -
   long enough for a cycle-accurate AUN peer to time out its own ack window
   and retransmit. So a transmit "kicks" the RX into full-rate draining for
   this window, catching the reply with minimal latency. Idle links (no tx)
   still back off and save SDIO bandwidth. */
#define WIFI_LWIP_RX_KICK_US 8000u

/* Ceiling on one servicing pass through the RX drain and TX retry loop.
   About the worst main-loop iteration seen under load, so this cannot make
   the loop less responsive than it already was. */
#define WIFI_LWIP_SERVICE_BUDGET_US 1200u
static uint32_t s_rx_aggressive_until_us;

void wifi_lwip_rx_kick(void)
{
   s_rx_aggressive_until_us = RPI_GetSystemTime() + WIFI_LWIP_RX_KICK_US;
}

/* If the WiFi link has still not associated this long after the lwIP
   stack was brought up, treat the boot as failed: report the error and
   stop polling.  Association is normally a sub-second operation once the
   radio works, so this window is deliberately generous - it only needs
   to cover a join whose WLC_E_LINK event arrives late in the poll loop. */
#define WIFI_LWIP_LINK_TIMEOUT_US (30u * 1000000u)

u32_t sys_now(void)
{
   /* Must be milliseconds that wrap modulo 2^32, because that is what lwIP's
      TIME_LESS_THAN assumes.  Dividing the 32-bit microsecond counter by 1000
      does NOT do that: it produces a ramp that climbs to 4,294,967 and snaps
      back to zero every ~71 minutes, and after the first snap every scheduled
      timeout looks like it is still in the future - so the whole timer chain
      (TCP retransmit and persist, DHCP renew, ARP aging, DNS) stops running
      until the ramp climbs back past it, most of an hour later.  A timeout
      scheduled in the last moments before the snap is worse still: its
      deadline is above the ramp's ceiling and can never be reached at all.

      Taking the milliseconds from the 64-bit timer and truncating gives a
      counter that really does wrap modulo 2^32.  aun_now_ms() was fixed the
      same way for the same reason. */
   return (u32_t)(RPI_GetSystemTime64() / 1000u);
}

u32_t sys_jiffies(void)
{
   return RPI_GetSystemTime();
}

static void wifi_lwip_debug_log(const char *format, ...)
{
   va_list args;
   char line[192];
   int written;

   if (!wifi_debug_enabled())
      return;

   va_start(args, format);
   written = vsnprintf(line, sizeof(line), format, args);
   va_end(args);

   if (written <= 0)
      return;

   wifi_debug_printf("WIFI-LWIP: %s\r\n", line);
}

static bool wifi_lwip_address_ready(const struct netif *netif)
{
   if (netif == NULL)
      return false;

   return !ip4_addr_isany_val(*netif_ip4_addr(netif));
}

static void wifi_lwip_update_runtime_state(void)
{
   if (g_wifi_lwip_context.netif_added) {
      if (sdio_runtime_link_is_up()) {
         netif_set_link_up(&g_wifi_lwip_context.netif);
         if (g_wifi_lwip_context.use_dhcp
             && !g_wifi_lwip_context.dhcp_started) {
            if (dhcp_start(&g_wifi_lwip_context.netif) == ERR_OK)
               g_wifi_lwip_context.dhcp_started = true;
         } else if (!g_wifi_lwip_context.use_dhcp
                    && g_wifi_lwip_context.static_configured) {
            netif_set_addr(&g_wifi_lwip_context.netif,
                           &g_wifi_lwip_context.ipaddr,
                           &g_wifi_lwip_context.netmask,
                           &g_wifi_lwip_context.gateway);
         }
      } else {
         netif_set_link_down(&g_wifi_lwip_context.netif);
      }
   }

   g_wifi_lwip_context.link_up = g_wifi_lwip_context.netif_added
      && netif_is_link_up(&g_wifi_lwip_context.netif);
   g_wifi_lwip_context.address_ready = g_wifi_lwip_context.netif_added
      && wifi_lwip_address_ready(&g_wifi_lwip_context.netif);

   /* Latch the first time the link comes up.  Once set, the boot-time
      link-up timeout in wifi_lwip_poll() is disabled, so a later
      transient link drop keeps polling and is allowed to recover. */
   if (g_wifi_lwip_context.link_up) {
      g_wifi_lwip_context.link_established = true;
      s_err_backoff_us = 0;      /* healthy again: reset the ERROR-retry backoff */
   }

   /* A fresh association gets a fresh power-save state, so tell the driver
      to re-assert it - see sdio_runtime_powersave_note_link_change(). */
   sdio_runtime_powersave_note_link_change(g_wifi_lwip_context.link_up);

   if (!g_wifi_lwip_link_logged || g_wifi_lwip_last_link_up != g_wifi_lwip_context.link_up) {
      wifi_lwip_debug_log("link %s", g_wifi_lwip_context.link_up ? "up" : "down");
      g_wifi_lwip_last_link_up = g_wifi_lwip_context.link_up;
      g_wifi_lwip_link_logged = true;
   }

   if (!g_wifi_lwip_address_logged || g_wifi_lwip_last_address_ready != g_wifi_lwip_context.address_ready) {
      if (g_wifi_lwip_context.address_ready) {
         char ipaddr_text[20];
         wifi_lwip_debug_log("address ready ip=%s",
                             ip4addr_ntoa_r(netif_ip4_addr(&g_wifi_lwip_context.netif),
                                            ipaddr_text,
                                            (int)sizeof(ipaddr_text)));
      } else {
         wifi_lwip_debug_log("address not ready");
      }

      g_wifi_lwip_last_address_ready = g_wifi_lwip_context.address_ready;
      g_wifi_lwip_address_logged = true;
   }

   if (g_wifi_lwip_context.address_ready)
      wifi_note_network_ready();
}

void wifi_lwip_disconnect(void)
{
   if (!g_wifi_lwip_context.netif_added)
      return;

   if (g_wifi_lwip_context.use_dhcp
       && g_wifi_lwip_context.dhcp_started) {
      dhcp_release_and_stop(&g_wifi_lwip_context.netif);
      g_wifi_lwip_context.dhcp_started = false;
   }
   netif_set_link_down(&g_wifi_lwip_context.netif);
   ip_addr_set_zero_ip4(&g_wifi_lwip_context.netif.ip_addr);
   ip_addr_set_zero_ip4(&g_wifi_lwip_context.netif.netmask);
   ip_addr_set_zero_ip4(&g_wifi_lwip_context.netif.gw);
   g_wifi_lwip_context.link_up = false;
   g_wifi_lwip_context.address_ready = false;
   g_wifi_lwip_last_link_up = false;
   g_wifi_lwip_last_address_ready = false;
   wifi_lwip_debug_log("association and address state cleared");
}

/* Frames held back when the chip's credit window is shut.
 *
 * Refusing to transmit is fine for TCP, which retransmits, but it is silent
 * loss for everything else: an ARP reply, an ICMP echo reply or an AUN
 * datagram is simply gone, and the window is shut for a good fraction of any
 * sustained download.  That is why pings to this Pi looked lossy exactly when
 * it was busy while the transfer causing it completed fine.
 *
 * A short queue rather than a single slot, because one slot only covers the
 * first refusal - a burst refused back-to-back still lost everything after
 * the first.  Order is preserved absolutely: queued frames always go before
 * new ones, and a new frame that cannot be sent joins the back rather than
 * overtaking.  When the queue is full the newest frame is the one dropped,
 * which is the right choice for the traffic this protects: an older ARP or
 * ping reply is closer to being useful than a newer one.
 *
 * Held frames are dropped once stale, because a ping reply or an ARP
 * response delivered a second late is worse than useless. */
#if WIFI_LWIP_ICMP_PROBE_DIAG
/* ICMP echo timing probe (diagnostic).
 *
 * The question this answers: when a ping takes half a second, is the request
 * arriving at the chip late, or is the Pi slow to answer it?  Recording the
 * arrival time of each echo request and the time its reply is handed back to
 * the chip separates the two.  A uniform ~1 s arrival gap with a fast
 * turnaround puts the delay on the air; bursty arrivals mean something ahead
 * of us is buffering; a slow turnaround means it is our own loop. */
#define WIFI_LWIP_ICMP_PROBE_DEPTH 32u

typedef struct {
   uint32_t gap_ms;              /* since the previous echo request */
   uint32_t turnaround_us;       /* request in -> reply out */
} wifi_lwip_icmp_probe_t;

static wifi_lwip_icmp_probe_t s_icmp_probe[WIFI_LWIP_ICMP_PROBE_DEPTH];
static uint32_t s_icmp_probe_count;
static uint32_t s_icmp_rx_stamp_us;
static uint32_t s_icmp_prev_rx_us;
static bool s_icmp_pending;
/* Echo id+seq of the request being timed, so a reply parked in the hold
   queue is never timed against a LATER request's arrival. */
static uint32_t s_icmp_echo_tag;
static uint32_t s_icmp_rx_seen;
static uint32_t s_icmp_tx_seen;

/* proto: 1 = ICMP; type lives at the first byte of the ICMP header, which
   for a header-length-5 IPv4 packet in an Ethernet frame is offset 34. */
static bool wifi_lwip_is_icmp(const uint8_t *frame, uint16_t len, uint8_t type)
{
   if (len < 42u)
      return false;
   if (frame[12] != 0x08u || frame[13] != 0x00u)
      return false;
   if (frame[23] != 1u)
      return false;
   return frame[34] == type;
}

static void wifi_lwip_icmp_probe_rx(const uint8_t *frame, uint16_t len)
{
   uint32_t now;

   if (!wifi_lwip_is_icmp(frame, len, 8u))
      return;                    /* not an echo request */

   s_icmp_rx_seen++;
   now = RPI_GetSystemTime();
   s_icmp_rx_stamp_us = now;
   s_icmp_echo_tag = ((uint32_t)frame[38] << 24) | ((uint32_t)frame[39] << 16)
                   | ((uint32_t)frame[40] << 8) | frame[41];
   s_icmp_pending = true;
   s_icmp_prev_rx_us = (s_icmp_prev_rx_us == 0u) ? now : s_icmp_prev_rx_us;
}

static void wifi_lwip_icmp_probe_tx(const uint8_t *frame, uint16_t len)
{
   uint32_t now;
   wifi_lwip_icmp_probe_t *slot;

   if (!wifi_lwip_is_icmp(frame, len, 0u))
      return;                    /* not an echo reply */
   s_icmp_tx_seen++;
   if (!s_icmp_pending)
      return;                    /* no request outstanding to time it against */
   if ((((uint32_t)frame[38] << 24) | ((uint32_t)frame[39] << 16)
        | ((uint32_t)frame[40] << 8) | frame[41]) != s_icmp_echo_tag)
      return;                    /* reply to some other request - skip, do not
                                    mis-time it against this one's arrival */

   now = RPI_GetSystemTime();
   slot = &s_icmp_probe[s_icmp_probe_count % WIFI_LWIP_ICMP_PROBE_DEPTH];
   slot->gap_ms = (s_icmp_rx_stamp_us - s_icmp_prev_rx_us) / 1000u;
   slot->turnaround_us = now - s_icmp_rx_stamp_us;
   s_icmp_probe_count++;
   s_icmp_prev_rx_us = s_icmp_rx_stamp_us;
   s_icmp_pending = false;
}

/* Copy out up to `max` of the most recent samples, newest last.  Returns the
   number written; *total receives the lifetime count. */
void wifi_lwip_icmp_probe_counts(uint32_t *rx_seen, uint32_t *tx_seen)
{
   *rx_seen = s_icmp_rx_seen;
   *tx_seen = s_icmp_tx_seen;
}

uint32_t wifi_lwip_icmp_probe_read(uint32_t *gap_ms, uint32_t *turnaround_us,
                                   uint32_t max, uint32_t *total)
{
   uint32_t have = (s_icmp_probe_count < WIFI_LWIP_ICMP_PROBE_DEPTH)
                 ? s_icmp_probe_count : WIFI_LWIP_ICMP_PROBE_DEPTH;
   uint32_t n = (have < max) ? have : max;
   uint32_t i;

   if (total != NULL)
      *total = s_icmp_probe_count;
   (void)0;

   for (i = 0u; i < n; ++i) {
      uint32_t idx = (s_icmp_probe_count - n + i) % WIFI_LWIP_ICMP_PROBE_DEPTH;
      gap_ms[i] = s_icmp_probe[idx].gap_ms;
      turnaround_us[i] = s_icmp_probe[idx].turnaround_us;
   }
   return n;
}
#endif /* WIFI_LWIP_ICMP_PROBE_DIAG */

#define WIFI_LWIP_TX_HOLD_MAX_AGE_US 250000u
/* 32: deep enough to feed a full 16-frame superframe and keep a second
   batch forming behind it.  The original 16 was sized for the
   frame-at-a-time path; the glom-of-4 bench showed throughput is bound
   by frames-per-credit-refill, so the queue must hold at least one max
   batch plus headroom or the batch assembler starves. */
#define WIFI_LWIP_TX_QUEUE_DEPTH 32u
/* Capped traffic may take at most this many slots, so ARP and ICMP - the
   traffic with no second chance - always find a free one.  Queueing TCP
   with no cap measured 23% ping loss during a download; the reservation
   is the fix (4 reserved slots, scaled with the deeper queue).  Without
   glom batching only TCP is flagged for the cap (the original
   behaviour); with batching on, the coalescing feed flags UDP bulk too,
   so a blast cannot take the reserve either - the cost is that DHCP
   (UDP) also counts against the cap then, refused only when 28 bulk
   frames are already parked, and the DHCP client retries. */
#define WIFI_LWIP_TX_QUEUE_TCP_MAX 28u

typedef struct {
   uint32_t stamp_us;
   uint16_t len;
   bool is_tcp;
   _Alignas(4) uint8_t data[WIFI_LWIP_RX_FRAME_MAX_LEN];
} wifi_lwip_tx_slot_t;

static wifi_lwip_tx_slot_t s_tx_queue[WIFI_LWIP_TX_QUEUE_DEPTH];
static uint8_t s_tx_head;      /* next to send */
static uint8_t s_tx_count;     /* frames waiting */
static uint8_t s_tx_tcp_count; /* of which TCP (capped at TCP_MAX) */
/* Retry pacing.  Only a received frame can reopen the credit window (max_seq
   rides in every SDPCM header), so once a flush attempt is refused there is
   no point re-asking until something new has arrived.  The first failed
   attempt (16-slot queue, no pacing) retried on every main-loop pass and
   turned a brief window-shut into a ~292k/s hammering loop; this bounds it
   to one attempt per received frame - ACK-paced under load, and during
   total RX silence the queue simply ages out at 250 ms. */
static bool s_tx_flush_blocked;
static uint32_t s_tx_flush_rx_stamp;
/* Send-path counters: what happened between lwIP producing a frame and the
   frame actually reaching the chip - refusals, queueing, staleness, and the
   longest wait.  This span covers the hold queue and the credit gate. */
static uint32_t s_tx_queued;        /* refused by the chip, parked here */
static uint32_t s_tx_dropped_stale; /* aged out before credit appeared */
static uint32_t s_tx_direct_fail;   /* send refused on the direct path */
static uint32_t s_tx_hold_max_us;   /* longest a frame waited in the queue */
static uint32_t s_tx_dropped_full;  /* arrived with every slot taken - LOST */
static uint32_t s_tx_batch_lost;    /* dropped after a superframe CMD53
                                       completion failure - the card may hold
                                       any prefix, so a retry would replay
                                       consumed sequence numbers.  LOST (TCP
                                       rides RTO). */

/* Pop the head slot's bookkeeping (shared by the sent, stale and aged-out
   paths). */
static void wifi_lwip_tx_hold_pop(void)
{
   if (s_tx_queue[s_tx_head].is_tcp && s_tx_tcp_count > 0u)
      s_tx_tcp_count--;
   s_tx_head = (uint8_t)((s_tx_head + 1u) % WIFI_LWIP_TX_QUEUE_DEPTH);
   s_tx_count--;
}

/* Push as much of the queue as the credit window will take.  Returns true
 * when the queue is empty afterwards. */
static bool wifi_lwip_tx_hold_flush(void)
{
   uint32_t rx_stamp = sdio_runtime_last_any_rx_stamp();
   uint8_t batch_limit = sdio_runtime_txglom_batch_limit();

   while (s_tx_count > 0u) {
      wifi_lwip_tx_slot_t *slot = &s_tx_queue[s_tx_head];

      /* Age out first, so a stuck head cannot pin the queue while the
         pacing gate below is holding sends back. */
      if ((RPI_GetSystemTime() - slot->stamp_us) > WIFI_LWIP_TX_HOLD_MAX_AGE_US) {
         s_tx_dropped_stale++;
         wifi_lwip_tx_hold_pop();
         continue;               /* too old to be worth delivering */
      }

      /* Refused before and nothing has arrived since: the window cannot
         have reopened, so asking again only burns a wake_bus + gate probe. */
      if (s_tx_flush_blocked && rx_stamp == s_tx_flush_rx_stamp)
         return false;

      /* Glom batching: when TX glom is negotiated and more than one frame
         is waiting, hand a contiguous run of fresh head frames to the
         superframe path in one call.  Only frames ALREADY queued in this
         same service pass are batched - never a wait for a partner - so
         the added latency is zero by construction.  Ordering, age-out and
         the RX-paced retry latch behave exactly as the single path. */
      if (batch_limit >= 2u && s_tx_count >= 2u) {
         const uint8_t *frames[SDIO_RUNTIME_TXGLOM_MAX];
         uint16_t lens[SDIO_RUNTIME_TXGLOM_MAX];
         uint32_t stamps[SDIO_RUNTIME_TXGLOM_MAX];
         uint32_t now_us = RPI_GetSystemTime();
         uint8_t gather = 0u;

         while (gather < batch_limit && gather < s_tx_count) {
            const wifi_lwip_tx_slot_t *cand =
               &s_tx_queue[((unsigned)s_tx_head + gather)
                           % WIFI_LWIP_TX_QUEUE_DEPTH];

            if ((now_us - cand->stamp_us) > WIFI_LWIP_TX_HOLD_MAX_AGE_US)
               break;            /* stale mid-run: send what precedes it */
            frames[gather] = cand->data;
            lens[gather] = cand->len;
            stamps[gather] = cand->stamp_us;
            ++gather;
         }

         if (gather >= 2u) {
            int8_t sent = sdio_runtime_send_ethernet_frames(frames, lens,
                                                            gather);

            if (sent > 0) {
               uint32_t sent_at = RPI_GetSystemTime();
               uint8_t i;

               for (i = 0u; i < (uint8_t)sent; ++i) {
                  uint32_t waited = sent_at - stamps[i];

                  if (waited > s_tx_hold_max_us)
                     s_tx_hold_max_us = waited;
#if WIFI_LWIP_ICMP_PROBE_DIAG
                  wifi_lwip_icmp_probe_tx(frames[i], lens[i]);
#endif
                  wifi_lwip_tx_hold_pop();
               }
               s_tx_flush_blocked = false;
               continue;         /* a partial send loops back for the rest */
            }
            if (sent < 0) {
               /* Superframe CMD53 failed after the command phase: the
                  card may hold any prefix and the sequence numbers stay
                  consumed, so these frames are unretryable - drop and
                  count them (TCP recovers by RTO). */
               uint8_t drop = (uint8_t)-sent;
               uint8_t i;

               for (i = 0u; i < drop; ++i)
                  wifi_lwip_tx_hold_pop();
               s_tx_batch_lost += drop;
               s_tx_flush_blocked = true;
               s_tx_flush_rx_stamp = rx_stamp;
               return false;
            }
            /* Refused (credit window / bus asleep): nothing consumed -
               the same back-pressure latch as the single path. */
            s_tx_flush_blocked = true;
            s_tx_flush_rx_stamp = rx_stamp;
            return false;
         }
         /* gather < 2 (a stale frame right behind the head): fall through,
            the single path sends the head and the stale one ages out on
            the next loop iteration. */
      }

      if (sdio_runtime_send_ethernet_frame(slot->data, slot->len)) {
         uint32_t waited = RPI_GetSystemTime() - slot->stamp_us;
         if (waited > s_tx_hold_max_us)
            s_tx_hold_max_us = waited;
#if WIFI_LWIP_ICMP_PROBE_DIAG
         wifi_lwip_icmp_probe_tx(slot->data, slot->len);
#endif
         s_tx_flush_blocked = false;
         wifi_lwip_tx_hold_pop();
         continue;
      }

      s_tx_flush_blocked = true;
      s_tx_flush_rx_stamp = rx_stamp;
      return false;              /* no credit; retry when something arrives */
   }

   s_tx_flush_blocked = false;
   return true;
}

/* True for frames that carry their own retransmission.  TCP is admitted to
   the queue but capped (WIFI_LWIP_TX_QUEUE_TCP_MAX): a refused segment
   returned to lwIP as ERR_IF is not re-offered until the next ACK arrives,
   which wastes the very credit-refill moment - 17% of transmit attempts
   during a download died that way.  The cap keeps the last four slots for
   the traffic with no second chance (ARP, ICMP, DHCP); TCP past the cap
   still gets ERR_IF and rides lwIP's own unacked queue. */
static bool wifi_lwip_frame_self_retries(const uint8_t *frame, uint16_t len)
{
   uint16_t ethertype;

   if (len < 34u)
      return false;
   ethertype = (uint16_t)(((uint16_t)frame[12] << 8) | frame[13]);
   if (ethertype != 0x0800u)
      return false;              /* not IPv4: ARP and friends */
   return frame[23] == 6u;       /* IPv4 protocol 6 = TCP */
}

/* IPv4 TCP or UDP: the bulk traffic worth coalescing into superframes
   when glom batching is on.  Everything else (ARP, ICMP, non-IPv4)
   keeps the low-latency direct path - it gains nothing from batching
   and some of it has no second chance. */
static bool wifi_lwip_frame_is_bulk(const uint8_t *frame, uint16_t len)
{
   uint16_t ethertype;

   if (len < 34u)
      return false;
   ethertype = (uint16_t)(((uint16_t)frame[12] << 8) | frame[13]);
   if (ethertype != 0x0800u)
      return false;
   return frame[23] == 6u || frame[23] == 17u;   /* TCP or UDP */
}

/* Take ownership of a frame the chip would not accept.  Returns false when
   the frame was NOT taken (queue full, or TCP over its reservation) - the
   caller must then report the loss to lwIP or count it. */
static bool wifi_lwip_tx_hold_push(const uint8_t *frame, uint16_t len,
                                   bool is_tcp)
{
   wifi_lwip_tx_slot_t *slot;

   if (s_tx_count >= WIFI_LWIP_TX_QUEUE_DEPTH)
      return false;              /* full: refuse the newest, keep the order */
   if (is_tcp && s_tx_tcp_count >= WIFI_LWIP_TX_QUEUE_TCP_MAX)
      return false;              /* reservation: TCP rides lwIP's own queue */

   slot = &s_tx_queue[((unsigned)s_tx_head + (unsigned)s_tx_count)
                      % WIFI_LWIP_TX_QUEUE_DEPTH];
   memcpy(slot->data, frame, len);
   slot->len = len;
   slot->stamp_us = RPI_GetSystemTime();
   slot->is_tcp = is_tcp;
   if (is_tcp)
      s_tx_tcp_count++;
   s_tx_count++;
   return true;
}

void wifi_lwip_tx_path_counts(uint32_t *queued, uint32_t *stale,
                              uint32_t *direct_fail, uint32_t *hold_max_us)
{
   *queued = s_tx_queued;
   *stale = s_tx_dropped_stale + s_tx_dropped_full + s_tx_batch_lost;
   *direct_fail = s_tx_direct_fail;
   *hold_max_us = s_tx_hold_max_us;
}

/* ------------------------------------------------------------------ */
/* UDP blast test rig.  /udpblast primes it, the poll loop drains it.  */
/* Takes lwIP TCP (ACK clocking, snd_buf) out of the loop entirely:    */
/* the rate the RECEIVER counts is pure driver+SDPCM capacity.  The    */
/* datagrams still travel the ordinary link_output -> hold-queue ->    */
/* credit-gate path, which is the point: same bottleneck, no TCP.      */
/* Payload 1472 bytes -> exactly one 1500-byte IP packet per datagram. */
/* Idle cost: one s_blast_remaining check per service pass.            */
/* ------------------------------------------------------------------ */
static struct udp_pcb *s_blast_pcb;
/* Source tag for the per-pass feed histograms: true while the blast poll
   is emitting, so its frames are attributed to "udpblast" rather than
   "other" in wifi_lwip_pass_count_tx(). */
static bool s_in_blast_poll;
static ip_addr_t s_blast_dst;
static uint16_t s_blast_port;
static uint32_t s_blast_remaining;   /* datagrams still to send */
static uint32_t s_blast_sent;
static uint32_t s_blast_start_us, s_blast_end_us;
/* Datagrams emitted per service pass.  4 was the original fixed burst;
   /udpblast?burst=N raises it as the clean pipeline-capability test -
   the producer then demonstrably offers N frames per pass, so whatever
   batch size the flush forms is the pipeline's doing, not starvation. */
#define WIFI_LWIP_UDPBLAST_BURST_DEFAULT 4u
#define WIFI_LWIP_UDPBLAST_BURST_MAX 16u
static uint8_t s_blast_burst = WIFI_LWIP_UDPBLAST_BURST_DEFAULT;

void wifi_lwip_udpblast_start(const ip_addr_t *dst, uint16_t port,
                              uint32_t datagrams, uint8_t burst)
{
   if (s_blast_pcb == NULL)
      s_blast_pcb = udp_new();
   if (s_blast_pcb == NULL)
      return;
   s_blast_dst = *dst;
   s_blast_port = port;
   if (burst == 0u)
      burst = WIFI_LWIP_UDPBLAST_BURST_DEFAULT;
   if (burst > WIFI_LWIP_UDPBLAST_BURST_MAX)
      burst = WIFI_LWIP_UDPBLAST_BURST_MAX;
   s_blast_burst = burst;
   s_blast_remaining = datagrams;
   s_blast_sent = 0u;
   s_blast_start_us = RPI_GetSystemTime();
   s_blast_end_us = s_blast_start_us;
}

static void wifi_lwip_udpblast_poll(void)
{
   uint8_t burst;

   /* A few per pass: enough to saturate, bounded so the Beeb-facing
      main loop never stalls. */
   s_in_blast_poll = true;
   for (burst = 0u; burst < s_blast_burst && s_blast_remaining != 0u; ++burst) {
      struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 1472u, PBUF_RAM);
      if (p == NULL)
         break;                  /* pool pressure: try next pass */
      memset(p->payload, 0x55, 1472u);
      if (udp_sendto(s_blast_pcb, p, &s_blast_dst, s_blast_port) == ERR_OK) {
         ++s_blast_sent;
         --s_blast_remaining;
         s_blast_end_us = RPI_GetSystemTime();
      }
      pbuf_free(p);
      if (s_blast_remaining == 0u)
         break;                  /* done: end stamp already taken */
   }
   s_in_blast_poll = false;
}

void wifi_lwip_udpblast_stats(uint32_t *sent, uint32_t *remaining,
                              uint32_t *elapsed_us)
{
   *sent = s_blast_sent;
   *remaining = s_blast_remaining;
   *elapsed_us = s_blast_end_us - s_blast_start_us;
}

/* ------------------------------------------------------------------ */
/* Per-pass feed diagnostics (wifi_diag=1).  The 3D bench showed glom   */
/* batches pin at ~4 because only ~4 frames reach the TX path per       */
/* service pass; these histograms prove where that number comes from.  */
/* Buckets 0 / 1 / 2-3 / 4-7 / 8-15 / 16+.  A pass is recorded only    */
/* when it moved at least one frame in that direction, so idle passes  */
/* do not swamp bucket 0: in the three TX-source histograms, bucket 0  */
/* means "a productive pass in which this source contributed nothing". */
/* ------------------------------------------------------------------ */
static bool s_pass_diag;            /* cached once per service pass */
static uint8_t s_pass_tx_tcp;
static uint8_t s_pass_tx_blast;
static uint8_t s_pass_tx_other;
static uint8_t s_pass_rx_frames;
static uint32_t s_feed_hist_tcp[6];
static uint32_t s_feed_hist_blast[6];
static uint32_t s_feed_hist_other[6];
static uint32_t s_rx_pass_hist[6];

static uint8_t wifi_lwip_pass_bucket(uint8_t n)
{
   return (n == 0u) ? 0u
        : (n == 1u) ? 1u
        : (n <= 3u) ? 2u
        : (n <= 7u) ? 3u
        : (n <= 15u) ? 4u : 5u;
}

/* One frame handed to the TX path (queued or sent direct), attributed to
   its producer.  Saturating add: a runaway pass must not wrap. */
static void wifi_lwip_pass_count_tx(const uint8_t *frame, uint16_t len)
{
   uint8_t *counter;

   if (!s_pass_diag)
      return;
   counter = s_in_blast_poll ? &s_pass_tx_blast
           : wifi_lwip_frame_self_retries(frame, len) ? &s_pass_tx_tcp
           : &s_pass_tx_other;
   if (*counter != 0xffu)
      ++(*counter);
}

/* Called at the end of every service pass: fold the pass counters into
   the histograms and reset them. */
static void wifi_lwip_pass_diag_fold(void)
{
   if (!s_pass_diag)
      return;
   if ((uint8_t)(s_pass_tx_tcp | s_pass_tx_blast | s_pass_tx_other) != 0u) {
      ++s_feed_hist_tcp[wifi_lwip_pass_bucket(s_pass_tx_tcp)];
      ++s_feed_hist_blast[wifi_lwip_pass_bucket(s_pass_tx_blast)];
      ++s_feed_hist_other[wifi_lwip_pass_bucket(s_pass_tx_other)];
   }
   if (s_pass_rx_frames != 0u)
      ++s_rx_pass_hist[wifi_lwip_pass_bucket(s_pass_rx_frames)];
   s_pass_tx_tcp = 0u;
   s_pass_tx_blast = 0u;
   s_pass_tx_other = 0u;
   s_pass_rx_frames = 0u;
}

bool wifi_lwip_pass_diag_read(uint32_t tcp_hist[6], uint32_t blast_hist[6],
                              uint32_t other_hist[6], uint32_t rx_hist[6])
{
   unsigned int i;

   if (!sdio_runtime_diag_enabled())
      return false;
   for (i = 0u; i < 6u; ++i) {
      tcp_hist[i] = s_feed_hist_tcp[i];
      blast_hist[i] = s_feed_hist_blast[i];
      other_hist[i] = s_feed_hist_other[i];
      rx_hist[i] = s_rx_pass_hist[i];
   }
   return true;
}

#if WIFI_LWIP_RX_PROFILE
#if (__ARM_ARCH >= 7)
#error "WIFI_LWIP_RX_PROFILE uses the ARM1176 CP15 c15 cycle counter, which faults on the A53 (kernel7). Profile on kernel.img (rpi) instead."
#endif
/* CCNT/64 ticks; the counter is started once at boot (poll_ticks_start in
   Pi1MHz.c), so reads here are free-running.  Same mechanism and caveats
   as POLL_PROFILE. */
static inline uint32_t wifi_lwip_ccnt(void)
{
   uint32_t v;
   __asm volatile ("mrc p15,0,%0,c15,c12,1" : "=r" (v));
   return v;
}
static uint32_t s_rxprof_sdio_ticks;   /* CMD53 peek+body fetch */
static uint32_t s_rxprof_lwip_ticks;   /* pbuf + netif input (TCP callbacks) */
static uint32_t s_rxprof_frames;

void wifi_lwip_rx_profile_read(uint32_t *sdio_cycles_avg,
                               uint32_t *lwip_cycles_avg, uint32_t *frames)
{
   uint32_t n = s_rxprof_frames;

   *frames = n;
   *sdio_cycles_avg = (n != 0u)
      ? (uint32_t)((uint64_t)s_rxprof_sdio_ticks * 64u / n) : 0u;
   *lwip_cycles_avg = (n != 0u)
      ? (uint32_t)((uint64_t)s_rxprof_lwip_ticks * 64u / n) : 0u;
}
#endif /* WIFI_LWIP_RX_PROFILE */

// cppcheck-suppress constParameterCallback
static err_t wifi_lwip_link_output(struct netif *netif, struct pbuf *p)
{
   /* static: this is on the cooperative poll path and is large (~1.6 KB).
      Keeping it off the stack avoids a deep RX->TX nesting blowing the
      bare-metal stack.  The function is never re-entered. */
   static uint8_t frame[WIFI_LWIP_RX_FRAME_MAX_LEN];
   uint16_t offset = 0u;
   const struct pbuf *cursor = p;

   (void)netif;

   if (p == NULL || p->tot_len > sizeof(frame))
      return ERR_IF;

   while (cursor != NULL) {
      if ((uint16_t)(offset + cursor->len) > sizeof(frame))
         return ERR_IF;
      memcpy(&frame[offset], cursor->payload, cursor->len);
      offset = (uint16_t)(offset + cursor->len);
      cursor = cursor->next;
   }

   /* Coalescing feed: while glom batching is negotiated, bulk traffic
      (IPv4 TCP/UDP) always rides the hold queue - never the direct
      path - so a burst emitted within one service pass (segments from
      several ACKs, a blast burst) leaves as ONE superframe at the
      pass's flush points instead of N glom-of-1 CMD53s.  This is what
      lets batches form deeper than the credit-shut leftovers: the
      direct path used to peel off every frame the open window would
      take, one CMD53 each.  Added latency is bounded at one flush
      point - the service pass flushes after every drain slice and on
      non-drain passes - and the frame is flagged for the bulk cap so
      a UDP flood cannot take the 4 reserved no-second-chance slots.
      With wifi_txglom < 2 (or glom not negotiated) this branch never
      runs and the path below is exactly the pre-glom behaviour. */
   if (sdio_runtime_txglom_batch_limit() >= 2u
       && wifi_lwip_frame_is_bulk(frame, offset)) {
      if (wifi_lwip_tx_hold_push(frame, offset, true)) {
         wifi_lwip_pass_count_tx(frame, offset);
         wifi_lwip_rx_kick();
         return ERR_OK;
      }
      /* Queue full or over the bulk cap: fall through to the legacy
         path - its flush makes room, and its refusal semantics
         (ERR_IF, drop accounting) already handle the rest. */
   }

   /* Anything already held goes first, or this frame would overtake it.  When
      the queue cannot drain, the new frame joins the back rather than being
      dropped - which is what makes the queue a queue.  A queued frame is
      reported ERR_OK, so for TCP a later stale-drop is real loss recovered
      only by RTO - acceptable because staleness means 250 ms of shut
      window, which is already an outage. */
   if (!wifi_lwip_tx_hold_flush()) {
      bool tcp = wifi_lwip_frame_self_retries(frame, offset);
      if (wifi_lwip_tx_hold_push(frame, offset, tcp)) {
         s_tx_queued++;           /* parked after a refusal */
         wifi_lwip_pass_count_tx(frame, offset);
         return ERR_OK;
      }
      if (!tcp)
         s_tx_dropped_full++;     /* LOST - count it, or a full queue reads
                                     as "nothing wrong" on /status */
      return ERR_IF;              /* TCP resends; others are counted lost */
   }

   if (!sdio_runtime_send_ethernet_frame(frame, offset)) {
      s_tx_direct_fail++;
      bool tcp = wifi_lwip_frame_self_retries(frame, offset);
      if (wifi_lwip_tx_hold_push(frame, offset, tcp)) {
         s_tx_queued++;           /* parked after a refusal */
         wifi_lwip_pass_count_tx(frame, offset);
         return ERR_OK;           /* accepted: we own it now */
      }
      if (!tcp)
         s_tx_dropped_full++;
      return ERR_IF;
   }
   wifi_lwip_pass_count_tx(frame, offset);

#if WIFI_LWIP_ICMP_PROBE_DIAG
   /* Stamped here, not on entry: the useful number is request-in to
      frame-actually-sent, which is the window the queue and the credit gate
      live in. */
   wifi_lwip_icmp_probe_tx(frame, offset);
#endif

   /* Anything we send is a reason to listen harder.  The RX throttle decides
      the link is idle from *inbound* frames alone, which is precisely
      backwards for a file server: a download is almost all outbound, and the
      only thing coming back is the occasional ACK - so between ACKs the drain
      keeps finding an empty FIFO and backing off, and the next ACK then waits
      in the chip for up to the idle interval before we look.  Since the
      download only refills its window when an ACK arrives, that delay lands
      directly on throughput rather than merely on latency.  The AUN path
      already kicked the drain for exactly this reason; TCP never did. */
   wifi_lwip_rx_kick();
   return ERR_OK;
}

/* Hand one received frame to lwIP.  Returns false only when the pbuf pool
   is exhausted (the caller stops this drain cycle and resumes next poll).
   On a per-packet failure it drops just that packet and returns true so
   the caller keeps draining the remaining frames from the chip;
   abandoning the whole budget on one bad packet leaves the rest queued
   in the chip until the next poll tick. */
static bool wifi_lwip_deliver_rx_frame(const uint8_t *frame,
                                       uint16_t frame_length)
{
   struct pbuf *packet = pbuf_alloc(PBUF_RAW, frame_length, PBUF_POOL);

   if (packet == NULL)
      return false;

   if (pbuf_take(packet, frame, frame_length) != ERR_OK) {
      pbuf_free(packet);
      return true;
   }

   if (g_wifi_lwip_context.netif.input(packet, &g_wifi_lwip_context.netif) != ERR_OK)
      pbuf_free(packet);
   return true;
}

/* Returns true if the chip had at least one frame this cycle (the bus is
   "active"), so the caller can keep draining at full rate; false when the
   fn2 FIFO came up empty, letting the caller back off the idle poll. */
/* budget_end_us: wall-clock ceiling for this drain.  Checked per frame, not
   just per call, because the expensive part happens *inside* one iteration:
   handing a frame to lwIP runs the TCP callbacks, and those do the webserver's
   synchronous SD reads and writes.  Eight frames of an upload can therefore
   chain several 32 KB f_writes before a caller-side check would ever be
   reached - tens of milliseconds of main loop, which the Beeb's timing-
   sensitive paths feel directly. */
static bool wifi_lwip_drain_rx_frames(uint32_t budget_end_us)
{
   /* static: this is on the cooperative poll path and is large (~1.6 KB).
      Keeping it off the stack avoids a deep RX->TX nesting blowing the
      bare-metal stack.  The function is never re-entered. */
   static uint8_t frame[WIFI_LWIP_RX_FRAME_MAX_LEN];
   uint16_t frame_length;
   uint8_t frame_index;
   bool drained_any = false;

   if (!g_wifi_lwip_context.netif_added)
      return false;

   for (frame_index = 0u; frame_index < WIFI_LWIP_RX_FRAME_BUDGET; ++frame_index) {
      if (frame_index != 0u
          && (int32_t)(RPI_GetSystemTime() - budget_end_us) >= 0)
         break;                   /* out of time; the rest waits for next poll */

      frame_length = 0u;
#if WIFI_LWIP_RX_PROFILE
      {
         uint32_t t0 = wifi_lwip_ccnt();
         bool got = sdio_runtime_poll_ethernet_frame(frame, sizeof(frame),
                                                     &frame_length);
         s_rxprof_sdio_ticks += wifi_lwip_ccnt() - t0;
         if (!got)
            break;                /* SDIO error / FIFO empty: stop this cycle */
      }
#else
      if (!sdio_runtime_poll_ethernet_frame(frame, sizeof(frame), &frame_length))
         break;                   /* SDIO error / FIFO empty: stop this cycle */
#endif

      drained_any = true;

      if (frame_length == 0u)
         continue;

      if (s_pass_diag && s_pass_rx_frames != 0xffu)
         ++s_pass_rx_frames;      /* delivered data frames this pass */

#if WIFI_LWIP_ICMP_PROBE_DIAG
      wifi_lwip_icmp_probe_rx(frame, frame_length);
#endif

#if WIFI_LWIP_RX_PROFILE
      {
         uint32_t t1 = wifi_lwip_ccnt();
         bool room = wifi_lwip_deliver_rx_frame(frame, frame_length);

         s_rxprof_lwip_ticks += wifi_lwip_ccnt() - t1;
         ++s_rxprof_frames;
         if (!room)
            break;                /* pbuf pool exhausted: stop this cycle,
                                     resume next poll once pbufs free up */
      }
#else
      if (!wifi_lwip_deliver_rx_frame(frame, frame_length))
         break;                   /* pbuf pool exhausted: stop this cycle,
                                     resume next poll once pbufs free up */
#endif
   }

   return drained_any;
}

static err_t wifi_lwip_netif_init(struct netif *netif)
{
   netif->name[0] = 'w';
   netif->name[1] = 'f';
   netif->output = etharp_output;
   netif->linkoutput = wifi_lwip_link_output;
   netif->hwaddr_len = 6;
   /* Use the chip's real WiFi MAC: the firmware associates with that
      address, so the lwIP netif must present the same one or DHCP and
      ARP replies are addressed to a station the access point does not
      know.  The MAC is captured by the per-tick QUERY_MAC stage
      (cur_etheraddr WLC_GET_VAR) which always runs to completion before
      wifi_lwip_init_stack is allowed to start - so reaching here with
      no captured MAC means the SDIO state machine is broken.  Surface
      that as a hard error rather than silently falling through to a
      locally-administered placeholder: the placeholder would associate
      against the wrong station and DHCP/ARP would mysteriously fail,
      which is exactly the regression we just spent days chasing. */
   if (!sdio_runtime_get_chip_mac(netif->hwaddr)) {
      wifi_set_error("chip MAC unavailable; SDIO QUERY_MAC stage did not complete");
      return ERR_IF;
   }
   netif->mtu = 1500;
   netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
   return ERR_OK;
}

static void wifi_lwip_copy_ip4(ip4_addr_t *target, const wifi_ipv4_addr_t *source)
{
   if (target == NULL || source == NULL)
      return;

   IP4_ADDR(target,
            source->octets[0],
            source->octets[1],
            source->octets[2],
            source->octets[3]);
}

void wifi_lwip_prepare(void)
{
   const wifi_network_config_t *network_config = wifi_get_network_config();

   /* A BBC RST re-runs the whole WiFi bring-up (wifi_init allows re-init
      from ERROR/DISABLED - the natural way a user retries after a boot-time
      failure).  If a previous attempt already stood the lwIP core up, the
      netif is live and linked into lwIP's netif_list, the webserver holds a
      listen PCB and netname holds mDNS/NBNS PCBs - all from the lwIP memp
      pools.  Re-running from here would memset the live netif in place, then
      lwip_init() would wipe those pools under everything still pointing into
      them, and netif_add() would re-add a netif already in the list -> lwIP's
      "netif already added" assert, which this build turns into reboot_now().
      A reset meant to RETRY WiFi would instead reboot the whole Pi mid-
      session.  So once the core is up, leave the existing stack untouched;
      init_stack() below just re-arms address acquisition. */
   if (s_lwip_core_up)
      return;

   memset(&g_wifi_lwip_context, 0, sizeof(g_wifi_lwip_context));
   g_wifi_lwip_link_logged = false;
   g_wifi_lwip_address_logged = false;
   g_wifi_lwip_context.use_dhcp = network_config->ip_mode == WIFI_IP_MODE_DHCP;
   g_wifi_lwip_context.prepared = network_config->valid;
#if LWIP_NETIF_HOSTNAME
   g_wifi_lwip_context.netif.hostname = wifi_get_config()->hostname;
#endif

   ip4_addr_set_zero(&g_wifi_lwip_context.ipaddr);
   ip4_addr_set_zero(&g_wifi_lwip_context.netmask);
   ip4_addr_set_zero(&g_wifi_lwip_context.gateway);
   ip4_addr_set_zero(&g_wifi_lwip_context.dns);
   ip_addr_set_zero_ip4(&g_wifi_lwip_context.netif.ip_addr);
   ip_addr_set_zero_ip4(&g_wifi_lwip_context.netif.netmask);
   ip_addr_set_zero_ip4(&g_wifi_lwip_context.netif.gw);

   if (!g_wifi_lwip_context.prepared || g_wifi_lwip_context.use_dhcp)
   {
      wifi_lwip_debug_log("prepare mode=%s prepared=%u",
                          g_wifi_lwip_context.use_dhcp ? "dhcp" : "static",
                          g_wifi_lwip_context.prepared ? 1u : 0u);
      return;
   }

   wifi_lwip_copy_ip4(&g_wifi_lwip_context.ipaddr, &network_config->address);
   wifi_lwip_copy_ip4(&g_wifi_lwip_context.netmask, &network_config->netmask);

   if (network_config->has_gateway)
      wifi_lwip_copy_ip4(&g_wifi_lwip_context.gateway, &network_config->gateway);

   if (network_config->has_dns) {
      wifi_lwip_copy_ip4(&g_wifi_lwip_context.dns, &network_config->dns);
      g_wifi_lwip_context.has_dns = true;
   }

   ip_addr_set_ip4_u32(&g_wifi_lwip_context.netif.ip_addr, ip4_addr_get_u32(&g_wifi_lwip_context.ipaddr));
   ip_addr_set_ip4_u32(&g_wifi_lwip_context.netif.netmask, ip4_addr_get_u32(&g_wifi_lwip_context.netmask));
   ip_addr_set_ip4_u32(&g_wifi_lwip_context.netif.gw, ip4_addr_get_u32(&g_wifi_lwip_context.gateway));
   wifi_lwip_debug_log("prepare mode=static ip=%s",
                       ip4addr_ntoa(&g_wifi_lwip_context.ipaddr));
}

void wifi_lwip_init_stack(void)
{
   ip_addr_t dns_address;

   if (!g_wifi_lwip_context.prepared)
      return;

   if (s_lwip_core_up) {
      /* Reset-retry (see wifi_lwip_prepare): the lwIP core and netif already
         exist - do NOT lwip_init()/netif_add() again.  Just restart the
         address acquisition that failed the first time round.  DHCP is the
         only part that can fail after netif_add (memory exhaustion); static
         config cannot, so there is nothing to redo there. */
      /* wifi_init() already cleared any prior error at the start of this
         re-init, so a successful dhcp_start here simply lets the state
         machine proceed. */
      if (g_wifi_lwip_context.use_dhcp && !g_wifi_lwip_context.dhcp_started) {
         if (dhcp_start(&g_wifi_lwip_context.netif) == ERR_OK)
            g_wifi_lwip_context.dhcp_started = true;
      }
      wifi_lwip_update_runtime_state();
      return;
   }

   lwip_init();
   wifi_lwip_debug_log("lwip core initialised");
   g_wifi_lwip_context.initialized = true;
   g_wifi_lwip_context.init_time_us = RPI_GetSystemTime();

   if (netif_add(&g_wifi_lwip_context.netif,
                 g_wifi_lwip_context.use_dhcp ? NULL : &g_wifi_lwip_context.ipaddr,
                 g_wifi_lwip_context.use_dhcp ? NULL : &g_wifi_lwip_context.netmask,
                 g_wifi_lwip_context.use_dhcp ? NULL : &g_wifi_lwip_context.gateway,
                 &g_wifi_lwip_context,
                 wifi_lwip_netif_init,
                 ethernet_input) == NULL) {
      /* Surface the failure so the wifi boot state machine stops -
         otherwise it would happily advance to webserver_init against
         a netif that was never added and report the stack as ready.
         wifi_lwip_netif_init may have already set a more specific
         message (e.g. "chip MAC unavailable") via wifi_set_error;
         don't clobber it. */
      if (wifi_get_state() != WIFI_STATE_ERROR)
         wifi_set_error("lwIP netif_add failed");
      return;
   }

   g_wifi_lwip_context.netif_added = true;
   /* Core is now up: from here on a reset-retry reuses this netif rather
      than rebuilding lwIP (see wifi_lwip_prepare).  Set before dhcp_start
      so that if THAT fails and the user resets, the retry takes the reuse
      path instead of the assert-reboot one. */
   s_lwip_core_up = true;
   netif_set_default(&g_wifi_lwip_context.netif);
   netif_set_up(&g_wifi_lwip_context.netif);
   netif_set_link_down(&g_wifi_lwip_context.netif);
   g_wifi_lwip_context.timers_running = true;
   /* No poll registration: wifi_lwip_poll is called from
      wifi_dispatch_poll in wifi.c so the whole WiFi stack costs a
      single slot in the main Pi1MHz poll table.  timers_running
      gates the poll so it stays a no-op until this point. */
   wifi_lwip_debug_log("netif added");

   /* Start the NetBIOS / mDNS name responders so the Pi can be reached
      by name as well as by IP address. */
   netname_init();

   if (g_wifi_lwip_context.use_dhcp) {
      err_t dhcp_result = dhcp_start(&g_wifi_lwip_context.netif);
      if (dhcp_result == ERR_OK) {
         g_wifi_lwip_context.dhcp_started = true;
      } else {
         /* dhcp_start only fails on memory exhaustion at boot.  The
            link-up timeout in wifi_lwip_poll won't catch this because
            the link itself can come up just fine - surface it now so
            the user isn't left guessing why no address ever arrives. */
         wifi_set_error("lwIP dhcp_start failed");
      }
      wifi_lwip_debug_log("dhcp_start result=%d", (int)dhcp_result);
      wifi_lwip_update_runtime_state();
      return;
   }

   g_wifi_lwip_context.static_configured = true;
   if (g_wifi_lwip_context.has_dns) {
      ip_addr_set_ip4_u32(&dns_address, ip4_addr_get_u32(&g_wifi_lwip_context.dns));
      dns_setserver(0, &dns_address);
   }

   wifi_lwip_debug_log("static network configured");

   wifi_lwip_update_runtime_state();
}

static const char *wifi_lwip_dhcp_state_name(u8_t state)
{
   switch (state) {
      case DHCP_STATE_OFF:         return "off";
      case DHCP_STATE_INIT:        return "init";
      case DHCP_STATE_SELECTING:   return "selecting (Discover sent)";
      case DHCP_STATE_REQUESTING:  return "requesting (Offer in, Request sent)";
      case DHCP_STATE_CHECKING:    return "checking (Ack in, conflict check)";
      case DHCP_STATE_BOUND:       return "bound (address acquired)";
      case DHCP_STATE_RENEWING:    return "renewing";
      case DHCP_STATE_REBINDING:   return "rebinding";
      case DHCP_STATE_REBOOTING:   return "rebooting";
      case DHCP_STATE_RELEASING:   return "releasing";
      case DHCP_STATE_BACKING_OFF: return "backing off (timed out, retrying)";
      case DHCP_STATE_PERMANENT:   return "permanent";
      case DHCP_STATE_INFORMING:   return "informing";
      default:                     return "unknown";
   }
}

/* Log every DHCP state change with a millisecond timestamp, so the time
   from link-up to address acquisition - and any retries - can be seen.
   wifi_lwip_debug_log() self-gates, so this is silent unless wifi_debug
   is enabled. */
static void wifi_lwip_log_dhcp_state(void)
{
   struct dhcp *d = netif_dhcp_data(&g_wifi_lwip_context.netif);

   if (d == NULL)
      return;
   if (d->state == g_wifi_lwip_last_dhcp_state
       && d->tries == g_wifi_lwip_last_dhcp_tries)
      return;        /* log on every state OR tries change */

   g_wifi_lwip_last_dhcp_state = d->state;
   g_wifi_lwip_last_dhcp_tries = d->tries;
   wifi_lwip_debug_log("dhcp %s  t=%lu ms  tries=%u",
                       wifi_lwip_dhcp_state_name(d->state),
                       (unsigned long)(RPI_GetSystemTime() / 1000u),
                       (unsigned int)d->tries);
}

/* Association retry schedule.  The first attempt waits out the boot link-up
   window (an association in progress must not be interrupted), then each
   failure doubles the wait to a ceiling, so a Pi carried out of range costs
   one join burst a minute rather than a continuous stream of ioctls. */
/* React quickly - a deauth is over in an instant and the join burst itself
   takes about a second, so waiting longer than this is pure downtime.  The
   doubling is what protects the bus when there is genuinely no AP to find. */
#define WIFI_LWIP_REJOIN_FIRST_US  (2u * 1000000u)
/* How long the chip may deliver nothing at all before the link is presumed
   dead regardless of what it claims.  Generously above any normal quiet
   period - broadcast and multicast traffic alone keep a real network far
   busier than this. */
#define WIFI_LWIP_RX_SILENCE_LIMIT_US (45u * 1000000u)
/* Transmit continuously refused for this long forces a rejoin even though RX
   is healthy.  The wedge this covers was watched happen twice: the credit
   window died (replayed sequence numbers are discarded uncredited, so
   max_seq froze), RX carried on - broadcasts kept arriving - and the
   RX-silence trigger therefore never fired; only a power cycle recovered.
   A rejoin replays the 44-command join list, and every command's response
   refreshes max_seq from live chip state, which re-derives the window.
   Well past the 1 s resync probes and the 250 ms queue staleness, so it
   only fires when those have already failed repeatedly. */
#define WIFI_LWIP_TX_DEAD_LIMIT_US (8u * 1000000u)
/* If rejoins have not revived transmit by this point, escalate to a full
   chip restart (sdio_runtime_start: WL_REG_ON power cycle, firmware
   re-download, fresh sequence space on both sides).  A rejoin cannot cure a
   poisoned sequence window - its own 38 commands travel through that same
   window and the chip discards them; watched happen: "rejoin attempt 8",
   every command sent, event_type=0, link still down.  Only re-powering the
   chip resets its side of the sequence space. */
#define WIFI_LWIP_TX_DEAD_RESTART_US (25u * 1000000u)
static bool s_full_restart_active;
/* Rejoins issued since the link was last seen healthy.  Escalation must not
   depend on WHY the link is dead: one wedge flavour poisons the credit
   window (rejoin commands are discarded), another wedges the bus itself
   (rejoin commands fail at CMD53) - and any future flavour will be a third
   thing.  Three failed rejoins mean rejoining is not the cure, whatever the
   disease. */
static uint8_t s_rejoins_since_healthy;
/* Full restarts since the link was last healthy.  Capped: a restart takes
   several seconds of bus-heavy bring-up, and if three in a row have not
   revived the link the fault is not something a fourth will fix - stop
   escalating and leave the ordinary rejoin backoff (which tops out at a
   minute) as the only retry.  Observed uncapped: a restart thrash loop
   dense enough to starve the main loop into a crawl. */
static uint8_t s_full_restarts_since_healthy;
/* When the current stretch of health began; 0 while unhealthy.  The restart
   budget refunds only after this dwell - past the 8 s rejoin and 25 s restart
   ladders - because one optimistic pass is free after any restart that
   associates (the freshness clocks are zeroed), and refunding on that pass
   let a TX-side wedge cycle restart -> join -> "healthy" -> re-wedge ->
   restart forever, which is the thrash the cap exists to stop.  The dwell
   must also clear the RX-silence window: a chip that associates but then
   hears nothing reads "healthy" (link_up, tx not yet dead) for up to
   RX_SILENCE_LIMIT, so a shorter dwell would refund the budget mid-window
   and re-arm three restarts every ~50 s.  Keep it strictly past that limit. */
static uint32_t s_healthy_since_us;
#define WIFI_LWIP_HEALTH_DWELL_US (WIFI_LWIP_RX_SILENCE_LIMIT_US + 5u * 1000000u)
#define WIFI_LWIP_REJOIN_MAX_US    (60u * 1000000u)

static uint32_t s_rejoin_interval_us = WIFI_LWIP_REJOIN_FIRST_US;
static uint32_t s_rejoin_due_us;
static bool     s_rejoin_scheduled;

/* Arm the next association attempt and grow the backoff.  Schedule first,
   THEN double, so the first retry really uses the first interval (doubling
   before scheduling quietly made it 4 s). */
static void wifi_lwip_schedule_next_rejoin(uint32_t now_us)
{
   s_rejoin_due_us = now_us + s_rejoin_interval_us;
   if (s_rejoin_interval_us < WIFI_LWIP_REJOIN_MAX_US) {
      s_rejoin_interval_us *= 2u;
      if (s_rejoin_interval_us > WIFI_LWIP_REJOIN_MAX_US)
         s_rejoin_interval_us = WIFI_LWIP_REJOIN_MAX_US;
   }
}

/* Drive association retries.  Called from wifi_lwip_poll() on every service,
   in both the never-joined and lost-the-link cases - they differ only in
   which timestamp starts the clock. */
static void wifi_lwip_rejoin_service(void)
{
   uint32_t now_us = RPI_GetSystemTime();

   /* A full restart in flight owns the state machine outright: drive it to
      DONE or ERROR.  The tick itself is the progress test - NOT
      sdio_runtime_started(), which goes true at WRITE_INTR_MASK, five
      stages before the join has run.  Gating the tick on started()
      abandoned the bring-up at PREPARE_JOIN: "full restart complete" with
      no SSID ever sent, no link possible, and - because rejoin_start()
      refuses off STAGE_DONE and every escalation trigger read satisfied -
      no ladder rung left able to fire.  An absorbing zombie, 2 of 2 on
      hardware. */
   if (s_full_restart_active) {
      if (sdio_runtime_tick())
         return;                  /* still working through bring-up */
      if (!sdio_runtime_ready()) {
         /* ERROR: pace the next attempt on the rejoin backoff rather than
            re-entering bring-up on the very next pass.  A full restart is
            several seconds of bus-heavy work; back-to-back failed restarts
            with no gap are exactly the thrash that starves the main loop -
            so space them (2 s stretching to a minute) before retrying. */
         s_full_restart_active = false;
         wifi_lwip_debug_log("full restart failed; will retry");
         wifi_lwip_schedule_next_rejoin(now_us);
         return;
      }
      s_full_restart_active = false;
      /* Fresh schedule with a grace period: the join and DHCP need time
         before "still not healthy" may mean anything.  Without this the
         stale pre-restart schedule fired the escalation again on the very
         next pass - watched loop: restart, complete, restart, forever. */
      s_rejoin_interval_us = WIFI_LWIP_REJOIN_FIRST_US;
      s_rejoin_scheduled = true;
      s_rejoin_due_us = now_us + 20u * 1000000u;
      s_rejoins_since_healthy = 0u;
      wifi_lwip_debug_log("full restart complete");
   }

   /* A rejoin in flight owns the runtime state machine until it finishes.
      tick() returning false means it has reached DONE (or ERROR); the link
      itself is reported separately, by the chip's events. */
   if (sdio_runtime_rejoin_busy()) {
      (void)sdio_runtime_tick();
      return;
   }

   if (g_wifi_lwip_context.link_up
       && sdio_runtime_rx_idle_us() < WIFI_LWIP_RX_SILENCE_LIMIT_US
       && sdio_runtime_tx_dead_us() < WIFI_LWIP_TX_DEAD_LIMIT_US) {
      /* Associated and still hearing traffic: reset the schedule so the next
         outage starts fresh. */
      s_rejoin_interval_us = WIFI_LWIP_REJOIN_FIRST_US;
      s_rejoin_scheduled = false;
      s_rejoins_since_healthy = 0u;
      if (s_healthy_since_us == 0u)
         s_healthy_since_us = (now_us == 0u) ? 1u : now_us;
      if ((uint32_t)(now_us - s_healthy_since_us) >= WIFI_LWIP_HEALTH_DWELL_US)
         s_full_restarts_since_healthy = 0u;
      return;
   }

   s_healthy_since_us = 0u;

   /* Falling through with link_up still set means the chip has gone quiet
      without saying so.  That happens: observed with USB and the main loop
      healthy, no link-down event, no deauth, and nothing arriving for
      minutes - so every status the driver owns said the link was fine while
      no traffic moved, and an event-driven rejoin could never fire.  Total
      silence is the evidence, and it is trustworthy here because a LAN
      carries broadcast traffic continuously; this Pi sees tens of frames a
      second even with nothing talking to it. */

   if (!s_rejoin_scheduled) {
      /* First time down.  Before the link has ever come up, allow the full
         boot window - the join sequence is slow and DHCP follows it - and
         after that, react to a drop on the normal interval. */
      uint32_t first_wait = g_wifi_lwip_context.link_established
                               ? s_rejoin_interval_us
                               : WIFI_LWIP_LINK_TIMEOUT_US;

      s_rejoin_due_us = (g_wifi_lwip_context.link_established
                            ? now_us
                            : g_wifi_lwip_context.init_time_us) + first_wait;
      s_rejoin_scheduled = true;
      return;
   }

   if ((int32_t)(now_us - s_rejoin_due_us) < 0)
      return;

   /* "!ready" makes a FAILED restart re-eligible: sdio_runtime_start()
      zeroes both dead-clocks at entry and rejoin_start() refuses from
      STAGE_ERROR, so without this trigger a bring-up that errors out mid-way
      left NOTHING able to fire again - an absorbing dead state, found in
      review before it was ever hit in the field.  ready(), not started():
      a stage that errors AFTER the firmware boot leaves started() true
      with the machine parked at STAGE_ERROR - the same absorbing shape. */
   /* The restart cap stops *thrash*, but must never leave the machine wedged.
      When the runtime is parked at STAGE_ERROR a rejoin cannot run at all
      (sdio_runtime_rejoin_start refuses off a non-DONE stage), so a restart
      is the ONLY rung that can recover - the cap must not veto it, or three
      failed restarts latch an absorbing dead state the counter never clears
      (it only refunds after 30 s+ of health, unreachable at ERROR).  Failed
      restarts are now backoff-paced above, so uncapped retries here cannot
      thrash.  The cap therefore only bites when the runtime is still ready
      and a rejoin remains a viable fallback (TX-wedged / three failed
      rejoins) - exactly the thrash it was written for. */
   if ((!sdio_runtime_ready()
        || sdio_runtime_tx_dead_us() >= WIFI_LWIP_TX_DEAD_RESTART_US
        || s_rejoins_since_healthy >= 3u)
       && (s_full_restarts_since_healthy < 3u || !sdio_runtime_ready())) {
      /* Rejoins are not reviving transmit: re-power the chip.  The firmware
         and NVRAM images persist in RAM (cyw43_release_images is never
         called), so the whole bring-up can rerun without touching the SD. */
      wifi_lwip_debug_log("transmit dead through rejoins - full chip restart");
      /* The boot images were freed after the first download to reclaim RAM
         (cyw43_release_boot_images), so a restart must re-read them from the
         SD first.  Blocking the loop for the ~0.5 s read is fine here: this
         path only runs when the network has already been dead for tens of
         seconds. */
      if (!cyw43_preload_images()) {
         wifi_lwip_debug_log("firmware re-preload failed; will retry");
      } else if (sdio_runtime_start()) {
         s_full_restart_active = true;
         s_full_restarts_since_healthy++;
         return;
      }
      /* preload or start failed: fall through to the schedule and retry */
   } else if (sdio_runtime_rejoin_start()) {
      s_rejoins_since_healthy++;
      wifi_lwip_debug_log("link down - re-issuing join");
   }

   /* Schedule the next attempt whether or not this one could start: if the
      runtime was busy we simply try again after the same interval. */
   wifi_lwip_schedule_next_rejoin(now_us);
}

void wifi_lwip_poll(void)
{
   /* This must run BEFORE the timers_running gate: an ERROR latched during
      first bring-up (SDIO failure, netif_add) happens with timers_running
      still false, and is exactly the case this retry exists for. */
   /* ERROR used to be an absorbing stop ("permanent").  But the recovery
      ladder's last rung - the full chip restart - re-runs the bring-up
      stages at RUNTIME, and a transient failure there (chip MAC query,
      lwIP netif_add, dhcp_start) latched ERROR and killed polling: the
      network then stayed dead until a BBC RST re-ran wifi_init().  Do the
      same thing from a timer instead: retry the whole init on a doubling
      backoff (1 min .. 10 min).  Genuine config errors (bad SSID) refail
      fast and just idle in ERROR between attempts. */
   if (wifi_get_state() == WIFI_STATE_ERROR) {
      static uint32_t err_since_us;
      uint32_t now = RPI_GetSystemTime();
      if (s_err_backoff_us == 0u)
         s_err_backoff_us = 60000000u;
      if (err_since_us == 0u) {
         err_since_us = now | 1u;
         wifi_lwip_debug_log("wifi error state - retry in %lus",
                             (unsigned long)(s_err_backoff_us / 1000000u));
      } else if (now - err_since_us >= s_err_backoff_us) {
         err_since_us = 0u;
         s_err_backoff_us <<= 1;
         if (s_err_backoff_us > 600000000u)
            s_err_backoff_us = 600000000u;
         wifi_lwip_debug_log("wifi error state - retrying full init");
         wifi_init();
      }
      return;
   }

   if (!g_wifi_lwip_context.timers_running)
      return;

   /* Cache the diag gate once per pass: the per-frame counting sites then
      test one static bool instead of calling into sdio.c. */
   s_pass_diag = sdio_runtime_diag_enabled();

   /* Keep trying to associate.  The chip does not re-associate by itself and
      the boot path issues the join exactly once, so a scan that comes back
      empty - WLC_E_SET_SSID status 3, which one noisy moment is enough to
      produce - used to be terminal: this code reported an error, latched
      timers_running false, and the Pi stayed off the network until it was
      power-cycled.  An AP rebooting an hour later ended the same way.

      So the link-up timeout now schedules a retry instead of giving up, and
      the same schedule covers a link lost long after boot.  The backoff is
      what keeps a hopeless retry (no AP in range at all) from hammering the
      SDIO bus: attempts start a few seconds apart and stretch to a minute. */
   wifi_lwip_rejoin_service();

   /* Drain inbound frames and hand them to lwIP.  sdio_runtime_poll_-
      ethernet_frame() also processes the chip's async events (WLC_E_LINK,
      WLC_E_SET_SSID, etc.) as a side effect, so this single drain covers
      both events and data.

      A separate sdio_runtime_poll_events() call used to run here first -
      but it read frames into a throwaway buffer, so every inbound TCP/UDP
      data frame it touched was consumed from the chip and silently
      discarded before lwIP could see it.  That crippled throughput
      (constant retransmits); draining straight into lwIP fixes it. */
   /* Adaptive RX throttle: drain on every poll while frames are flowing
      (full throughput, no added latency), but once the FIFO comes up empty
      back off to WIFI_LWIP_RX_IDLE_INTERVAL_US so an idle link does not
      issue SDIO commands on every iteration of the 1 MHz poll loop.  The
      drain itself still processes the chip's async events, so link-state
      changes are picked up within one idle interval. */
   {
      static uint32_t s_rx_next_us;   /* 0 on first call -> drains immediately */
      uint32_t now_us = RPI_GetSystemTime();

      /* full-rate while a transmit expects a reply (see wifi_lwip_rx_kick) */
      bool aggressive = (int32_t)(now_us - s_rx_aggressive_until_us) < 0;

      /* Once expired, drag the deadline along with now: a stale deadline
         35.8 minutes in the past would otherwise wrap the signed compare and
         re-assert "aggressive" for half of every 71.6-minute cycle. */
      if (!aggressive)
         s_rx_aggressive_until_us = now_us;

      if (aggressive || (int32_t)(now_us - s_rx_next_us) >= 0) {
         uint32_t budget_end = now_us + WIFI_LWIP_SERVICE_BUDGET_US;
         bool active = false;

         /* Keep going while frames keep arriving, instead of draining once
            and leaving.  One drain per main-loop iteration meant an ACK that
            landed just after it waited a whole iteration - 0.5-1.2 ms
            measured - before anything could act on it, and since a download
            only refills its window when an ACK is seen, that latency was
            paid on every window.  Looping here converts several
            ACK-to-transmit exchanges per iteration.

            The flush belongs inside the loop: draining is what refreshes the
            chip's credit window, so a frame refused a moment ago will often
            go out immediately after the next drain.

            Bounded by wall-clock rather than a frame count, because time is
            what the rest of the machine cares about.  The budget is about one
            of today's worst iterations, so no other poll callback sees a
            delay it does not already tolerate, and the loop exits the moment
            the FIFO is empty - an idle link pays nothing. */
         for (;;) {
            bool drained = wifi_lwip_drain_rx_frames(budget_end);

            active = active || drained;
            (void)wifi_lwip_tx_hold_flush();
            if (!drained)
               break;
            if ((int32_t)(RPI_GetSystemTime() - budget_end) >= 0)
               break;
         }

         /* With the SDIO interrupt gate armed, an idle drain costs one MMIO
            read - so there is nothing left to throttle, and every main-loop
            pass may look.  A frame is then collected the moment the chip
            raises the line rather than up to a full idle interval later.
            Unarmed (boot, emulator, arming failure) the old backoff stands. */
         s_rx_next_us = active ? RPI_GetSystemTime()
                               : (now_us + (sdio_runtime_rx_gate_is_armed()
                                            ? 0u
                                            : WIFI_LWIP_RX_IDLE_INTERVAL_US));
      } else {
         /* Not draining this pass, but a frame the window refused must still
            get its retry rather than waiting for the next drain. */
         (void)wifi_lwip_tx_hold_flush();
      }
   }
   if (s_blast_remaining != 0u)
      wifi_lwip_udpblast_poll();
   sys_check_timeouts();
   wifi_lwip_log_dhcp_state();
   wifi_lwip_pass_diag_fold();
   g_wifi_lwip_context.last_service_time_us = RPI_GetSystemTime();
   g_wifi_lwip_context.service_calls++;
   wifi_lwip_update_runtime_state();
}

const wifi_lwip_context_t *wifi_lwip_get_context(void)
{
   return &g_wifi_lwip_context;
}