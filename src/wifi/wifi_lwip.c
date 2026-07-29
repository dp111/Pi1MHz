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
#include "lwip/etharp.h"
#include "netif/ethernet.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static wifi_lwip_context_t g_wifi_lwip_context;
static bool g_wifi_lwip_link_logged;
static bool g_wifi_lwip_last_link_up;
static bool g_wifi_lwip_address_logged;
static bool g_wifi_lwip_last_address_ready;
static u8_t g_wifi_lwip_last_dhcp_state = 0xFFu;   /* 0xFF = not seen yet */
static u8_t g_wifi_lwip_last_dhcp_tries = 0xFFu;
static void wifi_lwip_debug_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#define WIFI_LWIP_RX_FRAME_MAX_LEN 1600u
#define WIFI_LWIP_RX_FRAME_BUDGET 8u

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
      if (sdio_runtime_link_is_up())
         netif_set_link_up(&g_wifi_lwip_context.netif);
      else
         netif_set_link_down(&g_wifi_lwip_context.netif);
   }

   g_wifi_lwip_context.link_up = g_wifi_lwip_context.netif_added
      && netif_is_link_up(&g_wifi_lwip_context.netif);
   g_wifi_lwip_context.address_ready = g_wifi_lwip_context.netif_added
      && wifi_lwip_address_ready(&g_wifi_lwip_context.netif);

   /* Latch the first time the link comes up.  Once set, the boot-time
      link-up timeout in wifi_lwip_poll() is disabled, so a later
      transient link drop keeps polling and is allowed to recover. */
   if (g_wifi_lwip_context.link_up)
      g_wifi_lwip_context.link_established = true;

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
#define WIFI_LWIP_TX_QUEUE_DEPTH 8u

typedef struct {
   uint32_t stamp_us;
   uint16_t len;
   _Alignas(4) uint8_t data[WIFI_LWIP_RX_FRAME_MAX_LEN];
} wifi_lwip_tx_slot_t;

static wifi_lwip_tx_slot_t s_tx_queue[WIFI_LWIP_TX_QUEUE_DEPTH];
static uint8_t s_tx_head;      /* next to send */
static uint8_t s_tx_count;     /* frames waiting */
/* Send-path counters: what happened between lwIP producing a frame and the
   frame actually reaching the chip - refusals, queueing, staleness, and the
   longest wait.  This span covers the hold queue and the credit gate. */
static uint32_t s_tx_queued;        /* refused by the chip, parked here */
static uint32_t s_tx_dropped_stale; /* aged out before credit appeared */
static uint32_t s_tx_direct_fail;   /* send refused on the direct path */
static uint32_t s_tx_hold_max_us;   /* longest a frame waited in the queue */
static uint32_t s_tx_dropped_full;  /* arrived with every slot taken - LOST */

/* Push as much of the queue as the credit window will take.  Returns true
 * when the queue is empty afterwards. */
static bool wifi_lwip_tx_hold_flush(void)
{
   while (s_tx_count > 0u) {
      wifi_lwip_tx_slot_t *slot = &s_tx_queue[s_tx_head];

      if (sdio_runtime_send_ethernet_frame(slot->data, slot->len)) {
         uint32_t waited = RPI_GetSystemTime() - slot->stamp_us;
         if (waited > s_tx_hold_max_us)
            s_tx_hold_max_us = waited;
#if WIFI_LWIP_ICMP_PROBE_DIAG
         wifi_lwip_icmp_probe_tx(slot->data, slot->len);
#endif
         s_tx_head = (uint8_t)((s_tx_head + 1u) % WIFI_LWIP_TX_QUEUE_DEPTH);
         s_tx_count--;
         continue;
      }

      if ((RPI_GetSystemTime() - slot->stamp_us) > WIFI_LWIP_TX_HOLD_MAX_AGE_US) {
         s_tx_dropped_stale++;
         s_tx_head = (uint8_t)((s_tx_head + 1u) % WIFI_LWIP_TX_QUEUE_DEPTH);
         s_tx_count--;
         continue;               /* too old to be worth delivering */
      }

      return false;              /* still no credit; try again next poll */
   }

   return true;
}

/* True for frames that carry their own retransmission.  TCP segments are
   deliberately NOT queued: lwIP already keeps them on its unacked queue and
   will resend, so holding a copy here buys nothing and actively harms - a
   saturated download refuses frames constantly, and queued segments then fill
   every slot and crowd out the ARP and ICMP replies this queue exists to
   protect.  Measured: with TCP queued, ping loss during a download was 23%
   with spikes over a second; the queue must stay for the traffic that has no
   second chance. */
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

/* Take ownership of a frame the chip would not accept. */
static void wifi_lwip_tx_hold_push(const uint8_t *frame, uint16_t len)
{
   wifi_lwip_tx_slot_t *slot;

   if (s_tx_count >= WIFI_LWIP_TX_QUEUE_DEPTH) {
      s_tx_dropped_full++;       /* this frame is LOST - count it, or a full
                                    queue reads as "nothing wrong" on /status */
      return;                    /* full: drop the newest, keep the order */
   }
   s_tx_queued++;

   slot = &s_tx_queue[((unsigned)s_tx_head + (unsigned)s_tx_count)
                      % WIFI_LWIP_TX_QUEUE_DEPTH];
   memcpy(slot->data, frame, len);
   slot->len = len;
   slot->stamp_us = RPI_GetSystemTime();
   s_tx_count++;
}

