# WiFi txglom branch — hardware validation runbook

> **STATUS 2026-09-02: historical.** This was written before the branch ran
> on hardware. It has since been run: the implementation landed
> (`wifi_txglom` in Pi1MHz.cfg, the TX glom batch histogram on `/status`) and
> the investigation is closed - glomming works, throughput did not move, and
> the governor turned out to be the dongle's credit-grant cadence. The
> conclusion, with the numbers, is in
> `docs/dev/wifi-throughput-plan.md`. Kept for the procedure below, which is
> still how you validate a WiFi change on this hardware.

What to run on the target Pi at each checkpoint for this branch (the
implementation of `docs/dev/wifi-throughput-plan.md`).

Standing rules that bind every checkpoint (from the project memory):

- Flash only a settled Pi: wait for `/status` + 30 s after boot before
  `kernel.now`.
- Never persist an untested kernel to SD; keep the SD fallback kernel
  same-lineage as the flashed one (release chains to release).
- After a hardware test, leave the state as it is and report it.
- All benches: `pi-http.sh` against `/bench.bin` (RAM-sourced), three runs
  minimum, same-day A/B only.

Config keys added by this branch (`Pi1MHz.cfg`):

| key | default | meaning |
|---|---|---|
| `wifi_diag=1` | off | credit-window depth + shut→reopen histograms, batch-size histogram, frames-per-credit-refill on `/status` |
| `wifi_txglom=N` | 0 (off) | TX superframe limit: 1 = glom-of-1 headers only, ≥2 = batching; clamped to 16 |

With both keys absent the TX path is byte-identical to the pre-branch
driver — that is the safe first deployment.

---

## Checkpoint 0 — clean-RF baseline re-bench (plan §2, no new code needed)

The `d4fe2d7` hold queue's predicted gain has never been measured on clean
RF.  Do this before judging any glom number, or every later A/B is against
an unknown.

1. Clean-RF gate (ALL must hold, else abort the bench day):
   - `/status` "Link rate (TX)" ≥ 65 Mbit/s (re-query until populated);
   - RSSI near the −34 dBm reference, not tens of dB worse;
   - ping the GATEWAY from the same host: ~0 % loss.
2. Record `/status` before-values: "TX path" (queued/stale/fail/max wait),
   "Frames sent/received", "Transmit resyncs", "Rejoins",
   "RX interrupt gate" MISSED, chip pktcnts (`tx_bad`).
3. Bench ×3: `pi-http.sh -s -o /dev/null -w 'bytes=%{size_download} secs=%{time_total} rate=%{speed_download}\n' /bench.bin`
4. Re-read `/status`; record deltas.  Then the concurrency check: two
   parallel `/bench.bin` fetches + 12 pings (reference: 0/12 loss).

Interpretation (plan §2): ≥ 2.6 MB/s sustained = the queue's gain is real,
use as the Phase-3 baseline.  ≈ 2.2–2.4 MB/s with large `queued` delta and
small `stale` = fixed per-frame cost dominates, txglom confirmed as the
lever.

## Checkpoint 1 — instrumentation shakedown (commit "Phase 1")

Deploy: this branch, release build, `wifi_diag=1`, `wifi_txglom` ABSENT.

1. Confirm `/status` grows exactly three optional rows: "Credit depth
   (RX refresh)", "Credit reopen latency" (only with `wifi_diag=1`) and
   "UDP blast" (only after a blast).  Without `wifi_diag` the page must be
   unchanged.
2. Bench parity vs checkpoint 0: within ±0.1 MB/s.  If it moved more,
   suspect the measurement, not the counters, and re-run (plan §3 abort
   note).
3. During a `/bench.bin` download, snapshot "Credit depth": the histogram
   answers *how many credits the chip grants at a time*.  Mass at ≥ 4 =
   glom-of-4 is credit-feasible; mass at 1–2 = stop at wifi_txglom=2.
4. "Credit reopen latency": mass < 500 µs = ACK-paced refills (glom will
   help); mass ≥ 5 ms = chip-side slow crediting (glom still helps, but
   revise the §5.6 arithmetic down).
