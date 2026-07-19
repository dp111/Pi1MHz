# armstring — architecture-optimised string routines

Vendored, unmodified, from newlib `newlib/libc/machine/arm/`.

    upstream: newlib-4.6.0.20260123
    licence:  3-clause BSD (ARM Ltd / Linaro) — compatible with Pi1MHz's GPLv3

## Why these are here

The prebuilt `libc.a` shipped with the toolchain is unusable for both our
targets, for two independent reasons:

1. **Multilib mismatch.** No prebuilt multilib matches a bare-metal ARM-state
   hard-float build of either target, so the linker picks something far weaker:

       rpi  (arm1176jzf-s, ARMv6, VFP)  -> arm/v5te/hard    (ARMv5TE)
       rpi3 (armv8-a, NEON)             -> thumb/v7+fp/hard (ARMv7, Thumb, no NEON)

2. **newlib was built `-Os`.** Every dispatch file in `machine/arm` gates its
   optimised variant behind `#if !defined (__OPTIMIZE_SIZE__)`, so even where
   the architecture qualifies, the optimised code was compiled out. The result
   is that `memcpy`, `memset`, `strlen`, `strcmp` and friends are all
   byte-at-a-time loops of 14–28 bytes.

Compiling these sources as part of the firmware, with our real `-mcpu` flags
and at `-O2`, sidesteps both problems. The files are unmodified: their existing
`#if` chains select the correct variant per target, so there is one file list in
CMakeLists.txt and no per-target conditionals.

## What each target actually gets

| symbol | rpi (ARM1176) | rpi3 (ARMv8-A) |
| ------ | ------------- | -------------- |
| memcpy | *none* — see below | `memcpy-armv7a.S`, NEON + PLD |
| strlen | *none* — see below | `strlen-armv7.S`, vector + PLD |
| memchr | *none* — see below | NEON + PLD |
| strcmp | `strcmp-armv6.S`  | `strcmp-armv7.S` |

Where a dispatch file emits nothing for a target, the symbol simply resolves
from libc as it did before — adding these files can never make a target worse.

## ARM1176 gap

newlib has no ARMv6 implementation of `memcpy`, `strlen` or `memchr`: the fast
paths require ARMv7-A (`memcpy-armv7a.S`) or Thumb-2 + DSP (`strlen-armv7.S`),
and ARM1176 is neither. Those are covered by our own files in `../arm1176/`,
guarded so they emit nothing on other targets.

## Updating

Re-copy the same file list from a newer newlib and rebuild. Do not edit these
files locally — local changes would be silently lost on the next sync.
