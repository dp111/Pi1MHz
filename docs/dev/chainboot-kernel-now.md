# `kernel.now` chain-boot: what the copy actually touches

STATUS (2026-10-06, later): **kernel.now is no longer refused after video
use; the player shuts video down and hands the VideoCore connection on
instead** - see "Video across the jump" below. COMPILE- AND HOST-TESTED
ONLY: none of it has run on a Pi.

STATUS (2026-10-06): **the hand-over was rebuilt so that nothing it uses lies
in the copy's path** (review 2026-10-06 P2) - see "The handover" below.
COMPILE-VERIFIED ONLY: it needs cold-boot and kernel.now tests on a Pi Zero/1
and a Pi 3/Zero 2 W before anything here can be called fixed. The
non-deterministic failure described next has NOT been re-measured against it;
the rebuild removes hazards that were real (the copy running over the live
page table, the stack and the DMA control blocks, and the chain marker lost
for any image over ~806 KB), not a cause that was ever demonstrated. The
sections after "The handover" describe the old in-place copier and are kept
as the measurement record.

STATUS (2026-09-24): **the failure is real but NON-DETERMINISTIC.** The same
image onto the same running kernel both failed and succeeded on the same day,
so no rule in terms of image size or image content can be correct. Size,
content and the audio-DMA hypothesis have each been tested and eliminated; **no
suspect currently stands.** Use the two-hop push as a workaround and judge every
push by its banner. One failure left the VideoCore wedged rather than falling
back to the card, which is the most alarming thing here and is recorded below.

Large images do chain-boot: real-content and non-zero-padded images up to
900,000 bytes hand over cleanly on unmodified master, and the believed
557,056-byte threshold does not exist. But failures do occur, roughly 3 in 16
pushes on a busy day, and every static explanation tried so far has been
eliminated by measurement. No code change came out of this; the section on how
to test is the part worth keeping.

## The handover

`chainboot_poll()` (`src/chainboot.c`) takes the staged image from MTP or the
webserver and, from the main loop, takes USB off the bus, ejects the card,
tells the WiFi chip to stop signalling, disables interrupts, stops both audio
DMA channels (`audio_stop_dma`) and writes the chain-boot marker (cleaning
it to RAM at once). Then it calls `_copyandreboot(src, len)` in
`src/rpi/arm-start.S`, with the MMU and both caches still on. (Until
2026-10-06 the D-cache was turned off first: the old copier ran inside the
region it overwrote and could not do cache maintenance of its own. That
reason went with it, and an uncached copy of up to 4 MB was slow.)

Since 2026-10-06 the copy no longer runs in place. Everything the hand-over
needs lives below the kernel, at fixed addresses defined in
`src/rpi/lowmem.h`:

| address | what |
|---|---|
| 0x0100-0x13FF | Pi1MHz struct and callback table (unchanged) |
| 0x3A00-0x3CFF | the VPU's 1MHz-bus program (was run from `.rodata`) |
| 0x3D00 | chain-boot marker: `CHAIN_MAGIC`, its complement, the jump's 64-bit timer stamp and reset reason (5 words) |
| 0x3D20 | the video player's persisted GPU handles (was 0x7C20) |
| 0x3E00-0x3EFF | the copier |
| 0x4000-0x7FFF | the L1 page table (was `PageTable` in `.noinit`) |
| 0x8000- | the kernel |

`_copyandreboot` copies a small position-independent routine to 0x3E00,
cleans it out of the D-cache (DCCMVAU per line on `kernel7.img`, clean
entire D-cache on `kernel.img`), invalidates the I-cache, branch predictor
and prefetch (with the ARM1176 erratum 411920 workaround on `kernel.img`;
DSB/ICIALLU/BPIALL/DSB/ISB on `kernel7.img`) and branches to it. That
routine copies the image to 0x8000 with registers only - no stack - and
with the caches on, then cleans and invalidates the whole data side to the
point of coherency (set/way over every level to LoC, the A53's L2 included;
clean+invalidate entire D-cache on the ARM1176): the copy is still partly in
dirty lines, and this puts it, and everything else below 0x8000, in RAM.
Only then does it turn the MMU and caches off, invalidate the TLB, I-cache
and branch predictor and branch to 0x8000. The assembler refuses to build if it outgrows its 256-byte
page. Consequences:

