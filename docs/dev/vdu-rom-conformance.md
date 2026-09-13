# Testing the VDU/PLOT code against a real BBC Master

> **STATUS 2026-09-13: the suite now runs against a real Master 128 (MOS
> 3.20), covers text, windows, scrolling, colours, patterns and VDU 5 as well
> as PLOT, and runs on any PC from recorded results.** `tools/vdutest/` builds
> Pi1MHz's own `src/framebuffer/*` for the PC and diffs it, pixel for pixel
> and colour for colour, against the ROM under beebjit. **193 of 200 cases
> match.** Triangles, parallelograms, ellipses, and arcs/chords/sectors in
> every mode were brought to a match on 2026-09-13; what remains is listed
> under "What the Master does that we do not". The earlier OS 1.20 + GXR oracle is still there as
> `--machine b`. The suite is not part of the build; run it by hand after
> touching `src/framebuffer/`.

## Why

Nearly all of `src/framebuffer/` exists to reproduce something the BBC's OS
already does, so "is this right?" has an authoritative answer that does not
depend on reading the code or on anyone's memory of the PRM: run the same
VDU bytes through the real ROM and compare the pixels.

Two of the three defects found in September 2026 were invisible to review and
obvious to this method within minutes - the line drawn by PLOT 8 was not the
line drawn by PLOT 0 minus a pixel, and our lines were direction-dependent
where a real Beeb's are not. The 2026-09-13 pass found three more the same
way (circles in MODE 0/2/5, VDU 31 in VDU 5 mode, the missing VDU 23,16),
and Toby Nelson's reassembly of the GXR (github.com/tobylobster/GXR-pages,
`docs/gxr120_acme.a`) then supplied the ellipse algorithm outright, which no
amount of fitting had found.

## The oracle: a real Master, headless

`beebjit -master` runs a Master 128 with the real MOS 3.20 and no display,
driven by BASIC over the emulated serial line. beebjit's `-terminal` only
knows how to enable serial I/O in OS 1.20 (it patches three bytes of the OS
variable defaults), so `vdutest.py` builds a tree in `$TMPDIR/vdutest-master`
whose MOS 3.20 carries the equivalent patch, and points beebjit at it (ROMs
load by relative path):

| byte | was | now | what |
|------|-----|-----|------|
| `&E318` (OSBYTE 177 default) | 0 | 1 | input from RS423 |
| `&E353` (OSBYTE 236 default) | 0 | 5 | output to screen and RS423 |
| `&E91E` `LDX &0250` in the OSBYTE 156 handler | `AE 50 02` | `09 80 EA` (`ORA #&80 : NOP`) | ACIA receive interrupt forced on |

The third patch is needed because the Master rebuilds the ACIA control
register from CMOS at reset, so the table default alone never survives - the
Master boots and prints its banner over the serial line but never reads a
byte. The MD5 of the unpatched `mos.rom` is checked before patching. The
Master has the GXR built in, so every PLOT code except sprites (232-239)
works without a ROM in a sideways slot.

* Program lines must end `\r`, not `\n`; the RUN echo ends in a bare CR.
* beebjit's own `info:misc:` log lines land in the middle of the Beeb's
  output; the harness lifts them out before parsing.
* beebjit never exits on its own - it sits at the prompt burning its cycle
  budget - so the harness kills it when the program prints `ENDOFTEST`.

ROM checksums: `mos3.20/mos.rom` md5 `5f0d53f852bdcdf5f695c6f9c310fbbf`,
`os12.rom` md5 `0a59a5ba15fe8557b5f7fee32bbd393a`, `gxr.rom` md5
`82055d129d9aa834622319f7ecc45155`.

## The other side: our VDU driver, on the PC

`src/framebuffer/{framebuffer,screen_modes,primitives,fonts,teletext}.c` are
portable C and compile for the host untouched. `tools/vdutest/stubs.c`
supplies the bare-metal symbols they reference (screen buffer, timer,
interrupt controller, config, mouse pointer, a few no-ops) and
`tools/vdutest/vduhost.c` feeds VDU bytes to the real `fb_writec` and dumps
pixels with `prim_get_pixel`. In every mode the suite uses, the pixel value
*is* the logical colour, which is what `POINT` returns on the Beeb.

