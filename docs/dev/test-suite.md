# Beeb-side test suite

STATUS 2026-09-13: built and run on the bench (Zero W, Master 128 with the
internal co-processor, Pico keyboard v0.2 on COM9).  First full sweep 27
pass / 5 fail; every failure was the suite's own assumption, fixed the same
morning (see "What the first runs taught").  Lives in `tools/testsuite/`;
the how-to is its README.

## Why

Pi1MHz has host-side unit harnesses (`tools/vdutest`, `src/tests`) but no
way to ask the real Beeb "does every emulated peripheral still work" after
a firmware change.  Until now that was a human at the keyboard.  With the
Pico keyboard reworked (40 ms hold, 40 ms gap, queued and flow-controlled
keys) typing on the Beeb is reliable enough to script, so the suite drives
the Master exactly as a user would and reads what it prints back over the
Pico's serial echo.

## Shape

- **A BeebSCSI jukebox set** (`/BeebSCSI9/scsi0.dat`, no descriptor: the
  firmware derives the geometry from the file size):
  a 2 MB old-map ADFS disc built by `tools/make_vfs_menu.py` (given a
  `title` argument for this), holding one tokenised BASIC program per
  feature.  Full-size and writable so the DISC test can SAVE on it.
- **BASIC programs** print machine-readable lines
  (`T:<test>:<check>:PASS|FAIL|SKIP`, `I:` for information, `S:` summary,
  then `##DONE`).  A common footer gives every program the same printer and
  a CHAIN list; the resident integers `U%..Z%` carry the chain state so
  `RUNALL` (or `*!BOOT`) runs everything without a host.
- **The host driver** (`run.sh`) establishes the Master's state itself,
  assuming nothing: CTRL-BREAK (only after the Pi answered `/status`), F11
  to arm the echo, a probe character to detect CAPS, `PRINT ~PAGE` to detect
  the Tube and `*CONFIGURE NOTUBE` + CTRL-BREAK if BASIC is on the
  co-processor (the tests poke FRED/JIM directly, which the Tube cannot).
  It then `*BYE`s, selects the set, mounts, CHAINs each program and parses
  the echo, and adds host-side checks the Beeb cannot make.
- **Host-side cross-checks through the web interface**, not the HDMI
  capture: `/framebuffer.bmp` hashes before/after (VDU port, pointer),
  the `Bus diag` overrun counter around the bus-heavy tests, the BREAK row
  around five CTRL-BREAKs, the `F-code` row after `*FCODE`, the SD root
  listing for the WAV recorder, and a download of `scsi0.dat` to verify the
  bytes ADFS saved really reached the card (`build.py verify-wrote`).

## What the first runs taught

- **Byte-mode RAM is the last 16 MB of JIM.** `rampage_emulator_init` sets
  `byte_ram_addr = (JIM_ram_size-1) << 24`, so Sprow-style address A is
  page-mode page `(size-1)*&10000 + A DIV 256`.  Not in docs/user; the test
  reads the size from the status register (`?&FCCA=0` then `?&FCCB`).
- **The N: device cannot fetch from the Pi's own IP** (lwIP has no
  loopback to the WiFi address; `url_open` stays PENDING).  The test
  fetches the gateway's page by default (`NET_URL` overrides).
- **CTRL-BREAK leaves the Master in its CMOS filing system** (VFS on the
  bench), so `*MOUNT 0` after a BREAK showed the VFS disc, not the test
  disc.  The SCSI jukebox set itself survives a BREAK under both ADFS and
  VFS (verified 2026-09-13); the driver re-selects it anyway after `*ADFS`.
- **The VFS ROM's `*FCODE` prints no reply**; the Pi's `F-code` status row
  shows what was sent, so that is the check.
- **`X%=n:CALL &FC88` only selects a helper** (the code at `&FC88` is
  `STX &FC88 : NOP : RTS`); `*GO FD00` runs it, and the help screen ends
  in a BRK, so it can only be run from the prompt, not inside a program.