- **Nothing depends on the incoming image's layout any more.** The old
  constraint - "nothing may be inserted at or before `_fast_scroll_end`",
  because the copy loop ran in place over identical bytes - is gone, and so is
  every instruction-count comment that served it.
- **The only size limit is `CHAINBOOT_MAX_IMAGE` (4 MB).** The source is a
  heap buffer, and the heap starts at the running kernel's `_end`, so the
  source is always above the destination: the ascending copy can overlap it
  and still never overwrite a byte before reading it.
- **The chain marker survives any image and any pair of builds** that both
  have it - and is believed only for a jump that has just happened with no
  reset in between: it carries the 64-bit system timer and the reset-reason
  register from the moment of the jump, and the incoming kernel accepts it
  only if under 500 ms have passed and the register is unchanged
  (`bootstage.c`). Without that, a marker left by a failed hand-over (or by
  a push to an older kernel, which never clears it) was read by the SD
  kernel after the watchdog reset, which then skipped launching the VPU.
  So the `Boot time` row's "pre-kernel n/a (chain-boot)" can be trusted
  again, and with it the post-ring seeding in `Pi1MHz.c` that keys off
  `RPI_ChainBooted()`: a real pre-kernel figure after a push between two such
  builds means the chain-boot fell back to the card. INFERRED from the
  design, including that a real jump fits in 500 ms; confirm both on
  hardware before relying on it.
- **The VPU's bus program is no longer in the copy's path.** The VPU is
  never relaunched after a chain-boot (relaunching it does not reliably
  work, see `Pi1MHz.c`), so it goes on running the program it was started
  with. That program used to run straight out of the cold-booted kernel's
  `.rodata` (346 bytes; 0xb4940 in the `rpi` release of 876c829, and
  somewhere else in nearly every other build), which the copy then overwrote with whatever the new image held
  there. A cold boot now copies it to 0x3A00, cleans the D-cache over it and
  launches it from there; a chain-boot leaves 0x3A00 alone. **So a change to
  `vidcore/Pi1MHzvc.s` only takes effect from a cold boot (an SD install)** -
  kernel.now carries the new bytes but never runs them.
- **No FIQ runs through the previous kernel's callback table.** A chain-boot
  arrives with the old kernel's doorbell FIQ still selected and the VPU still
  ringing it, and the table at `Pi1MHz_CB_BASE` still holds the old kernel's
  function pointers - now pointing into the new image. `init_emulator`
  unmasks interrupts before it clears that table and seeds the post ring, so
  a FRED/JIM access in between used to call into arbitrary new code (review
  2026-10-06 C4). `kernel_main` now deselects the doorbell FIQ on entry, as
  it already is at a cold boot; `init_emulator` selects it again after the
  table is cleared and the ring seeded, with FIQ masked, exactly as the cold
  boot always did. A BBC reset is unchanged: `kernel_main` runs once.
- **The first push onto an older running kernel still goes through that
  kernel's in-place copier**, which only survives where the incoming image
  has the same bytes at its `_copyandreboot`..`_fast_scroll_end` and
  `_chainboot_mmu_off`. This layout does not: MEASURED by comparing the
  `rpi` release images of 876c829 and this build, every word differs at
  0x81a0-0x81bf (8/8), `_fast_scroll` 0x81c0-0x81e3 (9/9) and
  `_chainboot_mmu_off` 0x8218-0x823f (10/10). So that push will fail - a
  hang, or a watchdog fall-back to the card kernel (INFERRED: not tried).
  **The first install of this layout must be from the SD card.** Pushes from it onward use the new copier, which lands any image - but a
  kernel older than 1a328b4 (the timed marker) reads the 0x3D00 marker
  differently or not at all, takes the jump for a cold boot and relaunches
  VPU1, which does not reliably restart it. **Downgrades below that must go
  via the SD card too.** The older kernel's marker is in its `.noinit`, so
  the Boot time row is meaningless across the transition in either
  direction.

## Video across the jump

