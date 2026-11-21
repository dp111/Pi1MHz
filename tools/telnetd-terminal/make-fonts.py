#!/usr/bin/env python3
# make-fonts.py - makes the BBC Micro fonts for TelnetD sessions (the *.psf console fonts and the *.ttf fonts) from the
# framebuffer's own character sets, so the glyphs exist in one place only:
#   src/framebuffer/fonts/bbc.fnt.h      the MOS character set (MODE 0-6)
#   src/framebuffer/fonts/saa5050.fnt.h  the teletext chip's character set and its mosaic graphics (MODE 7)
# The teletext font is smoothed by character rounding exactly as the framebuffer does it (src/framebuffer/fonts.c).
# Only the MOS fonts' few characters that the MOS has no glyph for (MOS_EXTRA) and the teletext font's ten ASCII
# characters that the chip has no glyph for (TTX_EXTRA) are drawn here; the rest of the TrueType-only characters
# (separated mosaics, quadrants, the shade, the telnetd_font=bbc pieces) are made by rule from the tables.
#   python3 make-fonts.py           writes the eight fonts next to this script (needs python3-fonttools for the .ttf)
#   python3 make-fonts.py --check   fails if the committed fonts differ from what it would write
import os, re, struct, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FONTS = os.path.join(HERE, "..", "..", "src", "framebuffer", "fonts")
VERSION = "1.4"


def load_table(name, bytes_per_char):
    """The bytes of a framebuffer font table, read from its .fnt.h (with its #includes) - one list per character."""
    def text(fn):
        t = open(os.path.join(FONTS, fn)).read()
        return re.sub(r'#include\s+"([^"]+)"', lambda m: text(m.group(1)), t)
    t = re.sub(r"//[^\n]*|/\*.*?\*/", "", text(name), flags=re.S)
    data = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)", t[t.index("{") + 1:t.rindex("}")])]
    assert len(data) == 256 * bytes_per_char, name
    return [data[c * bytes_per_char:(c + 1) * bytes_per_char] for c in range(256)]


BBC = load_table("bbc.fnt.h", 8)          # 8 x 8, bit 7 = left
SAA = load_table("saa5050.fnt.h", 10)     # 6 x 10 in bits 5-0; 0x80-0xBF contiguous graphics, indexed by the six cells


# fonts.c combine_rows and copy_font_character: each dot becomes 2 x 2 pixels, then (not for graphics) a half dot is
# added wherever two dots meet only at a corner.  The 6-dot rows land in bits 11-0 of each 12-pixel row.
def combine_rows(a, b):
    return (a | ((a >> 1) & b & ~(b >> 1)) | ((a << 1) & b & ~(b << 1))) & 0xFFFF


def saa_glyph(c):
    rows = []
    for data in SAA[c]:
        expanded = 0
        for j in range(8):
            expanded = (expanded << 2) & 0xFFFF
            if data & 0x80:
                expanded |= 3
            data = (data << 1) & 0xFF
        rows += [expanded, expanded]
    if c < 128:
        for i in range(9):
            o_row, e_row = rows[i * 2 + 1], rows[i * 2 + 2]
            rows[i * 2 + 1] = combine_rows(o_row, e_row)
            rows[i * 2 + 2] = combine_rows(e_row, o_row)
    return (12, 20, rows)


def mosaic(v):
    """The teletext mosaic with cells v (bit 0 top left ... bit 5 bottom right), from the chip's own graphics."""
    return saa_glyph(0x80 + v)


def separated(v):
    """The separated mosaic with cells v, from the chip's own graphics (Unicode 16 sends it as U+1CE50 + v)."""
    return saa_glyph(0xC0 + v)


def sextant(v):
    """The Unicode character TelnetD sends for mosaic v (telnetd_proto.c ttx_mosaic)."""
    return 0x1FB00 + (v - 1) - (v > 21) - (v > 42)


def bbc_glyph(c):
    return (8, 8, list(BBC[c]))