This runs **the same code the firmware runs** - no transcription, no model of
the algorithm that can drift from it.

One wrinkle: `screen_allocate_buffer` returns a `uint32_t`, which a 64-bit
`malloc` pointer does not fit into, so the stub maps the buffer with
`MAP_32BIT`.

## Running it

```sh
tools/vdutest/vdutest.py                 # every case
tools/vdutest/vdutest.py circle text     # only cases whose name matches
tools/vdutest/vdutest.py -v circ-149-r4  # list every differing pixel
tools/vdutest/vdutest.py --golden        # against the recorded results only
tools/vdutest/vdutest.py --record        # re-record golden/master.txt
tools/vdutest/vdutest.py --machine b     # OS 1.20 + GXR instead of the Master
```

With beebjit installed the ROM is the reference; without it the recorded
results in `tools/vdutest/golden/master.txt` are, so the suite runs on any PC
with gcc and python3. Re-record after adding cases. Set `BEEBJIT=` to point
at another beebjit tree.

Each case is a screen mode, a list of VDU byte sequences and a box of pixels
to compare. Both sides are driven from that one table, so there is no way for
the two to test different things. Adding a case is one line:

```python
case("circ-157-fill-r5", plot(MOVE,14,12), plot(157,19,12)),                  # MODE 4
case("m5-circ-149", plot(MOVE,14,12,U5), plot(149,24,12,U5), mode=5),          # MODE 5
case("text-window-28", vdu(28,2,5,8,2) + text("ABCDEFGHIJKLMNOP"), box=(0,192,80,255)),
```

`plot()` takes pixels of the case's mode, `plotu()` OS units, `text()` and
`vdu()` bytes. The box is in pixels, y upwards.

### How the Beeb side keeps its hands off the screen

