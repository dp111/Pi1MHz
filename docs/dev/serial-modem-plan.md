# Serial redirect + modem emulation — plan

Redirect both directions of the BBC's RS423 traffic to Pi1MHz, then put a
Hayes-style modem behind it so `ATDT <ip>` opens a TCP connection instead of
dialling.

The whole Beeb-side driver is **40 bytes of 6502 living in FRED**, served by the
Pi, using no Beeb memory at all — same idiom as the screen redirector at
`&FCA0`. Everything else is C on the Pi.

---

## 1. The idiom

The screen redirector (`src/framebuffer/framebuffer.c:2478`) is the model:

```
FCA0: 8D A0 FC   STA &FCA0      ; the store IS the transfer
FCA3: 4C xx xx   JMP old_wrchv  ; operand stashed here at install time
```

`WRCHV` points into FRED and the 6502 executes the Pi's bytes. Four properties
fall out of that, and this design leans on all of them:

1. **Reads and writes at one FRED address are independent.** `WRITE_FRED` and
   `READ_FRED` are separate callback tables, and a write handled by a callback
   never touches `Pi1MHz->Memory`. So `&FCA0` reads as the opcode `8D` forever
   while writes to it are data going to the Pi. A write port costs no address
   space — overlay it on a code byte, even on an *opcode* byte.
2. **An instruction fetch is a bus read, so an immediate operand can BE a data
   port.** The Pi keeps the next byte in the operand of `LDA #xx`; the fetch
   delivers it. Two bytes, no dedicated address. (See the branch-shadow hazard
   in §3.)
3. **Spare FRED bytes are RAM.** Register `Pi1MHz_EmulatedMemoryByte` and the
   6502 can write them — which is how old vectors get stashed *inside the `JMP`
   operands*, with no separate storage.
4. **The Pi can rewrite the code between calls.** Flow control becomes a byte
   the Pi maintains, not instructions the 6502 executes.

Setup code is transient and lives in the JIM helper page at `&FD00`
(`6502code/6502code.asm`), so it costs no Beeb RAM either.

---

## 2. Where to cut in

The 6850 at `&FE08`/`&FE09` and the serial ULA at `&FE10` are not on FRED/JIM,
so Pi1MHz cannot see or claim them. Something on the 6502 must move the bytes;
the only question is where.

**Do what the 6850 does.** The real data paths are:

- **Output** — `OSWRCH` with the RS423 stream enabled inserts into buffer 2 via
  `INSV`; the ACIA's TDRE interrupt drains buffer 2 through `REMV` into `&FE09`.
- **Input** — the ACIA's RDRF interrupt reads `&FE09` and calls `INSV` with
  `X=1`, filling buffer 1. Everything downstream — the `OSRDCH` wait,
  `ADVAL(-2)`, `OSBYTE 145`/`152`, ESCAPE, `*FX2` stream selection — is built on
  buffer 1 actually containing bytes.

So: **hook `INSV` for the output half, and hook `IRQ1V` to play the part of the
receive interrupt for the input half.**

An earlier draft hooked `REMV` instead and served input straight from the Pi's
FIFO, skipping buffer 1 entirely. It was smaller (23 bytes) but it rested on an
unverified assumption — that `OSRDCH`'s wait loop calls `REMV` rather than
watching buffer state that only the receive interrupt updates. Filling the real
buffer 1 removes that assumption, and with it the need for `REMV`, `CNPV` and
examine hooks. 40 bytes, no open questions.

Conventions (MOS 1.20 disassembly — worth re-checking against your copy):

```
INSV  (&022A)  entry: A = byte, X = buffer number
               exit : C=0 inserted, C=1 buffer full; A,X preserved
Buffers        1 = RS423 input,  2 = RS423 output
IRQ1V (&0204)  entered with A saved at &FC; X and Y must be preserved
```

### Silencing the 6850

Set the ACIA to "RTS high, both interrupts off, 8N1" via `OSBYTE 156` (`&55`) so
the MOS's own copy of the control byte stays in step, and the real chip cannot
raise RDRF or TDRE alongside ours. Done once in the JIM setup code.

`&55` = `0101 0101`: bit 7 = 0 (RX interrupt off), bits 6:5 = `10` (RTS high, TX
interrupt off — on a 6850 those are one field, and the RTS-high encoding is the
only way to disable TX interrupts without transmitting a break), bits 4:2 =
`101` (8N1), bits 1:0 = `01` (÷16).

