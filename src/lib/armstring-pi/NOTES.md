# libc string routines — investigation notes and handover

Date: 2026-07-19. Working notes so this can be picked up cold.

## The original question

Newlib is pulled in via `--specs=nosys.specs --specs=nano.specs` (CMakeLists.txt:93),
LTO can't cross into it, and some functions (e.g. `toupper`) are inefficient.
Should newlib become a submodule?

**Answer: no.** Two things were conflated, and neither is fixed by a submodule.

### LTO across newlib buys nothing

`toupper` is already inlined by the header — no call crosses the libc boundary:

    ldr r3, .L4        @ &_ctype_+1
    ldrb r3, [r3, r0]  @ locale table lookup
    and r3, r3, #3
    ...

It's 6 instructions and a 257-byte table because it is *locale-correct by design*.
LTO cannot legally fold it to `and #0xDF`; that needs the locale fixed at compile
time, which is a configuration decision. Same for memcpy/strlen — GCC already
treats them as builtins without seeing newlib's source.

### The real problems (two, independent)

**1. Multilib mismatch.** No prebuilt multilib matches a bare-metal ARM-state
hard-float build of either target, so the linker silently picks something weaker:

    rpi  (arm1176jzf-s, ARMv6, VFP)  -> arm/v5te/hard     (ARMv5TE)
    rpi3 (armv8-a, NEON)             -> thumb/v7+fp/hard  (ARMv7, Thumb, NO NEON)

The A53 build was linking a Thumb, no-NEON, generic-ARMv7 libc, paying an
interworking branch on every string call.

**2. newlib was built `-Os`.** Every dispatch file in `libc/machine/arm` gates its
optimised variant behind `#if !defined (__OPTIMIZE_SIZE__)`, so even where the
architecture qualifies the fast code was compiled out. Measured, every routine in
the linked image was a byte loop: memcpy 28 B, strcmp 28 B, strlen 14 B, memset 24 B.

Toolchain is self-built at `/opt/arm-none-eabi-gcc-16.1`; newlib source is on disk
at `/root/src/arm-toolchain/newlib-4.6.0.20260123`.

## ARM1176 has no fast memcpy in newlib *at all*

`memcpy.S` dispatch requires `__ARM_ARCH >= 7 && __ARM_ARCH_PROFILE == 'A'`
(ARMv7-A) or Thumb2-only cores. ARM1176 predefines:

    __ARM_ARCH 6, __ARM_ARCH_PROFILE undefined, __ARM_ARCH_ISA_THUMB 1

so it falls through every gate. Not a misconfiguration — the code does not exist.
Same for strlen (needs Thumb2 + DSP) and memchr (needs NEON or Thumb2 + DSP).
`strcmp` is the exception: `#elif __ARM_ARCH >= 6` -> `strcmp-armv6.S` qualifies.

## VFP is NOT the answer on ARM1176

Tested hypothesis; it's wrong. tinymembench on real BCM2835 @700MHz:

| variant | MB/s |
| --- | --- |
| C copy (no prefetch) | 136.3 |
| **VFP copy** | **162.3** |
| standard memcpy | 387.6 |
| ARM copy prefetched (incr pld) | 387.6 |
| C copy prefetched (64 byte) | 392.6 |
| ARM fill (STM, 4 regs) — *memset, not copy* | 1458.4 |

VFP11 on ARM1176 is a coprocessor with poor VLDM/VSTM issue throughput. VFP/NEON
copies only win from Cortex-A8 onward.

**Key takeaway: every copy method lands at ~390 MB/s; the one at 136 is the one
without prefetch. PLD is worth ~2.9x, instruction selection is worth ~nothing.**
So ARM1176 memcpy does not need to be clever — it needs a PLD.

Note ARM *fill* is 1458 vs ~390 for copies: stores retire into the write buffer
with no read to stall on. memset and memcpy are different problems.

## Licence constraints

