#!/usr/bin/env python3
"""Compare Pi1MHz's VDU driver against a real BBC Master 128 (MOS 3.20).

Both sides are driven from the one test table below: each case is a screen
mode, a list of VDU byte sequences and a box of pixels to compare.  The Beeb
side runs them under beebjit (headless, driven over the emulated serial line)
and reads the screen back with POINT; the host side runs the very same bytes
through src/framebuffer/* built for the PC.  Both emit "L <name>" / "P x y c"
records (c = logical colour), which are then diffed.

  ./vdutest.py                  every case, against the ROM if beebjit is
                                installed, else against the recorded results
  ./vdutest.py text window      only cases whose name contains a pattern
  ./vdutest.py --golden         compare against golden/<machine>.txt only
  ./vdutest.py --record         re-record golden/<machine>.txt from the ROM
  ./vdutest.py --machine b      use a BBC B with OS 1.20 + GXR instead
  ./vdutest.py -v ...           list every differing pixel, not just ten

The recorded results mean the suite runs on any PC with gcc and python3; the
ROM is only needed to add or re-record cases.  It is not part of the build -
run it by hand after touching src/framebuffer/.

See docs/dev/vdu-rom-conformance.md for the method and its traps.
"""
import hashlib, os, re, shutil, subprocess, sys, tempfile

HERE     = os.path.dirname(os.path.abspath(__file__))
ROOT     = os.path.abspath(os.path.join(HERE, "..", ".."))
BEEBJIT  = os.environ.get("BEEBJIT", "/mnt/c/Archlinux/beebjit")
GOLDEN   = os.path.join(HERE, "golden")
BATCH    = 12         # cases per machine: both sides start afresh this often

# OS units per pixel, (x, y), for the modes the suite uses
UNITS = {0: (2, 4), 1: (4, 4), 2: (8, 4), 4: (4, 4), 5: (8, 4)}

# --------------------------------------------------------------------------
# Building blocks for the table

def P(n):                      # a 16-bit OS-unit coordinate, as VDU bytes
    n &= 0xFFFF
    return [n & 0xFF, n >> 8]

def plot(k, x, y, u=(4, 4)):   # VDU 25,k,x;y;  with x,y in pixels of a mode
    return [25, k] + P(x * u[0]) + P(y * u[1])

def plotu(k, x, y):            # VDU 25,k,x;y;  with x,y already in OS units
    return [25, k] + P(x) + P(y)

def text(s):                   # printable text, one byte per character
    return [ord(c) for c in s]

def vdu(*b):                   # any VDU bytes, verbatim
    return list(b)

def rows(n, s="ROW%d"):        # n lines of text, each ended by CR LF
    out = []
    for i in range(n):
        out += text(s % i) + [13, 10]
    return out

MOVE = 4                       # PLOT 4 = move absolute

# Every case starts from a fresh MODE with the cursor off, VDU 23,16 flags clear
# and default ECF/dot patterns (the Master keeps those across MODE).  A case
# that changes anything else persistent - a character definition - must put it
# back, so results never depend on which cases ran before.
class Case:
    def __init__(self, name, *cmds, mode=4, box=(0, 0, 30, 24)):
        self.name, self.cmds, self.mode, self.box = name, list(cmds), mode, box

def case(name, *cmds, **kw):
    return Case(name, *cmds, **kw)

# MODE 4 helpers: the original PLOT set lives here (1 px = 4 units both ways)
U4 = UNITS[4]
U5 = UNITS[5]
U0 = UNITS[0]

CURSOR_OFF = vdu(23,1,0,0,0,0,0,0,0,0)   # VDU 23,1,0;0;0;0;