### Honest limitation

Software that drives `&FE08`/`&FE09` itself is not caught. Retargeting such a
driver is a two-address patch, but it is a patch. Which packages need it should
be checked per-package, not assumed.

---

## 3. The FRED stub — 40 bytes

New emulator `Modem` in `src/serial_modem.c`, base `&FCB0` (free today —
`&FCA0`–`&FCAC` is framebuffer/services/mouse), overridable with
`Modem_addr=0xB0`, disabled with `Modem_addr=-1`, and gated off by default
behind `modem_enable=1` in `Pi1MHz.cfg` as `net_enable` gates the net service.

```
; ---- INSV hook: RS423 output (Beeb -> Pi) --------------------------
FCB0  E0 02     CPX #2         ; ours?     read: opcode / write: TX data port
FCB2  D0 05     BNE FCB9
FCB4  18        <FLAGTX>       ; Pi maintains: 18 CLC = room, 38 SEC = full
FCB5  8D B0 FC  STA &FCB0      ; hand it over; the Pi drops it if full
FCB8  60        RTS            ; C already correct
FCB9  4C 00 00  JMP old_insv   ; FCBA/BB stashed at install
                               ; also the trampoline used below

; ---- IRQ1V hook: RS423 input (Pi -> Beeb) --------------------------
FCBC  4C 00 00  <GATERX> old   ; the Pi flips ONE byte:
                               ;   4C = JMP old_irq1v - no byte, straight out
                               ;   2C = BIT old_irq1v - byte waiting, falls through
                               ; write to FCBC: "consume one byte"
                               ; FCBD/BE stashed at install
FCBF  8A 48     TXA PHA
FCC1  98 48     TYA PHA
FCC3  A9 xx     LDA #<byte>    ; NON-consuming peek; the operand is the port
FCC5  A2 01     LDX #1         ; buffer 1 = RS423 input
FCC7  20 B9 FC  JSR &FCB9      ; -> JMP old_insv, the real MOS INSV
FCCA  B0 03     BCS FCCF       ; buffer 1 full: leave the byte with the Pi
FCCC  8D BC FC  STA &FCBC      ; consume
FCCF  68 A8     PLA TAY
FCD1  68 AA     PLA TAX
FCD3  A5 FC     LDA &FC        ; restore the MOS's saved A
FCD5  4C 00 00  JMP old_irq1v  ; FCD6/D7 stashed at install
```

`&FCB0`–`&FCD7` inclusive, and not one byte of it is idle.

### The tricks, spelled out

**Flow control costs zero instructions.** `&FCB4` and `&FCBC` are opcodes the Pi
rewrites. On the output side `&FCB4` is `18` (`CLC`) or `38` (`SEC`): the 6502
executes one and carry is already the value `INSV` must return — the flag is
free there precisely because carry *is* the answer.

That ordering — flag *before* transfer — is what makes it race-free with no
handshake. Only the Beeb fills the TX FIFO and only the Pi drains it, so "has
room", observed at the flag byte, is still true one cycle later at the `STA`.
Draining only frees more. Monotone, so the flag can never be stale in the unsafe
direction and nothing depends on FIQ latency.

The full case is idempotent too: if `FLAGTX` says full the 6502 still executes
the `STA`, the Pi **silently drops it**, and `C=1` tells the MOS to retry the
same byte later. Sent exactly once — no duplicate, no loss, no protocol.

**On the input side the gate is the jump itself.** `&FCBC` is `4C` (`JMP`) or
`2C` (`BIT`) over one shared operand, the stashed `old_irq1v`. No byte waiting
and the 6502 jumps straight out in 3 cycles without touching a register; a byte
waiting and `BIT` reads the same address harmlessly and falls through into the
handler. That is the whole test — no flag byte, no branch, and the "not ours"
path (every interrupt on the machine) drops from 8 cycles to 3. The carry trick
earns its place on the output side, where carry *is* `INSV`'s return value; on
the input side it was only feeding a branch, and the branch can be deleted.