The VideoCore is not reset by a kernel.now. Until 2026-10-06 that made video
and kernel.now incompatible: the decoder (an MMAL component on the GPU,
importing our buffers through the SMEM service, both over VCHIQ) was never
torn down, so a jump left it running with nobody behind it, and the incoming
kernel could not reach the VideoCore at all - the firmware ignores a second
`TAG_VCHIQ_INIT` and never answers the new kernel's CONNECT (hardware
observation recorded in `h264-hardware-decode.md`). So kernel.now was refused
whenever the decoder had ever run (review 2026-10-06 R2).

The owner's decision: no refusal. Instead `chainboot_poll`, after the eject
(no card, so no F-code can bring the player back up) and before the WiFi chip
is quietened and interrupts go off, calls `videoplayer_shutdown()`:

1. the player's own output goes: video plane off, its audio producer
   released, the file and index dropped (never the Beeb's display);
2. `h264dec_shutdown()`: input and output ports disabled, the component
   disabled and destroyed, every SMEM import freed, the input staging buffers
   returned to the GPU pool, the SMEM and MMAL services closed (CLOSE, then
   the VideoCore's CLOSE in answer);
3. `vchiq_handover()`: protocol 8 has no disconnect, so the connection is
   handed on rather than shut. Where the stream stands (our read position,
   the next kernel's first local port) is recorded at the end of the
   pagelist page of the shared block itself - GPU memory, outside the copy -
   and the block's address goes in the 0x3D20 handle block as `'VCHQ'` in
   word 0, the address in word 1. The incoming kernel's first
   `videoplayer_init` passes it to `vchiq_adopt()` (no VideoCore work), and
   the first bring-up's `vchiq_init` takes the connection over - no INIT, no
   CONNECT - reading and dropping whatever arrived meanwhile so its slots go
   back. Each kernel's ports start past the previous one's, so a service the
   outgoing kernel could not close can never talk to a new one.

Every MMAL/SMEM call is bounded by its reply timeout (and a VideoCore that
stops answering latches each client dead, so later calls fail at once); each
close waits at most 200 ms. Nothing stops the jump: a failed step is logged
in DEBUG builds and the jump goes ahead. What a failed step leaves is
leaked, never freed under the VideoCore: the frame buffers' handles are then
dropped from the 0x3D20 block (word 2 cleared) instead of left for the next
kernel to release, and the input staging buffers are not returned. A clean
shutdown leaves `'VBF2'` and the two frame-buffer handles in place, and the
next kernel releases them at its first `videoplayer_init`, as before. If the
record does not check out, `vchiq_init` falls back to a fresh start - which,
with the VideoCore still on the old block, costs one CONNECT timeout and
leaves video unavailable until a power cycle, but never hangs.

Cross-build: a kernel older than this ignores `'VCHQ'`, starts a fresh
connection the VideoCore ignores, and has no video until a power cycle (as
before); a push from an older kernel after video use is still refused by
that kernel.

Host tests: `src/tests/chainboot` (no refusal on video; the shutdown is
sequenced after the eject and before the jump; the jump happens when the
shutdown fails) and `src/tests/vchiq` (hand-over and adoption against a
simulated VideoCore that, like the hardware, takes one INIT and ignores the
rest). The VideoCore side - that destroy/free/close are accepted, and that a
taken-over connection really carries on - needs the hardware test: play
video, push kernel.now, play video again after the jump without a power
cycle, five times over; and once more with the push landing while a PVF is
mid-playback.

## What the copy runs over (before 2026-10-06)

Historical: this is the old in-place copier's footprint, kept because the
measurements below were made against it. With the copier, page table and
marker below 0x8000, the copy still runs over the outgoing kernel's
`.noinit`, but nothing there is used after the jump begins.

For an image of length *L* the copy covers `0x8000 .. 0x8000+L`. In a release
build that crosses, in order:

| object | address | copy offset it is reached at |
|---|---|---|
| end of image / start of `.noinit` | 0x84000 | 507,904 |
| `boot_stage_block`, `chain_magic` | 0x84100, 0x84d84 | 508,160 / 511,364 |
| `pwm_cb` (DMA ch 5 control block) | 0x84da0 | 511,392 |
| `hdmi_cb` (DMA ch 4 control block) | 0x88de0 | 527,840 |
| `PageTable` | 0x90000 | 557,056 |
| `arm_stack` | 0xc0000 | 753,664 |

Two consequences that are real and worth knowing:

- Any image over ~508 KB destroyed `chain_magic`, so the incoming kernel did
  not know it was chain-booted and `/status` showed a nonsense pre-kernel
  figure. Under the old copier, **do not use the "Boot time" row to tell a
  chain-boot from a card fallback**; use the banner's per-build
  `-dirty.<hash>` suffix on the serial port, or the fact that the pre-kernel
  figure free-runs rather than resetting. (Fixed by the low-RAM marker, for
  pushes between builds that both have it.)
- `pwm_cb` and `hdmi_cb` are live `struct bcm2708_dma_cb` driving audio DMA
  channels 5 and 4, circularly linked via `cb->next` (`src/rpi/audio.c:52-56`
  and `:125`). The DMA engine reads them autonomously — disabling interrupts
  does nothing to it. Writing over `next` mid-copy is a genuine hazard: zeros
  terminate the chain harmlessly, arbitrary bytes point the engine at an
  arbitrary address. `dma_stop()` was never called on the chain-boot path,
  so this hazard was real — but it was tested as the cause and refuted (see
  below), so stopping the channels (`audio_stop_dma`, since 2026-10-06) is
  hardening only.

## Measured, 2026-09-24, Pi Zero W (`BOARD_REVISION 009000c1`)

Unmodified master, each run fingerprinted by the serial banner:

| image | content | result |
|---|---|---|
| 497,676 | real (release) | chain-boots |
| 567,008 | real (debug build; real code/strings across both DMA CBs) | chain-boots |
| 600,000 / 740,000 / 900,000 | zero-padded | chain-boots |
| 900,000 | random **non-zero** padding over both DMA CBs, `PageTable` and into the stack region | chain-boots |

`Reset reason: 000` throughout, and the pre-kernel figure free-ran across each
handover, so all were warm handovers rather than watchdog fallbacks.

**Failures seen elsewhere.** The gcc17 timing work on
the same board saw 2 failures in 9 pushes, both cold-booting to V1.31, and
both of the shape *incoming image larger than the running one* — 788 KB onto
the stock V1.34-16 image, and 751 KB onto a 700 KB image. Every one of their
7 successes had incoming <= running. Hopping up through a larger zero-padded
image cured both.

That has a plausible mechanism: the copy only stays inside the running
kernel's own text/rodata/data while `incoming <= running`; past that it writes
into the **running** kernel's live `.noinit` — the DMA control blocks,
`PageTable`, the stacks. It also fits the two-hop remedy, which raises the
running size first.

It was **not established, and has since been disproved** — see the
discriminator below. One measurement here already broke a pure size rule:
**900,000 bytes pushed onto a running 567,008-byte kernel chain-booted**, with
random non-zero padding landing across that kernel's `.noinit`. (A second run
of mine that looked like a counter-example was not one: the 567,008-byte push
went onto a 723,232-byte running image, so it was smaller-onto-larger.)

Treat the two-hop push as a **workaround, not a rule**.

### The discriminator was run, and the failure is non-deterministic

gcc17 ran it on 2026-09-24, fixed running kernel Z-R (699,560 B linked, banner
`62487a98`), banner confirmed before each arm:

| arm | pushed | result |
|---|---|---|
| a | 751,160 B = a 481,904 B kernel + 269,256 appended zeros | **landed** |
| b | Z-R, 699,560 B real, onto the running 481,904 B-linked kernel from (a) | **cold-booted to V1.31** |
| c | X-F, 751,160 B real — same size as (a), same baseline | **landed** |

Two conclusions, both solid:

- **Content is not the variable.** Real and zero-padded images of identical
  size both landed from the same running kernel.
- **Nothing static is the variable.** The X-F-onto-Z-R pair *failed* at 11:38Z
  and *landed* at 11:55Z the same day. The same bytes onto the same running
  kernel gave both outcomes, so no rule in terms of sizes or contents can be
  right. Across 16 pushes, all 3 failures had non-zero bytes landing past the
  running kernel's linked end — but so did 4 successes.

A subtlety that invalidates the naive size comparison: **use the running
kernel's *linked* size, not the size of the file that was pushed.** Appended
zeros do not move that kernel's `.noinit`; the image in (a) runs as a
481,904 B kernel, which is why (b) — pushing 699,560 B onto it — reached well
past its linked end.

### The audio-DMA hypothesis was tested and refuted

The suspicion was that the `pwm_cb`/`hdmi_cb` chains only matter while
channels 4 and 5 are actually running, which would explain the intermittency.
gcc17 logged the `/status` Audio `blk` counter (read twice, 2 s apart) before
every push on 2026-09-24. Audio was advancing before **every** push read, and
both outcomes occurred with it advancing:

- landed with audio active: 12:45:06Z, 12:45:27Z, 12:47:05Z
- failed with audio active: 12:45:50Z (cold-booted to V1.31), 12:48:54Z (see
  below)

So "audio running => failure" is **refuted**. At the time `dma_stop()` was
never called on the chain-boot path while the copy wrote over control blocks
the DMA engine was following, so stopping it was defensible as *hardening* —
and since 2026-10-06 `audio_stop_dma()` does — but there is no evidence it
was the cause, and it should not be described as a fix.

### A worse failure mode: the wedged VideoCore

The 12:48:54Z failure (a 784,052 B image) did not cold-boot to V1.31. It left
the board half-dead, and `/status` read, before the power cycle that cleared
it:

- `VideoCore: not answering (property calls skipped)`
- `Audio: none 0Hz q0 blk0` — the audio DMA had stopped entirely
- `kernel->poll 12928 ms`, against ~115 ms normally — the kernel grinding
  through mailbox timeouts
- `Boot init ms: WiFiSvc:2 Harddisc:1` — Framebuffer, Rampage and Helpers all
  absent
- `Reset reason: 000` — no watchdog fired
- COM5 read only NUL bytes at 1 Mbaud afterwards, consistent with the core
  clock never being set up so the UART divisor is wrong
- the board came back on a **new DHCP lease**, so any script pinning the old
  address loses it

The ARM side served HTTP throughout. Only a power cycle cleared it. Worth
knowing because it is silent on the serial port and invisible to a script
watching for a banner.

Full 16-push table in the gcc17 harness's `PI-CONTROL.md` under
"2026-09-24 gcc17 observations"; every push in `pi/logs/push-emu-0924.log`.

## Mechanisms tested and rejected

- **"The copy overwrites `PageTable` while the MMU is live."** Rejected as the
  cause of the intermittent failure, but the reasoning recorded here was
  wrong: the identity map is 1 MB sections, and a copy of 0.5-0.9 MB from
  0x8000 already reaches into the second section (0x100000), a 4 MB image
  into the fifth. The copy walks the table once for each new section it
  enters, so trampled entries ahead of it could fault the copy. Why that
  never showed in these tests is not known. Turning the MMU off before the copy was implemented,
  tested, and made no difference; it was reverted. Since 2026-10-06 the table
  is at 0x4000, out of the copy's reach.
- **"The copy must not reach `arm_stack`, because `_fast_scroll` returns
  through that stack."** 900,000 bytes writes past it and still boots.
- **"Real bytes over the live DMA control blocks break the handover."** Tested
  directly with both a real debug build and random non-zero padding. Did not
  fail. The mechanism remains plausible; sufficiency is disproved.

## How to test this without fooling yourself

1. **Zero padding is not a valid large image.** Pad with non-zero bytes, or
   better, use a genuinely large build.
2. **A probe after `_disable_interrupts()` cannot print.** The UART TX is an
   IRQ-drained ring buffer (`src/rpi/auxuart.c`), so nothing logged between
   that call and the copy will ever reach the wire. Absence of output there is
   not evidence.
3. **"MTP device disappeared" does not mean the push landed.** The device
   drops out ~1.1 s after `CopyHere` on every push, returning after ~1.7 s on a
   chain-boot and ~22-28 s on a cold boot. Only the serial banner settles it.
4. **Run the negative control before believing a fix.** Show the unmodified
   code failing on the same image, or the fix has proved nothing.
