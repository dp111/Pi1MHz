# Pi1MHz — Deep-Dive Code Review

**Scope:** `C:\Projects\Pi1MHz\src`, excluding the `wifi/` directory (and the lwIP tree it
pulls in) as requested.
**Date:** 2026-05-22
**Reviewer:** Claude (Cowork)
**Build context:** bare-metal `arm-none-eabi-gcc`, `-std=gnu23 -O2 -flto`,
`--specs=nosys.specs -nostartfiles`, a very aggressive warning set, no OS, a main poll loop
plus FIQ (1MHz-bus) and IRQ handlers, and a custom `sbrk` heap.

## Methodology

This is a fresh independent pass over the codebase, performed after the previous review
(`Pi1MHz_Code_Review.md`, 2026-05-14). Every compiled `.c`/`.h` file in the project's own
code was read — the root emulators, `rpi/`, `framebuffer/`, `BeebSCSI/`, and the USB glue
(`usb.c`, `usb/mtp_fs.c`, `usb/board_millis.c`). The vendored libraries that are part of
the build (`BeebSCSI/fatfs/`, `usb/tinyusb/src/`) were scanned rather than read line by
line; the TinyUSB *examples* directory is not compiled and was not reviewed. Each finding
below was checked against the current source.

A good number of the previous review's findings have since been fixed — those are listed
under "Already addressed" so this report can focus on what is still open. New findings not
in the previous review are marked **(new)**.

---

## Summary of open findings

| # | Severity | File:line | Issue |
|---|----------|-----------|-------|
| H1 | High | `discaccess_emulator.c:98,106,131,156` | Host-controlled 32-bit offset used to index `JIM_ram[]` with no bounds check — arbitrary read/write |
| M1 | Medium | `mouseredirect.c:291,294,297` | `mouse_pointer` (0–254) indexes a 4-entry sprite table — OOB address handed to the HVS |
| M2 | Medium | `rpi/cache.c:113–131` | `_clean_cache_area` ARMv7+ path has no DSB — stale-data DMA hazard |
| M3 | Medium | `rpi/fileparser.c:61,90` | Config buffer dereferenced before the `ptr < max` bound is tested |
| M4 | Medium | `rpi/fileparser.c:361` | `parse_releasekeyvalues` frees `v.string` without nulling it — double-free on reuse |
| M5 | Medium | `rpi/fileparser.c:113,122–330` | `malloc(filesize*4)` can overflow; almost all `outbuf[]` writes are unbounded |
| M6 | Medium | `harddisc_emulator.c:31,67–73` | `HD_status` is non-`volatile` and read-modify-written from both FIQ and the poll loop |
| M7 | Medium | `BeebSCSI/scsi.c:1479` | MODE SELECT page-length check is off by 2 — 2-byte OOB read of the parameter buffer |
| M8 | Medium | `BeebSCSI/filesystem.c:1124,1149` | Read6/Write6 never validate LBA+count; short `f_read` leaks stale `sectorBuffer` to host |
| M9 | Medium | `BeebSCSI/filesystem.c:476–482` | `filesystemCheckLunDirectory` reports success on a real error when debug is off |
| M10 | Medium | `framebuffer/framebuffer.c:762–766` | `graphics_cursor_tab` adds the text-window origin twice |
| M11 | Medium | `framebuffer/framebuffer.c:1945–1948,2390` | VDU queue has no overflow check (`// TODO` still in source) |
| M12 | Medium | `rpi/auxuart.c:59,141` | Non-atomic read-modify-write race on `MU_IER` between the UART IRQ and the poll loop |
| M13 | Medium | `rpi/mailbox.c:147–149` | `RPI_PropertyProcess` polling loop has no timeout; can hang with interrupts disabled |
| M14 | Medium | `videoplayer.c:33–39` | GPU buffer from `screen_allocate_buffer` is used by `memset`/`decompress_lz4` without a NULL check |
| M15 | Medium (new) | `M5000_emulator.c:384–390` | `music5000_rec_stop` loops forever if `f_open` returns any error other than `FR_EXIST` |
| M16 | Medium (new) | `ram_emulator.c:167`, `Pi1MHz.h:205` | `JIM_ram_size << 24` / `* 16*1024*1024` evaluated in `int` — signed overflow once the value reaches 0x80 |
| L1–L12 | Low/Info | various | Hardening items — see "Low / informational" |

