# The RAM probe

> **STATUS 2026-11-11:** as-built, checked against `ramprobe.c`, `ramprobe_api.c` and helpers
> 22 and 23 in `6502code.asm`, after the PR #51 review (branch `ramprobe-review`).  The kc-beeb host
> tests pass.  The review's changes are not yet tried on a real Model B: above all the hooks
> running from FRED, and the agent waiting and transferring from the bus again (see "How it gets
> in"), which needs a relay check.  The hardware notes below (timings, the relay) are from before
> them.

Stop a BBC Micro over the network, read and write its memory, call code, and let it go on.
No ROM is needed in the BBC: one typed line installs it.  Web-based clients can use it, for
example, for remote control, AI integration or video streaming.

Tested on a Model B (MOS 1.20, MMFS Turbo, a Pi Zero 2 W on Ken Lowe's level shifter, over
WiFi), and in the host tests on kc-beeb, an emulated Master 128.

```
 BBC Micro                           Pi1MHz                              on your network
 ┌───────────────────┐               ┌──────────────────────────────┐    ┌──────────────────────┐
 │ EVNTV, IRQ2V and  │◄── vsync event│ ramprobe.c: asks the BBC in  │◄───┤ a client:            │
 │ &0D00 (a JMP) to: │◄── nIRQ, NMI ─┤ ramprobe_api.c: /bbc routes  │HTTP│ tools/bbcprobe.py,   │
 │ &FC6F...: hooks,  │  1MHz bus     │ helpers 22, 23: agent,install│    │ or anything else     │
 │ in the Pi's FRED  │◄─────────────►│ the probe's JIM region       │    └──────────────────────┘
 │ &FD00: the agent, │  &FC88, &FCA6 │ JIM &E00000: the report      │
 │ in the Pi's memory│               └──────────────────────────────┘
 └───────────────────┘
```

The probe is only the mechanism: stop, read, write, call, go on.  Typing, reading the screen
and recovering after a Break are left to the client.

## Installing it

Type this on the BBC after switching on:

```
X%=23:CALL &FC88
```

It prints `Pi1MHz probe on`.  Helper 23 points `EVNTV` and `IRQ2V` at hooks the Pi serves in
FRED, writes their old values into the hooks, puts a 3-byte `JMP` for the NMI at `&0D00`, turns
the vsync event on, tells the Pi, and sets
`*KEY10 X%=23:IF HIMEM<40000 CALL &FC88|M`, so a soft Break puts the probe back by itself.  The
`HIMEM` test keeps the key from calling `&FC88` in a second processor's BASIC, where it is
ordinary RAM: the BBC's own `HIMEM` is never above `&8000`, except in the Master's BAS128, where
a soft Break then doesn't reinstall the probe (type the line again).  Ctrl-Break and power-on clear the
key: type the line again then.

On the BBC it takes `&0D00-&0D02`, `EVNTV`, `IRQ2V`, the vsync event (turned on, and left on)
and soft key 10.  The install uses `&70-&73`, BASIC's zero page for `CALL`, while it runs.  The install
replaces `*KEY10`, so a Break definition of your own is lost.  To keep yours, install with
`X%=23:Y%=1:CALL &FC88` (`CALL` passes Y%'s low byte in Y, and only 1 counts): the line then
leaves the key alone, and a soft Break doesn't reinstall the probe.  Y% is one of
BASIC's resident variables and stays 1 until changed, so type `Y%=0` to have the key set again.
The agent runs from the Pi's memory through the `&FD00` window. The 6502 code is helpers 22
and 23 in `6502code/6502code.asm`, assembled with beebasm into `6502code.bin` like every other helper,
so the SD card needs this build's `Pi1MHz/6502code.bin`.  The firmware loads it from the card
apart from the kernel, so page 22 carries a version byte (`RP_VERSION`, at `&FDFE`), and the
copy of the FRED code in page 23 has a 0 where `rp_in`'s `PHA` goes.  Only a kernel that finds
its own version writes the `PHA`, and the install line hooks nothing without it, so neither a
newer card with an older kernel nor the reverse can hook code that doesn't match.  On a
mismatch the Pi still writes the rest of the FRED code (an install from an earlier build doesn't
check, and its hooks must lead somewhere), refuses every stop, and `GET /bbc` says to copy this
build's file.

**This opens the BBC to remote code execution over your WiFi network: anyone on it can then stop the BBC, read and write its memory and run code on it, until Ctrl-Break or power-off.**  There is no password: the Pi1MHz's web pages are served in
the clear, so one would protect nothing.  Use it on a network you trust, and turn it off with
`ramprobe_addr=-1` if you don't want it.

## The JIM memory it uses

While the machine is stopped in the agent, the probe talks to the BBC through the services port
(`&FCA6-&FCA9`) at a fixed region of the JIM services buffer, services offset `&E00000`:

| JIM offset (from `DISC_RAM_BASE`) | What |
|---|---|
| `&E00000-&E000FF` | the report: the stopped registers, the halted flag, the call result, a sequence number and the installed marker |
| `&E00100-&E100FF` | the peek/poke data buffer, up to `RP_MAX_TRANSFER` (64 KB) |

`&E0-&EF` is a free 64 KB-aligned stretch of the services buffer: no fixed firmware or in-tree
ROM uses it.  It is clear of AUN's Econet staging (`&FE0000`), the SD explorer (`&D0-&D9`), the
help screen and the command blocks (`&FFxxxx`).  The region is written only once the probe is
installed (the line typed on the BBC) and only while the machine is stopped, with the running
program's own services-port pointer saved and put back around each exchange, so it does not
disturb a filing system or a program that uses JIM at other times.  The address is not
hard-coded in clients: `GET /bbc`'s `report` field gives it.  It is set in two places,
`RP_REPORT_HI` in `6502code.asm` and `RP_REPORT` in `src/ramprobe.h`; the other addresses the Pi
writes in the 6502 code are listed in `ramprobe.h` and checked by ASSERTs in the `.asm`.

## What lives where, and why

### In the Pi: two helper pages

The BBC sees the Pi's memory through one 256-byte window at `&FD00`, and runs a helper from
that window: `X%=n:CALL &FC88` shows page *n* there and jumps to it.  So a helper's code, and
any data it copies into the BBC, has to be in its own page.  The byte counts are beebasm's
(`PRINT ... spare bytes` in `6502code.asm`).

| Page | What | Spare | Why |
|---|---|---|---|
| 22, the agent | code `&FD00-&FD8E`; the Pi's code for a call `&FDA0-&FDEF`; the mailbox `&FDF0-&FDF5`; `RP_VERSION` at `&FDFE`; its number at `&FDFF` | 17 (before `&FDA0`) | It runs while the machine is stopped, and the Pi writes calls into it then.  `rp_in` waits for `22` at `&FDFF` before jumping in. |
| 23, the install | checks the FRED code is there, hooks `EVNTV` and `IRQ2V` (not again if the line is typed twice), puts the NMI's `JMP` at `&0D00`, tells the Pi, turns the vsync event on, sets `*KEY10` (unless `Y%` is 1), says so; and at `&FDCF-&FDFE`, the FRED code below, for the Pi to copy | 3 | |

Pages 24-30 are free.

### In FRED: 48 bytes, the Pi's

Only two moments need code outside the agent's page: getting in, while `&FD00` still shows the
program's page, and getting out, once it shows it again.  FRED, the Pi's `&FC00` page, never
changes when the window does, so that code runs from there, as the serial redirector's stub
does.  The Pi writes it into FRED at every reset, from the end of page 23.  The BBC can't
overwrite it: its writes to FRED go to the Pi, which keeps only the four bytes of the hooks'
`JMP`s that the install writes.

| Where | Bytes | What |
|---|---|---|
| `&FC6F-&FC86` | 24 | the `IRQ2V` hook (rung 1); `rp_in`, the way in for every rung: `PHA`, select page 22, wait for it, jump; and where rung 0 gives the event on to the old `EVNTV` |
| `&FC8F-&FC9F` | 17 | the `EVNTV` hook (rung 0) |
| `&FCB0-&FCB6` | 7 | `rp_exit`, the way out: select the program's JIM page, pull X and A, `RTI` |

Each hook starts with `JMP old-vector`.  To ask, the Pi changes that one byte to `BIT`, which
skips the address and falls through into the way in; it changes it back once the BBC is in, or
if it gives up.  One byte, so the 6502 sees one or the other, never half of each, and while
nothing is asked a vsync costs one `JMP` from FRED (about 3 µs).  The three blocks sit in the
gaps between the copy cursor (`&FC45-&FC6E`), the helpers' entry (`&FC88-&FC8E`), the services
port, the mouse (`&FCAC-&FCAF`) and the serial block (from `&FCB7`).  Keep other devices off
them while the probe is on.

### In the BBC: 3 bytes

| Where | Bytes | What | Why there |
|---|---|---|---|
| `&0D00-&0D02` | 3 | `JMP rp_in` | The NMI vector: the 6502 goes to `&0D00` on an NMI, so rung 2 can only come in here.  Rungs 0 and 1 don't use it. |
| `&0220`, `&0206` | 4 | `EVNTV` and `IRQ2V`, pointing at the hooks in FRED | The ways in for rungs 0 and 1. |
| soft key 10 | | `X%=23:CALL &FC88` and Return | So a soft Break reinstalls it.  `Y%=1` leaves it alone. |
| the stack | | while stopped: A, X, Y and the program's services port pointer, under the frame | The agent's only workspace, given back when it goes on. |

None of it is in zero page, in the OS's workspace or in the program's memory from `PAGE` up, so
a game's workspace, a disc access, `*TAPE` or a printer can't disturb rungs 0 and 1.  A program
that takes the OS's vectors removes them; typing the line again puts them back.  Measured on
beebjit (a Model B with DFS 0.90), Elite's disc version cleared the old hooks at `&0380` about
30 s after `*RUN ELITE2`, and wrote over the printer buffer at `&0880` too.

Running these few instructions from FRED, at 1 MHz, costs nothing measurable: on kc-beeb a
rung 1 stop and go took 1191 µs either way, and a 32 KB peek moved 64.2 KB/s either way (the
transfers run from page 22, on the bus as before).

## How it gets in

Each request names its rung, and the Pi tries only that one.  A halt with no rung uses rung 0.
A client starts at the gentlest and moves up if it gets no answer; `bbcprobe.py halt` tries 0,
then 1, and the NMI only with `--nmi`.

| Rung | Way in | Reaches | Gives up after |
|---|---|---|---|
| 0 | the vsync event, through `EVNTV` | ordinary programs | 300 ms |
| 1 | the Pi's nIRQ, through `IRQ2V` | programs that turn events off or take `EVNTV` | 40 ms |
| 2 | NMI | anything, with interrupts off too | 3 tries, 40 ms apart |

- Rung 1 is the quickest: a stop and go takes 4 ms on the Model B over WiFi, against about
  20 ms for rung 0, which waits for the next frame.  Rung 0 suits screen writes, which then
  land just after the vsync.
- Rung 2 needs `nmi=1` as well as `rung=2`, or it's refused with 400.  Keep it for a BBC that
  answers nothing else.  The 6502 jumps straight to `&0D00` on an NMI, with no check of what's
  there: after a disc access that is Acorn DFS's own NMI code, not the probe's `JMP` (Limits,
  below).
- A rung that doesn't answer leaves nothing behind: the Pi clears its request and `error`
  names the rung.
- The hooks enter only while the Pi has their first byte set to `BIT` (above).
- The agent waits and runs the Pi's code in its own page, on the 1MHz bus.  An earlier version
  ran those loops from the BBC's RAM because a Model B's cassette relay was heard to click (by
  microphone) when code ran from the bus for more than about 2 ms.  No mechanism is known: the
  relay switches only on a write to `&FE10`.  The NMI clicks of the first version, fixed by the
  event and IRQ rungs and multi-byte transfers, were the ones confirmed; the bus-loop clicks may
  have been the Pi's audio into the BBC's speaker, which probe traffic also made crackle.  To
  be checked on a real Model B: with `BeebAudio_Off=1`, does the motor LED light or the relay
  click during long peeks?

## HTTP routes

A POST starts something.  Either poll `GET /bbc` until `busy` is false, or add `wait=1` and the
answer comes when the work is done (api 2).  The server keeps connections open, so a client
that reuses one saves a TCP setup per request.

| Route | What it does |
|---|---|
| `GET /bbc` | The status: `api`, `build`, `resets` (BBC resets seen), `state` (`waiting` until the line is typed, then `running`, `halted` and so on), `busy`, `installed`, `rung` (the last stop's), `error`, the stopped registers, the last call's A, X, Y and P, and `report` (the JIM address of the probe's report; its data buffer is `report` + &100) |
| `POST /bbc/halt?rung=` | Stop the machine, by rung 0, 1 or 2 (0 if not given; 2 also needs `nmi=1`) |
| `POST /bbc/continue?pc=&a=&x=&y=&p=` | Let it go on, each register optional |
| `POST /bbc/peek?addr=&len=&bank=&rung=` | Read up to 64 KB into the Pi.  `bank` picks a sideways bank.  Refused on the I/O pages, `&FC00-&FEFF`: reads there have side effects (use `/bbc/run`).  On a running machine with `rung`, one request does the stop, the read and the go |
| `POST /bbc/peek?ranges=0x300:32,0x1900:256&bank=&rung=` | Several ranges in one request and one stop, up to 32 of them and 64 KB in all, their bytes back to back in `/bbc/data` in the order given (`peeked` is the total).  Each is checked as a single peek would be, and one bad range refuses the lot.  `:` and `,` may be sent as `%3A` and `%2C` |
| `GET /bbc/data?offset=&len=` | The last peek's bytes, in hex, up to 4 KB a request |
| `POST /bbc/poke?addr=&hex=&rung=` | Write up to 512 bytes as hex, or up to 32 KB as the raw request body.  Refused on the NMI's `JMP` (`&0D00-&0D02`) and the I/O pages, `&FC00-&FEFF`.  `rung` as for peek |
| `POST /bbc/poke?ranges=...&hex=&rung=` | Several ranges in one request and one stop: their bytes concatenated, in order, as `hex` or the body, which must be exactly the ranges' total.  Nothing is written if any range is refused |
| `POST /bbc/jsr?addr=&a=&x=&y=` | Call code that ends with RTS; up to 5 s |
| `POST /bbc/run?hex=&a=&x=&y=` | Run up to 80 bytes from the Pi's memory at `&FDA0` |

A game agent reading the same few places every frame wants `ranges` with `rung=1` and `wait=1`:
one HTTP request stops the game, reads them all and lets it go on.  The Pi copies as many ranges
as fit in each call it gives the agent (up to four pieces of 256 bytes, three with `bank`), so a
handful of small ranges cost one exchange with the agent, not one each.  `bbcprobe.py peek
0x300:32 0x1900:256` and `poke 0x2100:0102 0x2400:EEEE` do the same from the command line.

`GET /probe` is the same for a person: a page on the Pi1MHz's site with the live state, Halt,
Continue and Peek buttons, the install line and these routes.  Its HTML and script are in
`src/wifi/probe_page.c`; `webserver.c` keeps only the routing.

## Speed on the Model B, over WiFi

| | |
|---|---|
| `GET /bbc` on a kept-open connection | 2.3 ms |
| Stop and go, rung 1 | 4 ms |
| Stop and go, rung 0 | about 20 ms |
| 8 KB poke as a body | 93 ms |
| MODE 7 screens, 1000 bytes each, poked at rung 0 | 48.8 a second |

## What a client has to look after

- **Resets.**  When `resets` goes up, the install went with the Break.  Through `*KEY10` it is back
  within about a second (unless it was installed with `Y%=1`, or after Ctrl-Break).  Anything
  else the client put in, such as hooks on vectors, has to go back too.
- **Vectors put back.**  A program that resets `EVNTV` and `IRQ2V` leaves rungs 0 and 1 nothing
  to answer: disc Elite's first loader copies the OS's default vectors over all of them.
  `bbcprobe.py rehook` points them at the hooks again (by the NMI, if the machine is running, so
  `&0D00` must still be the probe's), with 59 bytes through `/bbc/run` that a client can send
  itself.  From then on rungs 0 and 1 work for the rest of the program, disc accesses and all.
- **Calling the OS.**  A stop can land inside the OS, which isn't re-entrant.  An OSBYTE called
  from a stop that landed in VDU code threw BASIC back to its prompt.  The stopped `pc` and `s`
  say where the machine was.
- **Interrupts are off while the machine is stopped.**  Stop, do the work, continue.
- **Code the Pi calls must not select a helper.**  A stop can land while another helper runs
  from `&FD00` (the help screen, the SD explorer, a ROM loader), or while the transfer page is
  in use: the way out puts back the page the stop found, as that helper left it.  `helpers.c`
  keeps the one page the agent's select replaced, so code run by `/bbc/run` or `/bbc/jsr` that
  writes `&FC88` itself loses it.
- **Timing, at the edges.**  The way in waits for the agent's page, but nothing on either side
  has the bytes to wait more.
  - The way out has 16 cycles (8 µs) between its `&FD` and the `RTI`. A stop that landed in
    another helper's code needs the FIQ to put that page back within that. The firmware's own
    `STX &FC88 : NOP : JMP &FD00`, which starts every helper, allows 2.5 µs.
  - `rp_in` takes `22` at `&FDFF` as the agent's page. A stop that lands while the window shows
    a page whose last byte is already `&16`, with the FIQ slower than one `CMP`, would jump in
    too soon. Probe and helper pages hold their own numbers there.
- **The Pi's audio.**  On the Model B, probe traffic made the Pi's audio into the BBC's speaker
  crackle.  `BeebAudio_Off=1` silences it.

## How a stop works

1. The Pi asks by the rung requested.
2. `rp_in`, in FRED, pushes A (rung 1 first loads it from `&FC`, where the OS left it), writes
   22 to `&FC88`, waits until `&FDFF` reads 22, and jumps to `&FD00`.  The wait matters: the firmware's usual `STX &FC88 : NOP :
   JMP &FD00` trusts the page swap to take under about 3 µs, and the board's FIQ has taken 5.
3. The agent pushes X, Y and the program's services port pointer, reports Y, X, A, P, PC and S
   from the stack at JIM `&E00000`, and polls its mailbox.  To continue with new registers, the
   Pi first has it write them into their places on the stack: with S as reported, A at S, X at
   S-1, Y at S-2, the frame at S+1 to S+3.
4. The Pi writes a command to `&FDF0`, then a new sequence number to `&FDF1`.  The agent acts
   when the number changes, so there is no flag to clear and no race.
5. To leave, the agent restores the port pointer and Y and jumps to `rp_exit`, in FRED, which
   writes `&FD` to `&FC88`, pulls X and A, and returns.  `&FD` (`HELPER_BACK` in `helpers.c`)
   shows again what the agent's select replaced: JIM RAM at the page it was on (the program's
   own, or the transfer page, whose session goes on), or another helper's page with the bytes
   that helper had written into it.  Neither select touches the `JMP`/`RTS` at `&FC8C`, which
   belongs to the stopped code's last select.  A stop that lands in the agent's own way out,
   after the Pi's go and before the `RTI`, selects 22 with 22 already showing: `helpers.c`
   counts it and shows the agent's page afresh, so it is an ordinary stop, and its `&FD` leaves
   that page in place for the outer agent's last instructions.  (The Pi also waits 1 ms after a
   go before an NMI, so this is rare.)  `&FD` with no stop to go back from is `&FF`; before the
   probe it was helper 0, like every number past the last page.

## Limits

- **A program can lock it out** by taking `EVNTV` and `IRQ2V` and turning interrupts off (only
  rung 2 then), or by overwriting `&0D00` as well, and a JAM opcode stops the CPU for good.  Only a Break recovers.  The Pi answers "no answer" rather than hanging.
- **Disc NMIs.**  Acorn DFS writes its own NMI handler over `&0D00` on a disc access.  Rungs 0
  and 1 don't care, but an NMI sent then lands in DFS's handler.  On an 8271 or 1770 interface that handler moves a data byte to
  wherever its transfer pointer was left, so it could change memory with no error reported
  (INFERRED, from how the handlers work; not tried on hardware).  That is why rung 2 needs
  `nmi=1`.  Type the line again to put the `JMP` back.  User-port MMFS uses no NMI and leaves
  it alone.
- **A stop isn't invisible.**  It takes CPU time, and cycle-exact code will notice.
- **The Pi doesn't own the bus.**  Everything goes through the 6502.

## Tests

`src/tests/ramprobe/run_tests.sh` builds the real `ramprobe.c`, `ramprobe_api.c`,
`services_emulator.c`, `helpers.c` (the `&FC88` select) and `ram_emulator.c` (the `&FD00`
window and the JIM page registers) against an emulated BBC, with ASan and UBSan, and runs the probe's pages
from the committed `firmware/Pi1MHz/6502code.bin`.  With beebasm about (on `PATH` or `BEEBASM`)
it first checks that bin is what `6502code.asm` assembles to, so the two can't drift apart.

- **`test_ramprobe.c`, on kc-beeb:** install, halt, peek, poke, calls, all three rungs and their
  failures, 2000 random stops on a program that checks its own registers and on BASIC printing
  through the OS, the program's own port and JIM page, lockout, a call that doesn't return, a
  reset, the FIQ late by up to 200 µs, stops inside another helper, a stop inside a stop, stops
  during a transfer page, a mismatched `6502code.bin`, and the routes with malformed requests.  The
  register checker has a control: spoiling the saved A must be caught.
- **`test_bbcprobe.py`, over HTTP:** the same firmware behind a small HTTP server, driven by
  `tools/bbcprobe.py` as a person would, with `wait=1`, body pokes, and a rehook after the vectors
  are put back.
  `run_tests.sh 8080` serves it for trying another client.

Not covered by the host tests: `webserver.c` itself (the tests use their own HTTP server), and
the board's timing, which the tests model (a select takes effect at once or after a delay, a
FIQ runs late or not).  Code fetched from FRED is: kc-beeb runs the hooks from the board's FRED
bytes.
