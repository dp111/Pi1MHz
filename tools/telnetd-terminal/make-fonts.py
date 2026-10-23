#!/usr/bin/env python3
# make-fonts.py - makes the BBC Micro fonts for TelnetD sessions (the *.psf console fonts and the *.ttf fonts) from the
# framebuffer's own character sets, so the glyphs exist in one place only:
#   src/framebuffer/fonts/bbc.fnt.h      the MOS character set (MODE 0-6)
#   src/framebuffer/fonts/saa5050.fnt.h  the teletext chip's character set and its mosaic graphics (MODE 7)
# The teletext font is smoothed by character rounding exactly as the framebuffer does it (src/framebuffer/fonts.c).
# Only the MOS fonts' few characters that the MOS has no glyph for (MOS_EXTRA) are drawn here.
#   python3 make-fonts.py           writes the eight fonts next to this script (needs python3-fonttools for the .ttf)
#   python3 make-fonts.py --check   fails if the committed fonts differ from what it would write
import os, re, struct, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FONTS = os.path.join(HERE, "..", "..", "src", "framebuffer", "fonts")
VERSION = "1.3"


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


def mos_font():
    slots = {c: (bbc_glyph(c), [c]) for c in range(0x20, 0x7F)}
    slots[0x60] = (bbc_glyph(0x60), [0x60, 0xA3])                # the MOS shows its pound for `
    extra = [0xBC, 0xBD, 0xBE, 0xF7, 0x2015, 0x2016, 0x2190, 0x2191, 0x2192, 0x25A0, 0x2588, 0x258C, 0x2590]
    for i, cp in enumerate(extra):
        slots[0x80 + i] = (bbc_glyph(0x7F) if cp == 0x2588 else drawn(MOS_EXTRA[cp]), [cp])
    return slots


def ttf_only():
    return {sextant(v): mosaic(v) for v in range(1, 63) if v not in (21, 42)}


# name, console font cell (w, h), dots of a glyph's row and column, TrueType advance (units of 1024 per em)
FONT_SET = [
    ("BBCMicroTeletext", "BBC Micro Teletext", teletext_font, 12, 20, 1, 1, 864),
    ("BBCMicroMOS", "BBC Micro MOS", mos_font, 8, 16, 1, 2, 512),
    ("BBCMicroMOS40", "BBC Micro MOS 40", mos_font, 16, 16, 2, 2, 1024),
    ("BBCMicroMOS20", "BBC Micro MOS 20", mos_font, 32, 16, 4, 2, 2048),
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


def ttf(path, family, slots, advance):
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    from fontTools.ttLib.tables.O_S_2f_2 import Panose
    from fontTools.misc.timeTools import timestampFromString

    chars = {cp: g for g, cps in slots.values() for cp in cps}
    chars.update(ttf_only())
    ps = family.replace(" ", "")
    order = [".notdef"] + ["uni%04X" % cp if cp < 0x10000 else "u%05X" % cp for cp in sorted(chars)]
    glyphs, metrics = {}, {}
    for name, cp in zip(order, [None] + sorted(chars)):
        pen = TTGlyphPen(None)
        if cp is not None:
            gw, gh, g = chars[cp]
            # dot (x, y) of a gw x gh glyph fills its share of the cell, from the ascent (960) down to the descent (-64);
            # a run of dots in a row, carried on down the rows that repeat it, is one rectangle, drawn clockwise
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
    fb.setupOS2(sTypoAscender=960, sTypoDescender=-64, sTypoLineGap=0, usWinAscent=960, usWinDescent=64,
                xAvgCharWidth=advance, fsType=0, panose=panose)
    fb.setupPost(isFixedPitch=1)
    stamp = timestampFromString("Fri Oct 23 00:00:00 2026")       # fixed, so the same tables give the same file
    fb.updateHead(created=stamp, modified=stamp)
    fb.save(path)


def make(where):
    for name, family, font, w, h, sx, sy, advance in FONT_SET:
        slots = font()
        open(os.path.join(where, name + ".psf"), "wb").write(psf(slots, w, h, sx, sy))
        ttf(os.path.join(where, name + ".ttf"), family, slots, advance)


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