# The MOS has no glyph for these, but TelnetD sends them for MODE 7's own characters, so a MODE 7 screen in a MOS font
# still reads.  8 x 8, as the MOS draws.
MOS_EXTRA = {
    0xBC: ".#...... ##...... .#...#.. .#..#... ...#..#. ..#..##. .#..#### ......#.",   # one quarter
    0xBD: ".#...... ##...... .#...#.. .#..#... ...#.##. ..#....# .#....#. .....###",   # one half
    0xBE: "###..... ..#..... .##..#.. ..#.#... ##.#..#. ..#..##. .#..#### ......#.",   # three quarters
    0xF7: "........ ...##... ........ .######. ........ ...##... ........ ........",   # divide
    0x2015: "........ ........ ........ ######## ........ ........ ........ ........", # long dash
    0x2016: ".##..##. .##..##. .##..##. .##..##. .##..##. .##..##. .##..##. ........", # double bar
    0x2190: "........ ...#.... ..##.... .####### ..##.... ...#.... ........ ........", # left arrow
    0x2191: "...#.... ..###... .#.#.#.. ...#.... ...#.... ...#.... ...#.... ........", # up arrow
    0x2192: "........ ....#... ....##.. #######. ....##.. ....#... ........ ........", # right arrow
    0x25A0: "........ .######. .######. .######. .######. .######. .######. ........", # block
    0x258C: "####.... ####.... ####.... ####.... ####.... ####.... ####.... ####....", # left half block
    0x2590: "....#### ....#### ....#### ....#### ....#### ....#### ....#### ....####", # right half block
}


def drawn(art):
    return (8, 8, [int(r.replace("#", "1").replace(".", "0"), 2) for r in art.split()])


# The ASCII characters the chip shows its own symbols for, drawn in the chip's style (5 x 9 dots, rows 1-9 of its
# 6 x 10 cell; missing rows are blank) and rounded as the chip rounds, for a teletext window showing another MODE.
# From Mark Usher's make-fonts-telnetd.py (PR #53), INFERRED there: no chip has them.
TTX_EXTRA = {
    0x5B: ".###. .#... .#... .#... .#... .#... .###.",   # [
    0x5C: "#.... .#... .#... ..#.. ...#. ...#. ....#",   # \
    0x5D: ".###. ...#. ...#. ...#. ...#. ...#. .###.",   # ]
    0x5E: "..#.. .#.#. #...#",                           # ^
    0x5F: "..... ..... ..... ..... ..... ..... ..... #####",   # _
    0x60: ".#... ..#..",                                 # `
    0x7B: "...## ..#.. ..#.. .#... ..#.. ..#.. ...##",   # {
    0x7C: "..#.. ..#.. ..#.. ..#.. ..#.. ..#.. ..#..",   # |
    0x7D: "##... ..#.. ..#.. ...#. ..#.. ..#.. ##...",   # }
    0x7E: "..... ..... .#... #.#.# ...#.",               # ~
}


def ttx_drawn(art):
    """A TTX_EXTRA character as the chip would draw it: its dots in the SAA table's layout, then saa_glyph's rounding."""
    dots = [0] * 10
    for r, s in enumerate(art.split()):
        dots[r + 1] = sum(0x10 >> i for i, ch in enumerate(s) if ch == "#")
    rows = []
    for data in dots:
        expanded = 0
        for j in range(6):
            expanded <<= 2
            if data & (0x20 >> j):
                expanded |= 3
        rows += [expanded, expanded]
    for i in range(9):
        o_row, e_row = rows[i * 2 + 1], rows[i * 2 + 2]
        rows[i * 2 + 1] = combine_rows(o_row, e_row)
        rows[i * 2 + 2] = combine_rows(e_row, o_row)
    return (12, 20, rows)


