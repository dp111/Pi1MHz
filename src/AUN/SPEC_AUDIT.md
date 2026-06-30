# AUNFS + Pi1MHz AUN — specification conformance audit

Audit of the complete AUN path on both sides — the 6502 **AUNFS** ROM patch
(ANFS 4.21/4.18) and the **Pi1MHz** C engine + glue — against the published
specification, with fixes applied inline.

## Reference sources

- **AUN transport**: RISC OS PRM Vol 5a, Ch. 122 *AUN*
  (`http://www.riscos.com/support/developers/prm/aun.html`). The authoritative
  description of AUN-over-IP: addressing, the two-way handshake, the
  retransmission strategy, immediate-operation limits, and duplicate handling.
- **AUN wire format / peer behaviour**: BeebEm `Src/Econet.cpp` (the reference
  AUN peer this deployment talks to) — `AUNType` enum, `AUNHeader`, and the
  four-way state machine.
- **Fileserver / four-way**: the Econet four-way handshake and Acorn fileserver
  reply protocol (command port &99, reply/data ports chosen by the client).

The Pi side: `AUN/aun.c`, `AUN/aun.h`, `AUN/aun_emulator.c`, `wifi/wifi_lwip.c`.
The ROM side: `pi1mhz-patch/eco_library.asm` (the foreground RX pump and the
transmit path that replaces the ADLC/NMI engine).

---

## 1. AUN transport conformance (Pi engine)

| Spec requirement (PRM / BeebEm) | Implementation | Verdict |
|---|---|---|
| UDP, default port 32768 | `AUN_DEFAULT_UDP_PORT 32768`, single PCB bound in `aun_emulator.c` | ✅ |
| Frame types: broadcast 1, data 2, ack 3, nak/reject 4, immediate 5, imm-reply 6 | `AUN_TYPE_*` identical; matches BeebEm `AUNType` exactly | ✅ |
| 8-byte header: type, port, control, pad, 4-byte LE sequence | `build_header()` / `aun_udp_input()`; pad=0 | ✅ |
| Control byte carried with **bit 7 cleared on the wire** | send: `ctrl & 0x7F`; deliver: `ctrl | 0x80` (matches BeebEm) | ✅ |
| Sequence advances by 4 per transaction; ack/nak **echo** it | `next_seq += 4`; ack/nak rebuild header with received `seq` | ✅ |
| Max 8192 data bytes per IP transmission | `AUN_MAX_DATA 8192`; lwIP IP reassembly on | ✅ |
| Failure returns only `NetError` / `NotListening` | `AUN_TX_NET_ERROR`, `AUN_TX_NOT_LISTENING` surfaced to the ROM | ✅ |
| Address = `1.network.net.station`, Class A, netmask &FFFF0000; station is the low dotted octet | explicit map is primary; learn-mode `learn_resolve`/`learn_attribute` place station in the high byte of the network-order word (= last dotted octet). Self-consistent and correct. | ✅ (see §5.4) |
| Only `MachinePeek` immediate supported over IP; others → `NotListening` | engine answers MachinePeek (`AUN_CTRL_MACHINE_PEEK`) itself; host-immediates are an explicit local extension | ✅ |
| Duplicate detection is the **application's** job (add a sequence number/bit); AUN re-ACKs a lost-ack retransmit | exact-`seq` retransmit → re-ACK, no re-deliver (`m->last_rx_seq`) | ✅ for seq; ⚠️ extra content heuristic, see §5.1 |
| **Retransmit — REJECT**: retransmit promptly, bounded | `AUN_REJECT_RETRIES = 10` on NAK | ✅ (see §5.2) |
| **Retransmit — SILENCE**: spec = background Net module waits ~5 s then retransmits | engine **fails after ~1 s, no retransmit**, lets ANFS NFS retry | ⚠️ deliberate reconciliation — **fixed**, see §4 |

Everything in the transport framing and addressing is spec-correct. The two ⚠️
rows are behavioural choices discussed below; one is now fixed.

---

## 2. Receive / four-way path

The fileserver four-way (scout → scout-ack → data → data-ack) collapses, per the
AUN model, to **Unicast(data) → ACK**. The engine ACKs **on receipt** (in
`aun_udp_input`, inside the lwIP receive callback) the instant a frame lands in
an open block — this is the equivalent of the ADLC/NMI acking into the RXCB, and
is fast enough to satisfy the peer's data-ack wait. This is correct: the AUN ACK
*is* the data-ack.

