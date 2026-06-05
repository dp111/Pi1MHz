# Econet over AUN/UDP for Pi1MHz

Replace the BBC Econet ROM's ADLC (6854) hardware layer with calls to
Pi1MHz, which speaks standard AUN over UDP via the onboard WiFi/lwIP
stack. Interoperates with PiEconetBridge, BeebEm/B-Em AUN mode and
RISC OS AUN stations.

Two work items: (1) Pi1MHz support — **implemented, this tree**;
(2) Econet ROM modifications — planned below, pending the NFS/ANFS
disassembly.

## Architecture

```
NFS/ANFS (unmodified above the NetCom layer)
   | replaced Tx/Rx primitives: FRED &FCAA command interface
discaccess_emulator.c          command dispatch (opcodes 30..41 routed out)
econet_emulator.c              JIM command-block parsing, lwIP UDP transport
econet_aun.c                   pure-C AUN engine (host-testable, no deps)
lwIP UDP  ->  WiFi             AUN datagrams, port 32768
```

The cut-point in the ROM is the Network Communication ("NetCom") layer
between NFS and the 6854 — not the ADLC FIFO/NMI machinery. Everything
above (OSWORD &14, fileserver protocol, `*I AM`) rides unmodified on
the replaced transmit/receive primitives.

## Pi side (implemented)

### Files

- `econet_aun.h/.c` — AUN protocol engine. Pure C, transport and clock
  injected via `aun_transport_t`, so it compiles on a PC for unit
  testing. Single outstanding transmit (matches NFS behaviour),
  non-blocking throughout, retries/timeouts driven from the main poll
  loop, per-peer duplicate suppression, loopback test responder.
- `econet_emulator.h/.c` — glue: command-block parsing (untrusted
  offsets bounds-checked against the disc RAM region), lwIP UDP pcb,
  poll hook. Registered from `discaccess_emulator_init()`.
- `discaccess_emulator.c` — dispatch: opcodes 30..41 forward to
  `econet_emulator_command()`.

### Command interface (FRED &FCAA, opcodes 30+)

Same convention as the disc commands: build a 256-byte command block in
the top 64K of disc RAM (block page selected by the byte written to
&FCAA), opcode in byte 0 of the block, then poll &FCAA for the result.
Multi-byte fields little-endian; 32-bit fields 4-byte aligned; buffer
offsets relative to DISC_RAM_BASE.

| #  | Command     | In (block offsets)                                   | Out                                  |
|----|-------------|------------------------------------------------------|--------------------------------------|
| 30 | INIT        | +1 stn, +2 net, +4 u32 listen UDP port (0=32768)     | result                               |
| 31 | STATUS      | —                                                    | +4 stn, +5 net, +6 ready flags, +8 ip[4], +12 u32 counters ×11 |
| 32 | TX          | +2 ctrl, +3 port, +4 dest stn, +5 dest net, +8 u32 data off, +12 u32 len | result (accepted/busy/no-route) |
| 33 | TX_POLL     | —                                                    | result: &80 pending / 0 ok / err; +8 u32 imm reply len |
| 34 | RX_OPEN     | +1 handle 0-7, +2 port (0=any), +4 stn, +5 net (&FF=any), +8 u32 buf off, +12 u32 buf size | result |
| 35 | RX_POLL     | +1 handle                                            | result: &80 waiting / 0 ready; +2 ctrl, +3 port, +4 src stn, +5 src net, +12 u32 len |
| 36 | RX_CLOSE    | +1 handle                                            | result                               |
| 37 | BCAST       | +2 ctrl, +3 port, +8 u32 data off, +12 u32 len       | result (completes immediately)       |
| 38 | IMMEDIATE   | +2 ctrl, +4 dest stn, +5 dest net, +8/+12 tx off/len, +16/+20 reply off/max | result; completion via 33 |
| 39 | MAP_ADD     | +1 stn, +2 net, +4 ip[4] a.b.c.d, +8 u32 UDP port    | result                               |
| 40 | TEST        | +1 enable, +2 stn, +3 net                            | result                               |
| 41 | SET_MACHINE | +4 machine id[4] (machine-peek reply)                | result                               |
| 42 | RX_DONE     | +1 handle (release held frame, re-arm)               | result                               |

