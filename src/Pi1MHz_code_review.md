# Pi1MHz — Code review (re-review pass 2)

**Scope:** `C:\Archlinux\Pi1MHz\src` — the project's own C/header/asm sources only. Vendored upstream code (`BeebSCSI/fatfs`, `usb/tinyusb`, `wifi/lwip`) excluded.

**This pass:** the maintainer made a large round of edits across the whole tree. This document (A) confirms which previously-reported items are now fixed, (B) reports **new bugs introduced by the edits**, and (C) lists what is still outstanding.

**Severity legend:** CRITICAL = clear correctness bug in normal operation · HIGH = likely bug, context-dependent · MEDIUM = performance / clear improvement · LOW = style.

---

## 0. Re-review summary

**Good news — a lot landed correctly.** Verified fixed this pass:

- `rpi/info.c` — `get_cmdline_prop` loop now bounded by `retptr < ret + sizeof(ret) - 1`
- `rpi/cache.c` — `_clean/_invalidate_cache_area` now use `const char *` (no const-cast)
- `framebuffer.c` — **the entire VDU path was rewritten**: `fb_writec` re-entrancy is structurally gone (no more `vdu_index`/`vdu_op`/`vdu_buf` statics), and the VDU queue now has real overflow handling (`vdu_enqueue` drops chunks when full)
- `framebuffer.c` — `graphics_cursor_tab` rewritten with `int` arithmetic and a single window-offset add
- `framebuffer.c` — `vdu23_19` scale-0 divide-by-zero guarded
- `BeebSCSI/scsi.c` — INQUIRY now clamps to stored length with `(i < inqLen) ? buf[i] : 0`
- `BeebSCSI/fcode.c` — picture-number parse now guards `byteCounter` 1..6 before `[byteCounter-1]`
- `Pi1MHz.c` — `Pi1MHzDisable` now accepts index 0 and bounds-checks the upper limit
- `discaccess_emulator.c` — `f_read`/`f_write`/`disk_read`/`disk_write` now bounds-check the JIM-RAM offset+length via `discaccess_buffer_ok`; `f_rename` is guarded by `discaccess_string_ok`
- `M5000_emulator.c` — the `f_open` retry loop is now bounded (`number < 1000`)

Plus the 11 patches from the previous passes (memory clears, SD_ERR macros, precedence, etc.).

**Bad news — the edits introduced 4 new problems, one of which is a regression.** See §1.

**Remaining counts:** ~9 Critical, ~28 High, 28 Medium, ~80 Low — plus the 4 new items below.

---

## 1. NEW issues introduced by the recent edits

These did not exist (or were not this shape) in the original review. Fix these first — a regression is worse than a known bug.

### 1.1 `BeebSCSI/filesystem.c:1564-1570` — REGRESSION: partial final FAT block is now discarded · HIGH
The short-read fix zero-pads correctly but then returns `false`:
```c
fsResult = f_read(&fileObjectFAT, buffer, 256, &byteCounter);
if (fsResult != FR_OK) { ... return false; }

if (byteCounter != 256) {
   for (UINT i = byteCounter; i < 256; i++) buffer[i] = 0;  // pad — good
   return false;                                            // <-- wrong
}
return true;
```
`return false` signals a hard failure to the caller (`scsiBeebScsiFatRead`, scsi.c:~2355), which then aborts the transfer and returns `SCSI_BUSFREE`. **Any FAT file whose size is not a multiple of 256 bytes** produces a short read on its last block — the block is correctly zero-padded but then thrown away, so the host never receives the tail of the file. Before this edit the function returned `true` (serving a stale tail); now it loses the block entirely.

**Fix:** distinguish "short read, data present" from "true EOF":
```c
   if (byteCounter == 0) return false;          // genuine EOF / no data
   if (byteCounter != 256) {
      for (UINT i = byteCounter; i < 256; i++) buffer[i] = 0;
   }
   return true;
```

### 1.2 `framebuffer.c:~2394` — FIQ vs IRQ producer/producer race on `vdu_wp` · HIGH
The rewritten VDU queue has two producers: `fb_emulator_vdu` (runs in **FIQ** context) and `vdu_enqueue` (runs in normal context, called by `fb_writec`/`fb_writes`). `vdu_enqueue` only does `_disable_interrupts_cspr()`, which masks **IRQ but not FIQ**. Sequence:

1. `vdu_enqueue` reads `wp = vdu_wp`, writes its bytes into the ring.
2. A FIQ fires before `vdu_enqueue` does `vdu_wp = wp;`.
3. The FIQ's `fb_emulator_vdu` reads the *old* `vdu_wp`, stores its byte, advances `vdu_wp` to `old+1`.
4. `vdu_enqueue` resumes and does `vdu_wp = wp;` — clobbering the FIQ's advance. The host byte is lost and a ring slot is left stale.