Pi1MHz is **GPLv3+** (ReadMe.md:349).

| source | licence | usable? |
| --- | --- | --- |
| Linux kernel `arch/arm/lib/memcpy.S` | GPL-2.0-**only** | **NO** — v2-only is incompatible with v3 |
| musl | MIT | yes, but ARM memcpy is plain C — useless |
| glibc | LGPL-2.1+ | yes, but AArch32 paths are Cortex-A15-era |
| Android bionic | Apache-2.0 | yes (v3-compatible), but Android dropped ARMv6 |
| ARM optimized-routines | MIT | yes |
| newlib `machine/arm` | 3-clause BSD | yes — what we used |

Being GPLv3 rather than v2 *opens* Apache-2.0 and *closes* the Linux kernel.

`ARM-software/optimized-routines` `string/arm/memcpy.S` picks its branch on
`__SOFTFP__`; we build hard-float, so we'd get its VFP path — the slow one on
ARM11 per the table above. Fine for A53/A72, wrong for Pi 1.

`bavison/arm-mem` (shipped in Raspbian as `raspi-copies-and-fills`) builds exactly
**armv6l and armv7l** variants and emits static `.a` — covers both our targets, and
provides memcpy/memmove/memset/memcmp + strlen(v7 only). Quoted 2-3x memcpy, 7x
memset. **Licence NOT verified** — no LICENSE file at repo root (404); check the
`.S` headers before using. This is the main unexplored option.

## Actual usage in this codebase (call sites)

    memset 498 | memcpy 390 | strlen 300 | memcmp 126 | strcmp 114
    strstr 40 | strncpy 38 | strncmp 32 | strchr 29 | strcpy 22
    memmove 9 | strcat 3 | memchr 1

Top five are ~93%. The tail is config/filename handling — cold, and no AArch32
library optimises strncmp/strcat/strstr/strncpy anyway (newlib included).
Leave them alone.

## DONE: vendored newlib sources (committed to `lib/armstring/`)

18 files copied unmodified from newlib-4.6.0.20260123 `libc/machine/arm`, plus a
README. Four dispatch files added to `core_files` in CMakeLists.txt:112 —
`memcpy.S`, `strcmp.S`, `strlen.S`, `memchr.S`. No per-target CMake logic: the
files' own `#if` chains dispatch, and a file with nothing to offer a target emits
no symbol and falls through to libc, so this cannot regress anything.

Measured on a clean `rpi3` build (`build/armstring-test`):

| symbol | before | after | |
| --- | --- | --- | --- |
| memcpy | 26 B | **1248 B** | NEON `vld1.8` + PLD |
| strcmp | 20 B | **734 B** | strcmp-armv7.S |
| memchr | 28 B | **232 B** | NEON + PLD |
| strlen | 16 B | **216 B** | vector + PLD |
| memset | 16 B | 16 B | unchanged — see below |
| memcmp | 32 B | 32 B | unchanged — see below |

Cost: +2072 B text (0.6%), data -1040 B. Net ~+1 KB.

ARM1176 (`rpi`) gets **only strcmp** from this — as the gating predicted.

WARNING: that build rewrote `firmware/kernel7.img` (it was already dirty in git
beforehand, so nothing clean was lost, but it is not the committed binary).

## DONE: `lib/armstring-pi/memset.S`

newlib has **no** `memset.S` in `machine/arm` at all — only `__aeabi_memset`
variants, which use a *different argument order*: `__aeabi_memset(dest, n, c)`
vs `memset(dest, c, n)`. n and c are swapped. Do NOT alias them naively; it
silently fills the wrong value for the wrong length. (`__aeabi_memset` IS
optimised — 244 B, 4x`str` per 16 bytes — while plain `memset` is a 24 B byte
loop. That asymmetry is why memset was the biggest remaining hole.)

