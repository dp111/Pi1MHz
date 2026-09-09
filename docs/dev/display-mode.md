# Display mode and geometry: native resolution at 50 Hz, one grid for two planes

STATUS (2026-09-08): **built, on master, hardware-verified on four sinks**
(Zero W: 16:10 monitor at 1920x1200, a USB HDMI capture stick, a 2006 HD-ready
set that prefers 1080i50, a 2006 Samsung 1080p set).  Not yet seen: a real
576p/480p screen, a 4:3 monitor (horizontal source-window path), a monitor
whose EDID refuses 50 Hz, and kernel7 on a Zero 2 W.  Open ideas at the end.

## Why

The Domesday video is 25 frames a second.  At 50 Hz every frame is shown
exactly twice; at 60 Hz the player alternates two and three refreshes per
frame and pans judder.  The Pi firmware picks the monitor's *preferred* EDID
mode, which is the native resolution at 60 Hz; its own mode tables only carry
50 Hz at the television sizes (1080p, 720p, 576p); and `config.txt` has no
"prefer 50 Hz".  A user with an arbitrary monitor therefore had to work out
`hdmi_cvt` by hand.  The goal was: plug in any screen, get its native
resolution at 50 Hz, nothing to configure.

## What the firmware offers (measured, start.elf of 2026-02-11)

The firmware exports the mode-set path its fake-KMS Linux driver uses
(`drivers/gpu/drm/vc4/vc4_firmware_kms.c`).  These work from bare metal, on
a display that was set up by `config.txt` (i.e. not in FKMS mode):

| tag | value | notes |
|---|---|---|
| `GET_EDID_BLOCK` | `0x00030020` | request 34 words: `[block, 0 x33]`; reply `[block, status, 128 bytes at byte offset 8]`.  Fetched over DDC on request: one read came back empty on the bench, so it is retried (4 tries).  Through an HDMI splitter the EDID is whichever sink the splitter forwards - the bench read the capture stick's EDID while the monitor showed the picture |
| `SET_TIMING` | `0x00048017` | request = Linux `struct set_timings`, 36 bytes (below); the reply is empty either way (and the property layer masks the per-tag response bit), so success is judged by reading the pixel valve back |
| `GET_DISPLAY_TIMING` | `0x00040017` | **returns a 36-byte reply of zeros** (display 2 and 0 tried).  Not used. |
| `GET_CLOCK_RATE(PIXEL=9)` | `0x00030002` | returned 0 at this point in boot.  Not used. |

`struct set_timings` (little-endian): `u8 display` (2 = HDMI0), `u8 pad`,
`u16 video_id_code` (CEA VIC, 0 for a custom mode), `u32 clock` (kHz),
`u16 hdisplay, hsync_start, hsync_end, htotal, hskew, vdisplay, vsync_start,
vsync_end, vtotal, vscan, vrefresh, pad`, `u32 flags` (bit0 hsync+, bit1
vsync+, bit2 interlace, bits4-7 aspect: 1 = 4:3, 2 = 16:9; bit8 RGB
limited; bit9 DVI; bit10 double clock).

The timing **in force** is read from the hardware instead: pixel valve 2
(`PERIPHERAL_BASE + 0x807000`, HDMI on the Pi 0-3) words 3..6 are
`HORZA = hbp<<16 | hsync`, `HORZB = hfp<<16 | hactive`, `VERTA = vbp<<16 |
vsync`, `VERTB = vfp<<16 | vactive`; word 1 (`V_CONTROL`) bit 4 is the
interlace flag, in which case those are *field* figures; the pixel clock is
PLLH/10 from the A2W registers (`hdmi_pixel_clock_hz()` in
`hdmi_audio.c`).  Decoded 720p50 as 1980x750 exactly.

## What `display_mode_select()` does (src/rpi/display_mode.c)

Called once from `init_emulator()` after `Pi1MHz.cfg` is loaded and before
any emulator sizes a plane (interrupts still off; a BREAK re-init skips it;
the caller kicks the watchdog around it and stamps `BOOT_STAGE_DISPLAY`).

1. `Display_refresh` (default 50; `off`/0 leaves `config.txt` in charge).
2. Read the timing in force.  If it is progressive and within 0.5 Hz of the
   target, stop: "already 50 Hz", no resync, and no EDID read - the two
   blocks over DDC cost ~14 ms of boot, so they are read only when a
   decision needs them (`/edid` fetches them on demand).
3. Read the EDID and take its preferred detailed timing: the panel's own geometry with
   the pixel clock rescaled to `htotal * vtotal * hz`.  Interlaced, zero
   fields, or a sync that falls outside the blanking = unusable.
4. If the target is 50 Hz and the CEA block lists a 50 Hz progressive mode,
   prefer it: at the preferred size when the preferred timing is usable
   (a television gets its real mode with its VIC, not a rescaled 60 Hz one it
   may refuse), else the best listed (1080p50, then 720p50, then 576p50).
   CEA modes go out RGB-limited, as CEA modes are.
5. A rescaled (non-CEA) timing is refused if the range-limits descriptor's
   minimum vertical rate is above the target.  No descriptor = allowed.
