# Pi1MHz code review (2026-07-02) — remaining TODO

**Scope:** project sources under `src/` (vendored tinyusb/lwip/fatfs excluded); every finding
verified against the code. The small verified fixes from this review were applied in commit
`5c27eff` (mouse Y nibble, M5000 gain floor, fcode 5-digit picnums, BSFATINFO/FATPATH buffer
hygiene, cursor raster clamp, line-fill/arc clip off-by-ones, ARMv6 MCRR range end, mailbox
invalidate on all models, upload maxslice/peek, sdio template guard + verify wrap, mtp_fs
truncated-path unlink, HVS context volatile, comment corrections). `tinyusb` was updated to
0.21.0 in `c425c0e`. **This document lists only what is still to do**, ranked by severity.

---

## 1. Critical

None outstanding. The READ6/WRITE6 past-EOF handling was resolved in `a682d5d` by implementing
grow-on-write: images extend on demand with fast seek disabled and the link map rebuilt, short
transfers (SD full) are now errors, and reads beyond the image return zeros. Note the accepted
semantics: READ6/WRITE6 still do not range-check the LBA against the configured geometry (a
21-bit CDB LBA bounds growth at 512MB), and gap sectors created by a forward jump hold
whatever the allocated clusters previously contained — like unwritten sectors on a real drive.

---

## 2. High

### 2.1 / 2.2 ~~SDIO error swallowing + boot-fatal tolerated failures~~ — FIXED in `97de3c0`
Error bits now always fail the command/transfer (the emulator no-bits fallbacks are kept); the
wifi.c mid-tick fatal check is removed (hard failures already finalize to STAGE_ERROR) and the
tolerating sites (CLM, SET_MAC, QUERY_MAC, KSO, join) clear the latched message.
Validated on real hardware 2026-07-03 (WiFi boots and runs with strict error handling).