Result codes (`AUN_*` in econet_aun.h): 0 OK, 1 not listening, 2 net
error, 3 no route, 4 busy, 5 bad param, 6 not ready, 7 no such rx
block, &80 pending.

Ordering: INIT must precede MAP_ADD/TEST/SET_MACHINE (INIT resets the
engine). INIT fails with 6 until WiFi has an address.

### AUN wire format

UDP, default port 32768. Datagram: type(1), port(1), ctrl(1, bit 7
cleared on wire, restored on delivery), pad(1), seq(4 LE, +4 per
transaction), data. Types: 1 broadcast, 2 data, 3 ack, 4 nak,
5 immediate, 6 immediate-reply. Four-way handshake collapses to
data→ack/nak; ACK/NAK echo the sequence. Source station identity comes
from reverse-lookup of sender IP:port in the station map (datagrams
from unmapped IPs are counted and dropped). Inbound machine peek
(wire ctrl &08) is auto-answered with the 4-byte machine id.
Retries: 4 attempts, 250 ms each (econet_aun.h constants).

### Constraints

- lwIP here has `IP_FRAG`/`IP_REASSEMBLY` off → max AUN payload 1464
  bytes (`AUN_MAX_DATA`). Fine for NFS (≤ ~1.3K bursts); peers sending
  larger datagrams will be dropped by the stack.
- Broadcasts are sent directed to every mapped peer (no subnet
  broadcast yet).
- One transmit in flight; TX while pending returns busy.

### Test responder (cmd 40)

Transmits addressed to the configured test station never touch the
network: ACKed locally, payload fed back through the rx path as if from
that station; immediates answer with the machine id. Closes the whole
Beeb → JIM → command → engine → rx-block → Beeb loop with no second
station, no fileserver, no network.

## ROM side — IMPLEMENTED (anfs-4.18-pi1mhz.asm / .rom, ANFS418 folder)

The patches below are applied; the patched ROM assembles byte-exact
outside the intended regions (verified against the disassembly's
per-line byte comments by `pi1mhz-patch/basm.py`, regenerable via
`pi1mhz-patch/apply.py`). Changed regions: &8028-&802C (svc5 econet IRQ
check), &8074-&80BC (init), &80BE-&80FC and &80FD-&84BA (the dead scout/
NMI-rx handlers, now the rx pump, svc5 handler, immediate-op handler and
Tube helpers), &858C-&89C9 (engine library over the dead NMI tx chain),
svc-table bytes for the svc11/12 stubs, &8A45-&8A52 (ADLC presence probe
— would otherwise disable the whole ROM with no ADLC fitted!), &8F40
(station id), &900C (clock test), &95FD (rx pump hook), &A619 (OSWORD
&12 pump hook). The reclaimed &80FD-&84BA stops just before the live SR-
interrupt dispatch table at &84BB (reached by the svc5 non-econet
fallback).

Beeb→Pi protocol details fixed during implementation: the FRED write
callback runs in FIQ on the Pi, so econet commands are executed from
the Pi main loop via a mailbox; command block pages are &E0-&E5
(INIT/STATUS/TX/TX_POLL/RX_OPEN/RX_POLL), every result is < &E0, and
the Beeb polls &FCAA until the value drops below &E0. JIM staging:
TX payload &FE0000, immediate reply &FE4000, RX buffer &FE8000.
Transmits run synchronously under SEI inside the patched `tx_begin`;
the JIM address registers are saved/restored so an interrupted disc
transfer is unharmed. Reception uses ONE Pi-side wildcard block plus
a foreground "rx pump" that replicates the NMI engine's control-block
scan (the &00C0 reply CB, then the NFS workspace slots) and updates
the matched CB exactly as `rx_complete_update_rxcb` did. INIT passes
station 0 = "use Pi-side configuration".