---

## High

### H1. Disc-access command handler is an arbitrary memory read/write primitive

`discaccess_emulator.c`, `discaccess_emulator_command()` (lines 83–273).

The command structure lives in JIM RAM and is filled in by the host (the Beeb). The
handler then does, for every transfer command:

```c
disk_read( Pi1MHz->JIM_ram[command_pointer+1],
           &Pi1MHz->JIM_ram[ jim_read32(command_pointer+4) + base_addr ],
           jim_read32(command_pointer+8),
           jim_read32(command_pointer+12) );
```

`jim_read32(command_pointer+4)` is a fully host-controlled 32-bit value. It is added to
`base_addr` and used directly as an index into `JIM_ram[]`, and the length
(`jim_read32(command_pointer+8/12)`) is host-controlled too. Nothing checks that the
resulting buffer lies inside the JIM RAM allocation. The same pattern is in the
`f_read`/`f_write` cases (lines 131, 156). On bare metal this is a clean arbitrary
read/write primitive: a malformed offset silently corrupts the heap, the `Pi1MHz_t`
structure at `0x100`, or a peripheral.

Secondary issue in the same function: the path-string cases
(`f_open`/`f_opendir`/`f_mkdir`/`f_chdir`/`f_rename`/`f_unlink`, e.g. line 114) pass
`(char *)&Pi1MHz->JIM_ram[command_pointer+N]` straight to FatFs. `strlen`/FatFs then run
over host data with no length bound; an unterminated name reads until it happens to hit a
zero byte.

**Fix:** before each `disk_*`/`f_*` call, validate `offset` and `offset+length` against the
JIM RAM size (mask the offset to `(JIM_ram_size<<24)-1` and reject lengths that run past
the end). For the string arguments, bound the scan to the JIM RAM region (e.g. `strnlen`
with the remaining region size).

This is the most serious item in the codebase. It is reachable from ordinary Beeb
software, so even without a malicious actor a buggy 6502 program will corrupt Pi memory in
ways that are very hard to debug.

---

## Medium

### M1. Mouse pointer index is unbounded — OOB address passed to the HVS

`mouseredirect.c`, `mouse_redirect_move_mouse()` (lines 279–303).

`mouse_pointer` is read from `Pi1MHz_MemoryRead(fred_address + 4)` — a host-written byte.
The only value rejected is `255` ("off", line 286). Any value 4–254 then reaches:

```c
screen_create_RGB_plane(MOUSE_PLANE, PTRMODE0WIDTH, PTRMODEHEIGHT, 0.5, 256, 3,
    (uint32_t) &mouse_pointer_data[PTRMODE0WIDTH*PTRMODEHEIGHT*mouse_pointer]);
```

`mouse_pointer_data[]` only holds `PTRMAX` (4) sprites per mode (total 2688 bytes). With
`mouse_pointer = 10` the index is `24*16*10 = 3840`, well past the array, and that address
is handed to the HVS as a plane source — the display controller then DMAs from arbitrary
memory.

**Fix:** reject out-of-range values up front, e.g.
`if (mouse_pointer >= PTRMAX && mouse_pointer != 255) return;` (or clamp).

### M2. `_clean_cache_area` ARMv7+ path is missing its DSB

`rpi/cache.c`, `_clean_cache_area()` (lines 111–136).

The pre-ARMv7 branch ends with `_data_memory_barrier()`. The ARMv7+ branch issues the
`DCCMVAC` loop and returns with **no barrier**. Cache-clean operations are not guaranteed
visible to other observers (the DMA engine, the VideoCore) until a DSB.
`_invalidate_cache_area` correctly ends with a barrier for both paths; `_clean_cache_area`
is the odd one out. `audio.c` (`rpi_audio_samples_written`, `init_dma_buffer`) and
`mailbox.c` (`RPI_Mailbox0Write`) both rely on `_clean_cache_area` to push data out before
the GPU/DMA reads it, so this is a real intermittent stale-data hazard on Pi 2/3/4.