CASES = [
    # ===== PLOT: the family measured in 2026-09 =============================
    case("line-5-solid",       plot(MOVE,0,0),  plot(5,26,7)),
    case("line-13-omitlast",   plot(MOVE,0,0),  plot(13,26,7)),
    case("line-21-dotted",     plot(MOVE,0,0),  plot(21,26,7)),
    case("line-37-omitfirst",  plot(MOVE,0,0),  plot(37,26,7)),
    case("line-5-reverse",     plot(MOVE,26,7), plot(5,0,0)),
    case("line-5-steep",       plot(MOVE,0,0),  plot(5,7,24)),
    case("line-1-relative",    plot(MOVE,3,3),  plot(1,20,5), plot(1,-10,8)),

    case("point-69",           plot(69,13,11)),
    case("point-69-edge",      plot(69,0,0)),

    case("tri-85-a",  plot(MOVE,1,1),  plot(MOVE,27,5),  plot(85,9,23)),
    case("tri-85-b",  plot(MOVE,3,20), plot(MOVE,25,2),  plot(85,14,22)),
    case("tri-85-flat-bottom", plot(MOVE,2,2), plot(MOVE,26,2), plot(85,14,20)),
    case("tri-85-flat-top",    plot(MOVE,2,20), plot(MOVE,26,20), plot(85,14,3)),
    case("tri-85-degenerate",  plot(MOVE,4,4), plot(MOVE,20,4), plot(85,26,4)),

    case("rect-101",   plot(MOVE,3,3),  plot(101,25,20)),
    case("rect-101-r", plot(MOVE,25,20), plot(101,3,3)),
    case("para-117",   plot(MOVE,2,2),  plot(MOVE,18,2),  plot(117,26,18)),
    case("para-117-b", plot(MOVE,4,18), plot(MOVE,20,22), plot(117,26,6)),

    case("circ-149-r10", plot(MOVE,14,12), plot(149,24,12)),
    case("circ-149-r7",  plot(MOVE,14,12), plot(149,21,12)),
    case("circ-149-r4",  plot(MOVE,14,12), plot(149,18,12)),
    case("circ-149-r1",  plot(MOVE,14,12), plot(149,15,12)),
    case("circ-157-fill-r10", plot(MOVE,14,12), plot(157,24,12)),
    case("circ-157-fill-r5",  plot(MOVE,14,12), plot(157,19,12)),

    case("ell-197-wide", plot(MOVE,14,12), plot(MOVE,26,12), plot(197,14,20)),
    case("ell-197-tall", plot(MOVE,14,12), plot(MOVE,20,12), plot(197,14,23)),
    case("ell-205-fill", plot(MOVE,14,12), plot(MOVE,26,12), plot(205,14,20)),
    case("ell-197-shear",plot(MOVE,14,12), plot(MOVE,24,12), plot(197,18,21)),

    case("arc-165",    plot(MOVE,14,12), plot(MOVE,26,12), plot(165,14,24)),
    case("chord-173",  plot(MOVE,14,12), plot(MOVE,26,12), plot(173,14,24)),
    case("sector-181", plot(MOVE,14,12), plot(MOVE,26,12), plot(181,14,24)),

    case("hfill-77",  plot(MOVE,4,4), plot(85,26,4), plot(MOVE,14,12), plot(77,14,12)),
    case("hfill-93",  plot(MOVE,2,2), plot(101,26,20), plot(MOVE,14,12), plot(93,14,12)),

    case("flood-133", plot(MOVE,4,4), plot(MOVE,26,6), plot(85,14,22),
                      plot(MOVE,14,10), plot(133,14,10)),
    case("flood-141-to-fg", plot(MOVE,4,4), plot(101,26,22), plot(MOVE,10,10), plot(101,16,14),
                      vdu(18,0,0), plot(MOVE,6,6), plot(141,6,6)),

    case("copy-189", plot(MOVE,2,2), plot(101,10,10),
                     plot(MOVE,2,2), plot(MOVE,10,10), plot(189,16,12)),
    case("move-185", plot(MOVE,2,2), plot(101,10,10),
                     plot(MOVE,2,2), plot(MOVE,10,10), plot(185,16,12)),

    # ===== ellipses over a spread of axes, shears and modes ==================
    case("ell-197-12-5",   plot(MOVE,30,30), plot(MOVE,42,30), plot(197,30,35), box=(0,0,60,60)),
    case("ell-205-12-5",   plot(MOVE,30,30), plot(MOVE,42,30), plot(205,30,35), box=(0,0,60,60)),
    case("ell-197-5-12",   plot(MOVE,30,30), plot(MOVE,35,30), plot(197,30,42), box=(0,0,60,60)),
    case("ell-205-5-12",   plot(MOVE,30,30), plot(MOVE,35,30), plot(205,30,42), box=(0,0,60,60)),
    case("ell-197-16-3",   plot(MOVE,30,30), plot(MOVE,46,30), plot(197,30,33), box=(0,0,60,60)),
    case("ell-197-3-16",   plot(MOVE,30,30), plot(MOVE,33,30), plot(197,30,46), box=(0,0,60,60)),
    case("ell-197-1-1",    plot(MOVE,30,30), plot(MOVE,31,30), plot(197,30,31), box=(0,0,60,60)),
    case("ell-197-0-5",    plot(MOVE,30,30), plot(MOVE,30,30), plot(197,30,35), box=(0,0,60,60)),
    case("ell-197-5-0",    plot(MOVE,30,30), plot(MOVE,35,30), plot(197,30,30), box=(0,0,60,60)),
    case("ell-205-5-0",    plot(MOVE,30,30), plot(MOVE,35,30), plot(205,30,30), box=(0,0,60,60)),
    case("ell-197-11-13",  plot(MOVE,30,30), plot(MOVE,41,30), plot(197,30,43), box=(0,0,60,60)),
    case("ell-205-13-11",  plot(MOVE,30,30), plot(MOVE,43,30), plot(205,30,41), box=(0,0,60,60)),
    case("ell-197-shear-p", plot(MOVE,30,30), plot(MOVE,42,30), plot(197,36,38), box=(0,0,60,60)),
    case("ell-197-shear-n", plot(MOVE,30,30), plot(MOVE,42,30), plot(197,24,38), box=(0,0,60,60)),
    case("ell-197-shear-big", plot(MOVE,30,30), plot(MOVE,38,30), plot(197,45,42), box=(0,0,60,60)),
    case("ell-205-shear-big", plot(MOVE,30,30), plot(MOVE,38,30), plot(205,45,42), box=(0,0,60,60)),
    case("ell-197-shear-down", plot(MOVE,30,30), plot(MOVE,42,30), plot(197,36,22), box=(0,0,60,60)),
    case("ell-205-shear-down", plot(MOVE,30,30), plot(MOVE,42,30), plot(205,36,22), box=(0,0,60,60)),
    case("ell-197-below",  plot(MOVE,30,30), plot(MOVE,18,30), plot(197,30,20), box=(0,0,60,60)),
    case("ell-197-shear-thin", plot(MOVE,30,30), plot(MOVE,32,30), plot(197,40,42), box=(0,0,60,60)),
    case("m0-ell-205",     plot(MOVE,20,12,U0), plot(MOVE,36,12,U0), plot(205,20,20,U0), mode=0, box=(0,0,44,24)),
    case("m0-ell-197-shear", plot(MOVE,20,12,U0), plot(MOVE,36,12,U0), plot(197,28,22,U0), mode=0, box=(0,0,44,24)),
    case("m5-ell-205",     plot(MOVE,14,12,U5), plot(MOVE,26,12,U5), plot(205,14,20,U5), mode=5),
    case("m5-ell-197-shear", plot(MOVE,14,12,U5), plot(MOVE,24,12,U5), plot(197,18,21,U5), mode=5),
    case("ell-gwin-clip",  vdu(24) + P(80) + P(80) + P(180) + P(180), plot(MOVE,30,30), plot(MOVE,50,30), plot(205,30,50), box=(0,0,60,60)),
    case("ell-197-eor",    vdu(18,0,1), plot(MOVE,20,20), plot(101,40,40), vdu(18,3,1), plot(MOVE,30,30), plot(MOVE,45,30), plot(197,30,40), box=(0,0,60,60)),

    # ===== arcs and circles through off-axis radius points ==================
    case("circ-149-offaxis", plot(MOVE,14,12), plot(149,22,17)),
    case("circ-157-offaxis", plot(MOVE,14,12), plot(157,21,18)),
    case("arc-165-offaxis",  plot(MOVE,14,12), plot(MOVE,22,17), plot(165,4,20)),
    case("sector-181-offaxis", plot(MOVE,14,12), plot(MOVE,22,17), plot(181,4,20)),
    case("chord-173-major",  plot(MOVE,14,12), plot(MOVE,26,12), plot(173,14,0)),
    case("sector-181-major", plot(MOVE,14,12), plot(MOVE,26,12), plot(181,2,12)),
    case("m5-arc-165-offaxis", plot(MOVE,14,12,U5), plot(MOVE,20,18,U5), plot(165,6,20,U5), mode=5),

    case("sector-181-slant-a", plot(MOVE,14,12), plot(MOVE,24,15), plot(181,10,22)),
    case("sector-181-slant-b", plot(MOVE,14,12), plot(MOVE,20,3),  plot(181,3,10)),
    case("sector-181-slant-major", plot(MOVE,14,12), plot(MOVE,22,17), plot(181,20,6)),
    case("chord-173-slant-a",  plot(MOVE,14,12), plot(MOVE,24,15), plot(173,10,22)),
    case("chord-173-slant-b",  plot(MOVE,14,12), plot(MOVE,20,3),  plot(173,3,10)),
    case("chord-173-slant-major", plot(MOVE,14,12), plot(MOVE,22,17), plot(173,20,6)),
    case("arc-165-slant-a",    plot(MOVE,14,12), plot(MOVE,24,15), plot(165,10,22)),
    case("arc-165-slant-major", plot(MOVE,14,12), plot(MOVE,22,17), plot(165,20,6)),
    case("m0-sector-181-slant", plot(MOVE,20,12,U0), plot(MOVE,36,17,U0), plot(181,6,20,U0), mode=0, box=(0,0,44,24)),
    case("m5-sector-181-slant", plot(MOVE,14,12,U5), plot(MOVE,22,16,U5), plot(181,6,20,U5), mode=5),
    case("m5-chord-173-slant",  plot(MOVE,14,12,U5), plot(MOVE,22,16,U5), plot(173,6,20,U5), mode=5),

    # ===== PLOT in modes whose pixels are not square =========================
    case("m0-line-5",   plot(MOVE,0,0,U0), plot(5,40,20,U0),          mode=0, box=(0,0,44,24)),
    case("m0-circ-149", plot(MOVE,20,12,U0), plot(149,36,12,U0),      mode=0, box=(0,0,44,24)),
    case("m0-circ-157", plot(MOVE,20,12,U0), plot(157,20,20,U0),      mode=0, box=(0,0,44,24)),
    case("m0-tri-85",   plot(MOVE,1,1,U0), plot(MOVE,40,4,U0), plot(85,12,22,U0), mode=0, box=(0,0,44,24)),
    case("m5-line-5",   plot(MOVE,0,0,U5), plot(5,26,7,U5),           mode=5),
    case("m5-circ-149", plot(MOVE,14,12,U5), plot(149,24,12,U5),      mode=5),
    case("m5-circ-157", plot(MOVE,14,12,U5), plot(157,14,20,U5),      mode=5),
    case("m5-ell-197",  plot(MOVE,14,12,U5), plot(MOVE,26,12,U5), plot(197,14,20,U5), mode=5),

    # ===== GCOL plot modes and colours (MODE 5, 4 colours) ==================
    case("gcol-0-colours", vdu(18,0,1), plot(MOVE,2,2,U5), plot(101,10,10,U5),
                           vdu(18,0,2), plot(MOVE,6,6,U5), plot(101,14,14,U5),
                           vdu(18,0,3), plot(MOVE,12,2,U5), plot(101,20,6,U5), mode=5),
    case("gcol-1-or",  vdu(18,0,1), plot(MOVE,2,2,U5), plot(101,14,14,U5),
                       vdu(18,1,2), plot(MOVE,8,8,U5), plot(101,20,20,U5), mode=5),
    case("gcol-2-and", vdu(18,0,3), plot(MOVE,2,2,U5), plot(101,14,14,U5),
                       vdu(18,2,1), plot(MOVE,8,8,U5), plot(101,20,20,U5), mode=5),
    case("gcol-3-eor", vdu(18,0,3), plot(MOVE,2,2,U5), plot(101,14,14,U5),
                       vdu(18,3,1), plot(MOVE,8,8,U5), plot(101,20,20,U5), mode=5),
    case("gcol-4-invert", vdu(18,0,1), plot(MOVE,2,2,U5), plot(101,14,14,U5),
                       vdu(18,4,0), plot(MOVE,8,8,U5), plot(101,20,20,U5), mode=5),
    case("gcol-bg-clg", vdu(18,0,130), vdu(16), vdu(18,0,1), plot(MOVE,4,4,U5), plot(101,8,8,U5), mode=5),
    case("gcol-3-eor-line", vdu(18,0,3), plot(MOVE,2,2,U5), plot(101,20,20,U5),
                       vdu(18,3,2), plot(MOVE,0,0,U5), plot(5,24,20,U5), mode=5),
    case("gcol-bg-line-plot7", vdu(18,0,2), plot(MOVE,2,2,U5), plot(101,20,20,U5),
                       plot(MOVE,0,0,U5), plot(7,24,20,U5), mode=5),

    # ===== ECF patterns and dot pattern (Master has GXR built in) ===========
    case("ecf-default-1", vdu(18,16,0), plot(MOVE,2,2,U5), plot(101,22,20,U5), mode=5),
    case("ecf-default-2", vdu(18,32,0), plot(MOVE,2,2,U5), plot(101,22,20,U5), mode=5),
    case("ecf-default-3", vdu(18,48,0), plot(MOVE,2,2,U5), plot(101,22,20,U5), mode=5),
    case("ecf-default-4", vdu(18,64,0), plot(MOVE,2,2,U5), plot(101,22,20,U5), mode=5),
    case("ecf-user-1", vdu(23,2,1,2,4,8,16,32,64,128), vdu(18,16,0),
                       plot(MOVE,2,2,U5), plot(101,22,20,U5), mode=5),
    case("ecf-simple-12", vdu(23,12,1,2,3,0,1,2,3,0), vdu(18,16,0),
                       plot(MOVE,2,2,U5), plot(101,22,20,U5), mode=5),
    case("ecf-11-reset", vdu(23,2,1,2,4,8,16,32,64,128), vdu(23,11,0,0,0,0,0,0,0,0),
                       vdu(18,16,0), plot(MOVE,2,2,U5), plot(101,22,20,U5), mode=5),
    case("ecf-mode4-1", vdu(18,16,0), plot(MOVE,2,2), plot(101,22,20)),
    case("ecf-circle", vdu(18,32,0), plot(MOVE,14,12,U5), plot(157,24,12,U5), mode=5),
    case("dot-23-6", vdu(23,6,0xF0,0,0,0,0,0,0,0), plot(MOVE,0,2), plot(21,28,2),
                     plot(MOVE,0,6), plot(21,28,12)),
    case("dot-23-6-len", vdu(23,6,0xC8,0,0,0,0,0,0,0), vdu(23,6,0xC8,3,0,0,0,0,0,0),
                     plot(MOVE,0,2), plot(21,28,2)),

    # ===== graphics window, origin ==========================================
    case("gwin-24-clip", vdu(24) + P(20) + P(20) + P(80) + P(60),
                         plot(MOVE,0,0), plot(101,28,22), box=(0,0,30,24)),
    case("gwin-24-clg",  plot(MOVE,0,0), plot(101,28,22),
                         vdu(24) + P(20) + P(20) + P(80) + P(60), vdu(16), box=(0,0,30,24)),
    case("gwin-24-line", vdu(24) + P(16) + P(16) + P(80) + P(60),
                         plot(MOVE,0,0), plot(5,29,23), box=(0,0,30,24)),
    case("gwin-24-circle", vdu(24) + P(16) + P(16) + P(80) + P(60),
                         plot(MOVE,14,12), plot(157,24,12), box=(0,0,30,24)),
    case("gwin-24-flood", vdu(24) + P(16) + P(16) + P(80) + P(60),
                         plot(MOVE,14,12), plot(133,14,12), box=(0,0,30,24)),
    case("gwin-26-reset", vdu(24) + P(20) + P(20) + P(80) + P(60), vdu(26),
                         plot(MOVE,0,0), plot(101,28,22), box=(0,0,30,24)),
    case("origin-29",    vdu(29) + P(40) + P(40), plot(MOVE,0,0), plot(101,8,8),
                         plot(MOVE,-4,-4), plot(69,-4,-4)),
    case("origin-29-window", vdu(29) + P(40) + P(40),
                         vdu(24) + P(0) + P(0) + P(40) + P(40), plot(MOVE,-10,-10), plot(101,20,20)),

    # ===== text: glyphs, cursor movement, windows, scrolling ===============
    case("text-hello",     text("Hello"),                       box=(0,224,48,255)),
    case("text-symbols",   text("#$%&@[\\]^_`{|}~"),            box=(0,224,120,255)),
    case("text-digits",    text("0123456789+-*/=<>?!\""),      box=(0,224,168,255)),
    case("text-upper",     text("ABCDEFGHIJKLMNOPQRSTUVWXYZ"),  box=(0,224,208,255)),
    case("text-lower",     text("abcdefghijklmnopqrstuvwxyz"), box=(0,224,208,255)),
    case("text-high-128",  vdu(128,129,130,131,140,150,160),   box=(0,224,64,255)),
    case("text-high-224",  vdu(224,225,226,240,253,254),       box=(0,224,56,255)),
    case("text-vdu31",     vdu(31,3,2) + text("X"),           box=(0,224,48,255)),
    case("text-cursor-8-9",  text("AB") + vdu(8) + text("C") + vdu(9,9) + text("D"), box=(0,224,56,255)),
    case("text-cursor-10-11",text("A") + vdu(10) + text("B") + vdu(11,11) + text("C"), box=(0,224,32,255)),
    case("text-cursor-13",   text("ABC") + vdu(13) + text("Z"),  box=(0,224,32,255)),
    case("text-cursor-30",   vdu(31,4,4) + text("Q") + vdu(30) + text("H"), box=(0,208,48,255)),
    case("text-127",         text("ABC") + vdu(127) + text("D") + vdu(127,127,127,127), box=(0,224,40,255)),
    case("text-wrap-eol",    text("A" * 42),                    box=(0,224,320,255)),
    case("text-127-wrap",    text("A" * 41) + vdu(127,127) + text("B"), box=(0,224,320,255)),
    case("text-lf-scroll",   rows(33, "L%d"),                   box=(0,0,40,255)),
    case("text-scroll-graphic", plot(MOVE,0,0), plot(101,30,30), rows(32, "R%d"), box=(0,0,40,255)),
    case("text-cls",         text("AB") + vdu(12) + text("C"),  box=(0,224,32,255)),
    case("text-window-28",   vdu(28,2,5,8,2) + text("ABCDEFGHIJKLMNOP"), box=(0,192,80,255)),
    case("text-window-scroll", vdu(28,2,5,8,2) + rows(6, "W%d"), box=(0,192,80,255)),
    case("text-window-cls",  text("ZZZZZZZZZZ") + vdu(28,2,5,8,2) + vdu(12) + text("A"), box=(0,192,88,255)),
    case("text-window-26",   vdu(28,2,5,8,2) + vdu(26) + text("A"), box=(0,224,16,255)),
    case("text-window-home", vdu(28,2,5,8,2) + text("ABC") + vdu(30) + text("Q"), box=(0,192,80,255)),
    case("text-window-31",   vdu(28,2,5,8,2) + vdu(31,1,1) + text("Q") + vdu(31,20,20) + text("N"), box=(0,192,80,255)),
    case("text-colour-17",   vdu(17,2) + text("AB") + vdu(17,129) + text("CD"), mode=5, box=(0,224,32,255)),
    case("text-colour-cls",  vdu(17,130) + vdu(12) + vdu(17,1) + text("A"), mode=5, box=(0,224,24,255)),
    case("text-vdu21",       text("A") + vdu(21) + text("B") + vdu(6) + text("C"), box=(0,224,32,255)),
    case("text-vdu27",       text("A") + vdu(27,65) + vdu(27,12) + text("B"), box=(0,224,40,255)),
    case("text-vdu1",        text("A") + vdu(1,66) + text("C"),  box=(0,224,32,255)),
    case("text-userchar",    vdu(23,255,0x18,0x24,0x42,0x81,0x81,0x42,0x24,0x18) + vdu(255) + text("A") + vdu(255), box=(0,224,32,255)),
    case("text-userchar-low", vdu(23,65,0xFF,0x81,0x81,0x81,0x81,0x81,0x81,0xFF) + text("AB")
                              + vdu(23,65,0x3C,0x66,0x66,0x7E,0x66,0x66,0x66,0x00), box=(0,224,24,255)),   # the ROM glyph back

    # ===== VDU 5: text at the graphics cursor ===============================
    case("vdu5-place",     vdu(5), plot(MOVE,6,20), text("Ab"), box=(0,0,30,24)),
    case("vdu5-cursor",    vdu(5), plot(MOVE,6,20), text("A") + vdu(8) + text("B") + vdu(10) + text("C") + vdu(11,11) + text("D"), box=(0,0,40,24)),
    case("vdu5-127",       vdu(5), plot(MOVE,4,20), text("AB") + vdu(127) + text("C"), box=(0,0,30,24)),
    case("vdu5-cr",        vdu(5), plot(MOVE,4,20), text("AB") + vdu(13) + text("C"), box=(0,0,30,24)),
    case("vdu5-eor",       vdu(18,0,1), plot(MOVE,0,0), plot(101,20,20), vdu(18,3,1), vdu(5), plot(MOVE,4,18), text("A"), box=(0,0,30,24)),
    case("vdu5-gcol",      vdu(18,0,2), vdu(5), plot(MOVE,4,20), text("A"), mode=5, box=(0,0,30,24)),
    case("vdu5-wrap",      vdu(5), plot(MOVE,304,20,(1,4)), text("ABC"), box=(0,0,319,24)),
    case("vdu5-window",    vdu(24) + P(16) + P(16) + P(80) + P(80), vdu(5), plot(MOVE,2,20), text("AB"), box=(0,0,30,24)),
    case("vdu5-back-4",    vdu(5), plot(MOVE,6,20), text("A"), vdu(4), text("B"), box=(0,0,30,24)),
    # The nine codes whose meaning VDU 4/5 switches (8-13, 30, 31, 127): the
    # three not covered above, a MODE change (which must drop back to VDU 4),
    # and all nine in text mode after a VDU 5 -> VDU 4 round trip.  VDU 4 and
    # MODE turn the text cursor back on: the Pi draws it into the frame, the
    # Beeb's is the 6845 hardware cursor that POINT cannot see, so those two
    # cases turn it off again before the snapshot.
    case("vdu5-ht-9",      vdu(5), plot(MOVE,4,20), text("A") + vdu(9) + text("B"), box=(0,0,40,24)),
    case("vdu5-home-30",   vdu(5), plot(MOVE,40,40), text("A") + vdu(30) + text("H"), box=(0,0,60,255)),
    case("vdu5-tab-31",    vdu(5), vdu(31,3,2) + text("T"), box=(0,200,48,255)),
    case("vdu5-then-4-all", vdu(5), vdu(4), vdu(31,4,4) + text("AB") + vdu(8) + text("C") + vdu(9) + text("D")
                            + vdu(10) + text("E") + vdu(11) + text("F") + vdu(13) + text("G") + vdu(127)
                            + vdu(30) + text("H") + CURSOR_OFF, box=(0,200,72,255)),
    case("vdu5-mode-resets", vdu(5), vdu(22,4), text("AB") + vdu(8) + text("C") + vdu(9) + text("D")
                            + vdu(127) + vdu(13) + text("E") + CURSOR_OFF, box=(0,224,48,255)),
    case("vdu5-vdu12",     plot(MOVE,0,0), plot(101,20,20), vdu(5), vdu(12), text("A"), box=(0,0,30,24)),

    # ===== VDU 23,7 scroll and VDU 23,8 clear block =========================
    case("scroll-23-7-up",    plot(MOVE,2,2), plot(101,10,10), vdu(23,7,0,3,1,0,0,0,0,0), box=(0,0,30,24)),
    case("scroll-23-7-right", plot(MOVE,2,2), plot(101,10,10), vdu(23,7,0,0,1,0,0,0,0,0), box=(0,0,30,24)),
    case("scroll-23-7-down-cell", plot(MOVE,2,240), plot(101,10,250), vdu(23,7,0,2,0,0,0,0,0,0), box=(0,200,30,255)),
    case("scroll-23-7-window", vdu(28,0,10,4,5), plot(MOVE,2,160), plot(101,20,180), vdu(23,7,1,3,1,0,0,0,0,0), box=(0,150,40,200)),
    case("clear-23-8", plot(MOVE,0,200), plot(101,60,255), vdu(23,8,0,0,2,2,5,5,0,0), box=(0,190,60,255)),
    case("clear-23-8-cursor", plot(MOVE,0,200), plot(101,60,255), vdu(31,3,3) + vdu(23,8,1,2,0,0,0,0,0,0), box=(0,190,60,255)),
    case("clear-23-8-window", vdu(28,2,5,8,2), plot(MOVE,0,200), plot(101,80,255), vdu(23,8,0,0,0,0,0,0,0,0), box=(0,190,80,255)),

    # ===== more text edges: wrapping, scrolling down, bad windows ============
    case("text-11-scroll-down", text("A") + vdu(13) + vdu(11) + text("B"), box=(0,232,16,255)),
    case("text-8-wrap",   vdu(31,0,1) + vdu(8) + text("Z"),               box=(0,240,320,255)),
    case("text-8-home",   vdu(8) + text("Z"),                             box=(0,240,320,255)),
    case("text-9-wrap",   vdu(31,39,0) + vdu(9) + text("Z"),              box=(0,240,320,255)),
    case("text-127-home", text("A") + vdu(13) + vdu(127) + text("Z"),     box=(0,240,320,255)),
    case("text-window-28-bad", vdu(28,8,2,2,5) + text("A"), box=(0,192,80,255)),
    case("text-window-28-off", vdu(28,2,40,8,2) + text("A"), box=(0,192,80,255)),
    case("gwin-24-bad", vdu(24) + P(80) + P(20) + P(20) + P(60), plot(MOVE,0,0), plot(101,28,22)),
    case("gwin-24-off", vdu(24) + P(20) + P(20) + P(2000) + P(60), plot(MOVE,0,0), plot(101,28,22)),
    case("text-colour-m2", vdu(17,9) + text("A") + vdu(17,14) + text("B") + vdu(17,130) + text("C"), mode=2, box=(0,224,24,255)),
    case("text-colour-m1", vdu(17,2) + text("A") + vdu(17,3) + text("B"), mode=1, box=(0,224,16,255)),

    # ===== VDU 23,16: the Master's cursor movement control ==================
    case("cursor-23-16-scrollprotect", vdu(23,16,1,0,0,0,0,0,0,0) + text("A" * 40) + vdu(8) + text("B"), box=(0,240,320,255)),
    case("cursor-nowrap-8-after-40",   text("A" * 40) + vdu(8) + text("B"), box=(0,240,320,255)),
    case("cursor-23-16-noscroll",      vdu(23,16,2,0,0,0,0,0,0,0) + rows(33, "L%d"), box=(0,0,40,255)),
    case("cursor-23-16-vdu10-wrap",    vdu(23,16,4,0,0,0,0,0,0,0) + vdu(31,0,31) + text("A") + vdu(10) + text("B"), box=(0,0,16,255)),

    # ===== more VDU 5 edges ==================================================
    case("vdu5-tab",   vdu(5), vdu(31,2,1) + text("T"), box=(0,0,40,24)),
    case("vdu5-home",  vdu(5), plot(MOVE,10,10), vdu(30) + text("H"), box=(0,224,16,255)),
    case("vdu5-home-window", vdu(24) + P(16) + P(16) + P(120) + P(80), vdu(5), vdu(30) + text("H"), box=(0,0,30,24)),
    case("vdu5-wrap-bottom", vdu(5), plot(MOVE,4,10), text("A") + vdu(10) + text("B") + vdu(10) + text("C"), box=(0,0,30,255)),
    case("vdu5-10-past-bottom", vdu(5), plot(MOVE,4,12), text("A") + vdu(10) + text("B") + vdu(10) + text("C") + vdu(10) + text("D"), box=(0,0,48,255)),
    case("vdu5-11-past-top",    vdu(5), plot(MOVE,4,244), text("A") + vdu(11) + text("B") + vdu(11) + text("C") + vdu(11) + text("D"), box=(0,0,48,255)),
    case("vdu5-8-past-left",    vdu(5), plot(MOVE,4,100), text("A") + vdu(8,8) + text("B") + vdu(8,8,8) + text("C"), box=(0,0,319,255)),
    case("vdu5-9-past-right",   vdu(5), plot(MOVE,300,100), text("A") + vdu(9) + text("B") + vdu(9) + text("C"), box=(0,0,319,255)),
    case("vdu5-13-then-10",     vdu(5), plot(MOVE,40,100), text("A") + vdu(13) + text("B") + vdu(10) + text("C"), box=(0,0,80,255)),
    case("vdu5-31-rows",        vdu(5), vdu(31,1,0) + text("A") + vdu(31,3,2) + text("B") + vdu(31,5,31) + text("C"), box=(0,0,60,255)),
    case("vdu5-127-colour", vdu(18,0,130), vdu(16), vdu(18,0,1), vdu(5), plot(MOVE,4,20,U5), text("AB") + vdu(127), mode=5, box=(0,0,30,24)),

    # ===== more fills and arcs ===============================================
    case("hfill-109", plot(MOVE,2,2), plot(101,26,20), vdu(18,0,0), plot(MOVE,8,8), plot(101,20,14),
                      vdu(18,0,1), plot(MOVE,14,12), plot(109,14,12)),
    case("hfill-125", plot(MOVE,2,2), plot(101,26,20), plot(MOVE,14,12), plot(125,14,12)),
    case("hfill-77-bg", plot(MOVE,2,2), plot(101,26,20), vdu(18,0,0), plot(MOVE,8,8), plot(101,20,14),
                      vdu(18,0,1), plot(MOVE,14,12), plot(77,14,12)),
    case("m0-arc-165",    plot(MOVE,20,12,U0), plot(MOVE,36,12,U0), plot(165,20,24,U0), mode=0, box=(0,0,44,24)),
    case("m0-sector-181", plot(MOVE,20,12,U0), plot(MOVE,36,12,U0), plot(181,20,24,U0), mode=0, box=(0,0,44,24)),
    case("m5-arc-165",    plot(MOVE,14,12,U5), plot(MOVE,26,12,U5), plot(165,14,24,U5), mode=5),
    case("m5-chord-173",  plot(MOVE,14,12,U5), plot(MOVE,26,12,U5), plot(173,14,24,U5), mode=5),
    case("m5-sector-181", plot(MOVE,14,12,U5), plot(MOVE,26,12,U5), plot(181,14,24,U5), mode=5),
    case("m5-copy-189",   plot(MOVE,2,2,U5), plot(101,10,10,U5), plot(MOVE,2,2,U5), plot(MOVE,10,10,U5), plot(189,16,12,U5), mode=5),
    case("m0-ell-197",    plot(MOVE,20,12,U0), plot(MOVE,36,12,U0), plot(197,20,20,U0), mode=0, box=(0,0,44,24)),
]

