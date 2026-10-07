# USB flash drive storage alongside a USB mouse - plan

STATUS (2026-10-07): PLAN ONLY - nothing implemented.  Written from a
read-only design study of master 0992048.  Claims are marked MEASURED (read
in the code), INFERRED or GUESSED.

## Goal

Boot from the SD card exactly as now.  If `Pi1MHz.cfg` asks for it and a USB
flash drive is plugged in, the emulators' storage (`/BeebSCSI*`,
`/BeebVFS*`, later the FAT service, FujiNet and the video) comes from the
flash drive instead - while a USB mouse keeps working on the same port
through a hub.  `Pi1MHz.cfg`, the kernel and the boot files stay on the SD
card.

## What exists today

| Piece | State |
|---|---|
| USB host | Default role since 49e4483; polled, never interrupt-driven (`src/usb.c`, see its comment on the board reset).  MEASURED |
| Hubs | `CFG_TUH_HUB 3`, `CFG_TUH_DEVICE_MAX 8` (`src/usb/tusb_config.h` ~187); dwc2 has split-transaction support for a low-speed mouse behind a high-speed hub.  MEASURED |
| Host mass storage | TinyUSB's `class/msc/msc_host.c` is vendored but not enabled: no `CFG_TUH_MSC`, and not in either source list in `src/CMakeLists.txt`.  API: `tuh_msc_read10/write10` with a completion callback, `tuh_msc_mount_cb/umount_cb`, `tuh_msc_get_block_count/size`.  MEASURED |
| FatFs | One volume (`FF_VOLUMES 1`), 512-byte sectors fixed, `FF_FS_REENTRANT 0`, `FF_FS_RPATH 2`, no exFAT (`src/BeebSCSI/fatfs/ffconf.h`).  MEASURED |
| diskio | Drive 0 = SD (`diskio.c`); its read-ahead cache already records which drive it holds, so a second drive can share it.  MEASURED |
| Card swap | `hd_card_service` (`src/harddisc_emulator.c`) ejects, remounts and re-inserts at run time, with eject/insert/remount hooks (`filesystemRegisterEject`, `filesystemRegisterRemount`).  MEASURED |
| Controller | Slave mode, no DMA, so no cache maintenance or alignment needs.  MEASURED (config) |

## Design

### Volumes - recommended: the drive is a second FatFs volume, `"1:"`

- `FF_VOLUMES 2`; diskio gets `DRV_USB 1` beside `DRV_SD 0`.
- A storage-root prefix ("" or "1:") is applied only where the emulators
  build paths:
  - `fsLunFilePath` (filesystem.c ~527) and its VFS lookups (~805);
  - `videoplayer.c` (~1112; `pvf_path[32]` must grow);
  - `fat_service.c` (it strips `"0:"`, ~143);
  - `fujinet/fn_store_fatfs.c` (~59, ~228);
  - the `f_mount` calls and `fsObject` in filesystem.c (~125, ~314, ~399,
    ~429, ~482).
- The webserver and the config keep using the SD card; a `/usb` folder in
  WebDAV is a later step.
- Rejected alternative: `f_chdrive("1:")` moves every un-prefixed path at
  once (WebDAV, config writes, the hooks), and unplugging the drive would hit
  every subsystem together.  MEASURED (ff.c CurrVol), INFERRED (consequence).

### Reading from the drive

- `disk_read`/`disk_write` are synchronous; TinyUSB's MSC is asynchronous.
  The USB diskio issues `tuh_msc_read10`, then services the host
  (`tuh_int_handler` + `tuh_task`) in a loop until the completion callback
  sets a flag, with a time limit.  INFERRED.
- The loop must not call `chainboot_poll`, and nothing in the MSC mount
  callback may touch FatFs (it would re-enter `tuh_task`); the callback only
  sets a flag.  INFERRED.
- FIQ rule: unaffected - the loop runs only from main-loop callers, as SD
  access does now.  INFERRED.
- Refuse a drive whose block size is not 512, and an exFAT drive (FatFs has
  exFAT off).  INFERRED.

### When to switch

- New key, e.g. `storage=usb` (default `sd`).  `Pi1MHz.cfg` is read before
  USB is even powered (`Pi1MHz.c` ~536; USB power-up ~527 ms, `usb.c`), and
  hub plus mass-storage enumeration takes a further 0.3-2 s.  MEASURED /
  GUESSED.  So the drive is never there at the instant of boot.