**Fix:** add `_data_synchronization_barrier();` at the end of the ARMv7+ path (the function
already exists in `asm-helpers.c`).

### M3. Config-file buffer is read one byte past the end

`rpi/fileparser.c`, `parse_numstrlen()` line 61 and `parse_strlen()` line 90.

```c
while ((buf[ptr] > ' ') && (buf[ptr] != '#') && (ptr < max))
```

`buf[ptr]` is dereferenced before `ptr < max` is evaluated, so at end-of-buffer this reads
`buf[max]` — one byte past the `malloc`'d file image.

**Fix:** put the bound first: `while ((ptr < max) && (buf[ptr] > ' ') && (buf[ptr] != '#'))`.

### M4. `parse_releasekeyvalues` frees without nulling — double-free on reuse

`rpi/fileparser.c`, lines 355–365.

```c
if (values[i].v.string != NULL) {
   free(values[i].v.string);
   values[i].length = 0;
}
```

`values[i].v.string` is freed but left dangling. These `keyvalues` arrays are reused across
config re-reads (`filesystem.c`); a second `parse_releasekeyvalues`, or any later
`!= NULL` test, will double-free or read freed memory.

**Fix:** add `values[i].v.string = NULL;` after the `free`.

### M5. `malloc(filesize*4)` can overflow and the output writes are unbounded

`rpi/fileparser.c`, `parse_readfile()` lines 113 and 122–330.

`filesize` comes from `filesystemReadFile(filename, &buffer, 0)` — the `0` means "no size
limit", so a large config file yields a large `filesize`, and `filesize * 4` can wrap
`size_t`, producing a tiny `outbuf`. Separately, only the `INTEGER` branch (lines 218–220)
bounds-checks its writes into `outbuf`; every other `outbuf[outptr++]` (whitespace copy,
comment copy, key copy, NUMSTRING/STRING expansion, end-of-line copy) is unbounded.

**Fix:** cap `filesize` at a sane maximum, compute the output size with an explicit
overflow check, and bounds-check every `outbuf[outptr++]` against the allocation.

### M6. `HD_status` is non-`volatile` and updated non-atomically across contexts

`harddisc_emulator.c`, `HD_status` (line 31), `hd_emulator_status()` (lines 67–73).

`hd_emulator_status` does `HD_STATUS_Write(HD_STATUS_Read & ~bit)` /
`HD_STATUS_Write(HD_STATUS_Read | bit)` — a read-modify-write of `HD_status`. It is called
both from FIQ-context memory callbacks (`hd_emulator_IRQ`) and from the poll-loop SCSI
engine (`hostadapterWriteRequestFlag`, `hostadapterWriteBusyFlag`, …). A FIQ landing in the
middle of the poll loop's RMW loses the change. `HD_IRQ_ENABLE` was correctly made
`volatile`; `HD_status` was not.

Also line 72: `HD_STATUS_Write( HD_STATUS_Read |= (uint8_t)bit )` both mutates `HD_status`
via `|=` *and* writes it again inside the macro — redundant, and inconsistent with the
clear branch on line 70. Drop the `|=` and pass `HD_STATUS_Read | bit`.

**Fix:** make `HD_status` `volatile`, and for true correctness bracket the RMW with
interrupt disable/restore (or give `HD_status` a single owning context).

### M7. MODE SELECT page-length check is off by 2

`BeebSCSI/scsi.c`, `scsiCommandModeSelect6()` line 1479.

```c
if (start + len  > length)            // rejects start+len > length
   { ... return SCSI_STATUS; }
filesystemWriteModePageData(commandDataBlock.targetLUN, page, (uint8_t)(len+2), &Buffer[start]);
```

`filesystemWriteModePageData` is given `len + 2` bytes starting at `Buffer[start]`, so it
reads `Buffer[start .. start+len+1]`. The guard only ensures `start+len <= length`, so when
`length-2 < start+len <= length` the call reads up to 2 bytes past `Buffer[length]` (a VLA
on the stack). `length` is host-controlled.