Written fresh: architecture-neutral plain ARM integer code, no NEON/VFP, no
ARMv6+ opcodes. **One file serves ARM1176 and ARMv8-A** — nothing to gate.
ARM state + `bx lr`, so interworking-safe.

**Rewritten 2026-07-25** (see "Timing-model pass" below). The original was 160
bytes: byte path for n<8, replicate byte to word, align to *word*, STM of 8
registers (32 B/iter), word tail, byte tail. Both tails were branchy loops,
which dominated the small-n cost. It is now **172 bytes** with a branchless
head and tail, 16-byte alignment, and a 16 B/iter STM of 4 registers.

**Added to `core_files` in CMakeLists.txt.** Both targets rebuilt clean. Final
symbol sizes in the linked images:

| symbol | rpi (ARM1176) | rpi3 (ARMv8-A) |
| --- | --- | --- |
| memcpy | 28 (stub — no ARMv6 impl exists) | **1248** |
| memset | **160** (ours) | **160** (ours) |
| strlen | 14 (stub) | **216** |
| strcmp | **912** | **734** |
| memchr | 52 (stub) | **232** |
| memcmp | 56 (generic C) | 32 (generic C) |

### Test status — PASSED, 13152/13152

Final result: **`checks=13152 failures=0`** on a clean single sweep.

The harness needed `-mno-unaligned-access`. qemu resets with SCTLR.A set, so the
alignment faults it raises on GCC's unaligned accesses (ARMv6 defines
`__ARM_FEATURE_UNALIGNED`) data-aborted the *C harness* and restarted it in a
loop — diagnosed via `qemu ... -d int`, which showed `DFSR 0x801` (alignment
fault) at an odd stack address. Nothing to do with memset.S. Real firmware has
unaligned access enabled, so this is a harness concern only. Trying to clear
SCTLR.A from the harness's `_start` did not take effect under qemu; the compiler
flag is the reliable fix.

Remaining wart: the final `RESULT: PASS` print raises
`Unsupported SemiHosting SWI 0xdeadbeef` — the register-asm semihosting helper
mis-allocates r0 for that one conditional call. The `checks=`/`failures=` line
above it is correct and is the line to trust.

### Historical note on the first run

Harness in `/tmp/claude-0/mstest/` (scratchpad, will not survive): bare-metal ELF
run under `qemu-system-arm -M versatilepb -cpu arm1176 -nographic -semihosting`.
Sweeps len 0..200 x offset 0..15 x 4 fill values, plus large sizes 255..1100 x
offset 0..7, checking return value, body contents, and guard bands on both sides
for under/overrun. 13152 distinct cases per full sweep.

**First run printed `checks=105216 failures=0`.** 105216 = 13152 x 8 exactly: the
harness lacked .bss clearing and its `sh_exit` faulted, restarting the program 8
times and accumulating the counter. So that is **8 complete sweeps, 13152 distinct
cases, zero failures** — memset.S is well validated.

Subsequent attempt to clean up the harness (added .bss zeroing + rewrote the
semihosting inline asm) regressed it: it now prints `START` repeatedly, i.e.
faults early and restarts ~2.6x/sec, never reaching the summary. `_start` and the
`sh_call` codegen were both disassembled and look correct, so **the fault is in
the harness, not in memset.S** — but this was not root-caused before running low
on budget. No user-mode qemu on this box (only `qemu-system-arm`), which is why
the bare-metal harness was needed at all.

## DONE: arm-mem (Ben Avison's) — licence VERIFIED, vendored, wired in

**Licence: 3-clause BSD**, `Copyright (c) 2013, Raspberry Pi Foundation` /
`Copyright (c) 2013, RISC OS Open Ltd`, consistent across every file. That is
GPLv3-compatible, so we can use it. (GitHub reports `license: None` only because
there is no top-level LICENSE file; the per-file headers are unambiguous. The
notices must be retained — do not strip them.)