- Switch when the drive mounts: run the existing card-swap sequence
  (eject, re-point the storage root, insert) as `hd_card_service` does.
  The Beeb sees a disc swap, which it already copes with.  INFERRED.
- Do not make the Beeb wait for the drive: a "not ready" answer to a filing
  system with no timeouts can hang it (CLAUDE.md).
- Unplugged: the unmount callback sets a flag; `disk_status` reports not
  ready; a transfer in progress fails like a pulled SD card; the swap
  sequence falls back to the SD card.  INFERRED.
- The eject/remount hooks are global with one mount state today
  (filesystem.c ~276-370).  They must belong to the active storage volume,
  so an SD-card eject while USB is active only touches WebDAV and the
  config.  This split is the main refactor.  INFERRED.

### Mouse and drive together

- Through a hub: about four devices with their control endpoints, plus hub,
  mouse and MSC bulk in/out endpoints - within `CFG_TUH_DEVICE_MAX 8` and
  dwc2's 16 endpoints.  INFERRED.  The BCM283x has 8 host channels.
  GUESSED.
- Pi Zero / Zero 2 W: an OTG adapter and a hub.  Pi 1/2/3 Model B: the
  on-board hub.  MEASURED (`board_usb_behind_hub`).
- Power: mouse plus a bus-powered drive is fine from a good 5 V supply; a
  hungry drive, or an early Pi 1 B with polyfuses, needs a powered hub.
  GUESSED.

## Risks

| Risk | Handling |
|---|---|
| Main loop stall: the diskio wait blocks every poll, and a flash drive's housekeeping can take over a second | Multi-second time limit (the Beeb has no timeouts, so an error is worse than a wait), below the watchdog's, keeping the watchdog fed.  GUESSED stall length |
| Throughput | Host FIFO ~2 KB, emptied each poll; the wait loop does not use the 250 us idle gate, so per-command latency (~0.4-1 ms vs SD's ~0.3 ms) is the limit, not bandwidth; the read-ahead cache hides much of it.  GUESSED |
| BREAK | `filesystemReset` remounts within a short budget; the USB `disk_initialize` must return at once from `tuh_msc_mounted`, never wait for enumeration.  INFERRED |
| kernel.now | Sync the drive and leave USB idle before the jump (chainboot.c explains why in-flight USB hung the Pi); the new kernel re-enumerates.  INFERRED |
| Domesday video from USB | The riskiest load through the blocking wait; keep video on the SD card until measured |
| Release-build cost | Nothing on the hot path when `storage` is unset; MSC only works when a drive enumerates; roughly +5-8 KB code, under 1 KB RAM.  GUESSED |
| MTP | Device-mode only, and host is now the default, so no conflict; storage on USB means no MTP.  MEASURED / INFERRED |

## Stages

Each stage is testable on its own and needs a negative control where it
claims a fix or an equivalence.

1. **Enumerate the drive** (~80 lines).  `CFG_TUH_MSC 1`, `msc_host.c` in
   both source lists, the test stub `tusb_config.h` updated; a `/status` row
   with the drive's VID:PID, capacity and block size.  No FatFs.  Proves the
   mouse and the drive enumerate together through a hub on a real Pi.
2. **Read-only `"1:"` volume** (~150 lines).  `FF_VOLUMES 2`, the USB read
   path in diskio with the wait loop and time limit, mounted from a poll
   callback when the mount flag is set.  Test: list the drive on `/status`
   or through a webserver read.
3. **VFS discs from USB** (~120 lines).  The storage-root prefix in
   `fsLunFilePath`'s VFS branch, and the swap through eject/insert when the
   drive mounts or goes.  VFS is read-only, so no write path.  Control: the
   same disc image from SD and from USB, same Beeb commands, same results.
4. **Writable hard-disc images on USB** (~200 lines).  The write path, sync
   on eject and before kernel.now, the hooks split by volume.
5. **The rest**, one small change each (30-80 lines): the FAT service,
   FujiNet, the video player (after measuring stalls), a WebDAV `/usb` folder.

## Decisions for the owner

- The config key's name and values (`storage=usb` / `sd`?).
- Whether a drive arriving after the Beeb has already read the SD card
  should swap at once (a disc swap under a running program) or only at the
  next BREAK.
- exFAT: enable it in FatFs (size, licence notes) or require FAT32 drives?