### Pi-side configuration (cmdline.txt)

Parsed at ECO_INIT time (econet_config.c, host-tested). Values must
not contain spaces:

```
econet_station=1.32
econet_port=32768
econet_map=1.254=192.168.1.10,1.200=192.168.1.11:32769
```

- `econet_station` — our station as net.stn (bare "32" means net 0).
  Used when the ROM's INIT block carries station 0, which the patched
  ANFS always sends. Default 0.32.
- `econet_port` — local AUN listen UDP port. Default 32768.
- `econet_map` — comma-separated peer entries `net.stn=a.b.c.d[:port]`,
  port defaulting to 32768. The `:port` suffix is only needed when
  several stations share one host IP (multiple emulator instances, or
  a PiEconetBridge exposing wire stations on per-station ports).
  Entries are also addable at runtime via ECO_CMD_MAP_ADD (39); a
  malformed entry stops parsing but keeps earlier entries.
- `econet_learn=<net>` — learn mode: resolve/attribute unmapped
  stations on that net by the classic IP-last-octet convention.
- `econet_machine=<8 hex digits>` — machine-peek reply bytes.
- `econet_debug=1` — log econet events on the wifi debug channel.

### Observability

`http://<pi>/econet` on the Pi web server shows live engine state
(station, peer map incl. learned entries, rx queue depth, IRQ status,
all counters) via `econet_status_text()`. The shared nIRQ line is now
arbitrated per-source in Pi1MHz.c (`Pi1MHz_SetnIRQ_src`), so econet and
harddisc requests cannot clear each other — closing former item 8.
The ROM titles itself "AUNFS 4.18 (Pi)" and ships as `AUNFS.rom`.
For hardware bring-up, run `tests/econet/beeb/ECOTEST.bas` on the Beeb
first (10 self-contained checks via the loopback responder), and see
SETUP.md in the ANFS folder.

### Status of the milestone-1 limitations (all now addressed except the shared-IRQ caveat)

- ~~Local-memory buffers only (no Tube transfers)~~ FIXED: a CB whose
  address high bytes are NOT the &FF/&FF "local" sentinel, with a
  co-processor present (`tube_present`), is treated as a Tube target —
  same test as the original `tx_calc_transfer`. The rx pump streams the
  payload JIM→R3, and `tx_begin` fetches JIM←R3, both via the OS Tube
  claim (`tube_addr_data_dispatch` &C2) with the original 3-NOP
  inter-byte timing and `tube_release_claim` afterwards. HARDWARE NOTE:
  the lockstep harness models the Tube registers and validates the
  emitted claim + byte stream, but the real ULA arbitration and R3
  timing can only be confirmed with a second processor attached — test
  there first.
- ~~Frames with no matching open CB are ACKed then dropped~~ FIXED:
  ACK/NAK is now deferred to the verdict. The engine queues the frame
  WITHOUT ACKing; the rx pump scans the open CBs and reports back via
  RX_DONE (+2 verdict byte): delivered → ACK, no listener / no room →
  NAK, so a sender sees a true "not listening" exactly as on a wire.
  Oversize-for-CB frames also NAK rather than overrun Beeb memory.
- ~~>1464-byte datagrams dropped~~ FIXED: IP reassembly/fragmentation
  enabled in lwipopts (IP_REASS_MAX_PBUFS 16); AUN_MAX_DATA raised to
  8K; the wildcard rx staging buffer is 8K. Validated with a 4K frame.
- ~~IRQs masked for the whole of a slow transmit~~ MITIGATED: the
  TX_POLL loops now open a one-instruction IRQ window per iteration
  (JIM state saved/restored around it), so the machine stays responsive
  during a transmit to a slow or dead peer.
- Inbound immediate operations (remote peek/poke/JSR/halt/continue) are
  now handled host-side (INIT +8 bit 1; &FC88 bit 6; commands 43/44),
  not just machine peek.