- **No-listener / queue-full / oversize → NAK** (`reject`) at receipt — spec's
  "reject message if there is currently no open receive block". ✅
- **Arm-race tolerance** (park-and-retry): a reply that arrives a beat before
  ANFS arms its receive control block is held out of the queue and re-presented,
  rather than dropped. This is an implementation refinement, not a spec
  requirement, and does not emit any extra wire packet. ✅
- **nIRQ** is level-driven on `rx[0].count` (`aun_irq_update`), so a queued
  frame keeps nIRQ asserted until ANFS drains it. Only `rx[0]` drives the IRQ —
  correct because ANFS funnels all inbound frames through the single wildcard
  block `h0` and the **ROM pump** demultiplexes to internal CBs by port/stn/net.

---

## 3. ROM pump & transmit contract

`eco_rx_pump` (foreground replacement for the NMI scout/data engine):

- `RX_POLL h0` → scans the same control-block lists the NMI scanned (the &00C0
  zero-page reply CB when `econet_flags` bit 7 is set, then the NFS workspace
  receive list when bit 6 is set), matching on port (0 = any), station (0 = any)
  and net (0 = any) — a faithful mirror of `scan_port_list`. ✅
- On match: capacity-checks the CB buffer (rejects oversize rather than
  overrunning Beeb memory), streams the payload from JIM &FE8000 to host or Tube,
  writes CB+8/9 = end pointer, CB+0..3 = `ctrl|&80, port, stn, net`, sets verdict
  0 (ACK), and fires the Econet receive event (&FE) with the original slot
  gating. ✅
- On no match / oversize: verdict 1 (reject) and `RX_DONE` pops the frame. With
  ACK-on-receipt this does **not** put a NAK on the wire — the Pi parks (new
  reply) or drops (duplicate). ✅
- `RX_DONE` re-arms the Pi block exactly once per frame; the drop path also pops,
  so the svc5 drain loop always terminates. ✅

Transmit (`eco_tx_begin`): builds the TX command at &FFE200, issues `ECO_TX`
(or `ECO_BCAST` for dest &FF), then **polls `TX_POLL` synchronously** in
`.etb_poll` until the engine result drops below &80, briefly reopening the IRQ
window each iteration. **There is no ROM-side timeout** — the loop blocks the
Beeb foreground until the engine completes. This is the key cross-side
constraint (§4).

The ROM↔engine command block layout (ctrl/port/stn/net at +2..+5, LE data
offset at +8, LE length at +12) matches what `aun_emulator.c` reads. ✅

---

## 4. FIX APPLIED — transmit timeout reconciled with the ROM's synchronous poll

**Finding.** The AUN spec's silence behaviour ("1 retransmission after a 5 s
timeout") assumes a *background* Net module: the application's SWI returns and
the module retransmits behind it. The AUNFS ROM is not built that way —
`.etb_poll` blocks the foreground until the engine finishes, with no timeout of
its own. So **the engine's timeout is literally the Beeb's freeze duration.** A
prior change set that timeout to the spec's 5 s; the result is that any single
lost command-ACK freezes the whole machine for 5 seconds. The ROM was designed
around a "~1 s" budget (its own comment), which the earlier 250 ms × 4 happened
to match.

**Fix** (`aun.h`, with `aun.c`/tests updated):

- `AUN_NORESP_TIMEOUT_MS 5000 → 1000` — fail after ~1 s of silence, matching
  the ROM's synchronous budget.
- `AUN_NORESP_RETRIES 2 → 0` — the engine does **not** retransmit on silence.
  It reports `NotListening` and lets ANFS's NFS layer retry the whole transmit,
  exactly as native Econet does on a missing scout-ack. This also guarantees the
  engine never emits a silence retransmit that could land mid-transaction on a
  strictly single-transaction peer (BeebEm) and abort its four-way — the failure
  mode that the original 250 ms retry caused.
- The **REJECT** (NAK) fast-retransmit path is unchanged and remains spec-correct
  (`AUN_REJECT_RETRIES = 10`, prompt).

Net effect: a lost command-ACK now costs ~1 s and an NFS-level retry instead of a
5 s foreground freeze. Host unit tests (19 cases, `-Wall -Wextra -Wconversion`)
pass.

---

## 5. Findings (status updated after the fix pass)

