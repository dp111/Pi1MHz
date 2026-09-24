# `kernel.now` chain-boot: what the copy actually touches

STATUS (2026-09-24): **the failure is real but NON-DETERMINISTIC.** The same
image onto the same running kernel both failed and succeeded on the same day,
so no rule in terms of image size or image content can be correct. Size and
content were each tested and eliminated; a time-varying agent is suspected and
untested. Use the two-hop push as a workaround and judge every push by its
banner.

Large images do chain-boot: real-content and non-zero-padded images up to
900,000 bytes hand over cleanly on unmodified master, and the believed
557,056-byte threshold does not exist. But failures do occur, roughly 3 in 16
pushes on a busy day, and every static explanation tried so far has been
eliminated by measurement. No code change came out of this; the section on how
to test is the part worth keeping.

## The handover

`mtp_fs_reboot_poll()` (`src/usb/mtp_fs.c`) takes the staged image and calls
`_copyandreboot(src, len)` in `src/rpi/arm-start.S`. Before that it disables
interrupts, tells the WiFi chip to stop signalling, and turns the D-cache off
so the copy lands in RAM.

`_fast_scroll` copies the image to **0x8000 upward, 64 bytes at a time**, then
the tail of `_copyandreboot_code` invalidates the caches and branches into the
new image.

The load-bearing constraint: **`_copyandreboot_code` runs IN PLACE while
`_fast_scroll` copies the new image straight over it.** It survives only
because those bytes are identical in both images. Nothing may be inserted at
or before `_fast_scroll_end` — doing so shifts the copy loop and the outgoing
kernel executes whatever landed at its old address. Verified addresses:

| symbol | ARMv6 (`rpi`) | ARMv7 (`rpi3`) |
|---|---|---|
| `_copyandreboot` | 0x818c | 0x81a4 |
| `_fast_scroll` | 0x81b8 | 0x8248 |

A corollary that is easy to miss: code placed *after* `_fast_scroll_end` is
free to differ between builds, but it is **not** free to be branched to after
the copy. By then the copy has written the incoming image over every address
below `0x8000 + image size`, so such a branch lands in the NEW image.

## What the copy runs over

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

- Any image over ~508 KB destroys `chain_magic`, so the incoming kernel does
  not know it was chain-booted and `/status` shows a nonsense pre-kernel
  figure. **Do not use the "Boot time" row to tell a chain-boot from a card
  fallback.** Use the banner's per-build `-dirty.<hash>` suffix on the serial
  port, or the fact that the pre-kernel figure free-runs rather than resetting.
- `pwm_cb` and `hdmi_cb` are live `struct bcm2708_dma_cb` driving audio DMA
  channels 5 and 4, circularly linked via `cb->next` (`src/rpi/audio.c:52-56`
  and `:125`). The DMA engine reads them autonomously — disabling interrupts
  does nothing to it. Writing over `next` mid-copy is a genuine hazard: zeros
  terminate the chain harmlessly, arbitrary bytes point the engine at an
  arbitrary address, and the channels are only armed while audio is actually
  playing — which would make any resulting failure intermittent. **This is the
  one mechanism not yet eliminated**, and it is the reason to prefer a real
  image over a zero-padded one when testing, even though padding turned out
  not to change the outcome (see the discriminator below).

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

**INFERRED, not measured:** something time-varying is involved. The only
mechanism identified so far that is real in the source, reaches exactly the
region the copy crosses, and is *intermittently* armed is the audio DMA — the
`pwm_cb`/`hdmi_cb` chains above only matter while channels 4 and 5 are
actually running. That would make the failure look exactly this intermittent.
Untested. The test: repeat one known-failing pair many times with audio
definitely active and definitely idle, and compare failure rates.

Full 16-push table in the gcc17 harness's `PI-CONTROL.md` under
"2026-09-24 gcc17 observations"; every push in `pi/logs/push-emu-0924.log`.

## Mechanisms tested and rejected

- **"The copy overwrites `PageTable` while the MMU is live."** The identity map
  is 1 MB sections, so the whole copy region is one already-resident TLB entry
  and the copy performs no table walks. Trampling the table is harmless.
  Turning the MMU off before the copy was implemented, tested, and made no
  difference; it was reverted.
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