**Fix:** change the test to `if (start + len + 2 > length)`.

(The previous review's wider MODE SELECT concern is otherwise addressed — there is now an
explicit `length < 4` check at line 1410.)

### M8. Read6/Write6 never validate LBA+count; short reads leak stale data

`BeebSCSI/filesystem.c`, `filesystemOpenLunForRead()` line 1119 and
`filesystemReadNextSector()` line 1139 (and the Write counterparts).

`scsiCommandRead6` takes `logicalBlockAddress` and `numberOfBlocks` straight from the host
CDB and passes them to `filesystemOpenLunForRead`, which simply does
`f_lseek(fileObject, startSector * 256)` with no check against
`filesystemGetLunTotalSectors()`. `scsiCommandVerify` *does* range-check
(`logicalBlockAddress >= lunSizeInSectors`); Read6/Write6 do not.

Compounding it, `filesystemReadNextSector` (line 1149) checks `fsResult != FR_OK` but never
compares the returned `fsCounter` against the requested byte count. A read past EOF returns
`FR_OK` with a short count and leaves stale `sectorBuffer` contents, which are then sent to
the host — an information leak. A Write6 past EOF extends the `.dat` file arbitrarily.

**Fix:** validate `logicalBlockAddress + numberOfBlocks <= filesystemGetLunTotalSectors(LUN)`
in Read6/Write6 (mirroring `scsiCommandVerify`), and treat a short `f_read`/`f_write` count
as an error (or zero-fill the remainder).

### M9. `filesystemCheckLunDirectory` reports success on a real error

`BeebSCSI/filesystem.c`, lines 476–485.

```c
if (fsResult != FR_OK) {
   if (debugFlag_filesystem) {
      debugString_P(...);
      filesystemPrintfserror(fsResult);
      return false;            // <-- inside the debug-flag block
   }
}
return true;
```

If `f_opendir` fails with anything other than `FR_NO_PATH` **and** debug is off, control
falls through to `return true`, reporting the LUN directory as present when it is not.

**Fix:** move `return false;` out of the `if (debugFlag_filesystem)` block.

### M10. `graphics_cursor_tab` adds the text-window origin twice

`framebuffer/framebuffer.c`, `graphics_cursor_tab()` lines 752–767.

```c
x = (uint16_t)(x * (font_width  << screen->xeigfactor));
y = (uint16_t)(y * (font_height << screen->yeigfactor));
x = (uint16_t)(x + g_window.left);     // origin added once
y = (uint16_t)(y + g_window.bottom);
g_x_pos = (int16_t)(g_window.left   + x);   // ...and again
g_y_pos = (int16_t)(g_window.bottom + y);
```

`g_window.left`/`g_window.bottom` are added on both line 762/763 and 765/766. With any
non-default text window the graphics cursor lands at twice the window origin plus the
scaled offset. (Truncation to `uint16_t` was tightened from the previous `uint8_t`, which
is an improvement, but the double-add remains.)

**Fix:** compute the position once, e.g.
`g_x_pos = (int16_t)(g_window.left + buf[1]*(font_width<<screen->xeigfactor));` and the
same for `y`.

### M11. VDU queue has no overflow protection

`framebuffer/framebuffer.c`, `fb_writec_buffered()` lines 1944–1950 and `fb_emulator_vdu()`
around line 2390.

Both producers do `vdu_queue[vdu_wp] = c; vdu_wp = (vdu_wp + 1) & (VDU_QSIZE-1);` with no
test against `vdu_rp` — the source still carries `// TODO: Deal with overflow`. If the Beeb
outruns the consumer (`fb_process_vdu_queue`), `vdu_wp` wraps past `vdu_rp` and the consumer
then processes up to `VDU_QSIZE` stale bytes. `VDU_QSIZE` is 8192 so it is unlikely in
practice, but the failure is silent corruption.

**Fix:** when `((vdu_wp + 1) & (VDU_QSIZE-1)) == vdu_rp`, drop the byte (or spin). The
cross-context locking is otherwise fine — `fb_writec_buffered` now brackets the write with
interrupt disable/restore.