**Fix:** make `fb_emulator_vdu` (the FIQ) the *sole* writer of `vdu_wp`, or have `vdu_enqueue` also disable FIQ across the read-modify-publish window. This is an architectural decision — see §4.

### 1.3 `framebuffer.c:~1180` — `vdu23_19` extended-form gate is too coarse · MEDIUM
The scale-0 divide-by-zero guard (a good fix) was placed as a gate over the *whole* extended-form block:
```c
if (buf[2] != 0 && buf[3] != 0) {
   ... set_scale_w(buf[2]); set_scale_h(buf[3]);
   ... parse buf[4]=h-spacing, buf[5]=v-spacing, buf[6]=rounding
}
```
The `&&` means a command that sets only spacing or rounding (leaving a scale byte as `0` = "ignore") skips the entire block — spacing/rounding never get applied. The documented `VDU 23,19` form treats each metric independently.

**Fix:** gate only the `set_scale_*` calls on their own byte being non-zero, and let the spacing/rounding fields through regardless — e.g. drop the outer `&&` gate and keep the per-field `!= 0 && != 0xff` tests that already exist inside.

### 1.4 `BeebSCSI/scsi.c:1402-1403` — MODESELECT6 VLA still UB for `length == 0` · MEDIUM
The bounds check was added (good — `if (length < 4)` at line 1410 stops the `Buffer[3]` over-read), **but the VLA is declared before the check**:
```c
uint8_t length = commandDataBlock.data[4];
uint8_t Buffer[length];          // line 1403 — VLA created here
...
if (length < 4) { ... return SCSI_STATUS; }   // line 1410 — check runs after
```
If the host sends `data[4] == 0`, a zero-length VLA is created at line 1403 — undefined behaviour in C99 — before the guard can reject it.

**Fix:** use a fixed-size buffer (`uint8_t Buffer[256];` — the max a single 8-bit length can request), or move the `length < 4` return above the VLA declaration. The fixed-size buffer is simplest and also kills the "VLA on the FIQ stack" concern entirely.

---

## 2. Top remaining items (after this pass)

| # | File:line | What | Status |
|---|---|---|---|
| 1 | `BeebSCSI/filesystem.c:1564` | Partial final FAT block discarded | **NEW regression — §1.1** |
| 2 | `framebuffer.c:~2394` | FIQ/IRQ race on `vdu_wp` | **NEW — §1.2** |
| 3 | `discaccess_emulator.c` | FatFS calls still run in FIQ context | still present |
| 4 | `discaccess`/`M5000`/`helpers` `_init` | No guard for `JIM_ram == NULL` after failed malloc | still present |
| 5 | `harddisc_emulator.c:31` | `HD_status` still non-`volatile`, RMW'd from FIQ + main | still present |
| 6 | `harddisc_emulator.c` | `TOC_MAX` ~1e9 busy-wait still blocks all emulators | still present |
| 7 | `rpi/sdcard.c` | `if (!dev) free(ret)` dead check — leaks on every SD error path | still present (changed-but-wrong) |
| 8 | `rpi/screen.c:102` | `context_memory` still not `volatile` | still present |
| 9 | `primitives.c:1300` + `:1158…` | Fill off-by-ones (boundary row/column) | still present (patch 10 not applied) |
| 10 | `videoplayer.c:52` | YUV plane created over unfilled buffer on malloc failure | still present |

---

## 3. Critical / High findings still outstanding

### 3.1 Core / emulators

#### `discaccess_emulator.c` — heavy FatFS calls run from FIQ context · CRITICAL
`discaccess_emulator_command` is still registered `WRITE_FRED` (FIQ callback) and still calls `f_open`, `f_read`, `f_write`, `f_lseek`, `f_mkdir`, `f_rename`, `disk_read`, `disk_write`, etc. directly. Bounds checks were added (good), but the heavy work still freezes the 1MHz bus for the duration of each SD access. **Fix:** queue from FIQ, execute in a polled function. (Design-level — see §4.)

