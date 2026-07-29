# Pi1MHz Project Code Review — 2026-05-30

Deep-dive pass for correctness errors and improvements across `src/`
(vendored `lwip/` and `tinyusb/` excluded).

## Method & coverage

- **Static analysis:** host `gcc -Wall -Wextra -Wshadow -fanalyzer` (no
  network for cppcheck/clang-tidy; the project's own `Pi1MHz.cppcheck`
  config should be run in CI for fuller coverage).
- **Analyzer actually ran on** (host-compiled cleanly): most of `src/rpi`,
  all of `src/BeebSCSI`, `wifi/{sdio,sdio_host,cyw43,md5,framebuffer_export}`,
  and the top-level emulators that compile on host.
- **Manually reviewed:** `wifi/netname.c` (UDP parser), `wifi/webserver.c`
  parser bounds (`ws_url_decode`, multipart/Content-Length), and the wifi
  files changed earlier this session.
- **Not machine-analyzable on host** (ARM target context / a host `usleep`
  macro clash): `wifi/webserver.c`, `wifi/wifi.c`, `wifi/wifi_lwip.c`,
  `usb.c`, `usb/mtp_fs.c`, `Pi1MHz.c`, and the ARM asm stubs. These were
  spot-checked by hand; recommend the project cppcheck/clang-tidy run for
  full automated coverage.

## Confirmed bug (FIXED in this pass)

### 1. Unchecked `malloc` → NULL dereference — `BeebSCSI/filesystem.c` — Medium

Four sites allocated a key-value buffer, **logged** an allocation failure,
then **fell through and wrote to the NULL pointer** — an out-of-memory
condition would crash instead of degrade:

| Function | Buffer | malloc line | crashed at |
|---|---|---|---|
| `filesystemLunToconfigGeometry` | MODEPAGE0 | 931 | 939 |
| `filesystemLunToconfigGeometry` | MODEPAGE4 | 953 | 960 |
| `filesystemConfigToLunGeometry` | MODEPARAMHEADER | 996 | 1001 |
| `filesystemConfigToLunGeometry` | LBADescriptor | 1020 | 1025 |

Notably, the same file's newer functions (`filesystemWriteModePageData`
:1339, `filesystemReadFile` :1606) and the sibling `rpi/fileparser.c`
already handle allocation failure correctly — these four were the
outliers.

**Fix applied:** added `return;` after each logged failure (both
functions are `void`). On OOM the key-value is simply left absent, which
downstream code already tests for (`if (!...v.string)`), so the geometry
path degrades instead of dereferencing NULL. Verified: diff is exactly the
4 inserted returns, braces balanced, `gcc` syntax-clean.

## Dismissed (verified false positives — no action)

- **`rpi/armc-cstubs.c:176` `-Wreturn-type`** ("control reaches end of
  non-void function"): `_sbrk` returns on every path; a host-compile
  artifact, not a real defect.
- **`filesystem.c` `-Wanalyzer-malloc-leak` (936/958/1005/1033):** the
  pointers are stored in the global `filesystemState.keyvalues[][]` array;
  the analyzer loses track of that retained storage. Not leaks.

## Clean areas worth noting

- **No unsafe string functions** (`strcpy`/`strcat`/`sprintf`/`gets`)
  anywhere outside the vendored libraries.
- **All other `malloc` sites are guarded** (`fileparser.c`, `videoplayer.c`,
  `webserver.c`, `framebuffer/primitives.c`).
- **The wifi subsystem is analyzer-clean.** `netname.c`'s NetBIOS receive
  path and `webserver.c`'s `ws_url_decode` are correctly bounds-checked
  against untrusted network input (length guards, NUL/control-char
  neutralisation, no over-reads).

## Improvement suggestions (non-bug)

1. **De-duplicate the key-value alloc pattern** in `filesystem.c` into a
   helper like `kv_alloc(lun, index, len)` that returns success/failure —
   removes the whole bug class above and shrinks four near-identical blocks.
2. **CI static analysis:** wire the existing `Pi1MHz.cppcheck` /
   clang-tidy config into the build so the network-facing files host-gcc
   can't reach (`webserver.c`, `wifi.c`, `wifi_lwip.c`, `usb*`) get
   continuous coverage.
3. **Cooperative-poll work already landed this session** (separate from
   this review): cross-tick SDIO function-enable, chunked firmware
   download, bounded per-command/host-reset busy-waits, and the adaptive
   idle RX-poll throttle in `wifi_lwip.c`.

## Caveats

Host-gcc analysis cannot see the ARM target context, so every finding here
was confirmed by reading the source. For full assurance, run the project's
own cppcheck/clang-tidy configuration and build + flash on hardware.
