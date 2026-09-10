# Diagnostic pages, rows and switches

What the firmware can tell you about itself over the web interface, for
development and bug-hunting.  None of it is needed to use Pi1MHz; the user
documentation lists only the everyday pages.  Everything here is read on
demand: the release build pays nothing for a row until the page is fetched,
and the switches that do cost something are off by default.

## Pages

| Address | What it returns |
|---|---|
| `/status` | The status page: every row below plus the WiFi, SD and emulator rows users see |
| `/fcodes` | The most recent F-code exchanges between the Beeb and the emulated LaserDisc player, oldest first (`src/BeebSCSI/fcode.c`) |
| `/vdulog` | Every VDU command the drain consumed, oldest first, with a millisecond stamp and a `!` on commands swallowed while VDU 21 had the drivers off; the header counts commands the FIQ refused for want of queue space.  Needs `vdu_log=1` |
| `/edid` | The monitor's EDID as read at boot, 16 bytes a line, with the display-mode verdict first (`src/rpi/display_mode.c`) |
| `/aun` | The Econet-over-WiFi engine's state and counters |
| `/bench.bin` | A large dummy download for network throughput tests |
| `/udpblast?host=a.b.c.d&port=n&mb=m` | Primes a UDP send rig that takes lwIP TCP out of a throughput measurement (`src/wifi/wifi_lwip.c`) |
| `/audio.wav` | A test tone through the audio path |

## /status rows for diagnosis

| Row | Reads |
|---|---|
| Boot time | Pre-kernel time (bootcode + start.elf) and kernel-to-poll-loop time |
| Boot init ms | Per-emulator init cost, in ms, for the last `init_emulator()` (a BREAK re-runs it, so after a BREAK these are the re-init's figures) |
| Display mode | What the EDID said and which mode was set, with the EDID read and mode-set times |
| BREAK | The last Beeb reset: `edges` (nRST falling edges the IRQ saw), `inits` (re-inits the poll loop ran), `missed` (edges too short for a poll pass - a bouncing key), `rst->init` (edge to re-init start), `init` (re-init length), `held` (nRST low as the poll loop saw it), `helper` (edge to the ROM's first helper bank select, i.e. the screen redirector being re-enabled), `vdu` (edge to the first VDU byte the Beeb wrote), `ovr +n first t` (post-ring overruns since the edge and when the first was), `worst init` and the smallest `margin` between re-init end and helper select ever seen.  `-` = not yet, or a stamp from before the last edge.  See `docs/dev/bus-post-ring.md` for the measurements this produced |
| Ring (DEBUG builds only) | The eight post-ring slot tags and the consumer's tag C: a producer that restarted with the consumer shows the lap-3 fill with a run of lap-0 entries; anything else is out of step |
| Bus diag | Hard disc engine state and adapter flags; `ovr` = post-ring FIQs that found the VPU a lap ahead (should stay 0); DEBUG builds add SCSI event counters and the last CDB |
| Planes | Each display plane's geometry, with `HIDDEN(off/wanted/gated)` when it is not being scanned out |
| Video player | Transport state, then the display side: plane on/off, video on/OFF, displayed and pending buffers, duplicates owed, stop picture, last seek op |
| H264 decoder | Frames decoded, the output-port handshake (enable, pending reconfigure, registered/armed buffers) and free input slots / EOS owed |
| Reset reason, Boot stage | Where the previous boot died and what reset the SoC last (`docs/dev/review-2026-09-07.md`, lockup forensics) |

## Switches in Pi1MHz.cfg

| Key | Cost | Effect |
|---|---|---|
| `vdu_log=1` | 64 KB RAM, one test per drained VDU command | Keeps the last 4096 VDU commands for `/vdulog` |
| `wifi_diag=1` | small | Extra WiFi link diagnostics on `/status` |
| `wifi_debug=1`, `aun_debug=1`, `teletext_debug=1` | serial output | Debug builds only: verbose logging on the UART |

## Reading them from a session

`claude-tmp/pi-status.sh 'BREAK|Bus diag'` prints matching rows as text
(never dump the HTML); `pi-http.sh -s /vdulog` and `/fcodes` are plain
text already.  `PI-CONTROL.md` has the bench procedure.