### M12. Non-atomic read-modify-write race on `MU_IER`

`rpi/auxuart.c` — IRQ handler line 59 (`MU_IER &= ~AUX_MUIER_TX_INT`) versus
`RPI_AuxMiniUartWrite` line 141 (`MU_IER |= AUX_MUIER_TX_INT`).

`RPI_AuxMiniUartWrite` runs in poll context with interrupts enabled; a UART IRQ landing
between the load and store halves of the `|=` loses the handler's clear of the
TX-interrupt bit. The buffer-full deadlock the previous review worried about is now
mitigated (lines 127–134 drop the character when IRQs are disabled), so the residual
impact is limited to a delayed character or a spurious TX interrupt — but it is still a
genuine RMW race.

**Fix:** bracket the `MU_IER |= …` with interrupt disable/restore (the pattern is already
used in `fb_writec_buffered`).

### M13. `RPI_PropertyProcess` mailbox read has no timeout

`rpi/mailbox.c`, lines 147–149.

```c
do {
   result = RPI_Mailbox0Read( MB0_TAGS_ARM_TO_VC );
} while ((uint32_t) result != ((uint32_t) pt) >> 4);
```

There is no bound on this loop, and `RPI_Mailbox0Read` itself spins unbounded on
`ARM_MS_EMPTY`. `RPI_PropertyGetWord`/`RPI_PropertyGetBuffer` call this with interrupts
disabled, so a VideoCore failure hangs the system with interrupts off.

Related: interrupt protection of the shared `pt[]` buffer is asymmetric.
`RPI_PropertyGetWord`/`GetBuffer` disable interrupts around the whole sequence, but
`RPI_PropertySetWord` and the bare `RPI_PropertyStart`/`Add`/`Process` sequences (used in
`Pi1MHz.c` `init_emulator` and throughout `screen.c`) do not. If any interrupt-context code
path ever issues a mailbox call it will corrupt a poll-loop sequence mid-build. It is
latent today — confirm no IRQ/FIQ path reaches `screen_allocate_buffer`/mailbox — but the
asymmetry is fragile.

**Fix:** add a bounded retry count to the read loop; protect all `pt[]` builders
consistently, or document that they are poll-loop-only.

### M14. Video buffer used without a NULL check

`videoplayer.c`, `videoplayer_init()` lines 27–39.

```c
buffer = screen_allocate_buffer( 768*576*2, &handle );
uint8_t * buf = malloc(768*576*2);
if (buf) {
   if (filesystemReadFile("frame.lz",&buf,768*576*2))
      decompress_lz4(buf, (uint8_t*) buffer);          // writes through buffer
   else { uint8_t *dst = (uint8_t *)(uintptr_t)buffer; memset(dst, 0, ...); }
   ...
}
```

The `if (buf)` test only covers the temporary `malloc`. `buffer` — the GPU buffer returned
by `screen_allocate_buffer`, which returns `0` on failure — is never checked, yet
`decompress_lz4`/`memset` write up to ~860 KB through it. If allocation fails this is a
write starting at address 0.

Also note line 26: `screen_release_buffer(handle)` sits inside `if (!handle)`, where
`handle` is always 0 — a dead call.

**Fix:** check `buffer` (and `handle`) after `screen_allocate_buffer` and bail out cleanly
on failure; drop the dead `screen_release_buffer` call.

### M15. `music5000_rec_stop` can loop forever on a disk error *(new)*

`M5000_emulator.c`, lines 384–390.

```c
do {
   sprintf(fn,"Musics%.3i.wav",number);
   result = f_open( &music5000_fp, fn, FA_CREATE_NEW | FA_WRITE);
   if ( result == FR_EXIST ) number++;
} while ( result != FR_OK );
```

`number` is only advanced on `FR_EXIST`. If `f_open` returns any *other* error
(`FR_DISK_ERR`, `FR_NOT_READY`, `FR_WRITE_PROTECTED`, …) the loop neither advances nor
exits — it spins forever re-opening the same filename, hanging the poll loop.

