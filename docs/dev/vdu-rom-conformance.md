# Testing the VDU/PLOT code against a real BBC Micro

> **STATUS 2026-09-06: harness working, lines conformant.** `tools/vdutest/`
> builds Pi1MHz's own `src/framebuffer/*` for the PC and diffs it, pixel for
> pixel, against a real OS 1.20 + GXR 1.20 running under beebjit. The line
> PLOT codes (0-63) were brought to an exact match by commits `ce0bc82` and
> `2c6ef25`; the other PLOT families now have coverage, and **22 of 35 cases
> match** - see "Where each PLOT family stands" at the end for the four
> differences that remain, none of which has been fixed yet.

## Why

Nearly all of `src/framebuffer/` exists to reproduce something the BBC's OS
already does, so "is this right?" has an authoritative answer that does not
depend on reading the code or on anyone's memory of the PRM: run the same
VDU bytes through the real ROM and compare the pixels.

Two of the three defects found in September 2026 were invisible to review and
obvious to this method within minutes - the line drawn by PLOT 8 was not the
line drawn by PLOT 0 minus a pixel, and our lines were direction-dependent
where a real Beeb's are not.

## The oracle: a real Beeb, headless

`beebjit` runs a BBC B with the real OS 1.20 and the real GXR ROM with no
display at all, driven by BASIC over the emulated serial line:

```sh
cd /mnt/c/Archlinux/beebjit
printf '10 MODE 4\r20 PRINT"hi"\rRUN\r' \
  | ./beebjit -headless -terminal -fast -rom f roms/gxr.rom -cycles 40000000000
```

* `-terminal` patches OS 1.20 so **stdin types into the machine and OSWRCH
  output comes back on stdout**. Program lines must end `\r`, not `\n`.
* ~200M cycles run in about 0.1 s, so a large `-cycles` budget is free. Long
  runs are dominated by `POINT` scanning inside BASIC, not by typing.
* **GXR is required.** Without `-rom f roms/gxr.rom` the PLOT codes 8-63
  (dotted and omit-endpoint lines) and everything from 128 up do not exist -
  the OS silently ignores them, which reads exactly like a passing test. GXR
  needs no `*` command; it claims the VDU vector on reset.
* ROM checksums: `os12.rom` md5 `0a59a5ba15fe8557b5f7fee32bbd393a`,
  `gxr.rom` md5 `82055d129d9aa834622319f7ecc45155`.

## The other side: our VDU driver, on the PC

`src/framebuffer/{framebuffer,screen_modes,primitives,fonts,teletext}.c` are
portable C and compile for the host untouched. `tools/vdutest/stubs.c`
supplies the ~18 bare-metal symbols they reference (screen buffer, timer,
interrupt controller, a few no-ops) and `tools/vdutest/vduhost.c` feeds VDU
bytes to the real `fb_writec` and dumps pixels with `prim_get_pixel`.

This runs **the same code the firmware runs** - no transcription, no model of
the algorithm that can drift from it.

One wrinkle: `screen_allocate_buffer` returns a `uint32_t`, which a 64-bit
`malloc` pointer does not fit into, so the stub maps the buffer with
`MAP_32BIT`.

## Running it

```sh
tools/vdutest/vdutest.py              # every case
tools/vdutest/vdutest.py circle line  # only matching case names
```

Each case in `vdutest.py` is a list of VDU byte sequences. Both sides are
driven from that one table, so there is no way for the two to test different
things. Set `BEEBJIT=` to point at another beebjit tree.

Adding a case is one line, e.g. a filled circle of radius 5:

```python
case("circ-157-fill-r5", plot(MOVE,14,12), plot(157,19,12)),
```

## Four traps

All four produce plausible, wrong data rather than an error.

1. **Never `PRINT` while scanning the screen with `POINT`.** Text output
   scrolls the screen and moves the image out from under the scan. Put the
   text window somewhere the graphics are not: `VDU 28,10,31,39,0`.
2. **Once a text window is set, `CLS` clears only that window.** The graphics
   survive and successive tests accumulate on top of each other, which looks
   like wildly wrong output. Use `CLG`.
3. **`MODE 0` leaves ~5.5K for BASIC** - two 27x27 integer arrays give
   `DIM space`. Use `MODE 4`, where a pixel is 4 OS units in both axes.