- REMAINING: nIRQ is a shared open-collector line also driven by
  harddisc_emulator — econet only changes it on state transitions, but
  simultaneous heavy use of both is untested (item 8, deferred).
- ~~Small race: a new frame could overwrite JIM &FE8000 mid-copy~~
  FIXED twice over: RX_POLL holds the presented frame until the pump
  issues RX_DONE (cmd 42), and the engine now queues up to
  AUN_RX_QUEUE (4) frames per block in its own RAM — frames are ACKed
  and buffered like PiEconetBridge does, with NAK (sender retries)
  only when the queue is genuinely full. Queued broadcasts are no
  longer lost while a frame is held.
- ~~User receive blocks only fill when the pump runs~~ FIXED: the Pi
  asserts nIRQ while frames are queued (enabled by INIT's +8 flag;
  status mirrored at FRED &FC88, bit 7 + count). The patched ANFS
  claims it via service call 5 (unrecognised interrupt, entry &8028)
  and drains the queue through the rx pump, so unsolicited frames
  arrive without any FS activity. Non-econet interrupts fall through
  to the original System VIA SR test. The pump also regenerates the
  Econet receive event (&FE via EVNTV) with the original gating —
  fs_flags bit 0 (*FX52) and CB slot ≥ 3 — and Y = slot on entry,
  deferred to pump exit with a re-entrancy guard so an event handler
  may itself call OSWORD &12. Caveat: nIRQ is a shared
  open-collector line also driven by harddisc_emulator — econet only
  changes it on state transitions, but simultaneous heavy use of both
  is untested.
- IRQs are masked for the duration of a transmit (worst case ~1 s if
  the peer is down — comparable to the old NMI-driven behaviour).

## Original patch plan (from anfs-4.18.asm disassembly)

Target: ANFS 4.18. Key workspace: `net_tx_ptr` &9A/&9B (foreground TXCB
pointer, usually &00C0), `net_rx_ptr` &9C/&9D (RXCB workspace),
`nmi_tx_block` &A0/&A1 (NMI-layer TXCB pointer), `tx_complete_flag`
&0D60, NMI shim at &0D00-&0D1F, scout buffer &0D20+.

TXCB layout (confirmed at `tx_begin`): +0 ctrl (bit 7 set = active;
NMI writes the result code back here with bit 7 clear), +1 port,
+2 dest stn, +3 dest net, +4..7 buffer start (32-bit), +8..11 buffer
end, +12..15 immediate-op params. Result codes: &00 OK, &40 line
jammed, &41 not listening, &43 no clock, &44 bad ctrl.

### Patch points

1. **`tx_begin` (&858C)** — the single transmit choke point. All three
   callers go through it: `poll_adlc_tx_status` (&98C9, the synchronous
   transmit-and-wait used by `send_net_packet` &983F and the pass-through
   path), `osword_10_handler` (&A5A4, fire-and-forget), and &A89A.
   Replace the body: read TXCB via `nmi_tx_block`; copy data
   [start,end) into JIM via the FRED data port (&FCA6-9); build cmd 32
   (or cmd 38 when ctrl≥&81 and port=0 — immediate ops); write the
   command page to &FCAA; poll cmd 33 until not pending; map AUN status
   → ANFS codes (0→&00, 1→&41, 2/4→&40, 6→&43); store to TXCB+0 and
   set `tx_complete_flag`. Because completion is synchronous, every
   caller's poll loop (e.g. &98D9, `wait_net_tx_ack`'s &95F4) sees an
   instant result — no other call site needs touching. Worst-case
   blocking = Pi-side retry budget (4×250 ms), comparable to real
   Econet retry behaviour; fine in the IRQ-context OSWORD &10 path too.