### 5.1 Content-based duplicate suppression — **FIXED (removed)**
The engine had, in addition to the spec-correct exact-`seq` re-ACK, a *content*
heuristic: a frame byte-identical to the previous frame on the same port that
was also still queued was re-ACKed and dropped. The PRM is explicit that
duplicate detection is the **application's** responsibility ("usually done by
adding a sequence number or bit"). With the **same file loaded repeatedly**
every data block is byte-identical, so the heuristic could silently drop a
legitimate, distinct data block under load (potential data loss) — the clash
flagged during development. **Fix:** the content heuristic (`rx_content_queued`
+ the `same_as_prev` REDUP branch) was removed. Only exact-`seq` retransmits
(a lost ACK) are now re-ACKed-without-redelivery; every new-sequence frame is
delivered, and ANFS's fileserver sequencing handles application-level
duplicates, per spec. `same_as_prev` no longer affects the verdict or the
park-and-retry gate at all (M4 removed that too); it survives ONLY as a flag
for the optional trace hook, so the whole previous-frame compare/copy is now
skipped entirely when no trace hook is installed (the field case). Tests
updated (case 19); suite green.

### 5.2 REJECT retransmit timing — **FIXED (10 ms spacing)**
Spec: on reject, retransmit after a 1-centisecond timeout, bounded. The engine
previously retransmitted immediately on NAK receipt. **Fix:** a NAK now
*schedules* the retransmit `AUN_REJECT_TIMEOUT_MS` (10 ms) later via the poll
timer (`reject_pending`), matching the spec's 1 cs spacing, still bounded at
`AUN_REJECT_RETRIES`. Test case 2 updated.

### 5.3 Single-funnel receive queue depth — **FIXED (deeper funnel, memory-neutral)**
All inbound frames share `rx[0]` (ANFS opens only handle 0). **Fix:** since only
the funnel block is used, the fixed frame budget was rebalanced from
`AUN_RX_BLOCKS 8 × AUN_RX_QUEUE 4` to `4 × 8` — the same 32 frame slots and
identical memory, but handle 0 now absorbs an 8-frame burst before it must NAK
at receipt. Confirmed safe: ANFS uses only handle 0, host tests use 0–1, and the
fuzz harness tolerates the smaller block count (ASan/UBSan soak clean).

### 5.4 Learn-mode netmask — **no change (the code is correct as-is)**
On review this is **not** a defect. The implementation uses the host interface's
real netmask (`netif_ip4_netmask`) for the subnet broadcast address and
learn-mode attribution — which is the correct behaviour for AUN running over a
real IP LAN. The PRM's /16 (&FFFF0000) describes AUN's *logical* Class-A station
addressing, not the IP transport netmask. Forcing /16 would compute the wrong
broadcast address on a /24 LAN (e.g. x.y.255.255 instead of x.y.z.255) and
mis-attribute out-of-subnet stations — a regression. Left unchanged
deliberately.

### 5.5 Immediate Count = 0/1 — **deferred (ROM SWI-veneer work, unrelated to the load path)**
Spec: an immediate (MachinePeek) with Count 0 or 1 returns `NotListening` with
no transmission. The gate belongs in ANFS's Econet SWI veneer (where the user's
Count lives), which is outside the transmit path this project patched; the AUN
engine is not told the Count. Implementing it means threading Count through the
immediate command block and risks the working machine-peek path, for a case
ANFS's own immediates do not hit (they use Count > 1) and that has no bearing on
the `*LOAD` issue. Deferred; can be done as a separate, isolated change on
request.

---

## 6. Open item — the per-load data-block re-send (needs peer-side data)

In the captured traces the **fileserver re-sends the same data block several
times within one load** (e.g. seq 684/688/692, each a fresh sequence — three
separate four-way attempts), and the attempt count grows across loads. The Pi
ACKs every attempt (`ackfail = 0`), so from our side each is handled; the peer
is simply not treating our first ACK as completing its send. Three mechanisms
fit, each with a different fix, and the captured Pi-side trace cannot
distinguish them because it has no timestamps:

1. **Timing miss** — our ACK lands after the peer's reply four-way window
   (gap between attempts ≈ a fixed timeout).
2. **State abort** — our ACK arrives while the peer is between four-way stages
   and is discarded (gap ≈ sub-millisecond, back-to-back).
3. **Queue-full NAK** (§5.3) under accumulated load.