# --------------------------------------------------------------------------

def parse(text):
    """-> {name: set of (x,y,c)} from L/P records."""
    out, cur = {}, None
    for ln in text.splitlines():
        f = ln.split()
        if not f:
            continue
        if f[0] == "L":
            cur = " ".join(f[1:]); out[cur] = set()
        elif f[0] == "P" and cur is not None and len(f) == 4:
            try:    out[cur].add((int(f[1]), int(f[2]), int(f[3])))
            except ValueError: pass
    return out

def build_host():
    exe = os.path.join(tempfile.gettempdir(), "vduhost")
    src = ["vduhost.c", "stubs.c"]
    fb  = ["framebuffer.c", "screen_modes.c", "primitives.c", "fonts.c", "teletext.c"]
    cmd = (["gcc", "-O1", "-I", f"{ROOT}/src/framebuffer", "-I", f"{ROOT}/src", "-o", exe]
           + [os.path.join(HERE, s) for s in src]
           + [os.path.join(ROOT, "src/framebuffer", s) for s in fb] + ["-lm"])
    subprocess.run(cmd, check=True, stderr=subprocess.DEVNULL)
    return exe

def run_host_batch(exe, cases):
    script = []
    for c in cases:
        script += [f"V 22 {c.mode}", "V 23 1 0 0 0 0 0 0 0 0", "V 23 16 0 0 0 0 0 0 0 0",
                   "V 23 11 0 0 0 0 0 0 0 0", "V 23 6 170 170 170 170 170 170 170 170", f"L {c.name}"]
        for cmd_ in c.cmds:
            script.append("V " + " ".join(str(b) for b in cmd_))
        script.append("V 29 0 0 0 0")     # POINT is origin-relative on the Beeb
        script.append("DUMP %d %d %d %d" % c.box)
    r = subprocess.run([exe], input="\n".join(script) + "\n",
                       capture_output=True, text=True)
    return parse(r.stdout)

