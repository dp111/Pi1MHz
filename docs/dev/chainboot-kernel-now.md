# `kernel.now` chain-boot: what the copy actually touches

STATUS (2026-09-24): **no reproducible size limit on the Pi Zero W bench.**
The long-standing belief that large images fail to chain-boot did not survive
measurement: real-content and non-zero-padded images up to 900,000 bytes hand
over cleanly on unmodified master. Several mechanisms that *look* like they
should break were tested and did not. One real hazard is documented below but
is unproven. No code change came out of this; the section on how to test is
the part worth keeping.

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
  arbitrary address. **This is the one mechanism still considered plausible**,
  and it is why a zero-padded test image is not a valid stand-in for a real
  one.

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

**Not reproduced here, reported elsewhere:** a real 723 KB image failing to
chain-boot twice off the **V1.31 card kernel** after a cold boot, where a
zero-padded 789 KB image succeeded. Every success above had a V1.34/V1.35-era
outgoing kernel already running from RAM. The outgoing kernel is the one that
runs the copy, so *that*, not image size, is the variable to isolate next:
push the same image once off V1.31 and once off V1.34+.

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