The gate now controls *entry*, not just a branch, which sharpens one Pi-side
invariant: **`2C` if and only if the peek byte at `&FCC4` is valid.** Leave it
at `2C` with an empty FIFO and every interrupt on the machine falls into the
handler and re-injects the stale peek byte. Order the Pi's writes accordingly —
peek byte first, then the gate — the same flag-before-transfer discipline as the
output side.

**Why one byte and not three.** The tempting version rewrites all three bytes
between `JMP old_irq1v` and `TXA PHA TYA`. It races: an interrupt can begin
executing at `&FCBC` on any cycle and the Pi has no lock against the 6502. Catch
a rewrite mid-instruction and the CPU takes `4C` with `48 98` as its target — a
jump to `&9848` — or takes `8A` and then executes the vector bytes as code.
Flipping a single byte between two *same-length* opcodes over a *constant*
operand keeps exactly the property the `CLC`/`SEC` flag had: whatever the 6502
fetches is a complete valid instruction, and the bytes after it never move.
`2C` is `BIT abs` on both NMOS and CMOS, 4 cycles on both, and touches no
register — only N/V/Z, which this path already trashes via carry.

**`BIT` does a real bus read of the old vector.** Harmless for a MOS ROM
address. But if something has already chained `IRQ1V` into `&FC00`–`&FDFF` —
another FRED-resident stub, or this one installed twice — that read lands in the
Pi's own window and could hit a side-effecting port. The install code refuses to
hook when `old_irq1v` points into FRED or JIM.

**`JSR &FCB9` reuses the `INSV` stub's own `JMP`** as the trampoline to the real
MOS `INSV`. One stashed vector serves both paths. Calling `INSV` from `IRQ1V` is
exactly what the MOS's own ACIA handler does, so it is a supported context.

**`&FCBC` is an opcode when read and a command when written.** Reads fetch the
gate opcode (`4C`/`2C`); the `STA` at `&FCCC` means "advance the FIFO". Because
write callbacks never touch `Pi1MHz->Memory`, the opcode survives.

**The old `IRQ1V` vector is stored twice** — at `&FCBD/BE` as the gate's operand
and at `&FCD6/D7` as the exit `JMP`. That is two extra stores in the transient
install code and costs no permanent space. If one copy is preferred, make the
exit `6C BD FC` (`JMP (&FCBD)`): same 3 bytes, 2 cycles slower on the rare
byte-waiting path, and `&FCBD` is nowhere near a page boundary so the NMOS
indirect-`JMP` bug does not apply.

**Peek-then-consume closes the loss window.** The operand at `&FCC4` advances
nothing — the Pi just keeps the FIFO head byte there. If `INSV` returns `C=1`
the consume is skipped and the byte stays with the Pi. Consequence: **no
`READ_FRED` callbacks anywhere in this design**.

That is worth less than it sounds, and the reason matters for future changes.
The VPU rings the doorbell on *every* FRED/JIM cycle in both directions
(`vidcore/Pi1MHzvc.s:134` read, `:155` write) and `src/FIQ.s` folds RNW into the
callback-table index and returns early on NULL — so the FIQ entry is already
being paid on every one of the stub's own instruction fetches. What a
`READ_FRED` callback would add is only the `push`/`blx`/`pop` and the handler
body, on that one address, exactly what the `WRITE_FRED` consume costs today.
So "no read callbacks" buys tidiness, not cycles; if a read port ever earns its
place, the FIQ cost is not the argument against it.

### Stack balance

Three exits, three stack states:

| Path | Exit | Stack |
|---|---|---|
| No byte waiting | the gate itself — `JMP old_irq1v` | nothing pushed, nothing popped |
| `INSV` accepted it | falls through | 2 pushed, 2 popped |
| Buffer 1 full | `BCS` → `&FCCF` — the restore | 2 pushed, 2 popped |

The gate exiting as a *jump* rather than a branch to `&FCD5` is what keeps the
first row honest: it leaves before the pushes at `&FCBF`–`&FCC2` exist, so there
is no way for a mis-targeted branch to land on the two `PLA`s and pull the
return address off the stack. That path runs on every interrupt on the machine
that is not ours, so such a crash would be immediate. It also skips `LDA &FC`
correctly — `A` is untouched when only the gate has executed.

The remaining `BCS` at `&FCCA` **must not** target `&FCD5`: by then two bytes
are pushed and skipping the `PLA`s would unbalance the stack the other way.