void wifi_lwip_tx_path_counts(uint32_t *queued, uint32_t *stale,
                              uint32_t *direct_fail, uint32_t *hold_max_us)
{
   *queued = s_tx_queued;
   *stale = s_tx_dropped_stale + s_tx_dropped_full;
   *direct_fail = s_tx_direct_fail;
   *hold_max_us = s_tx_hold_max_us;
}

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

   /* Anything already held goes first, or this frame would overtake it.  When
      the queue cannot drain, the new frame joins the back rather than being
      dropped - which is what makes the queue a queue.  It only ever held one
      frame before this: the push below is reached solely when the flush
      succeeded, so every frame refused while the head was still stuck was
      lost, that being exactly the burst the queue exists to survive. */
   if (!wifi_lwip_tx_hold_flush()) {
      if (wifi_lwip_frame_self_retries(frame, offset))
         return ERR_IF;           /* TCP resends; do not spend a slot on it */
      wifi_lwip_tx_hold_push(frame, offset);
      return ERR_OK;
   }

   if (!sdio_runtime_send_ethernet_frame(frame, offset)) {
      s_tx_direct_fail++;
      /* TCP looks after itself; everything else would simply be lost. */
      if (wifi_lwip_frame_self_retries(frame, offset))
         return ERR_IF;
      wifi_lwip_tx_hold_push(frame, offset);
      return ERR_OK;                  /* accepted: we own it now */
   }

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
      struct pbuf *packet;

      if (frame_index != 0u
          && (int32_t)(RPI_GetSystemTime() - budget_end_us) >= 0)
         break;                   /* out of time; the rest waits for next poll */

      frame_length = 0u;
      if (!sdio_runtime_poll_ethernet_frame(frame, sizeof(frame), &frame_length))
         break;                   /* SDIO error / FIFO empty: stop this cycle */

      drained_any = true;

      if (frame_length == 0u)
         continue;

#if WIFI_LWIP_ICMP_PROBE_DIAG
      wifi_lwip_icmp_probe_rx(frame, frame_length);
#endif

      packet = pbuf_alloc(PBUF_RAW, frame_length, PBUF_POOL);
      if (packet == NULL)
         break;                   /* pbuf pool exhausted: stop this cycle,
                                     resume next poll once pbufs free up */

      /* On a per-packet failure, drop just that packet and keep
         draining the remaining frames from the chip; abandoning the
         whole budget on one bad packet leaves up to 7 frames queued
         in the chip until the next poll tick. */
      if (pbuf_take(packet, frame, frame_length) != ERR_OK) {
         pbuf_free(packet);
         continue;
      }

      if (g_wifi_lwip_context.netif.input(packet, &g_wifi_lwip_context.netif) != ERR_OK) {
         pbuf_free(packet);
         continue;
      }
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
#define WIFI_LWIP_REJOIN_MAX_US    (60u * 1000000u)

static uint32_t s_rejoin_interval_us = WIFI_LWIP_REJOIN_FIRST_US;
static uint32_t s_rejoin_due_us;
static bool     s_rejoin_scheduled;

/* Drive association retries.  Called from wifi_lwip_poll() on every service,
   in both the never-joined and lost-the-link cases - they differ only in
   which timestamp starts the clock. */
static void wifi_lwip_rejoin_service(void)
{
   uint32_t now_us = RPI_GetSystemTime();

   /* A full restart in flight owns the state machine outright: drive it to
      DONE (started) or ERROR (give up this round; the schedule below retries
      with its usual backoff). */
   if (s_full_restart_active) {
      if (!sdio_runtime_started()) {
         if (sdio_runtime_tick())
            return;               /* still working through bring-up */
         if (!sdio_runtime_started()) {
            s_full_restart_active = false;   /* ERROR: fall through, retry later */
            wifi_lwip_debug_log("full restart failed; will retry");
            return;
         }
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
      s_full_restarts_since_healthy = 0u;
      return;
   }

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

   if ((sdio_runtime_tx_dead_us() >= WIFI_LWIP_TX_DEAD_RESTART_US
        || s_rejoins_since_healthy >= 3u)
       && s_full_restarts_since_healthy < 3u) {
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
         s_rejoins_since_healthy = 0u;
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
   if (s_rejoin_interval_us < WIFI_LWIP_REJOIN_MAX_US) {
      s_rejoin_interval_us *= 2u;
      if (s_rejoin_interval_us > WIFI_LWIP_REJOIN_MAX_US)
         s_rejoin_interval_us = WIFI_LWIP_REJOIN_MAX_US;
   }
   s_rejoin_due_us = now_us + s_rejoin_interval_us;
}

void wifi_lwip_poll(void)
{
   if (!g_wifi_lwip_context.timers_running)
      return;

   /* Stop polling once the WiFi boot has failed.  There is no point
      hammering the SDIO bus and lwIP timers when the stack can never
      come up; timers_running latches false so this is permanent. */
   if (wifi_get_state() == WIFI_STATE_ERROR) {
      g_wifi_lwip_context.timers_running = false;
      wifi_lwip_debug_log("wifi in error state - polling stopped");
      return;
   }

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
   sys_check_timeouts();
   wifi_lwip_log_dhcp_state();
   g_wifi_lwip_context.last_service_time_us = RPI_GetSystemTime();
   g_wifi_lwip_context.service_calls++;
   wifi_lwip_update_runtime_state();
}

const wifi_lwip_context_t *wifi_lwip_get_context(void)
{
   return &g_wifi_lwip_context;
}