5. UDP blast: `iperf -s -u -p 5001 -i 1` on the PC, then
   `pi-http.sh '/udpblast?host=<pc-ip>&port=5001&mb=8'`.  The receiver's
   rate is pure driver+SDPCM capacity (no TCP).  Record it; it is the
   TX-side ceiling every glom stage is compared against.
6. "TX data-phase fails" row should stay absent (it only appears nonzero).

## Checkpoint 2 — correctness gate (plan §4; HARD GATE for glom-on)

No new code — this validates the already-landed reclaim/ladder fixes that
glom multiplies the exposure of.

1. 30 min soak: UDP blast + two concurrent `/bench.bin` downloads.
   Require: "Transmit resyncs" 0, "Rejoins" 0, "TX data-phase fails"
   absent or, if nonzero, TX demonstrably alive afterwards (reopen
   histogram, not the ≥ 20 ms bucket).
2. Ladder round trip: induce a wedge (degrade RF mid-download or
   power-cycle the AP) and confirm rejoin at ~8 s TX-dead, full chip
   restart at 25 s / three failed rejoins, traffic resumes.
3. If the induced wedge does NOT recover through the ladder: STOP — do not
   enable `wifi_txglom` until that is fixed.

## Checkpoint 3A — glom-of-1 (commit "Phase 3A")

Deploy: `wifi_txglom=1` (plus `wifi_diag=1`).  SD fallback kernel stays a
`wifi_txglom`-less config so a failed boot falls back to proven behaviour.

1. Boot with `wifi_debug=1` once: expect
   `== STAGE_TXGLOM: negotiating TX glom (wifi_txglom=1) ==` then
   `TXGLOM: bus:rxglom accepted - glom headers active, limit 1`.
   `/status` "TX glom" row: `active, limit 1`.
   - `bus:rxglom refused` → clean outcome, THE PLAN ENDS HERE (plan §6
     row 3A); remove the key and report.
   - `bus:txglom=0 pin NOT confirmed` → same: glom stays off by design
     (this is the UNVERIFIED-iovar guard).
2. Join, DHCP, ping, web UI all work — every TX frame (control included)
   changed shape, so basic connectivity IS the test.
3. `ch3` on the "TX glom" row must stay 0.  Nonzero = the device→host pin
   did not hold ("TRIPWIRE!" appears): abort glom, remove the key, report.
4. Bench ×3: expect ≈ parity with checkpoint 0 (the 8 extra header bytes
   and padding are noise).  Parity lost → abort to `wifi_txglom=0`.
5. 30 min soak: resyncs 0, rejoins 0, no stale growth.
6. Rejoin/restart exercise: power-cycle the AP briefly; after recovery
   confirm the "TX glom" row again (a full chip restart must renegotiate
   and re-activate; a mere rejoin must not lose glom).

## Checkpoint 3B — glom-of-2 (commit "Phase 3B/C")

Deploy: `wifi_txglom=2`.

1. Bench ×3 vs the checkpoint-0/3A baseline.  Gate: ≥ +0.4 MB/s
   improvement (plan §6 row 3B; §5.6 predicts toward ~3 MB/s).  Less →
   fall back to `wifi_txglom=1` and re-examine with the checkpoint-1
   histograms.
2. `/status` "TX glom": `supers`/`subs` climbing during the bench, ratio
   approaching 2 subs/super under load; `fallbacks` 0.
3. Ping loss during a download ≤ checkpoint 0's concurrency reference;
   "TX path" `stale` not worse (the stale aggregate now also contains
   batch-loss drops — any jump here is a superframe failure, check
   `fallbacks`).
4. 30 min soak: resyncs 0, rejoins 0, fallbacks 0.

## Checkpoint 3C — glom-of-4 knee

Deploy: `wifi_txglom=4`.

1. Bench ×3: measure the knee (§5.6 bounds: N=4 approaches ~4 MB/s
   TX-side; Linux lands at 4.25).
2. Compare against the checkpoint-1 credit-depth histogram: if the window
   rarely opens ≥ 4, the bench will show why — stop at the depth the chip
   actually grants and leave `wifi_txglom` there.