2. **Receive path** — RXCB slots live in NFS workspace via
   `net_rx_ptr`; slot markers: 0 empty, &3F pending, &C0 active, bit 7
   set = frame delivered. Today the NMI scout/data handlers
   (&80BE-&84xx) fill them. Replace with:
   - `osword_11_handler` (&A5C1): after marking the slot pending,
     issue ECO_RX_OPEN (slot # = Pi handle 0-7; port/station filter and
     buffer range from the RXCB).
   - a small **rx pump**: issue ECO_RX_POLL; on ready, copy payload
     from JIM into the RXCB buffer, fill src/ctrl/len, set bit 7 of the
     slot byte. Call it from `wait_net_tx_ack`'s poll loop (&95F4) and
     from the OSWORD &12 read / rx-poll entry. Slot delete → ECO_RX_CLOSE.
3. **`adlc_init` (&8074)** — drop the &FE18 INTOFF read, the
   `adlc_full_reset` call, the OSBYTE &8F/&0C NMI claim and the NMI
   shim copy to &0D00. Instead: issue cmd 30 (INIT), read station via
   cmd 31 into `tx_src_stn` (&0D22), set `tx_complete_flag` and
   `econet_init_flag` to &80 as now. (Station number/map config best
   held Pi-side: `econet.cfg` on the SD card read at INIT; ECO_MAP_ADD
   kept for tests.)
4. **`adlc_full_reset` (&8969) / `adlc_rx_listen` (&8978)** — stub to
   RTS.
5. **Clock/idle checks** — `tx_line_idle_check` (&85E3) BIT of SR2 and
   the *NET clock test (~&9012): replace with the ready flag from
   cmd 31 so "No clock" maps to "Pi network down".
6. **Leave untouched** — `send_net_packet` retry logic, OSWORD
   &13/&14, the fileserver/printer protocol, error classification
   (`load_reply_and_classify`). The whole NMI handler suite
   (&80BE-&89A5) goes dead but can stay in place initially; reclaim the
   space later.

## Testing

All in `tests/econet/` (see its README), host-runnable, no hardware:

- **Unit tests** — `test_econet_aun.c` (15 scenarios incl. the
  verdict flow, learn mode and subnet broadcast) and
  `test_eco_config.c` (19 parser cases). All pass, also under
  ASan/UBSan.
- **Fuzzers** — 2M random engine operations and 400k hostile Beeb
  command blocks under ASan/UBSan with invariants asserted per
  iteration: no findings (`tests/econet/fuzz_*.c`).
- **6502 coverage** — the lockstep CPU records executed PCs: all 107
  code labels in the econet ROM regions execute under the suite,
  including every page-crossing copy loop, the workspace-slot scan,
  the pump hooks, remote halt/continue and the error paths.
- **Lockstep integration test** — `tests/econet/lockstep/`: the
  patched ANFS ROM bytes execute in a Python 6502 emulator whose
  FRED/JIM hooks drive the REAL econet_emulator/econet_aun/
  econet_config C code compiled on the host, with a scripted AUN peer
  validating the wire format. 86 checks pass: init + cmdline config,
  tx/ACK, NAK→&41, rx-pump delivery with exact RXCB completion
  semantics, rx-queue ordering (both frames ACKed, delivered in order,
  no retransmission), unmatched-frame drop, machine peek both ways,
  broadcast, and IRQ-driven reception (nIRQ assert → svc5 claim →
  pump delivery → release; non-econet interrupts passed on).
  This caught a real bug (eco_cmd_issue returned CMP-loop flags, not
  result flags — every command would have "failed" on hardware; fixed
  with ORA #0). Use `anfs-4.18-pi1mhz-fixed.rom`.
- **Beeb-side** — planned: BASIC/asm program driving cmds 30-41 via
  FRED &FCAA against the test responder (no network), then against a
  BeebEm AUN instance / PiEconetBridge fileserver.
- **First milestone** — `*I AM` round-trip against PiEconetBridge's
  fileserver: register Pi1MHz there as an AUN host
  (`A <net> <stn> <ip> 32768`); note v2.1+ requires non-zero network
  numbers in config (it substitutes net 0 on the wire).