`X` is pushed before `Y`, so `PLA TAY` / `PLA TAX` unwinds in the right order.

### The branch-shadow hazard

**A taken branch performs a real bus read of the byte at PC+2** before adding
the offset. Put a side-effecting read port there and every not-taken dispatch
silently eats a FIFO byte — a dropped character that only appears under some
other device's traffic, which is a miserable bug to find.

Replacing the `FLAGRX`/`BCS` pair with the gate removes one branch, and so one
shadow site. The one that remains is arranged so the shadow lands on an opcode:

```
FCCA  B0 03     BCS FCCF
FCCC  8D        <- dummy fetch lands here. STA. Safe.
```

This is also why the consume stays a `STA` and does not become a read-triggered
port at `&FCCC`: the taken branch above reads exactly that byte, and taken means
"buffer 1 full, do NOT consume". A read port there would eat a character on the
one path that must not, visible only under buffer pressure. If that consume is
ever made a read port to save the byte, it needs an inert opcode absorbing the
shadow first — e.g. `B0 02 / A9 xx`, where `A9` takes the dummy fetch and the
operand at `&FCCD` is the port.

Keep that invariant if the stub is rearranged. The whole stub is inside page
`&FC`, so no branch crosses a page and there is no second dummy fetch.

### Registration

Mirrors `fb_emulator_init`:

| Address | How |
|---|---|
| `&FCB0` | `WRITE_FRED` callback — TX byte (leaves the `E0` opcode intact) |
| `&FCBC` | `WRITE_FRED` callback — consume one RX byte (leaves the gate opcode intact) |
| `&FCBA/BB`, `&FCBD/BE`, `&FCD6/D7` | `Pi1MHz_EmulatedMemoryByte` — the install code writes the stashed vectors |
| `&FCB4`, `&FCBC`, `&FCC4` | `Pi1MHz_MemoryWrite` — the TX flag, the RX gate opcode and the peek byte, maintained by the Pi |
| everything else | `Pi1MHz_MemoryWrite` once at init — static code |

No `READ_FRED` callbacks.

### Cost

`INSV` ours: `CPX` 2 + `BNE` 2 + flag 2 + `STA` 4 + `RTS` 6 = **16 cycles**. Not
ours: 8 cycles on every keyboard, sound and printer buffer operation.

`IRQ1V` not ours: the gate alone, `JMP` 3 = **3 cycles on every interrupt on the
machine** (was 8 with a flag byte plus `BCS` plus the exit `JMP`). Ours: `BIT` 4
in place of `CLC` 2 + `BCS` 2, i.e. unchanged. These are FRED fetches on a
clock-stretched 1 MHz bus, so budget roughly double.

| `IRQ1V` path | Flag + branch | Gate | 
|---|---|---|
| Not ours (every interrupt) | 8 cycles, 3 bytes | **3 cycles**, 3 bytes |
| Byte waiting | 4 cycles | 4 cycles |

### The livelock, which must be designed in from the start

One byte per interrupt, self-clocking: nIRQ is level-sensitive, so the Pi holds
it until the FIFO drains and the MOS re-enters immediately after `RTI`. That
gives roughly a 50 KB/s ceiling, far above any modem rate.

**But if buffer 1 fills and the Beeb stops reading, nIRQ stays asserted and the
machine livelocks.** This is precisely the failure that froze the Beeb during
net service Stage 1. The Pi must back off, and it can detect the condition with
no extra 6502 bytes: it is asserting nIRQ and no consume-write is arriving.
Deassert for a few milliseconds, then retry. Build this in first — not after the
first freeze.

nIRQ is armed only when the port is installed and enabled, never by default.

---

## 4. The install code

A new helper page in `6502code.asm`. `helpers.c:139` already range-checks
against `sizeof(helper_ram)>>8`, so page 16 needs only `helper_ram[4*1024]` →
`[8*1024]` and `SAVE ..., 0, &2000`.

