# Econet test suite

Three layers, all host-runnable (no hardware, no cross-compiler):

## 1. AUN engine unit tests
`test_econet_aun.c` — 14 scenarios against the pure engine with a stub
transport and fake clock: tx ack/nak/busy/retry/timeout, no-route, rx
delivery + wildcards + ctrl bit-7 restore, the rx queue (frames ACKed
and ordered behind a held head, NAK only when AUN_RX_QUEUE is full),
duplicate suppression, unknown-source drop, broadcast fan-out,
immediates both directions, the loopback test responder, send-failure.

    gcc -std=gnu2x -I../.. -o t test_econet_aun.c ../../econet_aun.c && ./t

## 2. cmdline.txt parser unit tests
`test_eco_config.c` — 19 cases for econet_station / econet_port /
econet_map (valid forms, bounds, malformed entries).

    gcc -std=gnu2x -I../.. -o t test_eco_config.c ../../econet_config.c && ./t

## 3. Lockstep integration test (`lockstep/`)
The patched ANFS ROM bytes execute in a Python 6502 emulator whose
FRED/JIM hooks drive the REAL econet_emulator.c / econet_aun.c /
econet_config.c, compiled on the host against the stub headers here.
A scripted AUN peer validates the wire format independently.

91 checks across 18 scenarios: init + cmdline config; tx/ACK with
header+payload validation; NAK -> &41; rx-pump delivery with the RXCB
completion bytes checked individually; rx-queue ordering (two frames
both ACKed, delivered in order, no retransmission); unmatched-frame
drop; machine peek inbound and outbound; broadcast; IRQ-driven
reception (nIRQ assert -> svc5 claim -> pump -> release, FRED &FCAB
mirror); svc5 pass-on for non-econet interrupts; the Econet receive
event (&FE via EVNTV, *FX52 gating and Y=slot exactly as the NMI path
passed them); the service gate never declining (the *HELP/*Net bug); and
service 15 not clearing this ROM's cached type-table entry (the P12 fix
that stopped the Master de-servicing AUNFS after boot).

To run (needs `anfs-4.18-pi1mhz.rom` and `syms.txt` in lockstep/ —
both checked in; regenerate after ROM changes with
`pi1mhz-patch/basm.py <patched.asm> /dev/null symbols` filtered to
eco_ensure_init / eco_rx_pump / eco_tx_begin / tx_begin /
svc5_irq_check):

    ./lockstep/run.sh

Paths can be overridden with ECO_SRC / ECO_ROM / ECO_SYMS / ECO_HARNESS.

## Bugs these tests have caught
- `eco_cmd_issue` returned with flags from its CMP #&E0 loop guard
  rather than from the result value: every successful command read as
  a failure. (Lockstep scenario 1; fixed with ORA #0.)
- The rx single-buffer overwrite race, eliminated by the
  RX_POLL-holds / RX_DONE-pops handshake plus the per-block frame
  queue. (Lockstep scenario 4b proves ordering with no buffer reuse.)

## 4. Fuzzers (run under ASan/UBSan)
- `fuzz_engine.c` — 2M random operations against the engine: malformed
  datagrams, hostile lengths, random API interleavings, queue/map
  invariants asserted every iteration.
- `fuzz_cmd.c` — 400k hostile Beeb command blocks through the real
  dispatch (random opcodes 30-44, adversarial offsets/lengths) plus
  random inbound traffic.

    gcc -std=gnu2x -g -fsanitize=address,undefined -fno-sanitize-recover=all \
        -Ilockstep -o fz fuzz_engine.c ../../econet_aun.c && ./fz

## Coverage
The lockstep CPU records every executed PC; all 107 code labels in the
econet ROM regions are exercised (the three data tables excepted),
including every >=256-byte page-crossing copy loop, the workspace-slot
scan, both pump hooks, halt/continue, and all error paths.