**Fix:** break out of the loop on any result that is neither `FR_OK` nor `FR_EXIST`, and
report the failure.

### M16. `JIM_ram_size << 24` overflows a signed `int` *(new)*

`ram_emulator.c:167` and the `DISC_RAM_BASE` macro in `Pi1MHz.h:205`.

```c
// ram_emulator.c:167
filesystemReadFile("JIM_Init.bin",&Pi1MHz->JIM_ram,(size_t)(Pi1MHz->JIM_ram_size<<24));
// Pi1MHz.h:205
#define DISC_RAM_BASE ((uint32_t)( (Pi1MHz->JIM_ram_size) * 16 * 1024 * 1024 )- DISC_RAM_SIZE)
```

`JIM_ram_size` is `uint8_t`; it promotes to `int`, and the `<< 24` (line 167) and
`* 16*1024*1024` (the macro) are then evaluated in `int`. For any value `>= 0x80` that
shifts/multiplies into or over the sign bit — undefined behaviour, and the `(uint32_t)` /
`(size_t)` cast is applied to the *result*, too late. `DISC_RAM_BASE` is used throughout
`discaccess_emulator.c` and `helpers.c`.

`JIM_ram_size` is derived from `mem_info(1)` masked with `0xFF000000`, so this is latent on
the usual target boards (Pi Zero/3A+ ≈ 512 MB give a value around 0x1C) but becomes live on
a board with ≥ 2 GB of ARM-visible RAM. Under `-O2 -flto` signed-overflow UB is not benign.

For comparison, `ram_emulator.c:150` (`((size_t)Pi1MHz->JIM_ram_size - 1)<<24`) and
`:157` (`(size_t)Pi1MHz->JIM_ram_size<<24`) already cast *before* the shift and are
correct — apply the same pattern: cast to `size_t`/`uint32_t` before shifting/multiplying,
or make the literals unsigned (`16u * 1024u * 1024u`).

---

## Medium-low / hardening

**ML1. Text-rendering path has no clipping.** `framebuffer/screen_modes.c`
`default_set_pixel_8/16/32bpp` (lines 1076–1093) do raw pointer arithmetic with no bounds
check. The graphics primitives clip before calling `set_pixel`, but the *text* path
(`default_write_char` in `fonts.c:246`, `default_clear_screen`, `default_scroll_screen`,
`to_rectangle`) calls `screen->set_pixel` directly. It stays in range today only because of
caller window invariants. Clamp the rectangle in `to_rectangle`, or make
`default_set_pixel_*` clip. (The strict-aliasing half of the previous review's finding here
is fixed — these now use `memcpy` + `__builtin_assume_aligned`.)

**ML2. `snprintf` return value misused.** `helpers.c:54` —
`size = (size_t)snprintf(helpscreen, helpscreen_size, " %2.1fC", get_temp());`. `snprintf`
returns the length it *would* have written, which can exceed `helpscreen_size`; the
following `helpscreen += size; helpscreen_size -= size;` would then advance past the buffer
and underflow `helpscreen_size`. Harmless today (the 1 KB help buffer is far larger than
the text) but the pattern is unsafe — clamp `size` to `helpscreen_size - 1`.

**ML3. Cross-emulator dependency on `JIM_ram` being non-NULL.** If `rampage_emulator_init`
fails to `malloc` JIM RAM, `Pi1MHz->JIM_ram` is NULL and `JIM_ram_size` is 0, yet
`M5000_emulator_init` still runs `synth_reset(&m5000, &Pi1MHz->JIM_ram[0x3000])`
(`M5000_emulator.c:476`) and `helpers_screen_setup` still writes to
`&JIM_ram[DISC_RAM_BASE + 0x00FFE000]` (`helpers.c:124`). Both dereference a bad pointer.
Guard these emulators on `JIM_ram != NULL` / `JIM_ram_size != 0`.

**ML4. `filesystemReadFile` is passed the address of the live heap pointer.**
`ram_emulator.c:167` passes `&Pi1MHz->JIM_ram`. It works with the current
`filesystemReadFile` implementation, but `helpers.c:135` and `videoplayer.c:32` correctly
use a local temp for exactly this reason — if `filesystemReadFile` ever writes back through
the pointer, the heap pointer is corrupted. Use a local `uint8_t *p = Pi1MHz->JIM_ram;`.