def run_host(cases, batch=BATCH):
    """Same batching as the Beeb side: a fresh driver every few cases, so
    anything a case leaves behind (character definitions, patterns) is seen
    the same way by both."""
    exe = build_host()
    out = {}
    for i in range(0, len(cases), batch):
        out.update(run_host_batch(exe, cases[i:i+batch]))
    return out

# --- the Beeb side ----------------------------------------------------------

MOS320_MD5 = "5f0d53f852bdcdf5f695c6f9c310fbbf"

def master_tree():
    """beebjit loads its ROMs by relative path, so build a tree whose MOS 3.20
    has three bytes changed: *FX2,1 and *FX3,5 as the reset defaults, and the
    ACIA receive interrupt forced on in the OSBYTE 156 handler (the Master
    rebuilds that register from CMOS at reset, so the table default alone
    is not enough).  That is what beebjit's -terminal does for OS 1.20."""
    tree = os.path.join(tempfile.gettempdir(), "vdutest-master")
    mos  = os.path.join(BEEBJIT, "roms/mos3.20/mos.rom")
    if not os.path.exists(mos):
        return None
    with open(mos, "rb") as f:
        m = bytearray(f.read())
    if hashlib.md5(m).hexdigest() != MOS320_MD5:
        sys.exit(f"vdutest: {mos} is not the MOS 3.20 image this patch was written for")
    m[0x2318] = 1            # OSBYTE 177 default: input from RS423
    m[0x2353] = 5            # OSBYTE 236 default: output to screen and RS423
    i = 0xE91E - 0xC000      # LDX &0250 in the OSBYTE 156 handler ...
    assert m[i:i+3] == b"\xae\x50\x02"
    m[i:i+3] = b"\x09\x80\xea"   # ... becomes ORA #&80 : NOP - receive IRQ on
    out = os.path.join(tree, "roms/mos3.20/mos.rom")
    if os.path.exists(out) and open(out, "rb").read() == bytes(m):
        return tree
    shutil.rmtree(tree, ignore_errors=True)
    os.makedirs(os.path.dirname(out))
    for name in os.listdir(os.path.join(BEEBJIT, "roms")):
        if name != "mos3.20":
            os.symlink(os.path.join(BEEBJIT, "roms", name), os.path.join(tree, "roms", name))
    for name in os.listdir(os.path.join(BEEBJIT, "roms/mos3.20")):
        if name != "mos.rom":
            os.symlink(os.path.join(BEEBJIT, "roms/mos3.20", name), os.path.join(tree, "roms/mos3.20", name))
    with open(out, "wb") as f:
        f.write(m)
    return tree