Resolving this needs either BeebEm's Econet trace (`Set FWS_…` state lines) for
one re-sent block, or millisecond timestamps on the Pi `ECONET:` log so the
inter-attempt gap is visible. This is measurement, not a further speculative
change.

### 6b. Master (4.21) less reliable than Beeb (4.18) on repeated `*LOAD`
Reported observation: 4.18 on a BBC B is more reliable than 4.21 on a Master.
A direct diff of the two ROM patches shows the **pump and the entire ROM↔engine
contract are byte-identical**; the *only* divergence is the `svc5`
interrupt-claim entry, which is necessarily Master-specific (it saves X/Y
itself and does `lda #&80 : trb acccon` to release the ACCCON IRR mask before
draining, mirroring the stock 4.21 svc5). So the difference is isolated to
either (a) that Master svc5/ACCCON claim path or (b) Master hardware/timing
(shadow RAM, HAZEL paging at &C000–&DFFF, interrupt latency) — not the AUN
engine, which is shared. A focused audit of the 4.21 ROM (§8) then found a
concrete Master-specific defect in the patched pump. See §8.

---

## 8. Detailed 4.21 audit — Master shadow/HAZEL ACCCON gating missing in the pump (root-cause candidate for Master unreliability)

**Background — Master 128 memory paging.** On the Master, the &FE34 *ACCCON*
register controls which RAM bank `(zp),Y` addressing hits:
- bit 0 (D) / bit 1 (E): shadow screen display / VDU writes;
- bit 2 (X): &3000–&7FFF maps to **shadow** RAM for *all* access (not just VDU);
- bit 3 (Y): &C000–&DFFF maps to **HAZEL** filing-system RAM.

ANFS keeps its FS context and TXCB in HAZEL (&C000–&C12F) and a `*LOAD`/`*SAVE`
buffer may sit in the shadow region. So **every** stock buffer copy is bracketed
with an ACCCON save → set-to-the-right-bank → copy → restore. The Beeb (Model B,
and the 4.18 target) has **no** shadow/HAZEL and no ACCCON, so a plain copy is
always correct there.

**How the stock 4.21 does it.** `tx_calc_transfer` (&8900) computes the correct
ACCCON for the control block currently pointed to by `port_ws_offset`:
`lda acccon : ora #8` (page HAZEL in), then — unless the CB buffer is the
&FF/&FE Tube sentinel — re-reads ACCCON and, if shadow is enabled, sets bit 2.
The result is stored in `escapable` (&97). Each copy loop
(`save_acccon_for_shadow_ram` &840A, the bulk rx read &822E, the TX FIFO read
&87F8) then does `lda escapable : sta acccon` around the `(open_port_buf),Y`
access and restores the caller's ACCCON afterwards. The value is derived **fresh
per control block, immediately before the copy** — not a stale global.

**The defect.** The patched pump (`eco_rx_pump` → `erp_fits`/`erp_mem` in
`pi1mhz-patch/eco_library.asm`) ported only the **Tube-sentinel branch** of
`tx_calc_transfer` (it checks CB+7 = &FF and CB+6 ≥ &FE to pick the Tube path)
and **dropped the ACCCON computation and the save/set/restore wrap**. Its local
copy loop is a bare:

```
.erp_mem
    lda fred_jim_data
    sta (open_port_buf),y     ; no ACCCON gating
    ...
```

A repo-wide grep confirms the patched library's *only* ACCCON reference is the
`trb acccon` IRR-clear in `svc5`; there is no `escapable`/shadow handling
anywhere in the rx pump or the TX copy (`etb_mem`). Consequence on the Master:
because the pump runs from a deferred `svc5` IRQ, ACCCON is whatever the
interrupted foreground left. When a `*LOAD`/`*SAVE` buffer is in the shadow
region (X/E active — e.g. VIEW and other screen-touching work), the pump's copy
lands in the wrong bank → corrupted/incomplete transfer → the fileserver retries
→ the intermittent, load-dependent failures you see on the Master but not the
Beeb. The TX path (`etb_mem`) has the mirror problem for a shadow-resident source
buffer.

**Fix (IMPLEMENTED in source — needs reassemble + Master test).** A subtlety
found during implementation: the pump runs from **both** the foreground
wait-loop hook (`eco_pump_dec2`, where ACCCON correctly selects the buffer bank)
**and** svc5/IRQ (where ACCCON is the MOS's, not the buffer's). So the fix
captures the live foreground ACCCON (with HAZEL paged in) at each foreground
pump hook into a dedicated byte, and the pump's copy uses that snapshot —
correct in both contexts, and not dependent on the stale shared `&97`. Changes
(4.21 patch only; the Beeb has no ACCCON so 4.18 is left untouched):