6. `SET_TIMING`; read the valve again; the `/status` **Display mode** row
   and `/edid` say what happened.  The DVI flag is never set (same intent as
   the documented `hdmi_drive=2`).

Everything downstream reads the display size from the HVS (`ctrl1`), so the
planes follow the new mode with no other change.

## Bench results

| sink | firmware came up | result |
|---|---|---|
| capture stick (prefers 720p60) | 1280x720 @ 60 | CEA 720p50, measured 49.98-50.03 Hz; `Display_refresh=55` gave the rescaled path: 1280x720 @ 55.03, 68.06 MHz, clean picture |
| 2006 "HDMI TV", 71x40 cm, max pclk 110 MHz | 1080i50 (its preferred) | interlace never matches; no 1080p50 listed; CEA 720p50.  Its vertical stretch was the set's aspect mode |
| 2006 Samsung, lists VIC 31 | 1080p60 | CEA 1080p50.  Top/bottom Beeb rows off-glass = the set's overscan (EDID: underscan unsupported); Picture Size -> Screen Fit |
| chain-boot into a display already switched | inherited 50 Hz | "already 50 Hz", nothing sent |

A cold boot of the module was seen (`Boot time` row present, mode row
printed).  Cost, measured with the system timer (the row prints it):

| | kernel to first poll |
|---|---|
| display already at the target (no EDID read, nothing sent) | 60 ms - as before the module |
| a mode is set, full boot | 230 ms = 60 baseline + 24 EDID (two DDC blocks) + 145-170 SET_TIMING |
| the same with `hdmi_muting=0x10001` in `config.txt` (now shipped) | 108-118 ms: SET_TIMING falls to 22-29 ms.  Two cold boots, picture and HDMI audio unchanged |
| a mode is set from a **chain-boot** (second runtime mode set on the same VideoCore session) | 1.5-1.8 s in SET_TIMING alone.  Developer artefact only; a full boot does not show it |

The pixel clock changing or not made no difference (1920x1200 at 154 MHz
to 720p at 74 MHz was 145 ms).  Most of the 145 ms was the firmware's
HDMI mute/unmute waits around the switch: `hdmi_muting=0x10001` (a
`config.txt` key of the firmware, suggested by dp111) skips them, measured
2026-09-09 on the bench card with the Aug 2026 start.elf.  The monitor's own resync (~1 s) is what
the user sees.  The wait cannot be overlapped with the rest of boot: the
mailbox is one in-order slot already carrying the deferred USB power-on,
and the framebuffer allocation needs it too.  The zero-cost path is a
`config.txt` mode that is already right, when the switch never runs.

## The geometry stack this sits on (src/rpi/screen.c)

Both planes are scaled from the Beeb's PAL grid, 832x576 (52 us of line at
the Beeb's 16 MHz pixel clock, 576 frame lines):

* vertical factor: `grid_vscale(v_display)` - one table for both planes
  (the Beeb's 256 rows fill the height at a whole/half-integer factor; the
  video's 576 lines overscan and are cropped top and bottom);
* horizontal factor: vertical x `GRID_SAMPLE_PAR` (12/13, the shape of a
  16 MHz sample on a 4:3 line) x `panel_par(h, v)` (15/16 at 720x576, 9/8 at
  720x480 - standard-definition pixels are not square) x `Display_par` (user
  trim, default 1/1; 3/4 for a widescreen set fed 576p; 10/9 for a 16:10
  panel fed 1080p).  So the video is a true 4:3 frame and the Beeb picture
  sits registered on it at its true 1.15:1, on any square-pixel display.
* On 4:3 and 5:4 displays the video would overrun the width; the source is
  cropped horizontally and centred (the vertical path already did that).

The alignment update runs with interrupts off (the player's flip is in the
vsync IRQ and shares the offsets and pointers), as `screen_plane_alpha()`
does.

**Alignment** (`LDVideoXoffset`/`LDVideoYoffset` per disc side in
`scsi0.cfg`, defaults -5,-2 in Beeb pixels/rows): on an axis where the plane
fills the display (the height everywhere, the width on 4:3/5:4) the plane
position clamps at 0 and cannot move, so the *source window* slides inside
the overscan crop (even steps for the half-size chroma planes, clamped to the
crop's slack, nil at 576p); on an axis with room the plane moves.  The
current frame is re-pointed at once so a paused picture moves.  Only the
video plane moves; the Beeb plane never does.

## Open

* **Interlaced output is not supported by the plane layout** (a 1080i-only
  set gets 720p50 instead).  Teaching the display list about fields would be
  the only way to give such a set 1080 lines.
* **Overscanning sets** that cannot be told "Screen Fit": a
  `Display_overscan` percentage that shrinks the whole output would land the
  picture inside the visible area.  The firmware's `overscan_*` do not help -
  Pi1MHz builds its own display list sized to the full mode.
* A rescaled DTD is sent with the panel's own blanking; a panel that only
  advertises 60 Hz blanking has been fine so far, but CVT-RB blanking at the
  target rate is the textbook alternative if one is not.
* Untested paths listed in the STATUS line.