### 2.3 / 2.4 ~~cfg heap corruption + cfg re-parse leak~~ — FIXED in `279e223`
The parser now enforces key min/max at the point of entry (NUMSTRING values allocated at the
key's max and zero-padded, STRING truncated, INTEGER clamped); `filesystemWriteModePageData`
keeps the same allocation invariant and no longer NULL-derefs; geometry readers fall back to
defaults on zero BlockSize/SectorsPerTrack; `filesystemReadFile` NUL-terminates its allocation
(EOF OOB reads); `filesystemCheckExtAttributes`/`CreateLunDescriptor` release key values before
re-parsing. Parser verified with a host ASan/UBSan test harness.

### 2.5 ~~`Pi1MHz_MemoryWrite` RMW~~ — withdrawn
The VPU memory region only supports 32-bit accesses (no STRH), and no emulator writes the even
byte of a shared word from one context while another writes the odd byte, so the theoretical
lost-update race does not occur. Not a bug.

### 2.6 ~~`ttx_status` FIQ/poll races~~ — FIXED
DEW split into a poll-owned variable (author's design); FSYNC set and DOR latch masked
against FIQ; `|= INT` needs no mask (a lost clear is superseded by the new INT); volatile added.

### 2.7 ~~SCSI ACK busy-wait~~ — FIXED in `01d173d`
(Severity was overstated: `HD_ACK` is a cached volatile bool, so 1e9 iterations was seconds,
not minutes.) Now a shared `hd_wait_ack()` with a 100ms `RPI_GetSystemTime()` budget, timer
sampled every 256th spin.

### 2.8 / 2.9 ~~sdcard SEND_SCR use-after-free + ACMD41 forever-loop~~ — FIXED in `b992ea9`
Bounded in-function reinit retry (3 attempts, struct reused, no recursion); ACMD41 gives up
after 1000 × 1ms (SD spec 1s budget). Note: a failed SD init now returns −1 to the caller —
a path that previously never executed (it hung or crashed instead).

### 2.10 ~~VDU 23,22 degenerate custom modes~~ — FIXED in `3dffd0d`
Modes < 8×8 rejected; text grid clamped to 1..256 columns/rows; `set_text_area` guard signed.

### 2.11 ~~FIQ VDU byte-drop desync~~ — FIXED
FIQ producer now accepts/drops whole commands via `vdu_operation_table[].len`, reserving space
for outstanding parameter bytes; `vdu_enqueue` respects the reservation.

### 2.12 ~~MTP data-OUT error paths wedge the host~~ — FIXED in `3dffd0d`
Note: the documented "return negative to stall" contract is dead code in tinyusb 0.21's data
phase (`mtp_device.c:487` ignores the return, unlike the command phase at `:424`) — worth an
upstream issue. Fixed app-side: failures latch `failed_resp`, the stream is drained/discarded,
and the error response goes out from `tud_mtp_data_complete_cb`; partial uploads are unlinked.

### 2.13 ~~`JIM_ram` NULL when Rampage malloc fails~~ — WON'T FIX
Author's call: if the Rampage allocation fails there isn't much the firmware can usefully do.

---

## 3. Medium (bugs with narrower triggers)

Fixed in `fef8eb7`..`b4a0f43`: VDU 21/6 implemented (VDU 22 also re-enables so a Beeb BREAK
recovers), VDU 27 documented as a deliberate edit-key extension, `define_character` bounded
(SAA5050 read 2 bytes past the VDU block), VFS LUN directory mismatch, MODE SENSE allocation
length (incl. zero), MODESELECT6 VLA → fixed buffer, CMD53 `_Alignas(4)` batch, and sdcard
STOP_TRANSMISSION on failed multi-block PIO.

Withdrawn — **auxuart TX IER race**: single-core; the IRQ handler can preempt the writer but
never the reverse, and every writer-side interleave ends in at worst a harmless spurious TX
interrupt. Only a FIQ-context UART write could race the handler's IER RMW, which only debug
builds could ever do.

Still open:

- **Block size hardcoded 256** in BeebSCSI: `BlockSize` from cfg reaches READ CAPACITY/TRANSLATE
  but seeks/transfers use literal 256 — self-inconsistent if a cfg sets 512. Validate/force 256
  (author decision: is a non-256 block size ever meaningful here?).
- ~~`/framebuffer.bmp` TOCTOU~~ / ~~pipelined requests~~ — FIXED in `5859adf` (geometry
  re-checked per row refill, remaining rows blanked on change; pipelined bytes close the
  connection at response end so the client retries immediately).
- **mtp_fs pure-invalidate DMA glue is unsafe if ever enabled** (`cache.c`
  `_invalidate_cache_area` + `usb/broadcom/caches.h` + `CFG_TUSB_MEM_ALIGN` aligned(4)):
  unaligned head/tail lines are destroyed by pure invalidate. Dormant (dwc2 runs slave mode,
  DMA off). An edge-handling change to `_invalidate_cache_area` was proposed and REJECTED by
  the author — if DMA is ever enabled, handle alignment at the point of use instead (raise
  `CFG_TUSB_MEM_ALIGN` to the cache-line size).

---

## 4. Performance

Ordered by expected payoff:

1. **Framebuffer solid fills/scrolls bypass per-pixel `set_pixel`:**
   ~~`draw_hline` fast path~~ — DONE in `0b79b85` (per-mode `fill_hline`, PM_NORMAL only).
   Still to do: CLS/scroll-blank row memset (20-100×); windowed & down scrolls per-row
   `memmove` (only full-screen-up is fast today; 10-50×); PLOT 184-191 move/copy and MODE 7
   scroll re-render (5-20×).
2. ~~SD High-Speed switch~~ — DONE in `99ece53` (CMD6 check/set, clock raised only on a
   confirmed switch). MTP transfers validated on real hardware 2026-07-03.
3. ~~Skip CMD13 before every sector op~~ — DONE (cached in the device struct; any
   data-command give-up or re-init clears it, so card removal still recovers).
4. ~~Cache the SDIO backplane window~~ — DONE (brcmfmac-style sbwad cache, invalidated on
   boot-state reset and programming failure).
5. ~~Drop the fixed 10µs pre-poll delay~~ — DONE.
6. ~~MTP session-open cache build O(n²)~~ — DONE (handles assigned unchecked, collisions
   repaired as adjacent duplicates after the sort).
7. **teletext ring uses `% 4032` per byte** (`teletext_emulator.c:127-143`): make the ring a
   power of two.
8. **BeebSCSI FAT-transfer directory scan is O(N²)** (`filesystem.c:1416,1503`): rescans from
   entry 0 per file; cache the open `DIR` + index.

---

## 5. Layout / structure

- **~550 lines of dead framebuffer code:** `USE_NEW_SECTOR_SEGMENT_FILL` is always defined, so
  `arc_quadrant`/`arc_point`/old `prim_draw_arc`/`prim_fill_sector` (primitives.c ~243-348,
  1535-1694) and the `#if 0` blocks (owl/plot ~1686, `fb_destroy` ~1816) are dead;
  `prim_on_screen` is declared but never defined; `vdu_25` has 8 identical line cases;
  `fonts.c:12-53` includes 40 font headers for a catalog of 2.
- **sdcard.c SDHCI-era freight:** unused `TIMEOUT_WAIT`, write-only `use_sdma`/`SDMA_SUPPORT`
  plumbing, SDHCI command-flag bits the SDHOST path never consumes, dead `card_removal` branch —
  several hundred lines.
- **~400 lines of SDIO duplication:** identify/enable/ALP/KSO/ack each exist in blocking and
  per-tick form; the blocking set serves only the `wifi_sdio_probe=1` diagnostic.
- **BeebSCSI:** auto-start block triplicated in Read6/Write6/Translate — extract a helper.
  ~~`(size_t*)&headerlen` type-pun (`scsi.c` ~1567, an `int`) — declare it `size_t`.~~
  **DONE** (906abe1): read into `size_t` locals, narrow to int for the transfer arithmetic.
- **`Pi1MHzBus_read_Status`** (`Pi1MHz.c:311-314`) is an empty TODO still registered on &FCCB —
  implement or drop.
- **`mailbox.c:127`** `RPI_PropertyProcess(bool wait)` ignores `wait` (early-return commented
  out); callers pass `false` expecting async. Restore or drop the parameter. The single global
  `pt`/`pt_index` is non-reentrant; only the Get* helpers mask IRQs — add a guard or assert.
- **`cache.c:290-294`** `L2_CACHED_MEM_TOP` loop is dead (`== L1_CACHED_MEM_TOP` by definition).

---

## 6. Comments — DONE in `0c644d2`

All items corrected (cache alias/clean-and-invalidate notes, sdcard EMMC/printf/SDHBLC,
mailbox alignment rationale, screen hotplug intent, framebuffer font-12/garbled comments,
DeviceInfo trimmed to implemented ops, `stat()`/`times()` stub fixes, MTP extended-event
stall per contract).