Vendored at `arm-mem/`, upstream commit `ee8ac1d56adb7ceef4d39a5cc21a502e41982685`
(2024-03-16). All three ARMv6 files assemble unmodified with our exact `rpi`
flags, first try, no Linux dependencies. Sizes on ARM1176:

| symbol | arm-mem | current rpi |
| --- | --- | --- |
| memcpy | 1784 B | 28 B stub |
| memmove | 1832 B | 72 B |
| memcmp | 1492 B | 56 B generic C |
| memset | 180 B | 160 B (ours, in use) |

Arch-guarded wrappers written: `memcpy-arm.S`, `memcmp-arm.S`, each
`#if __ARM_ARCH == 6`, so they emit nothing on rpi3 where newlib's NEON versions
win, and newlib's emit nothing on ARMv6. Exactly one defines each symbol, so a
single unconditional file list still works.

**Now wired into `core_files` in CMakeLists.txt** (`lib/armstring-pi/memcpy-arm.S`, `lib/armstring-pi/memcmp-arm.S`) after passing the MMU harness -- see "MMU harness output" below for how testing was finally unblocked.

### Why they could not be tested: MMU-off alignment semantics

arm-mem's memcpy deliberately performs unaligned accesses (correct — real
firmware enables them). The qemu harness runs with the **MMU off**, which makes
all memory Strongly Ordered, and unaligned accesses to Strongly Ordered memory
**always** fault regardless of `SCTLR.A`/`SCTLR.U`. This is architectural, not a
qemu bug.

Confirmed by isolation: a 12-instruction test that clears `SCTLR.A`, sets
`SCTLR.U`, then does one unaligned `LDR` produced 2.2 million data aborts.
The failing harness runs showed `DFSR 0x1` (alignment fault) at `src[65]`.
Switching cpu model (arm1176 -> cortex-a8) does not help, for the same reason.

Our own `memset.S` passes because it aligns before doing word stores and never
makes an unaligned access — so its PASS is real, but it does not generalise.

### MMU harness output — RESOLVED: it was never broken, invocation was

`test/start.S` enables the MMU (flat identity map, 4096 x 1MB Normal
write-back sections, DACR all-manager, TTBR0, TTBCR=0, then M/C/I set and A
cleared / U set). `test/link.ld` reserves a 16KB-aligned `_page_table`. The
alignment blocker is solved: running `test_mem.c` + arm-mem under this harness
logs zero data aborts, and `-mno-unaligned-access` is no longer needed.

The previously-reported "output never reaches stdout" was a **tooling
artifact of the sandboxed agent's terminal, not a real qemu/semihosting/MMU
bug**. Running `qemu-system-arm ... -kernel test.elf` typed directly at the
top-level agent-terminal prompt prints output correctly (`START`,
`checks=N failures=0`, everything). Wrapping the *exact same command line* one
level deeper -- `sh run.sh`, or `bash -c '... | cat'`, or any nested
shell/script invocation of qemu -- silently produces **zero** stdout/stderr
output and the guest just runs until `timeout` kills it. Root cause not fully
chased down (smells like `-nographic`'s terminal/job-control handling
misbehaving when qemu isn't a direct child of the foreground shell in this
particular sandboxed pty), but it is 100% reproducible and has nothing to do
with the MMU, semihosting, or arm-mem.

**Practical fix for future agent sessions**: when running these test harnesses
from an agent tool, invoke the qemu command line directly in the terminal
(never via `sh run.sh`, `bash -c`, or any nested subshell). A real user's
interactive terminal does not have this problem, so `run.sh` itself needs no
change -- it works fine when run by a human. Do not "fix" `run.sh` by moving to
a UART0-at-0x101f1000 harness; that was the wrong diagnosis.

**arm-mem is now validated**: `test_mem.c` (memcpy/memmove/memcmp) run this way
gives `checks=20632 failures=0`. Wired into `core_files` in CMakeLists.txt.

### Historical: why the MMU was needed at all