#### `discaccess`/`M5000`/`helpers` `_init` — no guard for failed RAM malloc · HIGH
Patch 11 now sets `Pi1MHz->JIM_ram_size = 0` and leaves `JIM_ram == NULL` when `malloc` fails. But:
- `discaccess_emulator_init` computes `disc_ram_addr = DISC_RAM_BASE`; with `JIM_ram_size == 0`, `DISC_RAM_BASE` = `0 - 32M` wraps to `0xFE000000`, and the first callback dereferences `JIM_ram[0xFE000000]` on a NULL base.
- `M5000_emulator_init` unconditionally does `synth_reset(&m5000, &Pi1MHz->JIM_ram[0x3000])`.
- `helpers.c` indexes `&Pi1MHz->JIM_ram[DISC_RAM_BASE + 0x00FFE000]`.

**Fix:** in `init_emulator` (or each `_init`), skip / disable the JIM-RAM-dependent emulators when `Pi1MHz->JIM_ram == NULL`.

#### `harddisc_emulator.c:31` — `HD_status` non-volatile, RMW from FIQ + main · HIGH
`static uint8_t HD_status;` (the adjacent `HD_IRQ` *is* `volatile`; `HD_status` was not changed). It is read-modify-written from FIQ callbacks and from main-thread SCSI processing. A stale cached value can drop a bit set by the other context. **Fix:** mark `volatile` and serialise the RMW (disable FIQ around it).

#### `harddisc_emulator.c` — multi-second busy-wait timeouts · HIGH
`#define TOC_MAX 1000000000` with `if (++timeoutCounter == TOC_MAX)` in four loops still blocks every other polled emulator while it spins. **Fix:** `RPI_GetSystemTime()`-based microsecond timeout that yields to the poll loop.

#### `videoplayer.c:52` — YUV plane created over an unfilled buffer · MEDIUM
If `malloc(768*576*2)` fails, the code only logs and falls through to `screen_create_YUV_plane(YUV_PLANE, 768, 576, buffer)` + `screen_plane_enable(YUV_PLANE, true)` — enabling a plane over a GPU buffer that was never populated (garbage video). **Fix:** only create/enable the plane when the frame was successfully populated; otherwise leave `YUV_PLANE` disabled.

Other still-open core High items (unchanged from the previous report): `mouseredirect.c` mailbox calls from IRQ context; `Pi1MHz.c` `Pi1MHzDisable` still uses `atoi` (a non-numeric token silently disables emulator 0).

### 3.2 rpi / drivers

