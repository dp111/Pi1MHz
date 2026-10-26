# Screen redirector: the copy cursor

STATUS 2026-10-26 (later): REMV report armed by INSV, or by TelnetD's key reports with INSV left
alone while TelnetD tells keys (its cost and the soft-BREAK hang gone, measured); on branch `edit-cursor` (rebased
on master e09ed0b5); hardware-tested on a Master 128 (MOS 3.50) with a 6502
second processor, Zero 2 W kernel7; not on master. Supersedes the 2026-10-23
version (report on every REMV, 468c7843).

## How it works

The MOS moves the copy cursor by calling its VDU code directly, never through
WRCHV, so the redirector's VDU hook cannot see it. OSRDCH takes each key out of
the keyboard buffer through REMV and keeps calling REMV while it waits (~10k
calls/s at a prompt), so helper 2 also hooks REMV and INSV into Pi stubs at
`&FC45-&FC6E` (`fb_edit_cursor_init`):

| Offset | Code |
|---|---|
| +00 | `JMP`/`CMP` old REMV - the Pi switches the opcode (`CMP` keeps V, which REMV takes as examine/remove) |
| +03 | `TXA : BNE +17` / `LDA &0318 : STA +3` / `LDA &0319 : STA +4` / `LDA &D0 : STA +5` |
| +17 | `JMP (+1)` |
| +1A | `CPX #0 : BNE +21 : STA +1A` - other buffers go straight on (8 bus accesses) |
| +21 | `JMP` old INSV - the helper writes it at +1B/+1C, the Pi moves it here |
| +27, +29 | INSV / REMV stub has run since the reset |

- **Idle** the REMV stub is `JMP old`: 3 bus reads. Each insert into any other
  buffer (printer, sound, RS423) pays the INSV stub's 8.
- **A key into the keyboard buffer** (the keyboard interrupt, OSBYTE 138/153)
  runs the INSV stub, whose write makes the Pi switch REMV's opcode to `CMP`,
  which falls into the report: on X=0 calls it hands the Pi `&0318`/`&0319`
  and `&D0`, and the Pi queues a change as `VDU 23,0,255,on,x,y`.
- **100 ms after the last key** (`EDIT_ARM_US`) the next report puts the `JMP`
  back; a copy cursor still on is sent again at the next report. Both happen in
  the FIQ - no poll, no race with the arming.
- **TelnetD's keys** skip the INSV vector: the serial stub inserts with
  `JSR` to the INSV it saved, which is behind ours when helper 2 runs after
  TelnetD's reset claim. `ser_rx_consume` arms the stub itself
  (`fb_edit_cursor_arm`) when a byte goes into the keyboard buffer in telnet
  mode.
- **While TelnetD tells the Pi each key taken** (it claimed REMV: `&26` at the
  reset, and telnet mode), the Pi arms the stub on each report (`&24`/`&25`,
  any key, local or telnet) and sets the INSV stub's live byte, so helper 2
  leaves INSV alone (`fb_edit_cursor_keys_told`). TelnetD's REMV handler
  re-hooks on every keyboard REMV and is quick only while INSV is the serial
  stub itself; with our stub in front it cost ~57 us a REMV.
- **ESCAPE** goes round INSV (the MOS raises the escape condition instead); the
  line BASIC then starts (VDU 13) drops the copy cursor on the Pi.
- While the cursors are split (`&D0` bit 6) the copy cursor is the text cursor
  `&0318`/`&0319`; the write cursor waits in `&0364`/`&0365`. The MOS swaps them
  only while it prints (MOS 1.20 `&C56A`, 3.20/3.50 `&E2AC`), which interrupts
  can land inside - hence X=0 only.
- A Master's BREAK can leave a stub in the chain behind a ROM's reset claim
  (TelnetD claims REMV at reset); hooking it in again would loop. The Pi clears
  the live bytes at each reset and sets them when a stub runs; helper 2 skips a
  vector whose stub is live. The REMV stub starts armed after a reset, so it
  runs (and goes live) at the first REMV; the keys typed to run helper 2 go
  through INSV.
- Helper 2 hooks both vectors in one loop over a 3-byte table; the page had 3
  bytes free, so `cursoronoff` sends `VDU 23,1,n` + seven zeros from a loop
  instead of two 10-byte tables.

## Measured (2026-10-25, Master + Tube, 3000 `INKEY(0)` after a 200 ms pause)

| REMV chain | new stub idle | bypassed (control) | report every call (old) |
|---|---|---|---|
| stub -> MOS | 126, 126, 126 cs | 124, 124 | 136, 136 |
| stub -> TelnetD -> MOS | 214, 214, 214 | 213, 213 | 224, 224 |

So the stub costs a polling program ~3-7 us per call instead of ~37-40 us (1 cs
resolution). Stub opcode read back `&2C` straight after RETURN and `&4C` after
200 ms of `INKEY(0)` polling.

Behaviour on hardware: copy cursor tracks keyboard cursor keys and COPY, each
key a second apart (re-armed every time); ESCAPE clears it; running helper 2
twice does not double-hook; 4/4 BREAK / CTRL-BREAK with TelnetD hooked, then
helper 2, copy cursor right; telnet cursor keys track (the build before the
`fb_edit_cursor_arm` call did not - the control).

2026-10-26, after `CMP` and the X-first INSV stub, chain INSV -> stub -> serial
stub -> MOS, 3000 `OSBYTE 138` into the printer buffer: 108, 108, 108, 108 cs;
INSV bypassed (set to the serial stub by one SEI routine) 106, 105 - ~8 us an
insert (17 us with the old `BIT ... JMP ()` stub: 116 vs 111).