def beeb_program(cases, machine):
    """The BASIC program that runs the cases and scans the screen.  Results go
    to the serial line only (*FX3,3 turns the VDU driver off), so printing
    never scrolls the screen under the scan and any part of it can be read.
    The Master runs in a shadow mode, so BASIC has the full 29K under &8000
    whatever mode a case selects; the B sits at MODE 0's HIMEM instead."""
    if machine == "master":
        head = ["10 MODE 128"]; modebase = 128
    else:
        head = ["10 MODE 0"];   modebase = 0
    lines = ["NEW"] + head + [
              "20 *FX3,3",
              "25 PRINT",                 # ends the line MODE's VDU bytes leaked onto
              "30 READ NT",
              "40 FOR T=1 TO NT",
              "50 READ N$,M,NB,X0,Y0,X1,Y1,UX,UY",
              "60 *FX3,0",
             f"70 VDU 22,{modebase}+M,23,1,0;0;0;0;23,16,0;0;0;0;23,11,0;0;0;0;23,6,&AAAA;&AAAA;&AAAA;&AAAA;",
              "80 FOR I=1 TO NB:READ B:VDU B:NEXT",
              "90 VDU 29,0;0;",
              "100 *FX3,3",
              "110 PRINT \"L \";N$",
              "120 FOR X=X0 TO X1:FOR Y=Y0 TO Y1:P=POINT(X*UX,Y*UY):IF P>0 THEN PRINT \"P \";X;\" \";Y;\" \";P",
              "130 NEXT:NEXT",
              "140 NEXT",
              "150 PRINT \"ENDOFTEST\"",
              "160 END",
             f"500 DATA {len(cases)}" ]
    n = 501
    for c in cases:
        by = [b for cmd in c.cmds for b in cmd]
        ux, uy = UNITS[c.mode]
        lines.append(f'{n} DATA "{c.name}",{c.mode},{len(by)},{c.box[0]},{c.box[1]},{c.box[2]},{c.box[3]},{ux},{uy}'); n += 1
        for i in range(0, len(by), 16):
            lines.append(f"{n} DATA " + ",".join(str(b) for b in by[i:i+16])); n += 1
    lines.append("RUN")
    return "".join(l + "\r" for l in lines)