**To test arm-mem, the harness must enable the MMU** with an identity mapping
marked Normal (cacheable) memory — roughly 40 lines in `test/start.S`: build a
1MB-section first-level page table, set TTBR0 + DACR, enable `SCTLR.M`. The
firmware's own `rpi/arm-start.S` already does this and is the obvious model.
`test/test_mem.c` is written and ready (memcpy across all alignment pairs,
memmove with 12 overlap deltas in both directions, memcmp sign/zero) — it just
needs a harness that can run unaligned code.

## DONE: ctype — `lib/ascii_ctype.h` + `lib/newlib-str/`  (separate from the above)

Independent of the string-routine work. **Complete and in the build.**

### `lib/ascii_ctype.h` — force-included

Added to CMakeLists.txt next to `-std=gnu23`:

    set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -include ${PROJECT_SOURCE_DIR}/lib/ascii_ctype.h" )

Source files keep plain `#include <ctype.h>` and plain `toupper()`/`isdigit()`
calls — **nothing at any call site changed**. The header includes `<ctype.h>`,
then `#undef`s and redefines 10 names onto inline ASCII range checks:
`toupper tolower isdigit isupper islower isalpha isxdigit isspace isprint iscntrl`.

`isalnum`/`ispunct`/`isgraph`/`isblank`/`isascii` are unused tree-wide and were
deliberately left on newlib.

Codegen: 3 instructions and no data (`sub r3,r0,#97 / cmp r3,#25 / andls r0,r0,#223`)
versus newlib's 5 instructions + literal load + 257-byte `_ctype_` table.
The `bx lr` disappears on inlining. **The r3 scratch is free** — it is
call-clobbered, so no spill; eliminating it needs an extra instruction to restore
r0 on both paths. GCC emits the same 3-instruction shape for both the
mask-in-place and rebias formulations, so there is nothing to win here.

Two traps deliberately designed around — do not "simplify" either away:

1. **`& 0xDF` alone is NOT toupper.** It corrupts non-letters: `'3' & 0xDF` =
   0x13, `'{' & 0xDF` = `'['`. Note the asymmetry: `| 0x20` (tolower) leaves
   digits and most punctuation untouched, `& 0xDF` does not. `mtp_fs.c:786`
   lowercases file extensions, so a bare mask would silently break `"mp3"`.
   Hence the range check in both directions.
2. **They are inline functions, not macros.** Several need `c` twice, and call
   sites like `isdigit(*p++)` must not double-evaluate.

Validated exhaustively on the host against real libc in the C locale over every
input **-1..255** (covers EOF and the 128..255 range):
`PASS: all 10 match libc over -1..255`. Safe to apply globally including to
lwip/tinyusb/FatFs, since Pi1MHz never sets a locale — it is a pure
implementation swap, not a behaviour change.

Result: **-200 bytes text on each target, and faster.** Verified `parse_findindex`
(where `localstrcasecmp` inlines) now has zero `_ctype_` loads.

### `lib/newlib-str/` — strcase* recompiled

libc's prebuilt `strcasecmp`/`strncasecmp`/`strcasestr` reference `_ctype_`
directly, so the header trick cannot reach them. Fixed by vendoring newlib's own
sources (plus `str-two-way.h`, which `strcasestr.c` needs) and compiling them
here, where the force-include applies. Object-level check: zero `_ctype_` refs.

Built **`-Os`** via `set_source_files_properties` in CMakeLists.txt. This matters:

| build | rpi text | rpi3 text |
| --- | --- | --- |
| baseline (ascii_ctype.h only) | 358008 | 353216 |
| vendored at -O2 | 360256 (+2248) | 356104 (+2888) |
| **vendored at -Os** | **358080 (+72)** | **353384 (+168)** |

At -O2 newlib's two-way `strcasestr` unrolls into ~2.5KB. These are cold (12 call
sites total, config/filename handling), so we recompile them only to drop the
table lookups, not for speed — `-Os` recovers 94-97% of the regression.