# Each font: the glyphs of its console font in slot order, each with the characters it shows (the slot of an ASCII
# character is its code), and the extra characters only the TrueType font has.  Teletext has no glyph for the ASCII
# characters the chip replaces; TelnetD never sends them in MODE 7.
def teletext_font():
    slots = {c: (saa_glyph(c), [c]) for c in range(0x20, 0x7F) if chr(c) not in "#[\\]^_`{|}~"}
    slots[0x23] = (saa_glyph(0x5F), [0x23])                      # the chip's # is at 0x5F; 0x23 is its pound
    for i, (c, cp) in enumerate([(0x23, 0xA3), (0x7B, 0xBC), (0x5C, 0xBD), (0x7D, 0xBE), (0x7E, 0xF7),
                                 (0x60, 0x2015), (0x7C, 0x2016), (0x5B, 0x2190), (0x5E, 0x2191), (0x5D, 0x2192),
                                 (0x7F, 0x25A0)]):
        slots[0x80 + i] = (saa_glyph(c), [cp])
    for i, (v, cp) in enumerate([(63, 0x2588), (21, 0x258C), (42, 0x2590)]):
        slots[0x8B + i] = (mosaic(v), [cp])
    return slots


def teletext_ttf_extra():
    return {cp: ttx_drawn(art) for cp, art in TTX_EXTRA.items()}


def mos_font():
    slots = {c: (bbc_glyph(c), [c]) for c in range(0x20, 0x7F)}
    slots[0x60] = (bbc_glyph(0x60), [0x60, 0xA3])                # the MOS shows its pound for `
    extra = [0xBC, 0xBD, 0xBE, 0xF7, 0x2015, 0x2016, 0x2190, 0x2191, 0x2192, 0x25A0, 0x2588, 0x258C, 0x2590]
    for i, cp in enumerate(extra):
        slots[0x80 + i] = (bbc_glyph(0x7F) if cp == 0x2588 else drawn(MOS_EXTRA[cp]), [cp])
    return slots


# A character a program defines (VDU 23), without the BBC font: TelnetD sends 2 x 2 blocks, Unicode's quadrants
# (bit 0 top left, 1 top right, 2 bottom left, 3 bottom right; telnetd_proto.c put_udg).  The half and full blocks
# are already in every font.
QUADRANTS = {0x2598: 1, 0x259D: 2, 0x2580: 3, 0x2596: 4, 0x259E: 6, 0x259B: 7, 0x2597: 8, 0x259A: 9,
             0x259C: 11, 0x2584: 12, 0x2599: 13, 0x259F: 14}

# telnetd_font=bbc (BBC Micro MOS only): TelnetD draws a Beeb character as 80 / (the MODE's columns) cells of the
# 80-column font, so each MODE keeps its own shape - a MOS character's halves (MODE 1, 4, 6) at U+E900 + 2c, its
# quarters (MODE 2, 5) at U+E100 + 4c; a teletext cell g (32-127 the chip's characters by screen code, 128 + v a
# contiguous mosaic, 192 + v a separated one) in halves at U+E300 + 2 (256 h + g), h 0 normal height, 1 the top and
# 2 the bottom of double height; a VDU 23 character as 2 x 4 blocks at U+EA00 + v, bit 0 the top left ... 7 the
# bottom right (telnetd_proto.c PUA_MOS2 / PUA_MOS4 / PUA_TTX / PUA_UDG).  Nothing at U+E000-E0FF: terminals draw
# the Powerline symbols there.
PUA_MOS2, PUA_MOS4, PUA_TTX, PUA_UDG = 0xE900, 0xE100, 0xE300, 0xEA00


def columns(glyph, first, n):
    """Columns first .. first + n - 1 of a glyph, as a glyph n wide."""
    gw, gh, rows = glyph
    return (n, gh, [(r >> (gw - first - n)) & ((1 << n) - 1) for r in rows])