- `apply.py`: `eco_acccon = &0d25` (reclaimed dead scout var `tx_port`).
- `eco_library.asm`: init `eco_acccon = acccon | 8` at bring-up; bracket the
  `erp_mem` copy loop with `lda acccon : pha → sta eco_acccon's value → copy →
  pla : sta acccon`.
- `eco_lib_b.asm`: new `eco_cap_acccon` (snapshot `acccon | 8`, preserves A),
  called from `eco_pump_dec2` and `eco_osw12_shim` before `eco_rx_pump`.

Re-port of the stock `save_acccon_for_shadow_ram` idiom, so confidence is high,
but it must be validated on Master hardware (host C tests can't exercise it) and
the reassembly must confirm the extra bytes fit the dead-region budget.

**Build status: DONE.** `AUNFS-4.21.rom` rebuilt and verified. Because the
sandbox served stale copies of the freshly edited *source* files (and `apply.py`
was missing inputs `pins.asm`/`eco_library_421.asm`), the four edits were applied
directly to the already-generated `anfs-4.21-pi1mhz.asm` (which has the library
spliced in) and assembled with `basm.py`. Result: `comment-verify` passes on all
6656 stock lines (only the 4 pre-existing NMI-vector lines differ, and they
updated correctly for the +14 byte shift); all patch additions stay within their
`org` regions (the `&809b` block ends &8466 < &8589; the `&8589` block ends
&88d8 < &89ed — no overflow); and the ROM diff vs the prior build is confined to
the patch code, the two relinked hook `jsr` targets, and the NMI vector. The new
`eco_cap_acccon` lands at &8454. Untested on Master hardware — flash and run the
repeated `*LOAD`.

This cleanly explains the differential: identical pump logic, but the missing
ACCCON gating only matters where shadow/HAZEL exists (Master), not on the Beeb.

## 9. Summary

The AUN **transport framing, addressing, sequencing, ACK/NAK semantics and the
ROM↔engine contract are spec-correct.** Fixes applied this pass: the silence
timeout (5 s → ~1 s fail + NFS retry, matching the ROM's synchronous poll);
removal of the non-spec content dedup that could drop a legitimate identical
block (5.1); spec-correct 10 ms reject spacing (5.2); and a deeper, memory-neutral
funnel queue (5.3). Left deliberately: 5.4 (the host-netmask behaviour is
correct) and 5.5 (ROM SWI-veneer, unrelated to `*LOAD`).

The **Master-specific** unreliability (4.21 vs 4.18) is traced in §8 to the
patched pump dropping the ACCCON shadow/HAZEL gating that the stock ANFS applies
to every buffer copy — a concrete, well-localised defect with a designed fix
awaiting reassembly and Master testing. The per-load data-block re-send (§6)
remains the one item needing a timestamped capture to close.

> **Update:** the §8 ACCCON shadow/HAZEL gating fix has since shipped (4.21
> `eco_cap_acccon`/`eco_acccon` + the `erp_mem` save/restore around the buffer
> stores; lockstep green). §8 below is retained as the original root-cause
> analysis.

## 10. Review pass 2026-06-29 (multi-source: bridge + emulator/docs + BBC-B + engine)

A second end-to-end pass cross-checked the client against the **PiEconetBridge**
server source, audited the AUN C engine for bugs, and confirmed BBC-B (model-B)
compatibility of the 4.18 ROM.

### 10.1 Bridge conformance — CONFIRMED CORRECT (vs econet-hpbridge.c)
Header layout/byte order, type codes (BCAST1/DATA2/ACK3/NAK4/IMM5/IMMREP6),
ctrl bit-7 strip/restore, ACK echoes seq + swaps src/dst, **same-seq lost-ACK
retransmit** (bridge resends the unchanged queued packet under the same seq; our
`last_rx_seq`/`seq_valid` re-ACKs without re-delivering — load-bearing and
correct), **no inbound seq dedup on the bridge**, broadcasts not ACKed, and
machine-peek (ctrl &88→&08, 4-byte reply) all match the live server.

- **AUTOACK is a hard bridge-config prerequisite** (traced to
  `econet-hpbridge.c:5381`): with AUTOACK off and a successful enqueue the bridge
  sends *no* receipt-ACK to the AUN source, so our send (no silence retransmit,
  `NORESP_RETRIES=0`) times out as NOT_LISTENING. Not a client bug — the Pi1MHz
  station's `AUN MAP` line must specify `AUTOACK`. See [[aun-bridge-autoack-required]].
- Tuning notes (no code change): bridge `EB_CONFIG_AUN_RETX` (1000 ms) equals our
  `AUN_NORESP_TIMEOUT_MS`; bridge `NAKTOLERANCE=2` means >2 NAKs on one seq makes
  the bridge drop the block — so for a *transient* funnel-full we prefer the
  bridge's own retransmit timer over NAK-storming (current behaviour is bounded
  by the bridge sending one outstanding DATA per destination).

### 10.2 Engine bugs FIXED this pass
- **H1 — re-open-while-parked overrun (HIGH):** `aun_rx_open` now drops a frame
  parked for the handle before re-opening (its len was validated against the old
  buffer); plus a `len<=buf_size` guard in `rx_park_poll`. Test 23.
- **M3 — held-immediate wedge (MED):** a host-immediate the host never answers is
  reaped after `AUN_HIMM_TIMEOUT_MS`, NAKing the originator and freeing the slot
  (else all later inbound immediates were NAKed and nIRQ &40 stayed asserted).
  Counter `himm_timeout`. Test 24.
- **M4 — content-identical new-seq frame dropped (MED, silent data loss):** the
  park decision no longer keys on content equality (`f->dup` removed); a true
  retransmit reuses the wire seq and is suppressed before delivery, so a frame
  reaching the park decision is always a distinct block and is parked. Test 18
  (rewritten).
- **L5 — mailbox clear ordering (LOW):** `aun_emulator_poll` clears the command
  slot before `aun_execute`, so a command the Beeb queues the instant the result
  is written is not wiped.

### 10.3 Investigated and DISMISSED
- **H2 — engine 1000 ms timeout vs the ROM's ~0.4 s F7:** false alarm. F7 bounds a
  *single handshake* (a dead Pi), not the whole transmit; a live Pi answers each
  `TX_POLL` with &80 in microseconds, so the ROM polls the full ~1 s budget. The
  1000 ms engine timeout and the `aun.h` "~1 s budget" comment are correct.

### 10.4 Deferred (low value / would need soak or ROM work)
- **L6** mailbox occupied-slot guard (with L5 + by-value cp/addr the corruption
  race is gone; a lost command on a protocol violation is F7-recoverable).
- **Bridge F4/F5** INK type &07 / four-way immediates without the host-imm
  extension — defensible "not listening"; the bridge does not send INK to AUN.

**L7 — late-retransmit re-execution of a host immediate — FIXED.** A peer that
missed our IMM_REPLY retransmits the same immediate under the same seq; once the
host had already answered (`himm.active` cleared), the old code re-captured and
re-delivered it, executing a non-idempotent Poke/JSR/OSProcCall twice. The
engine now caches the last-answered immediate keyed on (ip,port,seq) — ctrl,
seq, peer, and the reply bytes — and replays that reply on a matching
retransmit instead of handing it to the host again (`aun_himm_cache_t`, counter
`himm_replay`). The fresh-seq path is unchanged; the cache is dropped on
`aun_set_host_imm`. Test 30. Firmware cross-compiles clean (the engine struct
grows ~2 KB for the one cache slot).

### 10.5 BBC-B (model-B) compatibility — COMPATIBLE
Full audit of every 4.18 patch (P0–P12): zero 65C02 opcodes, zero ACCCON
(&FE34)/INTOFF (&FE38)/shadow/HAZEL access; only the low-level Econet interface
(wire engine, NMI→IRQ via svc5, init/status/clock probes, service registration)
is replaced; fileserver/`*`command/OSWORD-&10–12 client logic untouched; every
OSBYTE/OS call (e.g. &EA tube flag, &7E ack-escape) behaves identically on OS
1.20. This is now a **standing lockstep invariant**: the 4.18 harness counts any
&FE34/&FE38 access and aborts on any 65C02 opcode across the whole suite (test
20), so a regression that pulls in Master-only behaviour fails immediately.

### 10.6 Second corroborating pass (independent reviews)
Three independent reviews re-ran the spec/source/compat checks; all material
findings confirm §10 and added the items below.

**AUN spec conformance** (vs BeebEm `econet.cpp`, RISC OS PRM Ch.122/47,
BeebWiki, stardot t=28973). Every wire item CONFORMANT: 8-byte header layout and
LE32 seq, frame types 1–6, ctrl bit-7 strip-on-wire/set-on-deliver, immediate
ctrl `op|&80` (MachinePeek `&88`→`&08`), ACK-on-receipt + same-seq re-ACK,
broadcast fire-and-forget, UDP base 32768 (everything on one port; the Econet
port travels in header byte 1 — there is no port-per-Econet-port UDP mapping),
and `AUN_MAX_DATA`=8192. Reject timing 10×10 ms is spec-exact. Two intentional,
documented deviations only: no silence retransmit (delegated to the NFS layer)
and the immediate Count 0/1 gate (belongs in the SWI veneer; never reaches the
engine). ACTIONED: corrected three stale figures in `aun_design.md` (it cited a
1464-byte max and "4 attempts/250 ms"; now 8192 / reject 10×10 ms / no silence
retransmit / subnet-broadcast on). NOTED: BeebEm/bridge send the four-way
"special" immediates (Poke `&82`, OSProcCall `&85`, …) as AUN **type 2 (Data)**,
not type 5, so the host-immediate path (type-5 only) does not catch them — they
arrive as DATA. Spec-consistent (PRM guarantees only MachinePeek over IP);
recorded as a known limitation, not a bug.

**Bridge conformance** (engine ↔ live `eb_aun_receiver()` path, both sides read
in full). Header, type codes, byte order, ctrl handling, sequencing,
ACK-on-receipt, same-seq dedup, NAK semantics, broadcast/immediate/machine-peek,
and retransmit budgets all AGREE; the design cannot produce duplicate delivery or
a spurious same-seq retransmit (engine→bridge silence yields a new-seq NFS retry,
never a same-seq resend). Two operational notes, no code change:
  - **AUTOACK is a hard prerequisite.** With AUTOACK off and a successful
    enqueue the bridge returns nothing (econet-hpbridge.c:5381); the engine then
    times out to NOT_LISTENING on every send. The Pi1MHz station's `AUN MAP` must
    set AUTOACK. (Already captured in project memory.)
  - **NAKTOLERANCE=2 burst risk.** Under sustained burst that keeps the engine's
    rx funnel (depth 8) full, the bridge drops a DATA after 3 NAKs (silent loss).
    Mitigation: keep the funnel ≥8 and drain rx[0] promptly (current behaviour).

**BBC-B compat** independently re-confirmed §10.5 (zero 65C02 opcodes, zero
Master register/shadow/HAZEL on the 4.18 path, change surface limited to the
low-level interface/NMI; lockstep test 20 covers the changed paths).

### 10.7 Robustness tests added (IP network-delay variance)
The end-to-end tests previously advanced the clock to a single fixed point and
injected every ACK at zero latency. Added delay-variance coverage:
`test_aun.c` tests 25–29 (ACK-latency sweep across the silence boundary; jitter;
reject re-arms a fresh window; out-of-order delivery; host-immediate answered
just under the reap deadline) and lockstep test 3b in both the 4.18 and 4.21
harnesses (sweep the ACK delay in poll cycles 1/5/20/60 — `TX_POLL` must still
complete &00 and not trip the F7 guard). 4.18 now 103 lockstep checks (was 99),
4.21 105 (was 101); engine unit suite green and the model-B invariant holds.
NOT_LISTENING is a reachable, tested state (test_aun 2 = NAK exhaustion, 3 =
silence; lockstep 3 asserts &41): in the field it means an absent/non-listening
peer, a missing-AUTOACK bridge, or a burst drop, after which the NFS layer
retries the whole transaction. That last loss path is now itself an end-to-end
lockstep test (3c, both 4.18 and 4.21): the first DATA is dropped, the engine
times out to NOT_LISTENING, and the stock ANFS `send_net_packet` (&983F on 4.18,
&9B2C on 4.21) re-issues the transmit under a fresh sequence number, which is
then ACKed to &00 success. Confirms the design split — engine owns lost-ACK
dedup + NAK retransmit; the NFS layer owns the Pi's send-side whole-transaction
retry; the bridge owns its own send-side retransmit. 4.18 now 106 lockstep
checks, 4.21 108.

### 11. Post-fix code review (round 3) — fixes applied + items deferred

A full algorithm review of the engine and BOTH ROMs (after the round-2 fixes)
surfaced one HIGH and several MED/LOW items. Applied:

- **HIGH — ms clock wrap-gap (FIXED).** `aun_now_ms` derived ms from the low
  32 bits of the 1 MHz timer (`RPI_GetSystemTime`), which wraps every ~71.6 min,
  so the ms value was a resetting ramp, not a clean mod-2^32 counter; a deadline
  computed in the ~1 s before a wrap fell into an unreachable gap and the
  wrap-safe compare never fired — hanging a transmit (the ROM's synchronous
  TX_POLL spins, F7 never trips since the Pi keeps answering), defeating the M3
  himm reap and wedging park-and-retry. Now derived from the full 64-bit timer
  (`RPI_GetSystemTime64`). Guard test 31.
- **MED — himm reap × L7 cache (FIXED).** A >1 s host immediate was reaped with
  no cache seed → its retransmit re-executed; cache never expired → peer-reboot
  seq reuse could replay stale; a late reply after a reap+refill mis-tagged a
  newer immediate. Now: reap seeds a NAK-marker, cache carries `due_ms`
  (AUN_HIMM_CACHE_MS), and an IMM_POLL/IMM_REPLY generation guard drops a stale
  reply. Tests 32/33.
- **F9 — empty &00C0 reply-CB dropped a workspace-list frame (FIXED, both ROMs).**
  `erp_check_slot` now falls through to the workspace list on an empty &00C0
  entry instead of dropping. Lockstep test 12b.
- **Perf/cleanup (DONE):** dbg_prev compare/copy skipped without a trace hook;
  `AUN_RX_BLOCKS 4->2` (-131 KB; the ROM uses only handle 0); machine-peek folded
  into `send_imm_reply`; dead `needs_verdict` removed; doc drift fixed.
- **4.18 svc5 X/Y (FIXED).** Pre-existing 4.18-only gap (4.21 was already safe):
  the service-5 immediate path clobbered X (ROM slot)/Y; now bracketed NMOS-safe
  around the sole clobberer (`eco_imm_handle`).

Deferred, with rationale (low value / unconfirmed real-world occurrence / risk
outweighs benefit on a working ROM):

- **F4 — outbound immediate reply ignored a Tube reply buffer — FIXED (it was
  a regression).** The original ADLC-based ANFS was Tube-aware on *every* data
  path: rx-complete (`rx_complete_update_rxcb`, &8382/&843a), data-tx (&883c),
  and the immediate-op data NMI (`nmi_imm_data`, &8837) all stream to/from R3.
  The Pi1MHz AUN reimplementation reproduced that for the data-tx (`etb_data`)
  and rx (`eco_rx_pump`) paths but its `eti_reply` copied the immediate reply
  host-only, so an outbound immediate (e.g. a machine peek) with a *parasite*
  reply buffer lost its reply into I/O-processor RAM. `eti_reply` now mirrors
  `erp_match`: parasite-flagged (+6/+7 != &FFFF) replies stream to Tube R3 via
  `eco_tube_claim_tx`/`eco_tube_stream_out`, host replies copy as before. Both
  ROMs; lockstep test 7b (flipped from confirming the bug to asserting the fix);
  test 7 still covers the host path. Restores original ANFS behaviour.
- **T1 — slow-NAK foreground stall — NOT a regression (left as-is).** The
  original ANFS also did synchronous, bounded transmit retries that froze the
  foreground (scout retransmit count x delay from the TXCB), so a stall during
  retries is inherent to the ROM model, not new. The AUN reject path is likewise
  bounded (REJECT_RETRIES); its ~10 s worst case needs an adversarial peer that
  NAKs just before each 1 s silence boundary - real peers / the bridge NAK
  promptly (reject path = ~100 ms). A transaction-level reject cap would still
  bound the pathological case if ever wanted, but it is low-frequency ROM work
  and not a behaviour the original lacked.
- **T2 — Escape leaves the engine tx uncancelled.** `etb_escape` abandons the
  poll but there is no TX-cancel mailbox op, so the next transmit can read BUSY
  (-> &40) for up to ~1 s. Needs a new protocol op on both sides.
- **E1 — TX completion polled via a full mailbox command.** A dedicated
  TX-status FRED byte (mirroring &FCAB) would cut `.etb_poll` to a single read;
  perf-only, and a wire-protocol change.
- **R1 — single park slot.** A 2nd un-armed reply in a burst is dropped; fine for
  the one-reply-in-flight load path.