### The table cannot be fully removed — do not chase it

`_strtol_l` is the **sole** remaining `_ctype_` referrer (verified by exact
address match, including the `_ctype_+1` form). Recompiling newlib's `strtol.c`
here does **not** help: the object still lists `_ctype_` undefined, so strtol
does not reach the table via the `<ctype.h>` macros. It also needs internal
newlib headers (`<reent.h>`, `"../locale/setlocale.h"`).

Removing the last 257 bytes therefore means writing `strtol` from scratch —
29 `strtol` + 19 `atoi` call sites depend on it, and correct strtol is fiddly
(base detection, LONG_MIN/LONG_MAX clamping, ERANGE, endptr). **Recommendation:
leave it.** Bad trade for 257 bytes of rodata.

## REMAINING WORK, in priority order

Items 1-3 and 5 below are **DONE** (2026-07-19 session). Kept here for the
history; item 4 is the only one still open.

1. ~~Fix the harness output path~~ — not a harness bug, see "MMU harness
   output" above. memset re-confirmed 13152/13152 directly (unwrapped) under
   the current MMU harness.
2. ~~Test and wire in arm-mem~~ — `test_mem.c` gives `checks=20632 failures=0`.
   `memcpy-arm.S`/`memcmp-arm.S` are in `core_files`. Verified in a full build:
   linked `memcpy` is 1784 B (was a 28 B stub), `memcmp` is 1492 B (was 56 B
   generic C), on the `rpi` target; `rpi3` picks up neither (newlib's NEON
   memcpy and generic-C memcmp are unaffected, exactly as designed).
