# Pi1MHz test suite

A BeebSCSI hard disc full of BBC BASIC test programs, plus a host driver
that types on the Beeb through the Pico keyboard (COM9), reads the results
back over the same serial link, and cross-checks the Pi through its web
interface.  It exercises the 1MHz-bus features from the Beeb's side, the
way a user does, and needs no HDMI capture.

## What it tests

| Test  | Where it runs | Checks |
|-------|---------------|--------|
| INFO  | BASIC `TINFO` | PAGE/HIMEM/Tube/MOS facts; BASIC on the host CPU |
| FRED  | BASIC `TFRED` | JIM page 0 greeting; FRED register dump (info); page-register readback (info) |
| RAM   | BASIC `TRAM`  | RAM expansion page mode across 12 pages up to 64 MB, full-page fill, byte mode vs page mode (byte mode is the last 16 MB of JIM), a 64 KB burst written and verified at 6502 speed, write-then-read-back coherence; the host checks the Pi's bus overrun counter stayed put |
| DISC  | BASIC `TDISC` | ADFS on the SCSI disc: 16 KB SAVE/LOAD (timed), sequential BPUT/BGET, EXT#, PTR#, *DELETE, restart after *BYE; the host then downloads the image and verifies the saved bytes reached the SD card |
| VDU   | BASIC `TVDU`  | writes to the framebuffer VDU port `&FCA0`; the host compares `/framebuffer.bmp` before and after |
| NET   | BASIC `TNET`  | the N: device fetches a LAN page over HTTP, the gateway by default (skipped when the net service is disabled) |
| M5000 | BASIC `TM5000`| a 1 kHz square wave on Music 5000 channel 0, recorded by the WAV recorder, then silence; the host checks RMS and the spectral peak of both files (`wavcheck.py`) |
| TTX   | BASIC `TTTX`  | Acorn Teletext Adapter `&FC10-&FC13`: field INT and DEW pulses, then 16 rows read and scanned for text (skips when no server data) |
| AUN   | BASIC `TAUN`  | the AUNFS command interface against the Pi's own loopback responder (from src/tests/aun/beeb/ECOTEST.bas) |
| BURST | BASIC `TBURST`| 32 writes at 3 µs straight after a page copy, after the services address registers, and after a helper page switch (the ROM loader's pattern), 20 repeats; nothing may be lost |
| FAT   | BASIC `TFAT`  | FAT service fopen / fread 16 KB / fclose five times, polled from host RAM; overruns must stay put |
| FATJ  | BASIC `TFATJ` | the same, dispatched and polled from code in JIM as the helper loader does; overruns must stay put (this is the case that fails today, see docs/dev/test-suite.md) |
| MOUSE | host          | pointer registers `&FCAC-&FCAF` on and off change the framebuffer snapshot |
| WAV   | host          | the Music 5000 WAV recorder (`*FX147,202/203`) creates `Musics*.wav`; the file is deleted afterwards |
| REDIR | host          | helper 2 (screen redirector): a PRINT on the Beeb changes the Pi's framebuffer |
| VIDEO | host          | on the first `/BeebVFSn` with `video.pvf`: `*FCODE F1000R` and `F1050R`, judged by the Video player status row |
| VFS   | host          | selects a `/BeebVFSn` set, `*VFS`, `*MOUNT 0`, `*CAT`, `*FCODE ?T` (seen on the Pi's F-code row), back to ADFS and the test disc |
| STRESS| host          | RAM burst and DISC test, then a CTRL-BREAK, while the host fetches 9 MB snapshots and uploads 2 MB files in a loop; overruns and reset latency must stay put |
| MMFS  | host          | helper 5 (MMFS2 through the FAT service): `*DIN 0 NET`, `*CAT`, a `*SAVE` and `*DELETE` verified in NET.ssd on the card |
| ROM   | host          | helper 6 loads BSRom into sideways RAM; after a CTRL-BREAK `*HELP` lists a new ROM |
| WIFI  | host          | helper 16 loads the 1MHz-WiFi ROM; `*VERSION` round-trips to the service, the extended vector table at `&0D9F` is unchanged by a command, and the machine still executes afterwards (checked over the bus, not by the echo) |
| BREAK | host          | five CTRL-BREAKs: the Pi's BREAK row counts every init, the ROM selected the helper, no bus overruns, the disc mounts again |

Every check prints one line the host parses:

    T:<test>:<check>:PASS | FAIL | SKIP:<why>
    I:<test>:<name>:<text>        informational (timings, register dumps)
    S:<test>:P=n:F=n:K=n          per-program summary, then ##DONE

## Files

- `bas/T*.txt` — one BASIC program per feature; `bas/common.txt` is
  appended to each (the PASS/FAIL printer and the CHAIN list);
  `bas/RUNALL.txt` chains every test for stand-alone use; `bas/BOOT.txt`
  is `!BOOT`.
- `build.py` — tokenises with `basictool` (round-trip checked) and builds
  `out/scsi0.dat`: an old-map ADFS disc, 2 MB (the firmware derives the geometry from the size), title
  `PI1MHZTEST`, `*OPT 4,3`.  Also `build.py root <dat>` to list an image
  and `build.py verify-wrote <dat>` for the DISC cross-check.
- `run.sh` — the host driver.  `out/results.txt` holds the parsed lines,
  `out/run-<time>.log` everything sent and received.

## Running

    tools/testsuite/run.sh --build            # tokenise, upload, run everything
    tools/testsuite/run.sh --only TRAM,TDISC  # a subset (names as in the table, T-prefixed for BASIC ones)
    tools/testsuite/run.sh --skip ROM,BREAK

Helper ROM loads (MMFS, ROM) are skipped when `*HELP` already lists the
ROM, since sideways RAM cannot be unloaded without a power cycle.  The
VIDEO test leaves the video player open; do not chain-boot a kernel.now
until it is closed.

Environment: `NSLOOK_HOST` (the name the WIFI test resolves, default
`www.google.com`; it skips when there is no DNS), `WIFI_SCAN=1` (also run
`*LAP`, which rescans on the radio carrying the session - `*JOIN` is never
run, it would drop the link), `PI_IP` (default 192.168.0.42), `VIDEO_SET` (VFS set for the VIDEO test; default the first with `video.pvf`), `JUKE` (jukebox set the disc
is uploaded to and selected from, default 9), `JUKE_RESTORE` (set selected
at the end, default 0), `NET_URL` (page TNET fetches; default the Pi's gateway, since lwIP cannot connect to the Pi's own address), `VFS_SET` (default: lowest `/BeebVFSn` on the
card), `PI1MHZ_TOOLS` (directory holding `beeb-run.sh`, `pi-http.sh`,
`pi-status.sh`; default `/mnt/c/Archlinux/claude-tmp`).

The driver assumes nothing about the Master.  It CTRL-BREAKs (only after
the Pi has answered `/status`), arms the Pico's serial echo with F11,
corrects CAPS, and if BASIC is on the co-processor it does `*CONFIGURE
NOTUBE` plus another CTRL-BREAK, because the tests poke FRED and JIM
directly.  At the end it restores jukebox set `JUKE_RESTORE` and, if it
changed it, `*CONFIGURE TUBE`; `--no-restore` leaves the bench as the
tests left it and `--keep-notube` keeps the co-processor off.  The last
line of the run lists what was changed.

Stand-alone, without the host: select the set from the Beeb (`*BYE`,
`*FX147,65,9`, `*MOUNT 0`, `*!BOOT`) and `RUNALL` chains every program,
printing the same lines on the Beeb's screen.

## Adding a test

Write `bas/TNAME.txt` starting with `PROCbegin("NAME")` and ending with
`PROCend`, using `PROCchk("check", condition)`, `PROCskip`, `PROCinfo`;
avoid the resident integers `U%..Z%` (the chain state) and JIM
`&3000-&5FFF` (Music 5000 registers).  Add the name to `TESTS` in
`build.py`, to the `DATA` line in `bas/common.txt`, and to `BASIC_TESTS`
in `run.sh` (with a timeout).  Host-side cross-checks go in `run_basic`.