def run_beeb_batch(cases, machine):
    prog = beeb_program(cases, machine)
    if machine == "master":
        cwd = master_tree()
        argv = [os.path.join(BEEBJIT, "beebjit"), "-master", "-headless", "-terminal", "-fast",
                "-cycles", "900000000000"]
    else:
        cwd = BEEBJIT
        argv = [os.path.join(BEEBJIT, "beebjit"), "-headless", "-terminal", "-fast",
                "-rom", "f", os.path.join(BEEBJIT, "roms/gxr.rom"),
                "-cycles", "900000000000"]
    p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, cwd=cwd)
    p.stdin.write(prog.encode("latin-1")); p.stdin.flush()
    out = bytearray()
    while True:                       # stop as soon as the program is done -
        chunk = p.stdout.read1(65536) # beebjit would otherwise burn its whole
        if not chunk:                 # cycle budget sitting at the prompt
            break
        out += chunk
        if re.search(rb"[\r\n]ENDOFTEST", out[-80:]):   # not the echoed PRINT
            break
    p.kill(); p.wait()
    # beebjit's own log lines land in the middle of ours; lift them out whole
    out = re.sub(rb"info:misc:[^\n]*\n", b"", bytes(out))
    text = out.decode("latin-1").replace("\r", "\n")   # the RUN echo ends in a bare CR
    if "ENDOFTEST" not in text.split("RUN", 1)[-1]:
        print("WARNING: the Beeb run did not reach ENDOFTEST - results truncated",
              file=sys.stderr)
    return parse(text)