3. Soak as 3B.  Any session reaching `fallbacks` ≥ 3 silently stops
   batching (limit collapses to 1) — that state on `/status` after a soak
   is itself a finding to report.

---

## Deviations from the plan (implementation notes)

1. **The `bus:txglom = 0` pin already existed**: HEAD's join burst sends
   `TXGLOM_OFF` on every join (plan §5.1 believed no glom iovar had ever
   been written).  The new TXGLOM stage still sends it with a *confirmed*
   status-0 ack before enabling anything; the join burst simply re-asserts
   it later.
2. **Pin confirmation is stricter than the plan's wording**: plan §5.4
   aborts when the pin write "fails"; this branch also aborts when the ack
   never arrives (no-ack is indistinguishable from unsupported), so an
   unresponsive firmware can never end up glom-active.
3. **Instrumentation is config-gated** (`wifi_diag`), not always-on as the
   plan's §3 diffs read — per the standing no-cost-in-release rule.  The
   data-phase-fail counter is the exception (failure-path only, always
   counted).
4. **Multi-frame API shape**: `sdio_runtime_send_ethernet_frames(frames[],
   lens[], n)` with an int8 contract (>0 sent / 0 retryable refusal /
   <0 must-drop count) instead of the plan's slot-array or
   begin/add/commit sketch — wifi_lwip.c keeps its slot type private.
