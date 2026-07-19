# arm-mem — vendored ARMv6 string routines

Vendored, unmodified, from <https://github.com/bavison/arm-mem>.

    upstream commit: ee8ac1d56adb7ceef4d39a5cc21a502e41982685  (2024-03-16)
    licence:         3-clause BSD
    copyright:       Raspberry Pi Foundation; RISC OS Open Ltd

3-clause BSD is compatible with Pi1MHz's GPLv3. The copyright notices at the top
of each `.S` file must be retained — do not strip them.

## Why

newlib has **no** ARMv6 implementation of `memcpy`: its fast paths require
ARMv7-A (`memcpy-armv7a.S`) or a Thumb-2-only core (`memcpy-armv7m.S`), and
ARM1176 is neither, so it falls through to a 28-byte byte loop. See
`../NOTES.md` for the full analysis.

This is the code Raspberry Pi ship in Raspbian as `raspi-copies-and-fills`,
written by Ben Avison. It is ARM11-tuned and uses `pld` throughout, which
matches the measurement that prefetch — not instruction selection — is
essentially the entire memcpy win on this core.

## What we use

Only the `-v6l` (ARMv6 little-endian) files, and only for the `rpi` target.
The `rpi3` build gets newlib's NEON routines instead, which are better there.

| file | provides |
| ---- | -------- |
| `memcpymove-v6l.S` | memcpy, memmove, mempcpy, __mempcpy |
| `memcmp-v6l.S`     | memcmp |
| `memset-v6l.S`     | memset — **not currently used**, see below |

`memset-v6l.S` is vendored for reference but not built: `../memset.S` is
architecture-neutral and already serves both targets from one file. Swap only if
measurement on real hardware justifies it.

These files are **not** added to CMakeLists.txt directly. They are pulled in by
the arch-guarded wrappers `../memcpy-arm.S` and `../memcmp-arm.S`, so that one
file list covers both targets with no CMake conditionals.

Not provided for ARMv6: `strlen` (upstream only has `strlen-v7l.S`).

## Updating

Re-fetch the same file list and update the commit hash above. Do not edit these
files locally — local changes would be silently lost on the next sync.