```
; Page 16
; Serial / modem redirector
{
ORG &FD00

ser   = &FCB0            ; stub base (Modem_addr)
INSV  = &022A
IRQ1V = &0204

    JMP setupwithmessage            ; &FD00 default entry
    JSR setup                       ; &FD03 quiet entry
    JMP serialexit

.setup
    LDA ser     : CMP #&E0 : BNE noport     ; stub present?  (CPX #2)
    LDA ser+&05 : CMP #&8D : BNE noport     ; (STA &FCB0)

    ; NB probe only STATIC bytes.  &FCB4 (TX flag) and &FCBC (RX gate) are
    ; rewritten by the Pi, so their value depends on FIFO state at probe time
    ; and proves nothing.

    LDA INSV+1 : CMP #(ser DIV 256) : BEQ done      ; already installed

    ; The RX gate's BIT reads through the old IRQ1V vector on every one of our
    ; interrupts, so it must not point into the Pi's own window: a chained
    ; FRED/JIM handler would turn that read into a port access.
    LDA IRQ1V+1 : AND #&FE : CMP #&FC : BEQ noport  ; old IRQ1V in FC/FD? refuse

    LDA INSV+0  : STA ser+&0A       ; stash old INSV inside the JMP operand
    LDA INSV+1  : STA ser+&0B
    LDA IRQ1V+0 : STA ser+&0D       ; old IRQ1V twice: the gate operand (&FCBD) ...
    LDA IRQ1V+1 : STA ser+&0E
    LDA IRQ1V+0 : STA ser+&26       ; ... and the exit JMP (&FCD6)
    LDA IRQ1V+1 : STA ser+&27

    SEI                             ; both vectors are live in IRQ context
    LDA #(ser MOD 256)       : STA INSV+0
    LDA #(ser DIV 256)       : STA INSV+1
    LDA #((ser+&0C) MOD 256) : STA IRQ1V+0
    LDA #(ser DIV 256)       : STA IRQ1V+1
    CLI

    LDA #156 : LDX #&55 : LDY #0 : JSR OSBYTE   ; ACIA: RTS high, no IRQs, 8N1
.done
    CLC : RTS
.noport
    SEC : RTS

.setupwithmessage
    JSR setup
    LDX #(msgok MOD 256)  : BCC print
    LDX #(msgbad MOD 256)
.print
    LDA &FD00,X : BEQ serialexit
    JSR &FFEE : INX : BNE print
.serialexit
    PAGERTS

.msgok  EQUS " Serial redirector enabled.", 13, 10, 0
.msgbad EQUS " No Pi1MHz serial port.", 13, 10, 0

    ENDBLOCK &1000
}
```

- **The presence probe is free**, and only possible because the code lives in
  FRED. With `modem_enable=0` the stub is absent and hooking would jump into
  nothing.
- **Don't use `PRTSTRING`** — 5 bytes per character, so two messages alone would
  overflow the 256-byte page. The loop is ~12 bytes plus strings.
- **Stash before you hook.** Writing the FRED operands is safe at any time
  because nothing executes the stub until the vectors point at it; the `SEI`
  need only cover the four vector bytes.
- **The base address is patchable by the Pi.** `helper_ram` is RAM and
  `helpers_screen_setup` already generates page 0 dynamically, so if
  `Modem_addr` is overridden the Pi can fix up the immediate operands in this
  page before serving it.
- On `BREAK` the vectors reset and the redirect is lost, as with the screen
  redirector. Re-run the helper.

Uninstall is free if wanted: the Pi writes `4C <old> <old>` over `&FCB0`, and
`&FCBC` only has to be left at `4C` — its passthrough is the resting state of
the gate, not a special case.

---

## 5. Stage B — the modem

The AT parser belongs on the **Pi, in C**: string handling with a state machine
and a socket behind it, host-testable with byte vectors the way `net_telnet.c`
already is. Stage B needs **zero changes to the 40-byte stub** — the pipe
carries AT commands and responses like any other bytes, which is exactly why
real comms software will drive it.

```
COMMAND  --- "ATD..." --> DIALLING --- connected --> ONLINE
   ^                          |                        |
   |                       failure                  "+++" (1s guard)
   |                          v                        |
   +----- "NO CARRIER" <------+                        |
   +--------------------- ATH / peer FIN --------------+
                                                    ATO -+
```

- **COMMAND**: bytes from the TX FIFO accumulate into a line buffer, echoed back
  into the RX FIFO when `E1`, parsed on CR.
- **ONLINE**: TX FIFO → TCP socket; socket → RX FIFO, through `telnet_filter()`
  when `NET1`.
