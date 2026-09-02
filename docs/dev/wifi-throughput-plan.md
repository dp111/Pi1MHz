# WiFi throughput — plan

> **OUTCOME (2026-08-26, investigation complete):** this plan was executed on
> the `worktree-agent-a1fcf31c5ad85f2d0` branch (15 commits; see its
> VALIDATION.md for every checkpoint's hardware numbers). The central
> per-frame-CMD53-overhead model was **falsified**: TX glomming works
> (batches of ~4, zero failures) but throughput did not move. The measured
> governor is the dongle firmware's credit-grant cadence (~4-5 credits every
> 1-2 ms), which no host-side knob reached (credall / maxtxpktglom /
> F2 watermark / MES all A/B'd - maxtxpktglom is actively harmful).
> Ceiling on this polled host: ~2.7-2.9 MB/s UDP, 2.2-2.4 TCP. Remaining
> levers, in order: RX-side service cost (lifts TCP toward the UDP ceiling),
> a Linux-side grant-pattern capture, acceptance. The instrumentation,
> coalescing feed and glom capability are worth merging (config-gated).
> Phases below are kept as the design record; §4's "Phase 2 fixes" were
> found already landed in the tree before the branch started.


Status: PLAN ONLY — no source changes applied. All `file:line` references were
verified against the working tree on 2026-08-26 (branch `master`, after
`d08242e`). brcmfmac references are to
`drivers/net/wireless/broadcom/brcm80211/brcmfmac/` at torvalds/linux master
commit `45c13f3f9e3bb15fd89ff2864c6f627a3b4b4229` (fetched and read for this
plan; details not derivable from that source are marked **UNVERIFIED**).

## 0. Established baseline (not to be re-litigated)

- Ceiling ~2.2 MB/s on `/bench.bin` (RAM-sourced, so the SD card is out of the
  picture); Linux brcmfmac on identical hardware: ~4.25 MB/s.
- The radio is not the bottleneck: MCS7 (65–72 Mbit/s) held under load, chip
  pktcnts show zero over-air TX retries.
- The bottleneck is SDPCM credit-window turnaround: 17% of TX attempts refused
  for credits during downloads; the transfer-size fit `R = W/(T0 + W/C)` gave
  `T0 ≈ 6.6 ms` fixed per transfer and `C ≈ 3.2 MB/s` asymptotic streaming
  rate.
- The SDIO bus itself is optimal already: 50 MHz high-speed
  (`SDIO_RUNTIME_HIGH_SPEED_CLOCK_HZ`, `src/wifi/sdio.c:23`, applied at
  `sdio.c:1089`), 4-bit, fn2 block size 512
  (`SDIO_PROBE_FUNCTION2_BLOCK_SIZE`, `sdio.c:314`). The cost is protocol
  turnaround per frame, not clocking.
- A TX hold-queue (RX-paced flush, depth 16, TCP capped at 12) landed in
  `d4fe2d7`; its predicted +0.3–0.4 MB/s is unproven (benched on a
  degraded-RF day only).
- The big structural gap vs Linux is txglom: brcmfmac batches up to 32 frames
  into one CMD53.

---

## 1. Map of the current driver

### 1.1 TX data path (lwIP → chip)

1. **`wifi_lwip_link_output`** (`src/wifi/wifi_lwip.c:462`): flattens the pbuf
   chain into a static 1600-byte frame buffer, then:
   - flushes the hold queue first so nothing overtakes
     (`wifi_lwip.c:490` → `wifi_lwip_tx_hold_flush`, `wifi_lwip.c:366`);
   - if the queue could not fully drain, the new frame joins the back
     (`wifi_lwip_tx_hold_push`, `wifi_lwip.c:429`) and lwIP is told `ERR_OK`;
     if it cannot be queued (full / TCP over its reservation) → `ERR_IF`
     (`wifi_lwip.c:497`);
   - otherwise sends directly via `sdio_runtime_send_ethernet_frame`
     (`wifi_lwip.c:500`), falling back to the queue on refusal;
   - kicks the RX drain aggressive window (`wifi_lwip_rx_kick`,
     `wifi_lwip.c:526`, window `WIFI_LWIP_RX_KICK_US` = 8 ms,
     `wifi_lwip.c:70`).

2. **Hold queue** (landed `d4fe2d7`):
   - constants: age-out 250 ms (`WIFI_LWIP_TX_HOLD_MAX_AGE_US`,
     `wifi_lwip.c:318`), depth 16 (`:319`), TCP cap 12 (`:323`);
   - RX-paced retries: a refused flush latches `s_tx_flush_blocked` plus the
     `sdio_runtime_last_any_rx_stamp()` value (`wifi_lwip.c:368,385–386,410`;
     stamp getter `sdio.c:5901`) — no retry until a new frame has arrived,
     because only RX refreshes `max_seq`;
   - TCP classifier `wifi_lwip_frame_self_retries` (`wifi_lwip.c:414`);
   - counters exposed by `wifi_lwip_tx_path_counts` (`wifi_lwip.c:452`),
     rendered as the `/status` "TX path" row (`src/wifi/webserver.c:2473–2478`).

3. **`sdio_runtime_send_ethernet_frame`** (`src/wifi/sdio.c:6371`):
   - credit gate check (`sdio.c:6398`), stall/resync ladder below it
     (`sdio.c:6403–6466`);
   - non-blocking bus wake (`sdio_runtime_wake_bus`, called `sdio.c:6476`,
     defined `sdio.c:6547`; fast path = "transfer completed < 2 ms ago",
     `SDIO_BUS_AWAKE_ASSUME_US`, `sdio.c:37`);
   - frame build (`sdio.c:6479–6491`): `total_length = 18 + frame_length`;
     bytes 0–1 len, 2–3 ~len, 4 sequence (taken at send time via
     `sdio_next_sdpcm_sequence`, `sdio.c:3256`, stamped at `sdio.c:6483`),
     5 channel (`SDPCM_DATA_CHANNEL`), 7 header length 14
     (`SDPCM_DATA_HEADER_LENGTH`, `sdio.c:410` area — defined `sdio.c:406`),
     14–17 BDC header, payload at 18;
   - one CMD53 via `sdio_function2_transfer` (`sdio.c:6493` →
     `sdio_function2_transfer_timeout`, `sdio.c:3371`: >512 bytes → block
     mode with 512-byte blocks (count clamped to 511), ≤512 → byte mode;
     CMD53 built in `sdio_cmd53_execute_timeout`, `sdio.c:4554`, submitted
     through `sdio_host_submit_arasan_command`, `src/wifi/sdio_host.c:801`,
     PIO drain, busy-waited);
   - on CMD53 failure: **conditional** sequence reclaim (`sdio.c:6504–6505`),
     see §1.4.

   A 1460-byte TCP segment therefore costs one 1478-byte SDPCM frame = one
   3-block CMD53 (1536 wire bytes).

### 1.2 TX control path

- Control frames are built from the shared probe template
  (`sdio_prepare_tx_control_template`, `sdio.c:3289`) and sent by
  `sdio_probe_send_single_tx_control_template_timeout` (`sdio.c:3888`), which
  takes its sequence number from the same shared space (`sdio.c:3906`).
  Layout: 12-byte SDPCM control header + 16-byte CDC header + payload
  (`sdio.c:3903–3918`).
- The runtime ioctl pollers self-gate on the credit window before sending:
  RSSI (`sdio.c:6295`), PKTCNTS (`sdio.c:6002–6006`), RATE (`sdio.c:6069`),
  PM query. The join/rejoin burst is **deliberately ungated** — it must be
  able to talk to a chip whose window state is unknown (comment at
  `sdio.c:6120–6126`).
- The CLM boot download sends chunked `WLC_SET_VAR` control frames
  (`sdio.c:4088–4124`) — boot-only, before data traffic exists.

### 1.3 Credit accounting

- One shared sequence counter for all channels: `g_sdpcm_tx_sequence`
  (`sdio.c:64`, rationale comment `sdio.c:60–63`), allocated at the moment a
  frame hits the bus (`sdio_next_sdpcm_sequence`, `sdio.c:3256`).
- Window refresh: **every** received frame's software header is decoded in
  `sdio_runtime_complete_read_ethernet_frame_timeout` (`sdio.c:3548`);
  byte 8 → `g_runtime_wlan_flow_control`, byte 9 → `g_runtime_max_seq`
  (`sdio.c:3604–3606`). If the refreshed window is open, the TX-dead clock
  clears (`sdio.c:3611–3612`) — the only place that may clear it. The
  any-channel RX stamp is set beside it (`sdio.c:3619`).
- The gate: `sdio_runtime_tx_window_shut` (`sdio.c:6127–6133`):

  ```c
  return g_runtime_max_seq_valid
     && (g_runtime_wlan_flow_control != 0u
         || (int8_t)(g_sdpcm_tx_sequence - g_runtime_max_seq) >= 0);
  ```

  i.e. the window depth is `(int8_t)(max_seq - tx_sequence)`; shut when ≤ 0
  or when a wireless flow-control stop is cached.

### 1.4 Failure and recovery paths (both prior death-traps are FIXED in tree)

The two "Phase 2 correctness fixes" this plan was commissioned to design are
**already landed and verified present in the source**:

1. **Conditional sequence reclaim** — landed (`f69e550` introduced the
   classifier; `065e223` refined the host layer). All three
   `--g_sdpcm_tx_sequence` sites are guarded by
   `sdio_host_last_failure_precommand()`:
   - data path `sdio.c:6504–6505`,
   - control template `sdio.c:3931–3932`,
   - CLM chunk `sdio.c:4118–4119`.

   The classifier (`sdio_host.c:1350–1359`) returns true only for
   `SD_ERR_MASK_CMD_TIMEOUT` (1<<16); the data-phase wait expiry is a
   deliberately distinct code (`SD_ERR_MASK_DATA_TIMEOUT`, 1<<20,
   `sdio_host.c:106–114`) so a completion-phase error — where the card may
   already have consumed the payload — never reclaims. The synthetic
   never-issued failures (fault holdoff, failed re-open) stamp
   `SD_ERR_MASK_CMD_TIMEOUT` precisely so reclaim stays correct
   (`sdio_host.c:817–843`).

2. **Sequence-rebase resync removed** — landed (`ce01484` "stop rebasing the
   SDPCM sequence into consumed space"). The sustained-stall handler now only
   clears the cached flow-control mask and re-asks the gate
   (`sdio.c:6456–6463`); the comment block at `sdio.c:6428–6455` documents why
   ("NEVER rewrite the sequence number"). Real credit exhaustion exits through
   the recovery ladder: TX-dead clock (`g_runtime_tx_shut_since_us`, fed by
   gate refusals `sdio.c:6405–6406` AND bus-level CMD53 failures
   `sdio.c:6510–6514`, cleared only by the chip advertising an open window
   `sdio.c:3611–3612`), rejoin at 8 s, full chip restart (WL_REG_ON cycle,
   firmware re-download, fresh sequence space both sides) at 25 s or three
   failed rejoins (`90e17a7`, hardened `cff63a3`).

   Stall tiers: 20 ms idle tier only when nothing has arrived since the stall
   began, 1 s under traffic (`SDPCM_TX_STALL_IDLE_RESYNC_US` `sdio.c:33`,
   `SDPCM_TX_STALL_RESYNC_US` `sdio.c:414`, comparison `sdio.c:6421–6426`).

   Consequence for this plan: **Phase 2 is a validation phase, not an
   implementation phase** (§4).

### 1.5 RX path

- **`sdio_runtime_poll_ethernet_frame`** (`sdio.c:6630`):
  - in-band interrupt gate first — one MMIO read when DAT1 is not asserted,
    10 ms safety sweep (`sdio.c:6669–6684`, `SDIO_RX_SWEEP_INTERVAL_US`
    `sdio.c:220`);
  - bus wake (`sdio.c:6694`);
  - INT_STATUS service only when the FIFO last read empty or 20 ms elapsed
    (`sdio.c:6697–6763`, `SDIO_INT_SERVICE_INTERVAL_US` `sdio.c:27`);
  - then up to 8 frames per poll (`SDIO_RUNTIME_MAX_RX_FRAMES_PER_POLL`,
    `sdio.c:20`), each frame costing **two CMD53s**: a 4-byte hwtag peek
    (`sdio.c:6774–6777`) plus the body read in
    `sdio_runtime_complete_read_ethernet_frame_timeout` (`sdio.c:3586–3590`,
    body rounded up to whole 512-byte blocks into a 2052-byte static buffer,
    `SDIO_RUNTIME_FRAME_BUFFER_SIZE` `sdio.c:19`).
- **Drain loop**: `wifi_lwip_drain_rx_frames` (`wifi_lwip.c:540`) hands frames
  to lwIP under a 1.2 ms wall-clock budget (`WIFI_LWIP_SERVICE_BUDGET_US`,
  `wifi_lwip.c:75`); the service pass loops drain+flush until the FIFO is
  empty (`wifi_lwip.c:1149–1160`), with `wifi_lwip_tx_hold_flush()` inside the
  loop so a frame refused a moment ago goes out right after the drain that
  refreshed the window (`wifi_lwip.c:1153`), and also on non-drain passes
  (`wifi_lwip.c:1173`).

### 1.6 lwIP configuration (relevant to any throughput work)

`src/wifi/lwipopts.h`: `TCP_MSS` 1460 (`:57`), `TCP_WND` 44×MSS = 64,240
(`:65`), `TCP_SND_BUF` 32×MSS (`:96`), `MEM_SIZE` 256 KB (`:97`),
`PBUF_POOL_SIZE` 96 (`:116`). A same-day A/B of `TCP_SND_BUF` 32→44 measured
identical — the SDPCM window binds, not the TCP window.

### 1.7 Per-frame cost model (why txglom is the big lever)

At the fitted asymptote `C ≈ 3.2 MB/s`, one 1460-byte segment consumes
`1460 / 3.2e6 ≈ 456 µs`. Wire time for its 1536-byte 3-block CMD53 at
50 MHz × 4-bit (25 MB/s) is ≈ 61 µs + ~10 µs command/response framing. So
**≈ 390 µs of every frame is fixed per-transfer overhead** — CMD53 issue and
busy-wait completion on the PIO EMMC path (`sdio_host.c:801` and the
issue/wait code around `sdio_host.c:770–795`), credit-gate turnaround, wake
checks, the RX peek+body pair for the ACKs coming back, and main-loop
scheduling. This ratio (fixed ≫ wire) is exactly what glomming amortizes.

---

## 2. Phase 0 — re-bench the landed queue on clean RF

The `d4fe2d7` queue's predicted +0.3–0.4 MB/s has never been measured on a
clean-RF day (baseline that day: ~1.8 MB/s vs 2.3–2.47 the day before). Do
this before touching anything, or every later A/B is against an unknown.

Procedure (Pi control per `PI-CONTROL.md`; standing authorization to
flash/reboot applies, but Phase 0 needs no flash — the queue is in the
installed kernel lineage):

```sh
# 1. Clean-RF gate: only proceed when ALL of these hold.
/mnt/c/Archlinux/claude-tmp/pi-http.sh -s /status
#    - "Link rate (TX)" >= 65 Mbit/s (MCS7); re-query until populated
#    - RSSI near the -34 dBm reference, not tens of dB worse
#    - ping the GATEWAY from the same host: ~0% loss (RF-day sanity,
#      see the wifi-ping-jitter finding: lossy Pi pings with a clean
#      gateway = RF problem, abort the bench day)

# 2. Record the /status "TX path" and frame counters (before-values).

# 3. Bench: three runs minimum, RAM-sourced so SD is excluded.
for i in 1 2 3; do
  /mnt/c/Archlinux/claude-tmp/pi-http.sh -s -o /dev/null \
      -w 'bytes=%{size_download} secs=%{time_total} rate=%{speed_download}\n' \
      /bench.bin
done

# 4. Re-read /status; record deltas.
/mnt/c/Archlinux/claude-tmp/pi-http.sh -s /status
```

Record per run: MB/s; deltas of "TX path" `queued / stale / fail / max wait`
(`webserver.c:2473–2478`); "Transmit resyncs" (must stay 0); "Rejoins" (0);
"RX interrupt gate" MISSED; "Link rate (TX)" mid-download; chip pktcnts
(`tx_bad` must stay 0). Then the concurrency check: two parallel `/bench.bin`
fetches + 12 pings (reference: 0/12 loss).

Interpretation:
- ≥ 2.6 MB/s sustained → the queue's gain is real; baseline for Phase 3 A/Bs.
- ≈ 2.2–2.4 MB/s with a large `queued` delta and small `stale` → the queue
  converts refusals but conversion isn't worth throughput → the fixed
  per-frame cost dominates → txglom is confirmed as the only big lever.
- `stale` growing during a single-stream download → the window shuts for
  > 250 ms stretches; capture that with the Phase 1 histogram before judging.

Effort: half a session. Abort criterion: none (measurement only) — but if the
clean-RF gate cannot be met across days, note it and A/B all later phases
same-day-only.

---

## 3. Phase 1 — zero-risk instrumentation (proposed diffs, NOT applied)

All three follow the existing patterns: plain counters + a `/status` row
(like "TX path"), no log calls (release builds have no output). None touch
the TX/RX decision logic.

### 3.1 Credit-window depth sampling

Sample the window depth at the only place truth arrives: the RX header
refresh. Answers "how many credits does the chip actually grant at a time?" —
which directly bounds the useful glom size in Phase 3.

```diff
--- a/src/wifi/sdio.c
+++ b/src/wifi/sdio.c
@@ static uint8_t g_runtime_max_seq;
 static uint8_t g_runtime_wlan_flow_control;
 static bool g_runtime_max_seq_valid;
+/* Credit-window depth histogram, sampled on every RX header refresh: how
+   many more frames the chip would accept at that instant.  Buckets
+   0,1,2,3,4-7,8-15,>=16; index 0 also counts flow-control stops. */
+static uint32_t g_credit_depth_hist[7];
+static uint8_t g_credit_depth_min = 0xffu;
```

```diff
--- a/src/wifi/sdio.c
+++ b/src/wifi/sdio.c
@@ (in sdio_runtime_complete_read_ethernet_frame_timeout, sdio.c:3604)
    g_runtime_wlan_flow_control = frame_buffer[8];
    g_runtime_max_seq = frame_buffer[9];
    g_runtime_max_seq_valid = true;
+   {
+      int8_t depth = (int8_t)(g_runtime_max_seq - g_sdpcm_tx_sequence);
+      uint8_t d = (depth <= 0 || g_runtime_wlan_flow_control != 0u)
+                     ? 0u : (uint8_t)depth;
+      ++g_credit_depth_hist[(d == 0u) ? 0u
+                            : (d <= 3u) ? d
+                            : (d <= 7u) ? 4u
+                            : (d <= 15u) ? 5u : 6u];
+      if (d < g_credit_depth_min)
+         g_credit_depth_min = d;
+   }
```

Plus a getter (near `sdio_runtime_rx_gate_counts`, `sdio.c:6582`), a
declaration in `src/wifi/sdio.h` (near `:315`), and a `/status` row beside
"TX path" (`webserver.c:2478`).

### 3.2 Shut→reopen latency histogram

`g_runtime_tx_shut_since_us` already stamps the first refusal
(`sdio.c:6405–6406`) and is cleared exactly once, at the RX refresh that
proves the window open (`sdio.c:3611–3612`). Bucket the elapsed time at that
clear:

```diff
--- a/src/wifi/sdio.c
+++ b/src/wifi/sdio.c
@@ (sdio.c:3607-3612)
    /* The chip has just told us where the window stands.  If it has room for
       our next frame and no flow-control stop, transmit is demonstrably
       alive - this is the ONLY place that may clear the TX-dead clock,
       because it is the only signal the chip itself vouches for. */
-   if (!sdio_runtime_tx_window_shut())
+   if (!sdio_runtime_tx_window_shut()) {
+      if (g_runtime_tx_shut_since_us != 0u) {
+         uint32_t shut_us = RPI_GetSystemTime() - g_runtime_tx_shut_since_us;
+         /* Buckets: <100us, <500us, <1ms, <5ms, <20ms, >=20ms. */
+         ++g_credit_reopen_hist[(shut_us < 100u) ? 0u
+                                : (shut_us < 500u) ? 1u
+                                : (shut_us < 1000u) ? 2u
+                                : (shut_us < 5000u) ? 3u
+                                : (shut_us < 20000u) ? 4u : 5u];
+         if (shut_us > g_credit_reopen_max_us)
+            g_credit_reopen_max_us = shut_us;
+      }
       g_runtime_tx_shut_since_us = 0u;
+   }
```

(with `static uint32_t g_credit_reopen_hist[6]; static uint32_t
g_credit_reopen_max_us;` beside the depth histogram, getter + `/status` row
as above). Interpretation: mass in the <500 µs buckets = fast ACK-paced
refills, txglom will help; mass ≥ 5 ms = the chip itself is slow to credit,
in which case glomming still helps (fewer, larger grants needed) but the
prediction arithmetic in §5.6 must be revised downward.

### 3.3 UDP blast (takes TCP out of the picture)

A poll-driven raw-UDP sender, triggered over HTTP, measured at the receiver.
Removes lwIP TCP (ACK clocking, snd_buf) from the loop entirely: the
remaining rate is pure driver+SDPCM capacity.

```diff
--- a/src/wifi/wifi_lwip.c
+++ b/src/wifi/wifi_lwip.c
@@ (new, near the tx-hold statics, wifi_lwip.c:332)
+/* UDP blast test rig: /udpblast primes it, the poll loop drains it.
+   Payload 1472 bytes -> exactly one 1500-byte IP packet per datagram. */
+static struct udp_pcb *s_blast_pcb;
+static ip_addr_t s_blast_dst;
+static uint16_t s_blast_port;
+static uint32_t s_blast_remaining;   /* datagrams still to send */
+static uint32_t s_blast_sent;
+static uint32_t s_blast_start_us, s_blast_end_us;
+
+void wifi_lwip_udpblast_start(const ip_addr_t *dst, uint16_t port,
+                              uint32_t datagrams)
+{
+   if (s_blast_pcb == NULL)
+      s_blast_pcb = udp_new();
+   if (s_blast_pcb == NULL)
+      return;
+   s_blast_dst = *dst;
+   s_blast_port = port;
+   s_blast_remaining = datagrams;
+   s_blast_sent = 0u;
+   s_blast_start_us = RPI_GetSystemTime();
+}
+
+static void wifi_lwip_udpblast_poll(void)
+{
+   uint8_t burst;
+
+   /* A few per pass: enough to saturate, bounded so the Beeb-facing
+      main loop never stalls. */
+   for (burst = 0u; burst < 4u && s_blast_remaining != 0u; ++burst) {
+      struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 1472u, PBUF_RAM);
+      if (p == NULL)
+         return;                 /* pool pressure: try next pass */
+      memset(p->payload, 0x55, 1472u);
+      if (udp_sendto(s_blast_pcb, p, &s_blast_dst, s_blast_port) == ERR_OK) {
+         ++s_blast_sent;
+         --s_blast_remaining;
+         s_blast_end_us = RPI_GetSystemTime();
+      }
+      pbuf_free(p);
+      if (s_blast_remaining == 0u)
+         break;                  /* done: end stamp already taken */
+   }
+}
```

Hook `wifi_lwip_udpblast_poll()` into the service pass beside
`sys_check_timeouts()` (`wifi_lwip.c:1174`), and add a `/udpblast` route in
the dispatcher next to `/bench.bin` (`webserver.c:5976`) that parses
`?host=&port=&mb=` and calls `wifi_lwip_udpblast_start`, plus a `/status` row
reporting `sent / elapsed`. Receiver side: `iperf -s -u -p <port>` (or a
socket-count script) on the PC — the receiver's byte count over its own clock
is the measurement; the Pi's elapsed stamp is the cross-check. Note the sent
frames still travel the ordinary `link_output` → hold-queue → credit-gate
path, which is the point: same bottleneck, no TCP.

Also worth one extra counter while in here (zero risk, quantifies how often
the no-reclaim branch of the landed fix actually triggers): increment a
`g_tx_data_phase_fail` counter in the `sdio.c:6494` failure block when
`!sdio_host_last_failure_precommand()`.

Effort for Phase 1: one session including flash + bench cycles (flash only a
settled Pi; keep the SD kernel same-lineage per the chain-boot rules). Abort
criterion: none — but if adding the rows perturbs the bench (> ±0.1 MB/s vs
Phase 0), suspect measurement, not the counters, and re-run.

---

## 4. Phase 2 — correctness: verify the landed fixes (nothing to implement)

Both fixes this phase was scoped to design are already in the tree (§1.4,
commits `ce01484`, `f69e550`, `065e223`, ladder `90e17a7`/`cff63a3`). What
remains is the validation debt and two documented residual gaps.

### 4.1 Validation plan for the landed fixes

1. **Reclaim correctness under real bus errors**: run the Phase 3.3 UDP blast
   and two concurrent `/bench.bin` downloads for ≥ 30 min; require
   "Transmit resyncs" = 0, rejoins = 0, and (with the §3.3 extra counter) if
   any data-phase failures occurred, confirm TX did not die afterwards — the
   window must reopen on the next RX refresh, visible in the §3.2 histogram
   rather than in the ≥ 20 ms bucket.
2. **Ladder round trip**: the wedge→recovery round trip has never been
   observed end-to-end. Deliberately induce it (the known method: physically
   degrade RF mid-download or power-cycle the AP) and confirm: rejoin at ~8 s
   of TX-dead, chip restart at 25 s, traffic resumes, restart budget refunds
   only after 30 s health. No code change; a hardware session.
3. **Regression fence**: any Phase 3 work must keep the three guarded reclaim
   sites (`sdio.c:3931`, `sdio.c:4118`, `sdio.c:6504`) guarded, and must not
   add any path that writes `g_sdpcm_tx_sequence` other than increment and
   the boot reset (`sdio.c:5490`).

### 4.2 Residual gaps (documented, deliberately not "fixed" here)

- The join/rejoin burst bypasses the credit gate by design (`sdio.c:6120–6126`).
  Bounded: it runs only when the link is already down. Leave it.
- A queued TCP segment is reported `ERR_OK`, so a later stale-drop is real
  loss recovered only by RTO (`wifi_lwip.c:484–489`). Accepted by design
  (staleness = 250 ms of shut window = already an outage).

Effort: one hardware session (mostly item 2). Abort criterion: if the induced
wedge does NOT recover through the ladder, stop all Phase 3 work — txglom
multiplies sequence-space risk and must sit on a proven recovery path.

---

## 5. Phase 3 — TX glomming (the big lever)

### 5.1 Protocol facts (from brcmfmac source; cites into that tree)

- **Superframe layout** (brcmfmac `sdio.c:1303–1331`): with TX glom active,
  every subframe carries a 20-byte header: 4-byte hw frame tag (le16 len +
  le16 ~len), 8-byte **hardware extension header** (word0 =
  `(sublen - 4) | lastfrm_bit << 24`; word1 = `tail_pad << 16`), 8-byte
  software header (seq, channel/flags, next-len **0 on TX**, data offset,
  then zeros). Packing code: `brcmf_sdio_hdpack` (brcmfmac `sdio.c:1487–1512`);
  `SDPCM_HWEXT_LEN 8` (`:1333`), `tx_hdrlen` 12→20 (`:3573,3584`).
- **First subframe's frame tag is rewritten to the padded superframe total**
  (`brcmf_sdio_txpkt_prep`, brcmfmac `sdio.c:2247–2251`); subframes 2..N keep
  their individual tags; there is no trailer — the lastfrm bit ends the chain.
- **Padding**: each subframe padded to a multiple of `sgentry_align`
  (default 4); the LAST subframe additionally padded so the whole superframe
  is an exact multiple of the fn2 block size (512) — mandatory, the transfer
  is whole blocks only (`brcmf_sdio_txpkt_prep_sg`, brcmfmac `sdio.c:2120–2175`;
  `mmc_submit_one`, `bcmsdh.c:334–363`). Per-subframe tail pad is declared in
  hwext bytes 6–7 so firmware strips it.
- **Credits: one sequence number per SUBFRAME.** The prep loop stamps
  `hd_info.seq_num = txseq++` per packet (brcmfmac `sdio.c:2199–2232`) and on
  success `tx_seq += qlen` (`:2322–2323`). Chain size =
  `min(tx_max - tx_seq, txglomsz, queued)` (`brcmf_sdio_sendfromq`,
  `:2332–2379`, credit min at `:2350`). So an N-glom needs window depth ≥ N —
  which is what the §3.1 histogram measures for our chip.
- **Enablement** (`brcmf_sdio_bus_preinit`, brcmfmac `sdio.c:3530–3592`) —
  iovar names are from the DEVICE's perspective:
  - `bus:rxglom = 1` ("device may RECEIVE glommed transfers") is what enables
    **host TX glom**. Allowed to fail; on failure brcmfmac stays
    frame-at-a-time.
  - `bus:txglom` controls device→host glom (host RX superframes). brcmfmac
    writes 0 only for SDIO core rev < 12; at rev ≥ 12 it leaves the firmware
    default. Our driver has never written any of these iovars and receives
    strictly one frame per read today, so device→host glom is evidently not
    active by default against a host that never negotiated — but to pin that,
    we will write `bus:txglom = 0` explicitly. **UNVERIFIED** that
    43430/43455 firmware honors it at rev ≥ 12 (brcmfmac only exercises the
    write on rev < 12); the guard in §5.4 covers the case it does not.
  - `bus:txglomalign = 4` tells firmware the host's subframe alignment.
  - There is no per-chip glom size in brcmfmac: `txglomsz` default 32
    (`common.c:34–38`); firmware-internal caps for 43430/43455 are
    **UNVERIFIED**.
  - Once glom mode is on, **every** host→device frame uses the 20-byte
    header, including single control frames (glom-of-1: lastfrm = 1, frame
    tag rewritten to the padded total; brcmfmac `sdio.c:2429–2438`).
- **RX glom can stay off**: the two directions are independent iovars;
  brcmfmac tolerates `bus:rxglom` failure, and we will hold `bus:txglom = 0`.
  RX deglom (glom-descriptor frames on channel 3, `SDPCM_GLOM_CHANNEL`,
  brcmfmac `sdio.c:1341,1514–1729`) is explicitly out of scope for this plan.

### 5.2 Design for this driver

No scatter-gather exists on the PIO EMMC path, so the superframe is assembled
by memcpy into one static buffer — a non-issue: ~1.5 KB per subframe at
PLD-optimized memcpy speeds is single-digit µs against the ~390 µs per-frame
turnaround being amortized.

Data structures (in `sdio.c`, beside the existing TX statics):

```c
#define SDPCM_TXGLOM_MAX 4u                 /* stage C ceiling; stage B runs 2 */
/* Worst subframe: 20B header + 4B BDC + 1500B payload + 3B align = 1528. */
#define SDPCM_TXGLOM_BUF_SIZE 8192u         /* 4*1528 rounded to blocks + slack */
static _Alignas(4) uint8_t g_txglom_buf[SDPCM_TXGLOM_BUF_SIZE];
static bool     g_sdpcm_txglom_active;      /* iovars accepted at bring-up */
static uint32_t g_txglom_superframes;       /* stats for /status */
static uint32_t g_txglom_subframes;
static uint32_t g_txglom_fallbacks;
```

New entry point `sdio_runtime_send_ethernet_frames(const wifi_lwip_tx_slot_t
*const *slots, uint8_t n)` (or an equivalent `begin/add/commit` triple so
`wifi_lwip.c` does not export its slot type):

1. Gate: window depth `(int8_t)(g_runtime_max_seq - g_sdpcm_tx_sequence)`
   must be ≥ n and `g_runtime_wlan_flow_control == 0`; else send what fits
   (n′ = depth) and leave the rest queued — same back-pressure semantics as
   today.
2. Build each subframe at the next 4-aligned offset: 20-byte header
   (tag = sublen/~sublen; hwext word0 = `(sublen-4) | (last ? 1u<<24 : 0)`;
   hwext word1 = `tail_pad << 16`; sw header: seq = `sdio_next_sdpcm_sequence()`
   per subframe, channel = `SDPCM_DATA_CHANNEL`, data offset = 22), 4-byte
   BDC, payload, zero tail pad.
3. Pad the last subframe so total is a 512 multiple; rewrite subframe 0's
   frame tag to the padded total.
4. One `sdio_function2_transfer(&g_runtime_device, true, g_txglom_buf, total)`
   (`sdio.c:3393` — already block-mode for > 512 bytes, up to 511 blocks, far
   beyond any glom here).
5. Failure handling generalizes the landed rule: if
   `sdio_host_last_failure_precommand()` → `g_sdpcm_tx_sequence -= n`
   (nothing reached the card, all n numbers provably unused); otherwise
   reclaim **nothing** — the card may hold any prefix, and guessing walks
   into replay territory. Frames are then lost (TCP rides RTO; non-TCP is
   counted), and the TX-dead evidence stamp fires exactly as at
   `sdio.c:6510–6514`.

Header-mode switch: when `g_sdpcm_txglom_active`, the single-frame data path
(`sdio.c:6479–6491`) and the control template sender (`sdio.c:3903–3918`)
both emit the 20-byte form (glom-of-1, lastfrm = 1, padded-total tag). This
is the widest-blast-radius part of the change: **every** TX frame changes
shape the moment the iovar is accepted. It is confined to the two builder
sites plus the CLM sender (boot-only, runs before the iovar, so it stays in
12-byte form — verify ordering: put the iovar stage after
`SDIO_RUNTIME_STAGE_CLM_DOWNLOAD`, e.g. beside `STAGE_SET_MAC` in the enum at
`sdio.c:242–268`).

### 5.3 Where glom assembly fits the polled main loop (no threads, no timers)

Batching source = frames that are **already waiting**, never a delay waiting
for a partner — latency is bounded at zero added:

- `wifi_lwip_tx_hold_flush` (`wifi_lwip.c:366`) becomes the glom assembler:
  instead of popping one slot per `sdio_runtime_send_ethernet_frame` call, it
  pops up to `min(s_tx_count, credit depth, SDPCM_TXGLOM_MAX)` non-stale
  slots and hands them to `sdio_runtime_send_ethernet_frames`. Ordering,
  age-out, and RX-paced retry logic all stay exactly as they are.
- `wifi_lwip_link_output` (`wifi_lwip.c:462`): when glom is active and the
  hold queue is non-empty, **always enqueue** (today's "queue not empty"
  branch already does this, `wifi_lwip.c:490–498`) so the flush can coalesce;
  when the queue is empty, send direct as today — a lone frame gains nothing
  from the queue detour. lwIP's `tcp_output` emits a window's worth of
  segments back-to-back within one call chain, and the service pass flushes
  right after each drain (`wifi_lwip.c:1153`) and on non-drain passes
  (`wifi_lwip.c:1173`) — so TCP bursts coalesce naturally with zero added
  wait. The only sizing change worth considering later: if benches show
  bursts longer than the queue, raise `WIFI_LWIP_TX_QUEUE_DEPTH` — but not in
  the first landing.
- Latency bound: a frame is glommed only with frames queued in the same
  service pass; the pass budget is 1.2 ms (`wifi_lwip.c:75`). No glom timer
  exists or is needed.

### 5.4 Capability probing, fallback, guards

Bring-up (new stage, after CLM download):

1. `WLC_SET_VAR "bus:txglomalign" = 4` — advisory; ignore failure.
2. `WLC_SET_VAR "bus:txglom" = 0` — pin device→host glom off. If this
   **fails**, abort glom enablement entirely (leave 12-byte mode): we cannot
   parse RX superframes.
3. `WLC_SET_VAR "bus:rxglom" = 1` — on CDC status 0 → `g_sdpcm_txglom_active
   = true`, switch headers; on error → stay exactly as today (this is
   brcmfmac's own tolerated-failure path, so old firmware degrades cleanly).
4. Config-gated: a `wifi_txglom` config key (read via `config_get` as in
   `wifi.c:249`), default **off** for the first flashed builds, so the SD
   fallback kernel and the new kernel behave identically until proven.

Runtime guards:

- RX frame with channel 3 (`SDPCM_GLOM_CHANNEL`) or sw-header glom-desc bit:
  count it on `/status` and drop it. If ever nonzero, the `bus:txglom = 0`
  pin did not hold (**UNVERIFIED** case from §5.1) — treat as a blocker for
  keeping glom on; the counter is the tripwire.
- `/status` row: `TX glom: off | 1 | N — supers X / subs Y / fallbacks Z`.
- Any superframe CMD53 failure → increment `g_txglom_fallbacks`; three in a
  session → stop glomming until reboot (belt-and-braces; the credit system
  needs no such cap, this is for unknown-unknowns).

### 5.5 Staged rollout

- **Stage A — glom-of-1**: iovars + 20-byte headers everywhere, every data
  frame sent as a single-subframe superframe. No batching yet. Proves the
  header/padding/tag arithmetic against real firmware with minimal moving
  parts. Validate: join, DHCP, ping, `/bench.bin` parity with Phase 0
  (expect ≈ equal; the extra 8 bytes and block padding are noise), 30-min
  soak, resyncs = 0.
- **Stage B — glom-of-2**: flush pops up to 2. Validate: bench uplift (§5.6
  predicts ~+1 MB/s even at N=2), ping loss during download ≤ Phase 0,
  `stale` not worse, soak.
- **Stage C — glom-of-4** (raise `SDPCM_TXGLOM_MAX`): measure the knee; the
  §3.1 depth histogram says whether credits even allow 4. If the chip's
  window rarely reaches 4, stop at the measured depth.
- **Stage D — tuning**: credit reserve for control frames (brcmfmac reserves
  2, `TXCTL_CREDITS`, brcmfmac `sdio.c:664,677–688` — consider reserving 1–2
  so RSSI/PM polls never wait behind a data glom), queue depth, and only then
  consider RX-side work (the ACK peek+body pair, §1.5) as a separate plan.
- Build `rpi` only during the edit loop; nothing here touches CP15/asm, so no
  A53 boot-test trigger — but the release checklist still applies before any
  SD-kernel promotion (untested kernels never go to SD).

### 5.6 Expected win (arithmetic)

From §1.7: per-frame cost ≈ 456 µs at the C ≈ 3.2 MB/s asymptote, of which
≈ 61 µs is TX wire time and ≈ 390 µs is fixed per-CMD53/turnaround overhead.
Glom of N amortizes the fixed part:

| N | per-frame ≈ 390/N + 66 µs (wire+hdr/pad) | implied C for 1460 B/frame |
|---|---|---|
| 1 (today) | 456 µs | 3.2 MB/s |
| 2 | 261 µs | 5.6 MB/s |
| 4 | 164 µs | 8.9 MB/s |

These are TX-side bounds, not predictions: the RX/ACK path (two CMD53s per
ACK), lwIP processing, and the ~30 Mbit/s practical MCS7 goodput ceiling all
bind before 8.9 — Linux with N up to 32 lands at 4.25 MB/s, which is the
realistic asymptote for this hardware. Expected outcome: **N = 2 lifts the
bench from ~2.3 to ≥ 3 MB/s; N = 4 approaches ~4 MB/s**, at which point the
remaining gap is on the RX path, not TX. The `T0 ≈ 6.6 ms` fixed
per-transfer term is untouched by all of this — on an 8 MB bench it is 0.2%
and can be ignored; it only matters for small-file workloads.

Caveat honestly stated: the 390 µs decomposition assumes the fixed cost is
per-CMD53, not per-frame-regardless-of-batching (e.g. if most of it were
credit-grant latency per sequence number, glom would amortize less). The
§3.1/§3.2 instrumentation exists precisely to split that before Stage B is
judged: if the depth histogram shows the window opening ≥ 4 at a time,
the cost is host-side turnaround and the table stands.

---

## 6. Execution order, effort, abort criteria

| # | Work | Effort | Abort / gate |
|---|---|---|---|
| 0 | Clean-RF re-bench of `d4fe2d7` queue (§2) | 0.5 session | Gate only: establishes the baseline number every later A/B compares against; blocked on a clean-RF day (link rate ≥ 65 Mbit, gateway pings clean) |
| 1 | Instrumentation: credit-depth + reopen histograms + UDP blast (§3) | 1 session | Abort a diff if it measurably moves the bench (it must not); counters ship permanently, blast rig can stay |
| 2 | Validation of the landed correctness fixes + ladder round trip (§4) | 1 session (hardware) | **Hard gate for Phase 3**: if an induced wedge does not recover through the ladder, fix that first — txglom multiplies sequence-space exposure |
| 3A | txglom stage A: iovars + glom-of-1 headers, config-gated off by default (§5.5) | 1–2 sessions | Abort if `bus:rxglom` rejected (stay as-is, plan ends — **that is a clean outcome, not a failure**), if the channel-3 tripwire fires, or if bench parity is lost |
| 3B | glom-of-2 via hold-queue flush | 1 session | Abort back to stage A if throughput does not improve ≥ +0.4 MB/s or ping loss/stale worsens; re-examine with §3 data |
| 3C | glom-of-4 + knee measurement | 0.5–1 session | Stop at the credit-depth the chip actually grants |
| 3D | Tuning; scope an RX-path plan if ≥ ~4 MB/s TX-side is reached | open | — |

Standing rules that bind all phases: never modify `vidcore/Pi1MHzvc.s`; no
new log calls; flash only a settled Pi; never persist an untested kernel to
SD; user pushes to GitHub manually; after hardware tests, report state —
don't tidy and reboot.
