# Econet test suite

Four layers, all host-runnable (no hardware, no cross-compiler). The
fourth is optional and skips itself when its prerequisite is absent:

## 1. AUN engine unit tests
`test_aun.c` — 14 scenarios against the pure engine with a stub
transport and fake clock: tx ack/nak/busy/retry/timeout, no-route, rx
delivery + wildcards + ctrl bit-7 restore, the rx queue (frames ACKed
and ordered behind a held head, NAK only when AUN_RX_QUEUE is full),
duplicate suppression, unknown-source drop, broadcast fan-out,
immediates both directions, the loopback test responder, send-failure.

    gcc -std=gnu2x -I../../AUN -o t test_aun.c ../../AUN/aun.c && ./t

## 2. cmdline.txt parser unit tests
`test_aun_config.c` — 19 cases for aun_station / aun_port /
aun_map (valid forms, bounds, malformed entries).

    gcc -std=gnu2x -I../../AUN -o t test_aun_config.c ../../AUN/aun_config.c && ./t

## 3. Lockstep integration test (`lockstep/`)
The patched ANFS ROM bytes execute in a Python 6502 emulator whose
FRED/JIM hooks drive the REAL AUN/aun_emulator.c / AUN/aun.c /
AUN/aun_config.c, compiled on the host against the stub headers here.
A scripted AUN peer validates the wire format independently.

164 checks (on the 4.26 ROM): init + cmdline config; IP-derived station
(aun_station=ip / ip.ip -> station and net from our own IPv4); tx/ACK with
header+payload validation; NAK -> &41; rx-pump delivery with the RXCB
completion bytes checked individually; rx-queue ordering (two frames
both ACKed, delivered in order, no retransmission); unmatched-frame
drop; machine peek inbound and outbound; broadcast; IRQ-driven
reception (nIRQ assert -> svc5 claim -> pump -> release, FRED &FCAB
mirror); svc5 pass-on for non-econet interrupts; the Econet receive
event (&FE via EVNTV, *FX52 gating and Y=slot exactly as the NMI path
passed them); the service gate never declining (the *HELP/*Net bug); and
service 15 not clearing this ROM's cached type-table entry (the P12 fix
that stopped the Master de-servicing AUNFS after boot); and Tube transfers
to and from a second processor, against a stub of the OS Tube code that
moves R3 data only for a started transfer of the right direction.

By default it runs the shipped Master ROM, `firmware/Pi1MHz/AUNFSM128.rom`
(AUNFS 4.26), as a 65C02, with its symbols in `lockstep/syms-4.26.txt`.
When the ROM changes, copy in the matching symbols: the Pi1MHzANFS repo's
`ANFS426/pi1mhz-patch/lockstep/syms.txt`, which its own lockstep run
regenerates from the patched source. That repo's 3.65 runner uses this
same harness for the model-B ROM. To run:

    ./lockstep/run.sh

Paths can be overridden with ECO_SRC / ECO_ROM / ECO_SYMS / ECO_HARNESS, and
the CPU with ECO_CPU (nmos for a model-B ROM; it is the default once ECO_ROM
is set).

## 4. Interop with a real PiEconetBridge (`interop/`) — optional
Layers 1–3 all judge the engine against a peer we wrote, so they can only
prove self-consistency: if our reading of the wire convention is wrong,
the unit tests, the fuzzers and the lockstep peer are wrong the same way.
This layer removes that blind spot — it runs Chris Royle's
PiEconetBridge on loopback (`-l`, IP-only, no Econet hardware) and lets
*it* judge our datagrams.

`interop.c` asserts the bytes we emit (machine peek `&88` still going as
a type-5 two-way; `*NOTIFY` as DATA to port 0; POKE's spliced byte
count); `run.sh` then greps the bridge's own debug log for the NOTIFY
string it reassembled from those datagrams — the third-party half, and
the point of the layer.

    ./interop/run.sh                       # or via run_tests.sh

It needs a compiled `econet-hpbridge`, which most machines will not have,
so with none present it prints `skipped` and exits 0 — never a hard
dependency. Point it at one with `PEB_BRIDGE=<path>` (a stale value falls
back to the search), or build one:

    cd PiEconetBridge/utilities && touch .noexplain && make econet-hpbridge

(`.noexplain` links without `libexplain`, which is often not packaged.)

Known quirk, deliberately not a failure: the bridge does not acknowledge
port-0 traffic to a local emulator station — its own TODO in
`econet-hpbridge.c` — so it decodes and acts on the NOTIFY, then stays
quiet, and our transmit ends NOT_LISTENING. A real Beeb behind the bridge
completes the four-way on the wire and the bridge relays a proper ACK.

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
        -Ilockstep -o fz fuzz_engine.c ../../AUN/aun.c && ./fz

## Coverage
The lockstep CPU records every executed PC; all 107 code labels in the
econet ROM