def run_beeb(cases, machine, batch=BATCH):
    """A fresh machine every few cases keeps the BASIC program small and
    stops state (character definitions, patterns) leaking between cases."""
    out = {}
    for i in range(0, len(cases), batch):
        out.update(run_beeb_batch(cases[i:i+batch], machine))
    return out

def have_beebjit():
    return os.path.exists(os.path.join(BEEBJIT, "beebjit"))

# --- golden files -----------------------------------------------------------

def golden_path(machine):
    return os.path.join(GOLDEN, f"{machine}.txt")

def load_golden(machine):
    try:
        with open(golden_path(machine)) as f:
            return parse(f.read())
    except FileNotFoundError:
        return {}

def save_golden(machine, results):
    os.makedirs(GOLDEN, exist_ok=True)
    old = load_golden(machine)
    old.update(results)
    with open(golden_path(machine), "w") as f:
        f.write(f"# Recorded from a real {machine} ROM under beebjit by vdutest.py --record\n")
        f.write("# L <case> then P <x> <y> <colour> per set pixel; do not edit by hand\n")
        for name in sorted(old):
            f.write(f"L {name}\n")
            for x, y, c in sorted(old[name]):
                f.write(f"P {x} {y} {c}\n")

# --------------------------------------------------------------------------

def main():
    args = sys.argv[1:]
    machine = "master"
    record = golden = verbose = False
    pats = []
    while args:
        a = args.pop(0)
        if a == "--machine": machine = args.pop(0)
        elif a == "--record": record = True
        elif a == "--golden": golden = True
        elif a in ("-v", "--verbose"): verbose = True
        elif a == "--list":
            for c in CASES: print(c.name)
            return 0
        else: pats.append(a)
    cases = [c for c in CASES if not pats or any(p in c.name for p in pats)]
    if not cases:
        sys.exit("no cases match")

    if record or (not golden and have_beebjit()):
        print(f"running {len(cases)} cases on the {machine} ROM ...", file=sys.stderr)
        beeb = run_beeb(cases, machine)
        if record:
            save_golden(machine, beeb)
            print(f"recorded {len(beeb)} cases into {golden_path(machine)}", file=sys.stderr)
        src = "ROM"
    else:
        beeb = load_golden(machine)
        src = "golden file"
        print(f"comparing {len(cases)} cases against {golden_path(machine)} ...", file=sys.stderr)
    host = run_host(cases)
    bad = 0
    for c in cases:
        b, h = beeb.get(c.name), host.get(c.name)
        if b is None:
            print(f"  {c.name:24s} NO {src.upper()} RESULT"); bad += 1; continue
        if h is None:
            print(f"  {c.name:24s} NO HOST RESULT"); bad += 1; continue
        if b == h:
            print(f"  {c.name:24s} match ({len(b)} px)")
        else:
            bad += 1
            print(f"  {c.name:24s} DIFFER  beeb={len(b)} host={len(h)} "
                  f"host-only={len(h-b)} beeb-only={len(b-h)}")
            n = 1000 if verbose else 10
            for tag, s in (("host-only", sorted(h - b)), ("beeb-only", sorted(b - h))):
                if s:
                    print(f"      {tag}: {s[:n]}{' ...' if len(s) > n else ''}")
    print(f"\n{len(cases)-bad}/{len(cases)} cases match the {machine} {src}")
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