3. ~~`strlen-arm1176.S`~~ — written, `#if __ARM_ARCH == 6`, the standard
   `(w - 0x01010101) & ~w & 0x80808080` zero-byte test, magic constants built
   with `mov`/`orr`/`lsl` (no ARM immediate encodes a repeated-byte pattern, and
   this avoids touching any callee-saved register). New test harness
   `test/test_strlen.c`: `checks=4840 failures=0`. Linked `strlen` on `rpi` is
   96 B (was a 14 B stub); `rpi3` is unaffected (keeps newlib's NEON version).
   In `core_files`.
4. **Still open.** Consider vendoring arm-mem's `-v7l` files
   (`memcpymove-v7l.S`, `memcmp-v7l.S` — confirmed present upstream at the same
   vendored commit `ee8ac1d5`) for rpi3's `memcmp` and `memmove`, which are
   still generic C there. **Blocker: no qemu harness for the rpi3 target.**
   `test/start.S` + `versatilepb` is ARM1176-only; rpi3 is ARMv8-A/Cortex-A53
   run in AArch32 state, which versatilepb cannot model. Testing this needs a
   different qemu machine/cpu combination (e.g. a `virt`-style board with
   `-cpu cortex-a53` in AArch32, or real hardware). Per the project's own rule
   below, do not wire in untested code — this is deliberately left undone
   rather than rushed in without a working test rig.
5. ~~Drop `-u _printf_float`~~ — done, but the original framing ("exactly one
   float format in the whole tree") was wrong: `rpi/info.c`'s
   `dump_useful_info()` has several (clock rates, voltages, temperature), it's
   just that the only call site (`Pi1MHz.c`) is `#ifdef DEBUG`-gated, so it is
   dead code (and its float formats un-referenced) in a release build. Fix:
   `-u _printf_float` moved into the `if( DEBUG )` block in CMakeLists.txt
   instead of being dropped outright, so DEBUG builds keep working float
   printf and release builds don't pay for it. `helpers.c`'s temperature
   display (the one release-build float format) is hand-formatted to tenths
   via integer arithmetic instead. Verified: release build has no `_dtoa_r`/
   `_ldtoa_r` symbols; `-DDEBUG=1` build still does, and still links and runs.

## DONE: timing-model pass over our own routines (2026-07-25)

Review of `memset.S` / `strlen-arm1176.S` / `ascii_ctype.h` against the gcc17
ARM1176 scheduler work (`/root/gcc17/arm1176-model-audit-2026-07-19.md` and the
v11/v12 hardware timing suites). Three silicon-confirmed numbers drove it:

- **load-use latency 3**, and **4 when the consumer reads the value as a
  *shifted* operand** (Early-Reg). v12 rows 4-9 confirm the +1 three ways.
- **ALU → shifted-operand = 2 cycles, not 1.** So every `orr rX, rX, rX, lsl #n`
  self-chain costs 2 per step, not 1.
- **`pop {pc}` ≈ 13.13 cycles vs `bx lr` ≈ 9.13** (v11 CALL group): load-to-PC
  defeats the return-stack predictor, +4.

### `memset.S` — branchless head/tail, 16-byte alignment (160 → 172 B)

Structure now follows arm-mem's `memset-v6l.S` (already vendored, 3-clause BSD),
because the old branchy head/tail dominated at the sizes most of the ~500 call
sites actually use — a 16-byte fill ran ~30 instructions through ~4 conditional
branches, and an ARM1176 mispredict is 5-7 cycles. `movs Rd, Rn, lsl #31` yields
two alignment bits at once (N=bit0, C=bit1), `lsl #29` the next two, and the
rest falls out as conditional stores. Now ~18 instructions and 1 branch for that
same 16-byte fill.

Two deliberate departures from arm-mem, both documented in the file header —
**keep them**:
1. It returns `bx lr`, not arm-mem's `pop {S, pc}`. Worth the measured 4 cycles.
2. r0 is never the working pointer (ip is), so S need not be pushed and reloaded
   just to return it. Only r4/lr are saved — 2 registers, not the old 6.

Aligning to 16 rather than 4 means a 16-byte STM always lies wholly inside one
32-byte ARM1176 cache line, so bulk stores stop straddling lines.

The leading run is `(-ip) & 15`, so **r2 is adjusted once** rather than by a
`sub<cond>` beside each of the four conditional stores, and the same `ands`
doubles as the already-aligned test that a separate `tst ip, #15` used to do.
That is 3 instructions cheaper than the arm-mem original and is why this is 172
bytes rather than 184. (Credit: user, mid-review.)

Honest limit: at large n this is a wash — the tinymembench number above
(~1458 MB/s ≈ 2 B/cycle) says bulk fill is bus-bound, not instruction-bound.
The win is small-to-medium n and branch behaviour.

**Re-tested: `checks=13152 failures=0`** on the existing harness.

### `strlen-arm1176.S` — branchless NUL-lane tail (96 → 104 B)

The old tail re-scanned the NUL-containing word with a byte loop: up to four
`ldrb` each paying the 3-cycle load-use latency, plus a mispredict on exit. The
mask already identifies the lane, so it is now a `tst`/`tsteq` chain over the
three low lanes. The **lowest set lane is always the first NUL** — a subtraction
borrow only propagates upward, and the existing `bic` removes false hits in
lanes whose own byte was >= 0x81. The chain self-terminates: once one `tst`
gives NE the remaining `tsteq`s do not execute, so no further `addeq` fires.

The `0x01010101` build was also hoisted above the alignment test and
**interleaved with it exactly**. Each `orr rX, rX, rX, lsl #n` reads its own
result as a shifted operand, which is a 2-cycle Early-Reg dependency, so each
one needs one independent instruction after its producer: `mov r1, r0` covers
the first, `tst r1, #3` covers the second. Putting the two `orr`s together (or
letting `tst` fill the first gap, which never needed filling) costs a stall —
7 cycles to the branch instead of 6.

Do NOT move the `beq` above the second `orr` to shorten this: the aligned path
branches straight into the word loop and needs r3 fully built. `orr` does not
set flags, so it is safe between the `tst` and the `beq`. (Credit: user.)

The constant cannot be made shorter — no ARM immediate encodes a repeated byte
pattern and ARM1176 is ARMv6 (no `movw`). A literal-pool `ldr` would be one
instruction, but the file deliberately avoids a pool; the shift chain is free
once scheduled as above.

Flag trivia worth not re-deriving: there is no way to get a third alignment bit
per flag-setting instruction. `TEQ`, like every logical op, cannot write V at
all (only the arithmetic ops do, and from signed overflow); its own trick is
sign-XOR into N, and Z is redundant with N for these shifts. Two bits (N and C)
per `movs` is the architectural ceiling, which memset's head already hits.

**Re-tested: `checks=4840 failures=0`.**

### "The caller knows the alignment — why re-check it?" — already banked

Recurring and reasonable idea; the numbers say leave it. Source occurrences vs
`bl` sites surviving into the linked `rpi` image:

| | in source | calls in image | eliminated |
|---|---|---|---|
| memset | 512 | 88 | 83% |
| memcpy | 426 | 100 | 77% |
| strlen | 318 | 40 | 87% |

Where GCC knows size and alignment it does not call a *better* memset — it
inlines the stores and never calls us at all. The win is already taken.

Three reasons not to chase the remainder:

1. **Writing these in C would not remove the check.** The parameter is `void *`
   and carries no alignment guarantee, so the callee must handle any alignment
   whatever language it is in — and the called path would then lose to the asm.
2. **Only the caller can skip the head**, by picking a pre-aligned entry point.
   That mechanism exists — EABI's `__aeabi_memset4`/`memset8` /
   `__aeabi_memclr4`/`memclr8` — but this image references **zero** of them;
   GCC for arm-none-eabi emits plain `memset`. Closed without a toolchain change.
3. **The surviving sites are the wrong shape to care.** They are dominated by
   large constant sizes — 572 (x6), 8192, 442368, 512, 128, 76, 48, 32 — where
   the head is `rsb`/`ands`/`beq`, 3 instructions and a predicted branch.

Related, and only looks like a problem: memset.S being *assembly* is opaque to
LTO, so GCC can never inline it. That costs nothing — GCC's inlining uses its
own `__builtin_memset` expansion and never needed to see our source. Different
situation from the newlib LTO question above.

### Checked and found clean — do not re-investigate

- **No `__aeabi_mem*` references in the linked image.** GCC can emit
  `__aeabi_memcpy`/`__aeabi_memclr` for struct copies, which would bypass all of
  this work (newlib's are byte loops on ARMv6). It does not here — only the div
  helpers appear. Verified by `nm` on the linked ELF.
- **`strlen` has no PLD, deliberately.** The strings here are short filenames;
  prefetch would be a net loss. PLD is worth ~2.9x for *memcpy*, not for this.

### Harness note — correcting the "MMU harness output" section above

That section says qemu must be invoked directly and never from a nested shell or
script. **Redirecting to a file works fine from an agent tool**:
`timeout -s TERM 45 qemu-system-arm ... -kernel x.elf > out.txt 2>&1`, then read
`out.txt`. It is the *pipe* (`| cat`) that loses the output, not the nesting.

Also: both harnesses were confirmed **non-vacuous** before trusting any PASS, by
deliberately mutating each routine — `failures=2392` (strlen, wrong lane mask)
and `failures=1888` (memset, wrong short/bulk threshold). Worth repeating for
any future change: a first mutation attempt silently failed to apply and
"passed", which is exactly how a vacuous green result looks.

## Design rules to keep

`lib/armstring/` is vendored and must never be edited (re-copy to update);
`lib/armstring-pi/` is ours, except `arm-mem/` which is also vendored.
Everything arch-guarded so there stays one file list in CMakeLists.txt and no
per-target build logic.

Every hand-written routine must clear the qemu harness over random sizes and
alignments before it goes near master — alignment and tail-length bugs here are
silent and awful.