- **The Pico must not be paced per character any more**: whole lines are
  sent as one burst and the driver waits for the echo of the line's tail
  (`beeb-run.ps1`), with a 120 s write timeout because the Pico
  flow-controls USB while its 64-key queue drains.

## Measurements (Zero W, V1.31, 2026-09-13)

| What | Figure |
|------|--------|
| 64 KB JIM write / verify at 6502 speed (15 cycles a byte) | 0.68 s each way, 0 errors |
| write-then-read-back 16 K (STA then CMP the same JIM byte) | 0 errors |
| ADFS SAVE / LOAD 16 KB | 240 ms / 180 ms |
| bus overruns across RAM, DISC and 5 CTRL-BREAKs | 0 |
| BREAK row after 5 CTRL-BREAKs | inits +5, helper selected ~374 ms after the edge |

## Bus overruns: mechanism found (2026-09-13 afternoon)

`TFAT` and `TFATJ` do the same FAT-service work (fopen, fread 16 KB,
fclose of Pi1MHz/6502code.bin, five times); `TFAT` polls the command
register from BASIC in host RAM, `TFATJ` runs its dispatch-and-poll loop
from JIM at `&FD00`, as the helper ROM loader does.  `TFAT`: 0 overruns.
`TFATJ`: +32 overruns and one in three fread results read back wrong.
So:

1. `services_emulator_command` (the FRED `&FCAA` write callback, FIQ
   context) calls `fat_service_command` directly, which runs `f_open`,
   `f_lseek`, `f_read` and `disk_read` on the SD card inside the FIQ, for
   milliseconds.  (The CLAUDE.md rule "never do SD/FatFs work in FIQ" is
   not honoured by this service; the net service latches and polls.)
2. While the FIQ is stalled, 6502 code executing from JIM keeps the bus
   busy at 1 MHz (the VPU posts every cycle, instruction fetches
   included), so the 8-slot ring overruns.  A Beeb polling from host RAM
   posts nothing and never overruns, which is why MMFS and the loader
   *usually* survive: the protocol tolerates a late Pi.
3. What loses data is the ring's resync policy: after an overrun the
   consumer keeps its expected lap and drops every entry until the tag
   comes round (up to three laps, about 24 posts, ~25 µs of a busy bus).
   The loader's very next writes after the result appears (`STY
   discaccess`, the fread setup) land in that window, so a lost byte
   garbles the next command and the loader spins in `fopencheckloop`.
   That is the MMFS `*GO FD00` hang seen after 12 dots with +12
   overruns, and it explains why helper loads are the one place today's
   overruns came from (+60, +6, +12; `TBURST` shows the page copies and
   data-port callbacks themselves are fast: 32 writes at 3 µs lose
   nothing).

Fixes, for the owner to choose: (a) in FIQ.s, on overrun resync at once
to the oldest surviving entry (scan the eight tags), so only what arrived
during the stall is lost, as before the ring; (b) move the FAT service's
FatFs work to the poll loop (latch the command in FIQ, answer from
`fat_service_poll`), which removes the stall and the IRQ starvation that
goes with it (the 576 µs nRST latency, audio underruns and WiFi TX
failures seen during helper loads).  Both are cheap; (b) is the root cause.

## Echo silences

Two runs went silent (a `*MOUNT 0` after an upload, and inside `TDISC`)
and were recovered by CTRL-BREAK; the first followed a helper ROM load
whose lost writes explain the hang.  The driver now records `ovr` after
every test, reports a BASIC error message as a program error, and on a
true silence (nothing received) types a blind `*FX147,202/203` recorder
start/stop and looks for the WAV, which says whether the Beeb is still
executing (echo lost) or hung.

## Not covered yet

BeebSID (off by default), teletext adapter semantics (registers dumped
only), M5000 audio content (only that the recorder writes a file), AUN
(needs a peer station; `src/tests/aun/beeb/ECOTEST.bas` covers the API),
UEF tape, raw SCSI commands outside ADFS, MMFS through the FAT service
(helper 4/5 load then `*DIN`), and the video plane itself.  Each is a new
`bas/T*.txt` or a host function in `run.sh`; the README says how.