5. **Small-frame padding**: a glom frame ≤ 512 bytes pads to 4-byte
   alignment and goes byte-mode (brcmfmac's roundup rule) rather than
   always padding to a 512 block; the tag equals the transfer length in
   both cases.
6. **One binary for every stage**: `SDIO_RUNTIME_TXGLOM_MAX` (originally 4,
   raised to 16 after the glom-of-4 bench, see below) bounds the config
   value, so the checkpoints differ only in `wifi_txglom`.

---

## Iteration 2 — results so far, and the credit-cadence reframe

Hardware results (main session, this branch at `bde3844`):

| checkpoint | result |
|---|---|
| CP1 (diag on, glom off) | parity OK.  Credit depth at RX refresh: typically **4–7**, **16+ common**, 0 at ~15 % of refreshes.  Reopen latency mostly **< 1 ms**. |
| CP3A (glom=1) | parity PASS, negotiation clean, fallbacks 0, ch3 0. |
| CP3C (glom=4) | supers 3031 / subs 11004 (**avg 3.63 frames/CMD53**), fallbacks 0, TX stable (24k good / 2 bad) — but throughput **FLAT** vs glom=1 and glom=0 (~2.2–2.4 MB/s TCP). |
| UDP blast (glom=4) | **2.88 MB/s**, ~2 % loss. |

Reconciliation: 2.88 MB/s ÷ 1472 B ≈ 1957 datagrams/s ≈ 540 superframes/s
× 3.63 subframes — the numbers agree with each other.  CMD53 count fell
3.6× with zero throughput change, so per-CMD53 setup cost is NOT the
binding constraint (the plan's §5.6 table is refuted in its mechanism,
though not in its remedy).  The governor is the **credit-refill cadence**:
the chip advances `max_seq` ~540 times/s, and each refill round trip
carried only ~3.6 frames because `wifi_txglom=4` capped the batch.

### Saturation arithmetic

Throughput ≈ R × D × payload, with R = credit-refill cadence and
D = frames carried per refill:

    R ≈ 540 refills/s          (observed, glom=4)
    payload = 1460 B (TCP MSS) / 1472 B (UDP)

| D (frames/refill) | rate at R = 540/s | note |
|---|---|---|
| 3.63 | 2.86 MB/s | observed (UDP 2.88 — the model fits) |
| 4 | 3.15 MB/s | glom-4's ceiling, hence the flat bench |
| **5.4** | **4.25 MB/s** | the Linux/brcmfmac reference = the radio's practical ceiling |
| 8 | 6.31 MB/s | already past what MCS7 goodput allows |
| 16 | 12.6 MB/s | irrelevant — radio binds long before |

So the arithmetic says **D ≈ 5.4 saturates the radio** at the observed
cadence; glom-16 is headroom, not a requirement.  Feasibility against the
CP1 credit histogram: grants are typically 4–7 (upper end ≈ the needed
5.4) with 16+ common, so batches of 6–8 should form regularly once the
limit and the queue allow them — there is **no structural blocker** from
grant size.  The two ways this still fails, which the new "TX glom
batches" row will distinguish:

1. Batches stay pinned at 4–7 and throughput rises to ~3.2–4 MB/s and
   stops: grant size × cadence binds → tune nothing further on TX; the
   remaining gap is the plan's stage-3D/RX-path territory.
2. Frames/refill rises to 6+ but throughput stays ~2.9: the refill
   cadence R dropped as batches deepened (the chip credits per drain, not
   per wall-clock) → TX-side glom is exhausted as a lever; write that
   verdict down and move to the RX path (ACK peek+body pair) per plan
   §5.5 stage D.

## Checkpoint 3D — glom-16 with the deeper queue (commits after `bde3844`)

Deploy: this branch tip, `wifi_txglom=16`, `wifi_diag=1`.

1. Sanity as CP3A: negotiation `active, limit 16`, fallbacks 0, ch3 0,
   ping/DHCP fine.  (Hold queue is now 32 deep, TCP cap 28 — watch ping
   loss during a download: the 4-slot no-second-chance reserve is
   unchanged, so it should match earlier checkpoints.)
2. Bench ×3 + UDP blast.  Read the new `/status` "TX glom batches" row:
   - buckets `8-15`/`16+` populated and frames/refill ≥ ~5.4 → expect the
     bench at ~4 MB/s (radio-bound); SUCCESS, tune `wifi_txglom` down to
     the knee (smallest value that holds the rate) for the final config.
   - outcome 1 or 2 from the table above → record the numbers in this
     file; TX-side work is done either way.
3. 30 min soak at the chosen depth: resyncs 0, rejoins 0, fallbacks 0,
   stale not worse than CP0, "TX path" max wait not pathological (a
   16-frame batch admits ~9 ms of wire+turnaround ahead of the last
   frame — expect max wait to rise a little; the 250 ms age-out is far
   away).

## Checkpoint 3D RESULT (2026-08-26, hardware)
Batches pin at 4-7 (16+: zero) with limit 16, queue 32, and 16+ credit
grants common. Frames/refill 4.12. TCP 1.94-2.18, UDP 2.27-2.88.
Per the decision matrix: TX glom is EXHAUSTED as a lever - the binding
constraint is queue occupancy at flush time (producer feeds ~4 frames per
service pass), not credits, not CMD53 count. Next levers: producer-side
pacing (frames enqueued per poll pass by lwIP/the blast generator) and
the RX/ACK service path. The glom implementation itself is safe and clean
(zero fallbacks, zero ch3, zero stale, chip tx errors 2 in 40k+).

---

## Iteration 3 — producer pacing and the RX/ACK path

### What the per-pass limits actually are (read from the code)

- **The webserver is NOT the TCP cap.**  `conn_pump`
  (`src/wifi/webserver.c:1864`) fills `tcp_sndbuf()` completely on every
  call via `ws_write_best_effort` (`webserver.c:1850`, ERR_MEM retry at
  one MSS), then calls `tcp_output`.  It runs from `ws_sent`
  (`webserver.c:6290`) on every ACK.  TCP production is therefore
  ACK-clocked: ~2 segments per delayed ACK, i.e. per-pass feed =
  (ACKs drained that pass) × 2.
- **The RX drain was never single-frame.**  `wifi_lwip_drain_rx_frames`
  consumes up to a slice of frames per call (`WIFI_LWIP_RX_FRAME_BUDGET`,
  `src/wifi/wifi_lwip.c:54`, was 8 now 16), and the service pass repeats
  drain+flush until the FIFO is empty (`wifi_lwip.c:1494`) inside the
  1.2 ms wall-clock budget (`wifi_lwip.c:80`).  The slice size is really
  the **flush cadence**.
- **The real 4-per-batch mechanism**: the direct-send path peeled off
  every frame the open credit window would take as its own glom-of-1
  CMD53; batches formed only from credit-shut leftovers, so batch size
  mirrored credit depth (4–7), never offered load.
- **/udpblast fed 4/pass by construction** (fixed burst, now
  `wifi_lwip.c:623` configurable).

### What changed (one commit each)

1. Per-pass feed histograms (TCP / udpblast / other) + RX drained/pass,
   `wifi_diag`-gated; compile-gated `WIFI_LWIP_RX_PROFILE` ccnt profiler
   (ARM1176 only, default off).
2. `/udpblast?...&burst=N` (1–16, default 4 = old behaviour).
3. **Coalescing feed** (`wifi_lwip.c:779`): with glom batching negotiated
   (limit ≥ 2), IPv4 TCP/UDP frames always ride the hold queue and leave
   as one superframe at the pass's flush points; ARP/ICMP keep the direct
   path and the 4-slot reserve.  Off (byte-identical path) whenever
   `wifi_txglom` < 2.
4. RX drain slice 8 → 16 so one slice's ACK run can feed a full 16-batch.

### RX glom (2c): design sketch, deliberately NOT implemented

Unpacking device→host superframes would cut the 2-CMD53s-per-RX-frame
cost (hwtag peek + body): at ~800 ACKs/s that is ~1600 CMD53s/s of pure
overhead, roughly 10 % of the send budget.  It requires: `bus:txglom = 1`
instead of the current 0-pin, parsing channel-3 glom descriptors, walking
subframe hw tags inside one block-mode read, and a deglom buffer
(~8–16 KB).  Do this only if Checkpoint 4 shows TX saturated while the
per-frame RX cost (WIFI_LWIP_RX_PROFILE) is a measurable share of the
pass — it trades the proven "one frame per read" invariant for bus
efficiency, and every RX parser assumption (12-byte headers, one frame
per transfer) changes shape.  Not before the TX side is proven at 4 MB/s.

## Checkpoint 4 — producer pacing + coalescing feed (branch tip)

Deploy: branch tip, release build, `wifi_diag=1`, `wifi_txglom=16`.
SD fallback stays a `wifi_txglom`-less kernel.

1. Sanity: negotiation `active, limit 16`; ping/DHCP/web fine; fallbacks
   0, ch3 0.  DHCP note: with coalescing on, DHCP (UDP) rides the queue
   and the bulk cap - confirm a lease renewal still lands (or just that
   the Pi keeps its address across the soak).
2. UDP pipeline test, stepping the offered load:
   `/udpblast?host=<pc>&port=5001&mb=8&burst=4`, then `burst=8`, then
   `burst=16` (receiver: `iperf -s -u -p 5001 -i 1`).  Expected
   signatures per step:
   - "TX feed/pass (udpblast)" mass moves 4-7 → 8-15 → 16+;
   - "TX glom batches" 8-15/16+ buckets populate (they were ZERO at 3D);
   - "TX glom" subs/supers ratio climbs toward burst;
   - receiver rate: burst=4 ≈ 2.9 (parity with 3D), burst=8 should move
     toward ≥ 3.5, burst=16 toward ~4+ MB/s if the cadence model holds
     (D ≈ 5.4 saturates the radio; see Iteration 2 arithmetic).
   - If rate stays ~2.9 while batches read 8+: the refill cadence dropped
     with depth - chip-side pacing governs; record and stop TX work.
3. TCP bench ×3 (`/bench.bin`): with the coalescing feed + 16-frame
   slices, "TX feed/pass (TCP)" should show mass at 4-7/8-15 (was ~4
   hard cap) and "TX glom batches" mass above 4.  Expect ≥ +0.4 MB/s vs
   the 2.2-2.4 plateau if ACK supply per pass allows; if TCP stays flat
   while UDP moved, the residual gap is ACK-delivery cadence (RX path),
   not the producer - that plus the RX-cost profile is the case for the
   RX plan, not more TX work.
4. Latency guard: 12 pings during a download - loss must not exceed the
   CP0 concurrency reference (the coalescing feed adds ≤ one flush point
   of delay to bulk only; ARP/ICMP path untouched).  "TX path" max wait
   may rise (deeper queue) but stale must stay ~0 during a clean run.
5. Soak 30 min mixed (blast burst=16 + two TCP downloads): resyncs 0,
   rejoins 0, fallbacks 0, stale ~0, no DHCP loss.
6. Abort criteria: ping loss above the CP0 reference, stale growth
   during a single-stream download, fallbacks > 0, or any wedge ->
   revert to `wifi_txglom=4` (the proven 3D config) and report; the
   coalescing feed disables itself with the batch limit, so
   `wifi_txglom=1` is the full behavioural fallback short of 0.

## Checkpoint 4 RESULT (2026-08-26, hardware)
Coalescing feed VERIFIED: batch-1 sends 12.7k -> 123; batches 4-7 dominate.
Burst ladder 4/8/16: rate flat 2.59-2.63 MB/s; udpblast feed/pass stays 4-7
even at burst=16 because queue space frees only at the drain rate - the
pipeline self-clocks to the credit grant cadence. Model closes: ~495
refills/s x 3.87 frames/refill = ~1900 frames/s = 2.7 MB/s observed.
VERDICT: every host-side lever is lifted and verified; the governor is the
dongle's credit-grant cadence (~4-5 credits ~500x/s). Remaining research
question (needs Linux-side comparison capture): does brcmfmac see deeper
or faster grants (interrupt-latency closed loop / different firmware
flow-control settings), or does it win purely on the same grants serviced
with lower latency? Candidate probes: sniff grant sizes under Linux on
identical hardware; investigate firmware flow-control iovars.

---

## Iteration 4 — the grant-cadence probes (Checkpoint 5)

Checkpoint 4's verdict: the pipeline self-clocks to the dongle's
credit-grant cadence (~4-5 credits, ~500 grants/s ≈ 2.9-3.6 MB/s hard
ceiling).  These probes attack the grant SOURCE.

### What brcmfmac does (and does not) configure

- Mainline brcmfmac (`brcmf_sdio_bus_preinit`, tree pinned in
  `docs/dev/wifi-throughput-plan.md`) sets ONLY `bus:txglom` /
  `bus:txglomalign` / `bus:rxglom` — the trio this branch already
  negotiates.  It has **no credit-behaviour iovars**: whatever grant
  cadence Linux gets, it gets from firmware defaults plus its F2
  watermark programming (below).
- `bus:credall` and `bus:maxtxpktglom` are **vendor-DHD** (bcmdhd) knobs,
  not mainline: credall is set there to reduce the chance of running out
  of bus credit (plausibly "grant credits for all buffered packets");
  maxtxpktglom tells the dongle the host's max TX glom size.  Attribution
  is from the bcmdhd tree, not re-verified locally — hence probes, not
  defaults.
- F2 watermark: our `sdio_runtime_complete_boot_stage` writes 8
  (`src/wifi/sdio.c`, register 0x10008) = cyw43-driver = brcmfmac's
  DEFAULT_F2_WATERMARK path.  brcmfmac's 43455 case instead programs
  watermark 0x60 (96), DEVCTL F2WM enable (0x10009 bit 0x10) and
  MESBUSYCTRL 0x1001D = 0x50 | 0x80.  That is the one real divergence
  from brcmfmac's 43455 init, and a deeper FIFO watermark plausibly
  changes when the dongle surfaces credit updates.

### Rejected: the immediate-drain tweak (probe 3c)

Already the current behaviour, so nothing to implement: the DAT1 gate
costs one MMIO read per main-loop pass (`src/wifi/sdio.c:7531`), the RX
backoff is zero whenever the gate is armed (`src/wifi/wifi_lwip.c:1511`),
and the service pass drains until the FIFO is empty
(`src/wifi/wifi_lwip.c:1494`).  The new "RX gate service latency"
histogram proves it with numbers instead of asserting it.

### New keys (all default off = byte-identical behaviour)

| key | effect | A/B value |
|---|---|---|
| `wifi_iovar_credall=1` | WLC_SET_VAR `bus:credall` = 1 | 1 |
| `wifi_iovar_maxtxpktglom=N` | WLC_SET_VAR `bus:maxtxpktglom` = N | 16 |
| `wifi_iovar_extra=name=value` | one arbitrary u32 iovar (0x hex ok) | — |
| `wifi_f2wm=N` | F2 watermark override (stock 8) | 96 |
| `wifi_mesbusy=N` | MESBUSYCTRL = N \| enable, + DEVCTL F2WM | 80 |

Ack/outcome of every iovar probe is on the /status "Cfg iovars" row
(OK / ERR(status) / NO ACK / SEND FAIL); the F2 override is echoed on
the "F2 tuning" row.  All are applied at the TXGLOM bring-up stage (the
iovars run even with `wifi_txglom=0`) and re-applied after a chip
restart.

## Checkpoint 5 — deploy matrix

Base config every run: `wifi_diag=1`, `wifi_txglom=16` (so the coalesced
pipeline from Checkpoint 4 is the load generator).  Bench = 3×
`/bench.bin` + one `/udpblast?...&burst=16`; read these /status rows
before/after each: "Grant interval", "Credit depth", "TX glom batches",
"Cfg iovars", "RX gate service latency".

0. **Baseline re-read** (no new keys): confirm "RX gate service latency"
   mass sits <100us and "Grant interval" mass at <2ms/<5ms (~500/s).
   If gate latency were a visible fraction of the grant interval the
   host would be the laggard — not expected given the gate design; if it
   IS, stop and report before touching iovars.
1. `wifi_iovar_credall=1` alone.  Signature of success: "Credit depth"
   mass shifts right (grants > 4-7 typical), UDP blast rate rises above
   2.9; "Grant interval" may stay put (bigger grants) or tighten (more
   grants) — either helps.  ERR/NO ACK on the row = firmware doesn't
   know the iovar: record and drop the key.
2. `wifi_iovar_maxtxpktglom=16` alone, then combined with credall if
   both individually accept.  Same signatures.
3. `wifi_f2wm=96` alone.  Signature: "Grant interval" tightening
   (refills surfaced sooner), RX gate asserting more often with the
   same traffic; watch "RX interrupt gate" MISSED stays ~0.
4. `wifi_f2wm=96` + `wifi_mesbusy=80` (the full brcmfmac 43455 recipe).
   The mesbusy write is unverified against this dongle: if the link
   fails to come up or TX wedges, remove the key — that is the abort,
   and the result ("43455 MES recipe not applicable") is still a
   finding.
5. Best combination soak 30 min: resyncs 0, rejoins 0, fallbacks 0,
   ch3 0, ping loss ≤ CP0 reference.

Abort criteria per step: link fails to associate, "Cfg iovars" shows
ERR/NO ACK (drop that key, continue with the rest), any TX wedge or
resync/rejoin growth, or a bench drop > 0.2 MB/s vs the same-day
baseline.  Every key is independent — the fallback is always "remove
the key", and with all keys absent the driver is byte-identical to
Checkpoint 4's binary behaviour.

Decision rule: if none of the five steps moves "Credit depth" or
"Grant interval", the grant cadence is a firmware-internal policy this
host cannot reach by configuration — the TX investigation ends, and the
remaining levers are the RX-side plan (fewer CMD53s per ACK) and, at
the horizon, RX glom (design sketch in Iteration 3).

## Checkpoint 5 RESULT (2026-08-26, hardware) - INVESTIGATION CLOSED
All probes accepted by the dongle; none moved the grant histograms:
baseline 2.71 / credall 2.80 / +maxtxpktglom=16 2.27 (harmful - batches
fragment, do not enable) / f2wm=96 2.47 / +mesbusy=80 2.57 MB/s, with
refill rate, frames-per-refill (3.74) and grant intervals (1-2ms) identical
throughout. Decision rule met: the credit-grant cadence is firmware-internal
policy. TX-side ceiling on this polled host: ~2.7-2.9 MB/s UDP, 2.2-2.4 TCP.
Remaining levers, in order: (1) RX-side service cost (lifts TCP toward the
UDP ceiling; RX-glom sketch above), (2) Linux-side grant-pattern capture
(answers whether ANY host can do better with this firmware), (3) accept.
Recommended merge subset if the branch is adopted: instrumentation +
coalescing feed + glom (all config-gated, benign); skip credall/maxtxpktglom.