#### `rpi/sdcard.c` — `if (!dev) free(ret)` dead check leaks the device struct · HIGH
`dev` is the `struct block_device **` parameter, never NULL at any call site, so `if (!dev) free(ret);` is always false and `ret` (malloc'd when `*dev == NULL`) leaks on **every** SD error path. The test should be `if (*dev == NULL) free(ret);`.

Related: **`sdcard.c:~1364`** — on the SEND_SCR-failure reinit path, `return sd_card_init(&ret);` updates the *local* `ret` but never assigns the outer caller's `*dev`. A caller that passed `*dev == NULL` sees a success return with its pointer still NULL. **Fix:** assign `*dev` before returning.

#### `rpi/screen.c:102` — `context_memory` not `volatile` · HIGH
`static uint32_t* context_memory = (uint32_t*) (PERIPHERAL_BASE+ 0x402000);` — the HVS reads this display-list memory concurrently with ARM writes. **Fix:** `static volatile uint32_t * const context_memory = ...`.

#### Still-open rpi items (unchanged)
- `rpi/mailbox.c` — `RPI_PropertyAdd*` no bounds check vs `PROP_BUFFER_SIZE`.
- `rpi/auxuart.c` — no `_data_memory_barrier()` between writing `tx_buffer[...]` and publishing `tx_head`.
- `rpi/screen.c` — `screen_get_nextplane` reads `plane_valid[planeno + 1]` unbounded (array size 8).
- `rpi/screen.c` — `(h_display - *scaled_width) / 2` can underflow (both `uint32_t`).
- `rpi/sdcard.c:~1605` — `printf("...CMD%ui...")` — `%ui` is a malformed specifier.
- `rpi/audio.c` — `RPI_DMABase->Enable = 1<<5;` overwrites other DMA-channel enables (use `|=`).

### 3.3 framebuffer

#### `primitives.c:1300` — sector/segment fill drops the top row · HIGH (patch 10, still not applied)
```c
if (xc + x1 > g_x_max || xc + x2 < g_x_min || yc - y < g_y_min || yc - y >= g_y_max) return;
```
`>= g_y_max` should be `> g_y_max` — every other clipper treats `g_y_max` as inclusive.

#### `primitives.c:1158, 1161, 1175, 1187, 1190, 1204` — horizontal fill misses edge columns · HIGH
`while (get_pixel(screen, x_right + 1, y) == bg_col && x_right + 1 < g_x_max)` — `<` should be `<=` (and `>` → `>=` for the `x_left` side); the rightmost / leftmost window columns are never filled.

#### Still-open framebuffer items (unchanged)
- `set_graphics_area` rejects single-pixel-wide/high windows (`>=` should be `>`).
- `prim_set_dot_pattern_len` — negative `len` leaves `g_dot_pattern_len` unclamped (latent; only caller passes 0 today).
- `prim_move_copy_rectangle` paints `g_bg_col` onto the destination for off-screen source pixels.
- `prim_define_sprite` doesn't clear `width`/`height` on malloc failure (no crash — `prim_draw_sprite` separately null-checks `data`).

### 3.4 BeebSCSI

#### `scsi.c:1564` — `int` variables passed as `size_t *` · MEDIUM (HIGH on a 64-bit port)
```c
int headerlen, LBAlen, modelen;
headerptr = filesystemGetModeParamHeaderData(..., (size_t *) & headerlen);
```
The callees write a `size_t` through the pointer. On the current 32-bit ARM build `int` and `size_t` are both 4 bytes so it works; on AArch64 the callee writes 8 bytes into a 4-byte stack slot. **Fix:** declare these as `size_t` and drop the casts.

#### `scsi.c:~1603` — MODESENSE6 header loop ignores host allocation length · MEDIUM
`for (int i = 1; i < headerlen; i++) hostadapterWriteByte(headerptr[i]);` sends all header bytes regardless of `sizerequested`; the later `length -= headerlen` can then go negative and the LBA/mode clamp logic behaves erratically. **Fix:** clamp the header send to `min(headerlen, length)`.

#### Still-open BeebSCSI items (unchanged)
- `filesystem.c:974` — `filesytemdattoconfigGeometry` divides by `SectorsPerTrack * BlockSize` with no zero check (malformed `.cfg` → divide-by-zero crash).
- `filesystem.c` — LUN read/write/seek still hard-code `* 256` despite a configurable `BlockSize`.
- `scsi.c:~1752` — VERIFY checks only the start LBA, ignores the block count (a verify spanning past end-of-LUN reports success).
- `scsi.c:850` — REASSIGN BLOCKS loop counter is `uint8_t`; a host `list_length/longlba > 255` wraps it (host-triggered hang).
- `filesystem.c:474` — `f_closedir` can be called on a never-opened `DIR` on some `f_opendir` error paths.

---

## 4. Design-level changes (do not auto-apply)

- **`discaccess_emulator.c` FIQ-context FatFS refactor** — move command dispatch to a polled function + queue.
- **`framebuffer.c` FIQ/IRQ producer race (§1.2)** — decide a single owner of `vdu_wp`, or extend the critical section to FIQ.
- **Hard-coded 256-byte sector size in BeebSCSI** — either drop the configurable `BlockSize` or thread it everywhere.
- **`rpi/cache.c` page-table flags** — shareable-bit decisions are hardware-dependent; the historical "core 3 crashes" comment suggests caution.

---

## 5. Remaining surgical patch

### Patch 10 — `framebuffer/primitives.c:1300` off-by-one on top row · NOT APPLIED
```diff
-   if (xc + x1 > g_x_max || xc + x2 < g_x_min || yc - y < g_y_min || yc - y >= g_y_max) {
+   if (xc + x1 > g_x_max || xc + x2 < g_x_min || yc - y < g_y_min || yc - y > g_y_max) {
       return;
    }
```

---

## 6. Suggested order of work

1. **§1.1** — revert the `return false` regression on partial FAT blocks (data loss, common case).
2. **§1.4** — fixed-size `Buffer[256]` in MODESELECT6 (kills the VLA UB and the FIQ-stack concern in one move).
3. **§1.3** — loosen the `vdu23_19` extended-form gate.
4. **§1.2** — decide the `vdu_wp` ownership and close the FIQ race.
5. **Patch 10** — one-character fill clip fix; then the `:1158…` family.
6. The `JIM_ram == NULL` guards (§3.1) — small and prevents a hard fault on low-memory boot.
7. `sdcard.c` `if (*dev == NULL)` leak fix.
8. The remaining High items at your own pace.

---

## 7. Static-analysis suggestion

Run cppcheck (`Pi1MHz.cppcheck` config exists) and a `-Wall -Wextra -Wsign-conversion -Wconversion -Wcast-align -Wundef -Wunreachable-code` build. The MODESELECT6 VLA-before-check, the `int`/`size_t` punning, and several of the off-by-ones would surface automatically.

---

*Re-review pass 2, 2026-05-23. Fixed items from earlier passes have been removed. File:line citations refer to the source tree as of this date.*