**TelnetD makes an INSV claim cost every REMV (MEASURED 2026-10-26).** With
TelnetD on, 3000 `OSBYTE 145` (REMV, X=0) take 195 cs while INSV is our stub
and 178 when INSV is the serial stub itself (`INKEY(0)`: 215 vs 198); null
`OSBYTE 0` 86 either way. A counting INSV handler in our stub's place was as
slow (194) and counted no INSV calls during the loops, so it is INSV's value,
not its calls: TelnetD's REMV handler runs `rehook` on every keyboard REMV,
which is quick only when INSV is `&FCCC` (telnetd.asm `.rehook`, INFERRED from
the source). Helper 2 run after TelnetD's reset claim puts our stub in front
of the serial stub - ~57 us a REMV, more than the REMV stub saves. Fixed by
leaving INSV alone while TelnetD tells keys: after a CTRL-BREAK boot running
helper 2, INSV = `&FCCC` -> `&EA43` (our stub not in the chain) and 3000
`OSBYTE 145` take 176, 175 cs (MEASURED; the 195/178 above are from earlier the
same day). The copy cursor still follows the local keyboard: up, up, COPY,
COPY after a `PRINT` line - the underline blinks under the third character of
the line above (HDMI frame diff), with nothing else to arm it.

**Helper 2 from `!BOOT` after a soft BREAK hangs the Master (MEASURED
2026-10-26, Master + Tube + TelnetD).** After a soft BREAK both stubs stay in
the chains behind the reset claims: INSV -> serial stub `&FCCC` -> its kept
INSV `&FC5F` (ours) -> `&EA43`; REMV -> `&FF42` (TelnetD) -> REMVOLD `&FC45`
(ours) -> `&E9F8`. The live bytes are clear after the reset and `*EXEC` reads
without REMV, so helper 2 from `!BOOT` hooks both again, each pointing back
at the claim in front of it. `!BOOT` = `*FX147 136 2` + `*GOIO FD00`, ADFS
option 3, `*CONFIGURE BOOT`: after BREAK the boot lines are echoed, then no key
is echoed (twice; Pi alive). Control, same state, `!BOOT` without `*GOIO
FD00`: responsive. CTRL-BREAK puts the vectors back: the same boot is fine.
(A Master reads BOOT from CMOS only at a hard reset; the first plain BREAK after
`*CONFIGURE BOOT` does not boot.)

The explanation above (helper 2 hooking a stub that is already in the chain) is
**withdrawn**: in the runs that did not hang, helper 2 from `!BOOT` hooked both
stubs over the MOS's own vectors (stub operands `&E9F8`/`&EA43`), and TelnetD
claimed in front of them afterwards (service `&27` comes after the boot), so
the vectors were reset by the soft BREAK. Control matrix, same `!BOOT`, a
CTRL-BREAK boot then a soft BREAK, MEASURED 2026-10-26:

| kernel | helper 2 | INSV before the soft BREAK | soft BREAK |
|---|---|---|---|
| 9c0d2951 | 9c0d2951 | our stub -> serial stub | hung (3/3 over the day) |
| 9c0d2951 | + REMV probe | our stub -> serial stub | hung (1/1) |
| + keys told | 9c0d2951 | serial stub -> MOS | fine (1/1) |
| + keys told | + REMV probe | serial stub -> MOS | fine (5/5, 4 from the post-BREAK chain) |

So it is our INSV stub in front of TelnetD's serial stub at a soft BREAK that
hangs; why is not known (not investigated: what the Master resets, TelnetD's
`&27` path with INSV in FRED but not the stub, its key report landing at
`&FC28` + INSV's low byte). Keys told keeps it from arising with TelnetD; a
helper 2 run while TelnetD is loaded but not telling keys could still put it
there (untested). The REMV probe (helper 2 examines the keyboard buffer,
OSBYTE 152, before testing the live bytes) is neither needed nor sufficient on
this evidence, and was dropped (9a138afe, reverted).

The old REMV / INSV the stubs saved on this Master were `&E9F8` / `&EA43`,
`&3C` above the MOS 3.50 disassembly in the scratchpad - that ROM file is a
different build from this machine's MOS.

## Limits and future work

1. A key typed ahead while a program is busy and read more than 100 ms later
   shows its cursor move only at the next key.
2. RS423 input (`*FX2,1`, buffer 1) is not tracked (X=0 only), as before.
3. **Untested:** a BBC B with MOS 1.20 on hardware, modes other than 7, the
   Zero W and Pi 2 kernels.
4. **Hook choice - REMV + INSV.** Considered and rejected: RDCHV (moves show
   only after the next real key), WRCHV (never sees moves), KEYV / event 2 (fire
   before the key is acted on; TelnetD also claims KEYV), the VSYNC event (in
   the interrupt it can catch the cursor swap), the CRTC cursor registers
   (`&FE00`, not on the 1MHz bus), JIM (paged).
5. **TelnetD (separate, pre-existing):** a telnet session that connects during
   an edit moves the Beeb's write cursor to the copy cursor - its screen read
   ends with a `VDU 31` back to the text cursor, which during an edit is the
   copy cursor and is printed with the cursors swapped.
6. **Unexplained (2026-10-23):** two Pi power-on restarts right after
   `*FX147,&5A,&42` on diagnostic builds of this branch; not reproduced since.

## Bench notes

`/vdulog` prints oldest-first into 64 KB and truncates, so its tail is not the
newest entries. Each new telnet session makes TelnetD read the screen, so do a
whole test in one session. The HDMI capture stick buffers frames; take
separate single captures. The copy cursor is a flashing underline: diff 4
frames ~0.3 s apart to find it. After a chain-boot, sideways RAM is empty -
load TelnetD with helper 12 and BREAK.