- **DIALLING**: async DNS + connect off the existing net poll. Success →
  `CONNECT 9600`; failure → `NO CARRIER` / `BUSY`.
- **Inbound**: bind + listen on `modem_answer_port`, emit `RING`, auto-answer
  after `S0` rings — so you can telnet *into* the Beeb.

Carrier loss is delivered in band as `NO CARRIER` pushed into the RX FIFO.
(There is no DCD line to drive: the Beeb sees a buffer, not a 6850. Software
that polls the real ACIA status for carrier is in the "needs a patch" category
already.)

### Dialling by IP

Three forms, because old software validates dial strings differently:

| Form | Example | Notes |
|---|---|---|
| Host/IP + port | `ATDT bbs.example.com:6502` | the natural one |
| Digits-only IP | `ATDT192168001005` | 12 digits → `192.168.1.5`, for diallers that reject non-digits |
| Phonebook slot | `ATDT1` | from `modem_phone_1=host:port` in `Pi1MHz.cfg`, also settable with `AT&Z1=` |

Default port 23 when none given.

### Command set

`ATZ`, `AT&F`, `ATE0/1`, `ATV0/1`, `ATQ0/1`, `ATH`, `ATO`, `ATA`, `ATI`,
`ATD[T|P]`, `ATS<n>=<v>`, `ATS<n>?`, `AT&Z<n>=`, `AT&C`, `AT&K`, plus `ATNET0/1`
(telnet IAC filtering, the tcpser/WiModem convention) and `AT&T1` local
loopback. Result codes verbose and numeric.

`net_service.c` already provides the TCP socket, DNS, listen/accept and the 8 KB
rings; `net_telnet.c` provides the IAC filter and outbound escaping. The modem
calls into those rather than opening its own pcbs.

---

## 6. Bring-up order

1. **Pi**: the FRED stub + `AT&T1` loopback only. No network, and the RX/nIRQ
   half **disabled** — output only.
2. **Beeb**: `*FX147,136,16 : *GO FD00`, then `*FX3,3` and `VDU` some text.
   Watch it arrive on the Pi. That proves the `INSV` hook alone.
3. Enable the RX half and the loopback. `*FX2,1` and type — characters should
   round-trip. This is the step that exercises nIRQ, so test the livelock
   deliberately: send a flood with the Beeb not reading and confirm the Pi backs
   off instead of freezing the machine.
4. Confirm `ADVAL(-2)`, `OSBYTE 145` and a blocking `A%=GET` all behave — they
   should, because buffer 1 is real.
5. **Pi**: AT parser + TCP. `ATDT` to the WSL test server already standing up
   for the AUN work, then a public telnet BBS.
6. Real comms software. MOS-mediated packages should work untouched;
   direct-ACIA packages need the two-address patch.
7. XMODEM binary transfer — finds flow-control edges and any `IAC` escaping
   mistake in one go.

Host tests alongside: AT-parser byte vectors and a FIFO/flow-control fuzz, in
`src/tests/` next to the existing net and telnet suites.

---

## 6a. Review findings (2026-08-23, unaddressed)

A verification pass against the current tree found concrete conflicts to
resolve before implementing:

1. **`&FCB0` collides with the mouse redirector's pointer-type register**
   (`mouseredirect.c`, `&FCAC`-`&FCB0` block) - the stub's port must move.
2. **The plan's `&FCB0`-`&FCD7` span swallows the live `*FX` status port
   at `&FCCA`/`&FCCB`** (`Pi1MHZ_FX_CONTROL` in `Pi1MHz.c`) - re-survey
   free FRED space against what is registered today, not the docs.
3. **The proposed helper_ram bump to 8 KB makes bogus page selects execute
   zeros** - the helper page needs a guard, not just more room.
   (Three further smaller points in the review log.)

## 7. Open items

1. **Confirm the `INSV` register convention** (`A` = byte, `X` = buffer, `C=1` =
   full) against your MOS disassembly. The stub rests on it.
2. **Confirm `&FC` is the IRQ accumulator save** on every target MOS, and that
   restoring it before chaining is right rather than merely harmless.
3. Whether the digits-only dial form and the phonebook earn their code.
4. Which comms packages are the real targets — that decides how much effort the
   direct-ACIA retarget path deserves.