**ML5. `baud_rate` division by zero.** `rpi/auxuart.c:104`
`MU_BAUD = ((sys_freq*2 / (8*baud)) - 1)/2`. `baud` comes from `atoi` of a `command.txt`
value; `baud_rate=0` (or a non-numeric value) makes `8*baud == 0`. Validate `baud != 0`
before the divide. `sys_freq` from `get_clock_rate` is likewise unchecked for 0.

**ML6. Non-RPI4 mailbox responses are not cache-invalidated.** `rpi/mailbox.c:151–153`
invalidates `pt[]` after the VideoCore writes the response **only** when `RPI4` is defined.
On Pi 0/1/2/3 the ARM may read a stale cached copy of the response. It evidently works in
practice, but the asymmetry with the RPI4 path is worth confirming against the page-table
attributes for `pt`.

---

## Low / informational

- **L1.** `rpi/gpio.c:91` uses `1 << (gpio-32)` where the sibling lines (74, 87) use
  `1u <<` — cosmetic inconsistency, no overflow given `gpio-32 <= 21`.
- **L2.** `rpi/cache.c:117,145` suppress `-Wdiscarded-qualifiers` with a `#pragma` for
  `char *startptr = start;`. The cache ops never write the memory — declare
  `const char *startptr` and drop the pragma.
- **L3.** `rpi/exception.c:86` `dump_info` dereferences memory around the faulting address;
  a bad address triggers a nested abort instead of the intended reboot. Acceptable for a
  crash handler, but worth a comment.
- **L4.** `rpi/screen.c:855` `if (~(RPI_hdmi->hotplug)&1)` enables the plane when the
  hotplug bit is **0**. Confirm this is not inverted (the struct comment says bit 0 is the
  hotplug state).
- **L5.** `framebuffer/framebuffer.c:1849` `fb_custom_mode` `do { xeigfactor++;
  x_pixels <<= 1; } while (x_pixels < 1280);` loops forever and overflows `xeigfactor` if
  `x_pixels == 0`; `vdu23_22` does not reject 0.
- **L6.** `BeebSCSI/fcode.c:594` `fcodeReadBuffer` has its entire body `#if 0`'d — dead
  code; remove it or the prototype.
- **L7.** `Pi1MHzvc.c:25` defines `Pi1MHzvc_asm_len` which is never referenced.
- **L8.** `discaccess_emulator.c:42` `disc_ram_addr_old = disc_ram_addr - 1` underflows on
  the first call — harmless (only a high-byte-changed comparison) but sloppy.
- **L9.** `rpi/armc-cstubs.c:255` `_read` declares its buffer `const char *ptr` — reading
  *into* a const buffer is nonsensical and mismatches newlib's prototype. The body is a
  stub returning 0, so it is harmless.
- **L10.** `M5000_emulator.c:385` `sprintf(fn,"Musics%.3i.wav",number)` into `char fn[22]`
  — fits today, but `%i` of a worst-case `int` plus the literals is exactly at the buffer
  limit. Use `snprintf`.
- **L11.** `ram_emulator.c:109` `putstring` advances by `strlcpy`'s return value (the
  *source* length, which can exceed `PAGE_SIZE`). Safe with the current 1–13 char callers;
  clamp to `PAGE_SIZE-1` to make it robust.
- **L12.** `rpi/info.c:116` `get_cmdline_prop` does a substring (`strcasestr`) match. It is
  guarded by requiring `=` immediately after, but a short property name that is a suffix of
  a longer key could still mis-match. Callers use full names, so this is latent.

---

## Already addressed since the 2026-05-14 review

For confidence, these previously-reported items were checked and are now fixed:

- **Page RAM bounds** — `ram_emulator_page_addr_high` now clamps with `>=`
  (`ram_emulator.c:70`); with the high byte clamped, page/byte JIM RAM accesses stay in
  bounds.