4. **beebjit's stdout contains NUL bytes** from the VDU stream, so plain
   `grep` treats a capture as binary and silently reports nothing. Use
   `grep -a`.

## What has been checked

Findings that came out of this, all confirmed on the ROM:

* **Lines are direction-independent on a real Beeb** - A to B and B to A are
  the identical pixel set. The OS achieves that by swapping the endpoints;
  `prim_draw_line` instead picks its initial error term from the major-axis
  direction, which gives the same pixels while still traversing from the
  caller's start point. That traversal order matters: the dot pattern is
  consumed from the `MOVE` end, and omit-first/omit-last name the caller's
  endpoints, not the loop's - a line the OS swaps internally still drops the
  endpoint the program passed to PLOT.
* **Omitting an endpoint removes that pixel and disturbs nothing else.**
* **PLOT 40-47 and 56-63 omit both endpoints**, not just the last.
* **Dotted lines**: a new dotted line restarts the pattern; an omit-first
  dotted line continues it seamlessly; an omitted point consumes no slot.
* **VDU 5 places the character cell's top-left at the graphics cursor** - an
  8x8 block plotted at pixel (24,24) covers x 24-31, y 17-24. Glyph rows, and
  any vertical scaling, therefore expand *downwards*.

## Where each PLOT family stands

From `tools/vdutest/vdutest.py` as of 2026-09-06 (22/35 cases matching).

| PLOT | family | status |
|------|--------|--------|
| 0-63 | lines, all 8 dotted/omit variants | **exact** - and 864 further line instances were compared pixel for pixel while fixing `2c6ef25` |
| 64-71 | point | **exact** |
| 72-79, 88-95, 104-111, 120-127 | horizontal line fills | `93` exact; `77` differs only because the triangle it fills against differs |
| 80-87 | triangle | **differs** - see below |
| 96-103 | rectangle | **exact**, both corner orders |
| 112-119 | parallelogram | **differs**, same edge behaviour as the triangle it is built from |
| 128-143 | flood fill | differs only by the triangle edge it floods up to |
| 144-151 | circle outline | exact at r=7 and r=10; **differs at r=4 and r=1** |
| 152-159 | filled circle | **exact** at r=5 and r=10 |
| 160-167 | arc | **exact** |
| 168-175 | chord | **exact** |
| 176-183 | sector | **exact** |
| 184-191 | move/copy rectangle | **exact**, both move and copy |
| 192-207 | ellipse, outline and filled | **differs** - one pixel too tall |
| 232-239 | sprite | **not tested** - GXR sprites need VDU 23,27 definitions the harness does not yet generate |

### The four open differences

1. **A degenerate triangle loses most of its span.** Three collinear points
   (`MOVE 4,4 : MOVE 20,4 : PLOT 85,26,4`) draw 23 pixels on a Beeb and 7
   here. In `prim_fill_triangle`, when all three y values are equal the
   sort leaves `y1 == y2 == y3`, `fill_bottom_flat_triangle` takes its
   `y1 == y2` branch and draws `draw_hline(x2, x3, y1)` - `x1` is never
   looked at, so the span is whatever two vertices happen to sort last.
   This one is a plain bug rather than a rasterisation difference.

2. **Triangle edges include different pixels.** Non-degenerate triangles
   have the right shape and nearly the right area (e.g. 304 pixels on the
   Beeb vs 299 here) but disagree along the sloped edges.
   `fill_bottom_flat_triangle` interpolates in `float` with a `+ 0.5` bias
   and truncates toward zero; the OS does not. This is what also shows up
   in the parallelogram (built from two triangles) and in the fills that
   run up to a triangle edge.

3. **Small circles differ.** r=7 and r=10 are exact, r=4 and r=1 are not:
   the OS's r=4 ring passes through (+-2,+-4) and (+-4,+-2) where our
   midpoint circle gives (+-2,+-3) and (+-3,+-2), and its r=1 circle is a
   full 8-pixel ring where ours is only the 4 axis points.

4. **Ellipses are one pixel too tall.** Outline and filled alike, our top
   and bottom rows sit one pixel beyond the OS's, so the shape is two
   pixels taller overall. Consistent across wide, tall and sheared cases.

None of these is likely to matter for the Domesday/AIV workload, which draws
text and video rather than BASIC graphics - they are recorded here so the
next person does not have to rediscover them, and so the harness has a
known-good baseline to regress against.