def pieces():
    out = {PUA_UDG + v: (2, 4, [(v >> (2 * r) & 1) << 1 | (v >> (2 * r + 1) & 1) for r in range(4)])
           for v in range(256)}
    for c in range(128):
        g = bbc_glyph(c) if 0x20 <= c < 0x7F else (8, 8, [0] * 8)     # what TelnetD shows: 32-126
        for h in range(2):
            out[PUA_MOS2 + 2 * c + h] = columns(g, 4 * h, 4)
        for q in range(4):
            out[PUA_MOS4 + 4 * c + q] = columns(g, 2 * q, 2)
    for g in range(256):
        full = (saa_glyph(g) if g >= 32 else (12, 20, [0] * 20))[2]
        for h, rows in ((0, full), (1, [full[y // 2] for y in range(20)]), (2, [full[10 + y // 2] for y in range(20)])):
            for half in range(2):
                out[PUA_TTX + 2 * (256 * h + g) + half] = columns((12, 20, rows), 6 * half, 6)
    return out


def ttf_only(grid, with_pieces):
    """The characters only the TrueType font has; grid is the font's own glyph grid, for the shade."""
    gw, gh = grid
    out = {sextant(v): mosaic(v) for v in range(1, 63) if v not in (21, 42)}
    out.update({0x1CE50 + v: separated(v) for v in range(1, 64)})
    out.update({cp: (2, 2, [(v & 1) << 1 | (v >> 1 & 1), (v >> 2 & 1) << 1 | (v >> 3 & 1)])
                for cp, v in QUADRANTS.items()})
    # U+2592, the medium shade - TelnetD's marker for a graphics colour code: every other dot
    out[0x2592] = (gw, gh, [int(("10" * gw)[y % 2:][:gw], 2) for y in range(gh)])
    if with_pieces:
        out.update(pieces())
    return out


# name, console font cell (w, h), dots of a glyph's row and column, TrueType advance (units of 1024 per em), the
# glyph grid, whether it carries the telnetd_font=bbc pieces, and its TrueType-only drawn characters
FONT_SET = [
    ("BBCMicroTeletext", "BBC Micro Teletext", teletext_font, 12, 20, 1, 1, 864, (12, 20), False, teletext_ttf_extra),
    ("BBCMicroMOS", "BBC Micro MOS", mos_font, 8, 16, 1, 2, 512, (8, 8), True, dict),
    ("BBCMicroMOS40", "BBC Micro MOS 40", mos_font, 16, 16, 2, 2, 1024, (8, 8), False, dict),
    ("BBCMicroMOS20", "BBC Micro MOS 20", mos_font, 32, 16, 4, 2, 2048, (8, 8), False, dict),
]


def psf(slots, width, height, sx, sy):
    """A PSF2 console font of 256 glyphs with a Unicode table; glyph g's dot (x, y) is sx x sy pixels."""
    row_bytes = (width + 7) // 8
    out = bytearray(struct.pack("<8I", 0x864AB572, 0, 32, 1, 256, row_bytes * height, height, width))
    table = bytearray()
    for s in range(256):
        rows = [0] * height
        if s in slots:
            (gw, gh, g), cps = slots[s]
            assert gw * sx == width and gh * sy == height
            for y in range(height):
                bits = g[y // sy]
                for x in range(width):
                    if bits & (1 << (gw - 1 - x // sx)):
                        rows[y] |= 1 << (row_bytes * 8 - 1 - x)
            for cp in cps:
                table += chr(cp).encode()
        for r in rows:
            out += r.to_bytes(row_bytes, "big")
        table.append(0xFF)
    return bytes(out + table)


def ttf(path, family, slots, advance, extra):
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    from fontTools.ttLib.tables.O_S_2f_2 import Panose
    from fontTools.misc.timeTools import timestampFromString

    chars = {cp: g for g, cps in slots.values() for cp in cps}
    chars.update(extra)
    ps = family.replace(" ", "")
    order = [".notdef"] + ["uni%04X" % cp if cp < 0x10000 else "u%05X" % cp for cp in sorted(chars)]
    glyphs, metrics = {}, {}
    for name, cp in zip(order, [None] + sorted(chars)):
        pen = TTGlyphPen(None)
        if cp is not None:
            gw, gh, g = chars[cp]
            # dot (x, y) of a gw x gh glyph fills its share of the cell, from the ascent (960) down to the descent (-64);
            # a run of dots in a row, carried on down the rows that repeat it, is one rectangle, drawn clockwise.
            # Where the row below the rectangle is lit, the rectangle reaches 8 units (1/128 em) into it, over those
            # dots only: shapes that merely meet leave a hairline seam in a terminal that draws the font anti-aliased
            # at a fractional size (found by Mark Usher, PR #53), and reaching under unlit dots would add a sliver.
            def runs(bits):
                found, x = [], 0
                while x < gw:
                    if bits & (1 << (gw - 1 - x)):
                        x0 = x
                        while x < gw and bits & (1 << (gw - 1 - x)):
                            x += 1
                        found.append((x0, x))
                    else:
                        x += 1
                return found
            open_runs = {}                                            # (x0, x1) -> first row
            for y in range(gh + 1):
                now = set(runs(g[y])) if y < gh else set()
                for run in sorted(r for r in open_runs if r not in now):
                    y0 = open_runs.pop(run)
                    left, right = round(run[0] * advance / gw), round(run[1] * advance / gw)
                    top, bottom = 960 - round(y0 * 1024 / gh), 960 - round(y * 1024 / gh)
                    pen.moveTo((left, bottom)); pen.lineTo((left, top))
                    pen.lineTo((right, top)); pen.lineTo((right, bottom)); pen.closePath()
                    below = g[y] if y < gh else 0
                    for x0, x1 in runs(below & (((1 << (run[1] - run[0])) - 1) << (gw - run[1]))):
                        l, r = round(x0 * advance / gw), round(x1 * advance / gw)
                        pen.moveTo((l, bottom - 8)); pen.lineTo((l, bottom))
                        pen.lineTo((r, bottom)); pen.lineTo((r, bottom - 8)); pen.closePath()
                for run in now:
                    open_runs.setdefault(run, y)
        glyphs[name] = pen.glyph()
    fb = FontBuilder(1024, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap({cp: n for n, cp in zip(order[1:], sorted(chars))})
    fb.setupGlyf(glyphs)
    glyf = fb.font["glyf"]
    fb.setupHorizontalMetrics({n: (advance, getattr(glyf[n], "xMin", 0)) for n in order})
    fb.setupHorizontalHeader(ascent=960, descent=-64)
    fb.setupNameTable({"familyName": family, "styleName": "Regular", "uniqueFontIdentifier": "%s-%s" % (ps, VERSION),
                       "fullName": family + " Regular", "version": "Version " + VERSION,
                       "psName": ps + "-Regular"})
    panose = Panose()
    panose.bFamilyType, panose.bProportion = 2, 9                 # monospaced
    # Latin 1 in the code page range and the Regular bit: without them Windows' GDI does not match the font for a
    # program that asks for the ANSI character set (PuTTY does) and draws Courier New instead (Mark Usher, PR #53)
    fb.setupOS2(sTypoAscender=960, sTypoDescender=-64, sTypoLineGap=0, usWinAscent=960, usWinDescent=64,
                xAvgCharWidth=advance, fsType=0, fsSelection=0x40, ulCodePageRange1=1, panose=panose)
    fb.setupPost(isFixedPitch=1)
    stamp = timestampFromString("Fri Oct 23 00:00:00 2026")       # fixed, so the same tables give the same file
    fb.updateHead(created=stamp, modified=stamp)
    fb.save(path)


def make(where):
    for name, family, font, w, h, sx, sy, advance, grid, with_pieces, drawn_extra in FONT_SET:
        slots = font()
        open(os.path.join(where, name + ".psf"), "wb").write(psf(slots, w, h, sx, sy))
        extra = ttf_only(grid, with_pieces)
        extra.update(drawn_extra())
        ttf(os.path.join(where, name + ".ttf"), family, slots, advance, extra)


if __name__ == "__main__":
    if sys.argv[1:] == ["--check"]:
        with tempfile.TemporaryDirectory() as tmp:
            make(tmp)
            stale = [n for n in sorted(os.listdir(tmp))
                     if open(os.path.join(tmp, n), "rb").read() != open(os.path.join(HERE, n), "rb").read()]
        if stale:
            sys.exit("out of date: " + " ".join(stale) + " (run make-fonts.py)")
        print("fonts up to date")
    elif not sys.argv[1:]:
        make(HERE)
    else:
        sys.exit("usage: make-fonts.py [--check]")