- **Negative emulator index** — `init_emulator` now checks `idx >= 0` (`Pi1MHz.c:321`).
- **Poll-table overflow** — `Pi1MHz_Register_Poll` now checks
  `Pi1MHz_polls_max >= NUM_EMULATORS` (`Pi1MHz.c:228`).
- **M5000 recording overrun** — `store_samples` now caps `Audio_Index` against `ram_max`
  (`M5000_emulator.c:411`).
- **Mouse `change` flag** — now declared `volatile` (`mouseredirect.c:250`).
- **`_sbrk` heap limit** — now enforced via `arm_setup_heap_limit`/`heap_limit`
  (`rpi/armc-cstubs.c:152–176`); the big JIM RAM `malloc` is now NULL-checked
  (`ram_emulator.c:158`).
- **Command-line buffer overflow** — `get_cmdline` now clamps the copy length
  (`rpi/info.c:98`); `get_cmdline_prop` now bounds its copy.
- **`screen.c` integer-division-to-float** — `case 768` is now `3.0/2` (`rpi/screen.c:494`).
- **GPIO `1 << gpio`** — now `1u << gpio` (one cosmetic exception, L1).
- **Sprite 16 bpp capture** — now reads `y1 + yp` (`framebuffer/primitives.c:1896`).
- **`copy_font_character` off-by-one** — now `c >= font->num_chars`
  (`framebuffer/fonts.c:136`).
- **Framebuffer pixel strict-aliasing** — `default_set/get_pixel_16/32bpp` now use
  `memcpy` + `__builtin_assume_aligned` (`framebuffer/screen_modes.c:1081–1108`); the same
  fix was applied to `Pi1MHz_MemoryWrite16/32` and the `discaccess` JIM accessors.
- **`fcode.c` `byteCounter-1` underflow** — the `0x46` handler now guards
  `byteCounter == 0` before using `byteCounter-1` (`BeebSCSI/fcode.c:300`).
- **MODE SELECT short-length** — `scsiCommandModeSelect6` now rejects `length < 4`
  (`BeebSCSI/scsi.c:1410`).
- **Font buffer sizing** — `MAX_FONT_HEIGHT` is 32 and the font buffer is sized
  `MAX_HEIGHT*MAX_CHARACTERS*2`; the rounded-font worst case fits.

---

## Cross-cutting themes

1. **Host/config-controlled values used as indices without bounds checks.** H1, M1, M3,
   M5, M7, M8 are all the same root cause — JIM RAM offsets, SCSI CDB fields, VDU
   parameters and config-file contents indexing fixed buffers. On bare metal an
   out-of-bounds access corrupts memory silently rather than faulting, so these matter more
   than on a hosted system. Validating untrusted lengths/offsets at the boundary would
   close most of them.
2. **Sharing state across FIQ / IRQ / poll without `volatile` or masking.** M6 (`HD_status`)
   and M12 (`MU_IER`) are concrete; M13 notes the mailbox `pt[]` asymmetry. The codebase is
   mostly careful here (the mouse flag and `HD_IRQ_ENABLE` were fixed) — `HD_status` is the
   notable straggler.
3. **Cache / barrier discipline.** M2 (`_clean_cache_area` missing DSB) and ML6 (non-RPI4
   mailbox invalidate) both affect DMA correctness; they are easy, localized fixes.
4. **Error returns swallowed.** M9 (LUN directory), M8 (short `f_read`), and M15
   (`f_open` loop) all continue as if successful on a real failure.

## Suggested priority order

1. **H1** — the disc-access arbitrary read/write.
2. **M2, M12** — cache DSB and the `MU_IER` race (DMA/UART correctness, tiny fixes).
3. **M3, M7, M8** — the remaining host-controlled out-of-bounds reads.
4. **M15, M16, M14** — the new infinite-loop / overflow / NULL-deref bugs.
5. **M4, M9, M5** — the file-layer use-after-free and error-swallowing.
6. **M1, M6, M10, M11, M13** — robustness and concurrency hardening.
7. Medium-low and Low/Info cleanup — much of this the strict warning set would surface if
   `-Werror` were enabled for the project's own translation units.
