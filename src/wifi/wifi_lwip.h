#ifndef WIFI_WIFI_LWIP_H
#define WIFI_WIFI_LWIP_H

#include <stdbool.h>

#include "lwip/ip4_addr.h"
#include "lwip/netif.h"

typedef struct {
   bool prepared;
   bool initialized;
   bool netif_added;
   bool static_configured;
   bool dhcp_started;
   bool timers_running;
   bool use_dhcp;
   bool has_dns;
   bool link_up;
   bool link_established;
   bool address_ready;
   struct netif netif;
   ip4_addr_t ipaddr;
   ip4_addr_t netmask;
   ip4_addr_t gateway;
   ip4_addr_t dns;
   uint32_t init_time_us;
   uint32_t last_service_time_us;
   uint32_t service_calls;
} wifi_lwip_context_t;

void wifi_lwip_prepare(void);
void wifi_lwip_init_stack(void);
void wifi_lwip_poll(void);
const wifi_lwip_context_t *wifi_lwip_get_context(void);
/* Drop the live lease and addresses while retaining the reusable lwIP core. */
void wifi_lwip_disconnect(void);

/* Drain the RX at full rate for a short window: call after transmitting a
 * datagram that expects a prompt reply, so the reply is not held by the
 * idle-throttle backoff. */
void wifi_lwip_rx_kick(void);
/* Diagnostic, normally compiled out.  Times each ICMP echo from arrival to
   the reply actually reaching the chip, splitting firmware latency from air
   latency.  The 2026-07 campaign measured 14-16 us turnaround against ~20 ms
   RTTs, exonerating the firmware; re-enable if that split is needed again. */
#define WIFI_LWIP_ICMP_PROBE_DIAG 0
#if WIFI_LWIP_ICMP_PROBE_DIAG
void wifi_lwip_icmp_probe_counts(uint32_t *rx_seen, uint32_t *tx_seen);
uint32_t wifi_lwip_icmp_probe_read(uint32_t *gap_ms, uint32_t *turnaround_us,
                                  uint32_t max, uint32_t *total);
#endif

/* Send-path counters for /status: refusals, queueing, staleness (which
   includes queue-full drops), and the longest wait in the hold queue. */
void wifi_lwip_tx_path_counts(uint32_t *queued, uint32_t *stale,
                              uint32_t *direct_fail, uint32_t *hold_max_us);

/* UDP blast test rig (/udpblast): a poll-driven raw-UDP source that takes
   TCP out of the throughput measurement.  start() primes it (datagrams of
   1472-byte payload = one full 1500-byte IP packet each); the lwIP service
   pass then drains it a few datagrams per pass.  The receiver's byte count
   over its own clock is the measurement; stats() is the cross-check. */
/* burst = datagrams emitted per service pass (0 = default 4, clamped to
   16): the pipeline-capability knob - at burst >= 8 the producer
   demonstrably offers enough frames per pass for deep glom batches. */
void wifi_lwip_udpblast_start(const ip_addr_t *dst, uint16_t port,
                              uint32_t datagrams, uint8_t burst);
void wifi_lwip_udpblast_stats(uint32_t *sent, uint32_t *remaining,
                              uint32_t *elapsed_us);

/* wifi_diag-gated per-pass histograms (buckets 0/1/2-3/4-7/8-15/16+):
   frames handed to the TX path per service pass, split by producer
   (TCP / the udpblast rig / everything else), and data frames drained
   from the chip per pass.  Only passes that moved at least one frame in
   that direction are recorded; in the TX histograms bucket 0 means "a
   productive pass in which this source contributed nothing".  False
   while wifi_diag is off. */
bool wifi_lwip_pass_diag_read(uint32_t tcp_hist[6], uint32_t blast_hist[6],
                              uint32_t other_hist[6], uint32_t rx_hist[6]);

/* Compile-gated RX cost profiler: average cycles per delivered RX frame,
   split into the SDIO fetch (hwtag peek + body CMD53) and the lwIP
   handoff (pbuf + netif input, which runs the TCP callbacks and hence
   the webserver's producer).  ARM1176 only - it reads the CP15 c15
   CCNT/64 counter, which FAULTS on the A53 (kernel7); debug/bench
   builds only, default off. */
#define WIFI_LWIP_RX_PROFILE 0
#if WIFI_LWIP_RX_PROFILE
void wifi_lwip_rx_profile_read(uint32_t *sdio_cycles_avg,
                               uint32_t *lwip_cycles_avg, uint32_t *frames);
#endif

#endif