Results go back over the serial line with the VDU driver switched off
(`*FX3,3` while printing, `*FX3,0` while a case's bytes are being drawn), so
printing never scrolls the screen under the scan and any part of it can be
compared - which is what makes text and window cases possible. `POINT` is
origin-relative, so the harness appends `VDU 29,0;0;` to every case. The
Master runs in a shadow mode (`MODE 128` first, then `VDU 22,128+m` per case)
so BASIC keeps the full 29K under `&8000` whatever mode a case selects.

### Cases must be independent of each other

Both sides start a fresh machine every `BATCH` (12) cases. Every case starts
from a fresh `MODE` with the cursor off (`VDU 23,1,0` - the Pi draws its
cursor into the framebuffer, a Beeb's is a CRTC overlay `POINT` never sees),
`VDU 23,16` flags cleared and default ECF and dot patterns, because **the
Master keeps `VDU 23,16` flags and the patterns across a MODE change**. That
bit a first draft of this suite: three `VDU 23,16` cases silently changed how
`VDU 30` and `VDU 31` behaved in the VDU 5 cases that followed them, and the
"finding" that the Master positions VDU 5 text from the bottom of the window
was an artefact. A case that changes anything else persistent - a character
definition - must put it back.

## Four traps

All four produce plausible, wrong data rather than an error.

1. **`POINT` is relative to the graphics origin.** A case that moves the
   origin scans the wrong place unless it is put back (the harness does this).
2. **Printing on the Beeb scrolls the screen** unless the VDU driver is off
   (`*FX3` bit 1) or a text window keeps it clear of the scan.
3. **`MODE 0` leaves ~5.5K for BASIC** on a model B; on the Master use the
   shadow modes as above.
4. **beebjit's stdout contains NUL bytes** from the VDU stream, so plain
   `grep` treats a capture as binary and silently reports nothing. Use
   `grep -a`.

## What has been checked

Findings confirmed on the ROM:

* **Lines are direction-independent on a real Beeb** - A to B and B to A are
  the identical pixel set. The OS achieves that by swapping the endpoints;
  `prim_draw_line` instead picks its initial error term from the major-axis
  direction, which gives the same pixels while still traversing from the
  caller's start point. That traversal order matters: the dot pattern is
  consumed from the `MOVE` end, and omit-first/omit-last name the caller's
  endpoints, not the loop's.
* **Omitting an endpoint removes that pixel and disturbs nothing else.**
  PLOT 40-47 and 56-63 omit both endpoints.
* **Dotted lines**: a new dotted line restarts the pattern; an omit-first
  dotted line continues it seamlessly; an omitted point consumes no slot.
* **VDU 5 places the character cell's top-left at the graphics cursor**; in
  VDU 5 mode `VDU 8-11` move a cell, `VDU 10` past the bottom of the graphics
  window wraps to the top, `VDU 8` past the left edge goes to the end of the
  row above, `VDU 13` returns to the window's left edge, `VDU 127` blanks the
  cell behind the cursor in the background colour, and `VDU 31,x,y` counts
  column x from the window's left edge and **row y down from its top**.
* **Circles in every mode** are the disc `x*x + y*y <= (R + h)^2` in OS
  units, R the exact unrounded distance to the radius point and h half the
  *smaller* pixel dimension (half a pixel in MODE 4, 1 unit in MODE 0, 2 units
  in MODE 5); the outline is that disc's row and column extremes. Fitted on
  MODE 0, 4 and 5 outlines and fills; `calc_radius_limit` now does this, and
  arcs, chords and sectors take their rows from the same disc.
* **Filled triangles and parallelograms** are the span fill of their own
  edges drawn as PLOT lines. `prim_draw_line`'s Bresenham is now a shared
  `line_stepper_t`, and `span_fill` walks the edges into 256-row bands of
  min/max scratch (1 KB in `.noinit`), so the collinear triangle, the edge
  pixels, the parallelogram and the fills that stop at a triangle edge all
  come right by construction.
* **Ellipses are the GXR's own algorithm**, transliterated (`gxr_ellipse`):
  row n has half-width (w/h)*sqrt(h*h - n*n) and shear n*s/h in 8.8 fixed
  point with an integer square root, rounded half up, the rounding
  accumulating exactly as the ROM's does; the outline joins each row to its
  neighbours with the ROM's rule (the right run stops at max(min(D, B), C),
  which the reassembly's comment gets wrong). 32 ellipses match, sheared,
  degenerate and oblong-pixel ones included.
* **Sectors and segments include the pixels of their radial and chord
  lines**, drawn as Bresenham lines, even where those sit half a pixel
  outside the exact edge. Adding them removed most of the slanted-edge
  differences; the last few pixels are the ROM's row-by-row line tracking
  (GXR chapter 16), still to do.
* The whole text side matches: every glyph 32-126 and the Master's 128-255,
  cursor codes 8-13, 30, 31 and 127 including wrapping at every edge and the
  scroll-down `VDU 11` causes at the top, text windows and what `CLS`, `VDU
  26` and `VDU 30/31` do inside them, scrolling with graphics on screen,
  `COLOUR` in MODEs 1, 2 and 5, `VDU 21/6`, `VDU 1`, user-defined characters
  above and below 128.
* GCOL 0-4 (set, OR, AND, EOR, invert) and the background variants, `CLG`
  to a background colour, all four default ECF patterns and user/simple
  patterns with `VDU 23,2-5`, `23,11`, `23,12-15`, the dot pattern `VDU 23,6`,
  the graphics window (clipping of every primitive, `CLG`, `VDU 26`, bad or
  off-screen windows ignored), the origin `VDU 29` alone and with `VDU 24`,
  `VDU 23,7` scrolling and `VDU 23,8` clear-block.

## Where each family stands

From `tools/vdutest/vdutest.py --golden` as of 2026-09-13 (193/200).

| family | cases | status |
|--------|-------|--------|
| lines 0-63, points 64-71 | 9 | **exact** (plus 864 line instances in 2c6ef25) |
| triangles 80-87, parallelograms 112-119 | 7 | **exact** (span fill, 2026-09-13) |
| horizontal fills 72-79 .. 120-127 | 5 | **exact** |
| rectangles 96-103, move/copy 184-191 | 4 | **exact** |
| flood fills 128-143 | 2 | **exact** |
| circles 144-159, MODE 0/4/5, off-axis radius | 14 | **exact** (MODE 0/5 fixed 2026-09-13) |
| arcs, chords, sectors 160-183 | 25 | exact in every mode with axis-aligned or radial edges; **4 slanted cases differ by 1-11 pixels** |
| ellipses 192-207 | 32 | **exact** (GXR algorithm, 2026-09-13) |
| sprites 232-239 | 0 | not testable on a Master; needs `--machine b` |
| GCOL modes and colours | 8 | **exact** |
| ECF and dot patterns | 11 | **exact** |
| graphics window, origin | 10 | **exact** |
| text glyphs, cursor, windows, scroll | 40 | **exact** |
| VDU 5 text | 20 | **exact** (`VDU 31` fixed 2026-09-13) |
| VDU 23,7 scroll, 23,8 clear block | 7 | **exact** |
| VDU 23,16 cursor control | 3 | **not implemented** - two of three differ |
| VDU 27 | 1 | differs by design, see below |

## What the Master does that we do not

The assessment. In order of how likely a Beeb program is to notice.

1. **`VDU 23,16`** (cursor movement control: scroll protect, no-scroll wrap,
   vertical wrap, and so on) is ignored. Text editors and games use bit 0 to
   print in the bottom-right cell without scrolling. Cases
   `cursor-23-16-*` measure it; `cursor-23-16-noscroll` and `-vdu10-wrap`
   differ today.
2. **Sectors and segments with slanted edges** differ by a few pixels along
   the edge (`sector-181-slant-b`, `chord-173-slant-b`, `m0-sector-181-slant`,
   one pixel on `sector-181-offaxis`). The ROM tracks the start, end and
   chord lines row by row with its line stepper and fills from the line's
   pixel; ours fills from the exact edge and then adds the line pixels, which
   is close but not the same thing. GXR chapter 16 has the algorithm.
3. **`VDU 27` is an escape here** (`VDU 27,c` injects edit-cursor keys
   136-139 and prints any other byte literally); on a Master it is a no-op
   and the next byte is an ordinary VDU byte. A deliberate extension noted in
   the source; a Beeb program that sends 27 followed by a control code will
   behave differently.
4. **`VDU 23,0` CRTC registers** other than 10 and 11 (cursor shape) are
   ignored - 12/13 (screen start, used for hardware scrolling) and 8
   (interlace) in particular. Not testable through `POINT`.
5. **Not observable with this method, so untested:** MODE 7 teletext
   rendering, `VDU 19` palette and flashing colours, `VDU 23,9/10` flash
   rates, the cursor's own appearance, paged mode `VDU 14/15`, printer
   `VDU 2/3`. Shadow modes 128-135 are treated as their base mode, which is
   right for the pixels.
6. **Sprites** (`VDU 23,27`, PLOT 232-239) exist here and in the GXR but not
   on a Master; untested either way.

## Still to do

### Sectors and segments with slanted edges

Transliterate the GXR's arc/sector/segment code (chapter 16 of the
reassembly, `.plotCircleSector` and `.circleDoLineAndQuadrantInitialisation`)
the way the ellipse was: it walks the circle pixel by pixel, tracks the
start, end and chord lines with the line stepper, and fills each row from the
tracked line pixel with a per-quadrant state byte. Until then the four
slanted cases above differ by a few edge pixels.

### VDU 23,16

Three cases exist; the Master's semantics per flag bit are in the Master
Reference Manual (bit 0 scroll protect, bit 1 no-scroll, bit 2 vertical
wrap, and so on). The driver has no handler at all.

### Sprites

PLOT 232-239 are untested; the harness needs to learn to emit the
`VDU 23,27` sprite definitions first, and the oracle has to be
`--machine b` with the GXR because the Master has no sprites.
