#!/usr/bin/env python3
# make-fonts-telnetd.py - makes TelnetD's own set of the four BBC Micro TrueType fonts, in telnetd-fonts/: an
# alternative to the ones make-fonts.py makes, with the same file and family names, so either set installs the same
# way (the kit's installers and profiles name the families, not the files).  make-fonts.py, its fonts and the .psf
# console fonts are not touched; this set has no .psf of its own (the console fonts are make-fonts.py's).
#
# What this set has that make-fonts.py's has not, all for TelnetD's line, shell and font modes (telnetd_proto.c):
#   - the separated sextants U+1CE51-1CE8F (Unicode 16) for MODE 7's separated graphics, in all four fonts;
#   - the quadrant blocks U+2580/2584/2596-259F for characters a program defines (VDU 23), in all four fonts;
#   - U+2592, the medium shade TelnetD shows a graphics colour code as, in all four fonts;
#   - [ \ ] ^ _ ` { | } ~ in the teletext font (a window in it shows the other MODEs too);
#   - in BBC Micro MOS only, the private-use pieces of telnetd_font=bbc (every MODE in its own shape in one window):
#     U+E100 MODE 2/5 quarters, U+E300 MODE 7 halves (normal, top and bottom of double height), U+E900 MODE 1/4/6
#     halves, U+EA00 VDU 23 characters as 2 x 4 blocks.
# Where the shapes differ from make-fonts.py's:
#   - the teletext characters are a real BBC Micro's, measured from its screen (HDMI capture), 5 x 9 dots rounded
#     as the chip does it, in a cell of 9:10 (advance 922); make-fonts.py rounds the SAA5050 table (fonts.c), 864;
#   - the mosaics have the chip's proportions as measured (rows of 6, 8 and 6 of 20 lines; separated blocks without
#     their left column and bottom row);
#   - a glyph's rows are drawn as runs that overlap the lit row below by 8 units, so a terminal drawing the font
#     anti-aliased at a fractional size shows no hairline seam between rows.
#
# Every glyph is made here from data or a rule, never from an existing font:
#   - data, with where each table comes from: MOS_ROM (MEASURED: the MOS 1.20 ROM's character set; --check proves it
#     is the same as src/framebuffer/fonts/bbc.fnt.h), MOS_DRAWN (INFERRED: drawn in the MOS style), TTX_MEASURED
#     (MEASURED on a real Beeb), TTX_DRAWN (INFERRED: drawn in the chip's style);
#   - rules: the teletext rounding, the sextants and separated sextants, the quadrants, the VDU 23 blocks, the shade
#     and the font-mode pieces, which are cut from the base glyphs.
#
#   python3 make-fonts-telnetd.py           writes the four fonts into telnetd-fonts/ (needs python3-fonttools)
#   python3 make-fonts-telnetd.py --check   fails if the committed fonts differ from what it would write, or if
#                                           MOS_ROM differs from bbc.fnt.h
import os, re, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "telnetd-fonts")
FONTS = os.path.join(HERE, "..", "..", "src", "framebuffer", "fonts")
VERSION = "1.7"
STAMP = 3875679823          # head created/modified, fixed (the 1.6 set's), so the same tables give the same file
UPM, ASC, DESC = 1024, 960, -64                    # the cell: 960 above the baseline, 64 below; 1024 high

# ---- data -------------------------------------------------------------------------------------------------------
# MOS glyphs, 8 x 8, '#' a pixel, rows top to bottom.
# MOS_ROM: MEASURED - the BBC Micro's own character set, MOS 1.20 at &C000, characters 32-126 (96 is the pound sign).
# MOS_DRAWN: INFERRED - MODE 7's own characters, which the MOS has no glyph for, drawn in the MOS style (TelnetD sends
# them for a MODE 7 screen shown in a MOS font).
# Teletext glyphs, 5 x 9 dots, rows 1-9 of the chip's 6 x 10 cell (a blank column on the left, a blank row at the
# top; missing rows are blank), keyed by the code point TelnetD sends for the character.
# TTX_MEASURED: MEASURED - every MODE 7 character measured from a real BBC Micro's screen (HDMI capture), each checked
# by rounding it again here, which gives back the picture the Beeb drew.
# TTX_DRAWN: INFERRED - the ASCII characters the chip shows its own symbols for, drawn in the chip's style (TelnetD
# sends the chip's own ones in MODE 7; these are for other MODEs shown in a teletext window).
MOS_ROM = {
    0x20: "........ ........ ........ ........ ........ ........ ........ ........",   # ' '
    0x21: "...##... ...##... ...##... ...##... ...##... ........ ...##... ........",   # '!'
    0x22: ".##.##.. .##.##.. .##.##.. ........ ........ ........ ........ ........",   # '"'
    0x23: "..##.##. ..##.##. .####### ..##.##. .####### ..##.##. ..##.##. ........",   # '#'
    0x24: "....##.. ..###### .##.#... ..#####. ....#.## .######. ...##... ........",   # '$'
    0x25: ".##..... .##..##. ....##.. ...##... ..##.... .##..##. .....##. ........",   # '%'
    0x26: "..###... .##.##.. .##.##.. ..###... .##.##.# .##..##. ..###.## ........",   # '&'
    0x27: "....##.. ...##... ..##.... ........ ........ ........ ........ ........",   # "'"
    0x28: "....##.. ...##... ..##.... ..##.... ..##.... ...##... ....##.. ........",   # '('
    0x29: "..##.... ...##... ....##.. ....##.. ....##.. ...##... ..##.... ........",   # ')'
    0x2A: "........ ...##... .######. ..####.. .######. ...##... ........ ........",   # '*'
    0x2B: "........ ...##... ...##... .######. ...##... ...##... ........ ........",   # '+'
    0x2C: "........ ........ ........ ........ ........ ...##... ...##... ..##....",   # ','
    0x2D: "........ ........ ........ .######. ........ ........ ........ ........",   # '-'
    0x2E: "........ ........ ........ ........ ........ ...##... ...##... ........",   # '.'
    0x2F: "........ .....##. ....##.. ...##... ..##.... .##..... ........ ........",   # '/'
    0x30: "..####.. .##..##. .##.###. .######. .###.##. .##..##. ..####.. ........",   # '0'
    0x31: "...##... ..###... ...##... ...##... ...##... ...##... .######. ........",   # '1'
    0x32: "..####.. .##..##. .....##. ....##.. ...##... ..##.... .######. ........",   # '2'
    0x33: "..####.. .##..##. .....##. ...###.. .....##. .##..##. ..####.. ........",   # '3'
    0x34: "....##.. ...###.. ..####.. .##.##.. .######. ....##.. ....##.. ........",   # '4'
    0x35: ".######. .##..... .#####.. .....##. .....##. .##..##. ..####.. ........",   # '5'
    0x36: "...###.. ..##.... .##..... .#####.. .##..##. .##..##. ..####.. ........",   # '6'
    0x37: ".######. .....##. ....##.. ...##... ..##.... ..##.... ..##.... ........",   # '7'
    0x38: "..####.. .##..##. .##..##. ..####.. .##..##. .##..##. ..####.. ........",   # '8'
    0x39: "..####.. .##..##. .##..##. ..#####. .....##. ....##.. ..###... ........",   # '9'
    0x3A: "........ ........ ...##... ...##... ........ ...##... ...##... ........",   # ':'
    0x3B: "........ ........ ...##... ...##... ........ ...##... ...##... ..##....",   # ';'
    0x3C: "....##.. ...##... ..##.... .##..... ..##.... ...##... ....##.. ........",   # '<'
    0x3D: "........ ........ .######. ........ .######. ........ ........ ........",   # '='
    0x3E: "..##.... ...##... ....##.. .....##. ....##.. ...##... ..##.... ........",   # '>'
    0x3F: "..####.. .##..##. ....##.. ...##... ...##... ........ ...##... ........",   # '?'
    0x40: "..####.. .##..##. .##.###. .##.#.#. .##.###. .##..... ..####.. ........",   # '@'
    0x41: "..####.. .##..##. .##..##. .######. .##..##. .##..##. .##..##. ........",   # 'A'
    0x42: ".#####.. .##..##. .##..##. .#####.. .##..##. .##..##. .#####.. ........",   # 'B'
    0x43: "..####.. .##..##. .##..... .##..... .##..... .##..##. ..####.. ........",   # 'C'
    0x44: ".####... .##.##.. .##..##. .##..##. .##..##. .##.##.. .####... ........",   # 'D'
    0x45: ".######. .##..... .##..... .#####.. .##..... .##..... .######. ........",   # 'E'
    0x46: ".######. .##..... .##..... .#####.. .##..... .##..... .##..... ........",   # 'F'
    0x47: "..####.. .##..##. .##..... .##.###. .##..##. .##..##. ..####.. ........",   # 'G'
    0x48: ".##..##. .##..##. .##..##. .######. .##..##. .##..##. .##..##. ........",   # 'H'
    0x49: ".######. ...##... ...##... ...##... ...##... ...##... .######. ........",   # 'I'
    0x4A: "..#####. ....##.. ....##.. ....##.. ....##.. .##.##.. ..###... ........",   # 'J'
    0x4B: ".##..##. .##.##.. .####... .###.... .####... .##.##.. .##..##. ........",   # 'K'
    0x4C: ".##..... .##..... .##..... .##..... .##..... .##..... .######. ........",   # 'L'
    0x4D: ".##...## .###.### .####### .##.#.## .##.#.## .##...## .##...## ........",   # 'M'
    0x4E: ".##..##. .##..##. .###.##. .######. .##.###. .##..##. .##..##. ........",   # 'N'
    0x4F: "..####.. .##..##. .##..##. .##..##. .##..##. .##..##. ..####.. ........",   # 'O'
    0x50: ".#####.. .##..##. .##..##. .#####.. .##..... .##..... .##..... ........",   # 'P'
    0x51: "..####.. .##..##. .##..##. .##..##. .##.#.#. .##.##.. ..##.##. ........",   # 'Q'
    0x52: ".#####.. .##..##. .##..##. .#####.. .##.##.. .##..##. .##..##. ........",   # 'R'
    0x53: "..####.. .##..##. .##..... ..####.. .....##. .##..##. ..####.. ........",   # 'S'
    0x54: ".######. ...##... ...##... ...##... ...##... ...##... ...##... ........",   # 'T'
    0x55: ".##..##. .##..##. .##..##. .##..##. .##..##. .##..##. ..####.. ........",   # 'U'
    0x56: ".##..##. .##..##. .##..##. .##..##. .##..##. ..####.. ...##... ........",   # 'V'
    0x57: ".##...## .##...## .##.#.## .##.#.## .####### .###.### .##...## ........",   # 'W'
    0x58: ".##..##. .##..##. ..####.. ...##... ..####.. .##..##. .##..##. ........",   # 'X'
    0x59: ".##..##. .##..##. .##..##. ..####.. ...##... ...##... ...##... ........",   # 'Y'
    0x5A: ".######. .....##. ....##.. ...##... ..##.... .##..... .######. ........",   # 'Z'
    0x5B: ".#####.. .##..... .##..... .##..... .##..... .##..... .#####.. ........",   # '['
    0x5C: "........ .##..... ..##.... ...##... ....##.. .....##. ........ ........",   # '\\'
    0x5D: "..#####. .....##. .....##. .....##. .....##. .....##. ..#####. ........",   # ']'
    0x5E: "...##... ..####.. .##..##. .#....#. ........ ........ ........ ........",   # '^'
    0x5F: "........ ........ ........ ........ ........ ........ ........ ########",   # '_'
    0x60: "...###.. ..##.##. ..##.... .#####.. ..##.... ..##.... .######. ........",   # the pound sign (character 96)
    0x61: "........ ........ ..####.. .....##. ..#####. .##..##. ..#####. ........",   # 'a'
    0x62: ".##..... .##..... .#####.. .##..##. .##..##. .##..##. .#####.. ........",   # 'b'
    0x63: "........ ........ ..####.. .##..##. .##..... .##..##. ..####.. ........",   # 'c'
    0x64: ".....##. .....##. ..#####. .##..##. .##..##. .##..##. ..#####. ........",   # 'd'
    0x65: "........ ........ ..####.. .##..##. .######. .##..... ..####.. ........",   # 'e'
    0x66: "...###.. ..##.... ..##.... .#####.. ..##.... ..##.... ..##.... ........",   # 'f'
    0x67: "........ ........ ..#####. .##..##. .##..##. ..#####. .....##. ..####..",   # 'g'
    0x68: ".##..... .##..... .#####.. .##..##. .##..##. .##..##. .##..##. ........",   # 'h'
    0x69: "...##... ........ ..###... ...##... ...##... ...##... ..####.. ........",   # 'i'
    0x6A: "...##... ........ ..###... ...##... ...##... ...##... ...##... .###....",   # 'j'
    0x6B: ".##..... .##..... .##..##. .##.##.. .####... .##.##.. .##..##. ........",   # 'k'
    0x6C: "..###... ...##... ...##... ...##... ...##... ...##... ..####.. ........",   # 'l'
    0x6D: "........ ........ ..##.##. .####### .##.#.## .##.#.## .##...## ........",   # 'm'
    0x6E: "........ ........ .#####.. .##..##. .##..##. .##..##. .##..##. ........",   # 'n'
    0x6F: "........ ........ ..####.. .##..##. .##..##. .##..##. ..####.. ........",   # 'o'
    0x70: "........ ........ .#####.. .##..##. .##..##. .#####.. .##..... .##.....",   # 'p'
    0x71: "........ ........ ..#####. .##..##. .##..##. ..#####. .....##. .....###",   # 'q'
    0x72: "........ ........ .##.##.. .###.##. .##..... .##..... .##..... ........",   # 'r'
    0x73: "........ ........ ..#####. .##..... ..####.. .....##. .#####.. ........",   # 's'
    0x74: "..##.... ..##.... .#####.. ..##.... ..##.... ..##.... ...###.. ........",   # 't'
    0x75: "........ ........ .##..##. .##..##. .##..##. .##..##. ..#####. ........",   # 'u'
    0x76: "........ ........ .##..##. .##..##. .##..##. ..####.. ...##... ........",   # 'v'
    0x77: "........ ........ .##...## .##.#.## .##.#.## .####### ..##.##. ........",   # 'w'
    0x78: "........ ........ .##..##. ..####.. ...##... ..####.. .##..##. ........",   # 'x'
    0x79: "........ ........ .##..##. .##..##. .##..##. ..#####. .....##. ..####..",   # 'y'
    0x7A: "........ ........ .######. ....##.. ...##... ..##.... .######. ........",   # 'z'
    0x7B: "....##.. ...##... ...##... .###.... ...##... ...##... ....##.. ........",   # '{'
    0x7C: "...##... ...##... ...##... ........ ...##... ...##... ...##... ........",   # '|'
    0x7D: "..##.... ...##... ...##... ....###. ...##... ...##... ..##.... ........",   # '}'
    0x7E: "..##...# .##.#.## .#...##. ........ ........ ........ ........ ........",   # '~'
}
MOS_DRAWN = {
    0x2190: "........ ...#.... ..##.... .####### ..##.... ...#.... ........ ........",   # left arrow
    0x2192: "........ ....#... ....##.. #######. ....##.. ....#... ........ ........",   # right arrow
    0x2191: "...#.... ..###... .#.#.#.. ...#.... ...#.... ...#.... ...#.... ........",   # up arrow
    0x2015: "........ ........ ........ ######## ........ ........ ........ ........",   # long dash
    0x2016: ".##..##. .##..##. .##..##. .##..##. .##..##. .##..##. .##..##. ........",   # double bar
    0x00BD: ".#...... ##...... .#...#.. .#..#... ...#.##. ..#....# .#....#. .....###",   # one half
    0x00BC: ".#...... ##...... .#...#.. .#..#... ...#..#. ..#..##. .#..#### ......#.",   # one quarter
    0x00BE: "###..... ..#..... .##..#.. ..#.#... ##.#..#. ..#..##. .#..#### ......#.",   # three quarters
    0x00F7: "........ ...##... ........ .######. ........ ...##... ........ ........",   # divide
    0x25A0: "........ .######. .######. .######. .######. .######. .######. ........",   # block
}
TTX_MEASURED = {
    0x0021: "..#.. ..#.. ..#.. ..#.. ..#.. ..... ..#..",   # !
    0x0022: ".#.#. .#.#. .#.#.",   # "
    0x0023: ".#.#. .#.#. ##### .#.#. ##### .#.#. .#.#.",   # #
    0x0024: ".###. #.#.# #.#.. .###. ..#.# #.#.# .###.",   # $
    0x0025: "##... ##..# ...#. ..#.. .#... #..## ...##",   # %
    0x0026: ".#... #.#.. #.#.. .#... #.#.# #..#. .##.#",   # &
    0x0027: "..#.. ..#.. ..#..",   # '
    0x0028: "...#. ..#.. .#... .#... .#... ..#.. ...#.",   # (
    0x0029: ".#... ..#.. ...#. ...#. ...#. ..#.. .#...",   # )
    0x002A: "..#.. #.#.# .###. ..#.. .###. #.#.# ..#..",   # *
    0x002B: "..... ..#.. ..#.. ##### ..#.. ..#..",   # +
    0x002C: "..... ..... ..... ..... ..... ..#.. ..#.. .#...",   # ,
    0x002D: "..... ..... ..... .###.",   # -
    0x002E: "..... ..... ..... ..... ..... ..... ..#..",   # .
    0x002F: "..... ....# ...#. ..#.. .#... #....",   # /
    0x0030: "..#.. .#.#. #...# #...# #...# .#.#. ..#..",   # 0
    0x0031: "..#.. .##.. ..#.. ..#.. ..#.. ..#.. .###.",   # 1
    0x0032: ".###. #...# ....# ..##. .#... #.... #####",   # 2
    0x0033: "##### ....# ...#. ..##. ....# #...# .###.",   # 3
    0x0034: "...#. ..##. .#.#. #..#. ##### ...#. ...#.",   # 4
    0x0035: "##### #.... ####. ....# ....# #...# .###.",   # 5
    0x0036: "..##. .#... #.... ####. #...# #...# .###.",   # 6
    0x0037: "##### ....# ...#. ..#.. .#... .#... .#...",   # 7
    0x0038: ".###. #...# #...# .###. #...# #...# .###.",   # 8
    0x0039: ".###. #...# #...# .#### ....# ...#. .##..",   # 9
    0x003A: "..... ..... ..#.. ..... ..... ..... ..#..",   # :
    0x003B: "..... ..... ..#.. ..... ..... ..#.. ..#.. .#...",   # ;
    0x003C: "...#. ..#.. .#... #.... .#... ..#.. ...#.",   # <
    0x003D: "..... ..... ##### ..... #####",   # =
    0x003E: ".#... ..#.. ...#. ....# ...#. ..#.. .#...",   # >
    0x003F: ".###. #...# ...#. ..#.. ..#.. ..... ..#..",   # ?
    0x0040: ".###. #...# #.### #.#.# #.### #.... .###.",   # @
    0x0041: "..#.. .#.#. #...# #...# ##### #...# #...#",   # A
    0x0042: "####. #...# #...# ####. #...# #...# ####.",   # B
    0x0043: ".###. #...# #.... #.... #.... #...# .###.",   # C
    0x0044: "####. #...# #...# #...# #...# #...# ####.",   # D
    0x0045: "##### #.... #.... ####. #.... #.... #####",   # E
    0x0046: "##### #.... #.... ####. #.... #.... #....",   # F
    0x0047: ".###. #...# #.... #.... #..## #...# .####",   # G
    0x0048: "#...# #...# #...# ##### #...# #...# #...#",   # H
    0x0049: ".###. ..#.. ..#.. ..#.. ..#.. ..#.. .###.",   # I
    0x004A: "....# ....# ....# ....# ....# #...# .###.",   # J
    0x004B: "#...# #..#. #.#.. ##... #.#.. #..#. #...#",   # K
    0x004C: "#.... #.... #.... #.... #.... #.... #####",   # L
    0x004D: "#...# ##.## #.#.# #.#.# #...# #...# #...#",   # M
    0x004E: "#...# #...# ##..# #.#.# #..## #...# #...#",   # N
    0x004F: ".###. #...# #...# #...# #...# #...# .###.",   # O
    0x0050: "####. #...# #...# ####. #.... #.... #....",   # P
    0x0051: ".###. #...# #...# #...# #.#.# #..#. .##.#",   # Q
    0x0052: "####. #...# #...# ####. #.#.. #..#. #...#",   # R
    0x0053: ".###. #...# #.... .###. ....# #...# .###.",   # S
    0x0054: "##### ..#.. ..#.. ..#.. ..#.. ..#.. ..#..",   # T
    0x0055: "#...# #...# #...# #...# #...# #...# .###.",   # U
    0x0056: "#...# #...# #...# .#.#. .#.#. ..#.. ..#..",   # V
    0x0057: "#...# #...# #...# #.#.# #.#.# #.#.# .#.#.",   # W
    0x0058: "#...# #...# .#.#. ..#.. .#.#. #...# #...#",   # X
    0x0059: "#...# #...# .#.#. ..#.. ..#.. ..#.. ..#..",   # Y
    0x005A: "##### ....# ...#. ..#.. .#... #.... #####",   # Z
    0x2190: "..... ..#.. .#... ##### .#... ..#..",   # ←
    0x00BD: "#.... #.... #.... #.... #.##. ....# ...#. ..#.. ..###",   # ½
    0x2192: "..... ..#.. ...#. ##### ...#. ..#..",   # →
    0x2191: "..... ..#.. .###. #.#.# ..#.. ..#..",   # ↑
    0x2015: "..... ..... ..... #####",   # ―
    0x00A3: "..##. .#..# .#... ###.. .#... .#... #####",   # £
    0x0061: "..... ..... .###. ....# .#### #...# .####",   # a
    0x0062: "#.... #.... ####. #...# #...# #...# ####.",   # b
    0x0063: "..... ..... .#### #.... #.... #.... .####",   # c
    0x0064: "....# ....# .#### #...# #...# #...# .####",   # d
    0x0065: "..... ..... .###. #...# ##### #.... .###.",   # e
    0x0066: "...#. ..#.. ..#.. .###. ..#.. ..#.. ..#..",   # f
    0x0067: "..... ..... .#### #...# #...# #...# .#### ....# .###.",   # g
    0x0068: "#.... #.... ####. #...# #...# #...# #...#",   # h
    0x0069: "..#.. ..... .##.. ..#.. ..#.. ..#.. .###.",   # i
    0x006A: "..#.. ..... ..#.. ..#.. ..#.. ..#.. ..#.. ..#.. .#...",   # j
    0x006B: ".#... .#... .#..# .#.#. .##.. .#.#. .#..#",   # k
    0x006C: ".##.. ..#.. ..#.. ..#.. ..#.. ..#.. .###.",   # l
    0x006D: "..... ..... ##.#. #.#.# #.#.# #.#.# #.#.#",   # m
    0x006E: "..... ..... ####. #...# #...# #...# #...#",   # n
    0x006F: "..... ..... .###. #...# #...# #...# .###.",   # o
    0x0070: "..... ..... ####. #...# #...# #...# ####. #.... #....",   # p
    0x0071: "..... ..... .#### #...# #...# #...# .#### ....# ....#",   # q
    0x0072: "..... ..... .#.## .##.. .#... .#... .#...",   # r
    0x0073: "..... ..... .#### #.... .###. ....# ####.",   # s
    0x0074: "..#.. ..#.. .###. ..#.. ..#.. ..#.. ...#.",   # t
    0x0075: "..... ..... #...# #...# #...# #...# .####",   # u
    0x0076: "..... ..... #...# #...# .#.#. .#.#. ..#..",   # v
    0x0077: "..... ..... #...# #...# #.#.# #.#.# .#.#.",   # w
    0x0078: "..... ..... #...# .#.#. ..#.. .#.#. #...#",   # x
    0x0079: "..... ..... #...# #...# #...# #...# .#### ....# .###.",   # y
    0x007A: "..... ..... ##### ...#. ..#.. .#... #####",   # z
    0x00BC: ".#... .#... .#... .#... .#..# ...## ..#.# ..### ....#",   # ¼
    0x2016: ".#.#. .#.#. .#.#. .#.#. .#.#. .#.#. .#.#.",   # ‖
    0x00BE: "##... ..#.. ##... ..#.. ##..# ...## ..#.# ..### ....#",   # ¾
    0x00F7: "..... ..#.. ..... ##### ..... ..#..",   # ÷
    0x25A0: "##### ##### ##### ##### ##### ##### #####",   # ■ (measured on its own, VDU 255)
}
TTX_DRAWN = {
    0x005B: ".###. .#... .#... .#... .#... .#... .###.",   # [
    0x005C: "#.... .#... .#... ..#.. ...#. ...#. ....#",   # \
    0x005D: ".###. ...#. ...#. ...#. ...#. ...#. .###.",   # ]
    0x005E: "..#.. .#.#. #...#",   # ^
    0x005F: "..... ..... ..... ..... ..... ..... ..... #####",   # _
    0x0060: ".#... ..#..",   # `
    0x007B: "...## ..#.. ..#.. .#... ..#.. ..#.. ...##",   # {
    0x007C: "..#.. ..#.. ..#.. ..#.. ..#.. ..#.. ..#..",   # |
    0x007D: "##... ..#.. ..#.. ...#. ..#.. ..#.. ##...",   # }
    0x007E: "..... ..... .#... #.#.# ...#.",   # ~
}

# ---- rules ------------------------------------------------------------------------------------------------------
def mos_rows(art):
    return [int(r.replace("#", "1").replace(".", "0"), 2) for r in art.split()]


MOS = {c: mos_rows(a) for c, a in MOS_ROM.items()}
MOS[0xA3] = MOS[0x60]                              # the Beeb's pound sign (character 96)
MOS.update({cp: mos_rows(a) for cp, a in MOS_DRAWN.items()})


def ttx_dots(art):
    """The 6 x 10 cell: 10 rows of 6 bits (bit 5 the blank left column)."""
    out = [0] * 10
    for r, s in enumerate(art.split()):
        out[r + 1] = sum(1 << (4 - i) for i, c in enumerate(s) if c == "#")
    return out


def rounded(d):
    """Character rounding as the chip does it: each dot two pixels wide and two lines high, and where two dots touch
    only at a corner a half dot fills each of the two corners between them: 6 x 10 dots -> 12 x 20 pixels (rows as
    12-bit ints, bit 11 the left)."""
    out = [0] * 20
    for r in range(10):
        for c in range(6):
            if d[r] & (0x20 >> c):
                out[2 * r] |= 3 << (10 - 2 * c)
                out[2 * r + 1] |= 3 << (10 - 2 * c)
    for r in range(9):
        a, b = d[r], d[r + 1]
        for c in range(5):
            a0, a1 = a & (0x20 >> c), a & (0x20 >> (c + 1))
            b0, b1 = b & (0x20 >> c), b & (0x20 >> (c + 1))
            if a0 and b1 and not a1 and not b0:          # a diagonal down to the right
                out[2 * r + 1] |= 1 << (9 - 2 * c)
                out[2 * r + 2] |= 1 << (10 - 2 * c)
            if a1 and b0 and not a0 and not b1:          # a diagonal down to the left
                out[2 * r + 1] |= 1 << (10 - 2 * c)
                out[2 * r + 2] |= 1 << (9 - 2 * c)
    return out


TTX = {0x20: rounded([0] * 10)}
TTX.update({cp: rounded(ttx_dots(a)) for cp, a in TTX_MEASURED.items()})
TTX.update({cp: rounded(ttx_dots(a)) for cp, a in TTX_DRAWN.items()})


def sextant_cells():
    """Code point -> the six cells set (bit 0 top left, 1 top right, 2 middle left, ... 5 bottom right), as TelnetD
    sends a contiguous mosaic (telnetd_proto.c ttx_mosaic); the separated ones are U+1CE50 + cells."""
    out = {0x2588: 63, 0x258C: 21, 0x2590: 42}
    for v in range(1, 63):
        if v not in (21, 42):
            out[0x1FB00 + (v - 1) - (v > 21) - (v > 42)] = v
    return out


MOSAIC = sextant_cells()

# The chip's mosaic cells, MEASURED on a real Beeb's screen (12 half dots x 20 lines): two columns of 6 half dots,
# three rows of 6, 8 and 6 lines (3, 4 and 3 lines a field); separated, each block loses its left column of dots
# and its bottom row: half dots 2-5 and 8-11, lines 0-3, 6-11 and 14-17.  All 128 cells checked on the Beeb.
MOSAIC_COLS = ((0, 6), (6, 12))
MOSAIC_ROWS = ((0, 6), (6, 14), (14, 20))
SEP_COLS = ((2, 6), (8, 12))
SEP_ROWS = ((0, 4), (6, 12), (14, 18))


def mosaic_rows(v, sep):
    """A mosaic's 12 x 20 picture."""
    cols, rws = (SEP_COLS, SEP_ROWS) if sep else (MOSAIC_COLS, MOSAIC_ROWS)
    out = [0] * 20
    for i in range(6):
        if v & (1 << i):
            (c0, c1), (r0, r1) = cols[i % 2], rws[i // 2]
            for y in range(r0, r1):
                for x in range(c0, c1):
                    out[y] |= 0x800 >> x
    return out


# Characters a program defines (VDU 23) without the BBC font: TelnetD sends 2 x 2 blocks, Unicode's quadrants (bit 0
# top left, 1 top right, 2 bottom left, 3 bottom right; telnetd_proto.c put_udg).  The half and full blocks are the
# mosaics' (MOSAIC).
QUADRANTS = {0x2598: 1, 0x259D: 2, 0x2580: 3, 0x2596: 4, 0x259E: 6, 0x259B: 7, 0x2597: 8, 0x259A: 9,
             0x259C: 11, 0x2584: 12, 0x2599: 13, 0x259F: 14}


def quadrant_pieces():
    return {cp: ([(v & 1) << 1 | (v >> 1) & 1, (v >> 2 & 1) << 1 | (v >> 3) & 1], 2, 2) for cp, v in QUADRANTS.items()}


def shade(w, h):
    """U+2592, the medium shade (a graphics colour code's marker): every other pixel of a w x h glyph."""
    return ([int(("10" * w)[y % 2:][:w], 2) for y in range(h)], w, h)


# ---- the pieces for telnetd_font=bbc (BBC Micro MOS only) --------------------------------------------------------
# TelnetD draws a Beeb character as 80 / (the MODE's columns) terminal cells of this font, so each MODE keeps its own
# shape: a MOS character's halves (MODE 1, 4, 6) at U+E900 + 2c, its quarters (MODE 2, 5) at U+E100 + 4c; a teletext
# cell g (32-127 the chip's characters by their screen codes, 128 + n a contiguous mosaic, 192 + n a separated one) in
# halves at U+E300 + 2 (256 v + g), v 0 normal height, 1 the top and 2 the bottom of double height; a VDU 23
# character as 2 x 4 blocks at U+EA00 + v, bit 0 the top left block ... 7 the bottom right (telnetd_proto.c,
# PUA_MOS2 / PUA_MOS4 / PUA_TTX / PUA_UDG).  Nothing at U+E000-E0FF: terminals draw the Powerline symbols there.
PUA_MOS2, PUA_MOS4, PUA_TTX, PUA_UDG = 0xE900, 0xE100, 0xE300, 0xEA00
SCREEN_TTX = {0x23: "£", 0x5B: "←", 0x5C: "½", 0x5D: "→", 0x5E: "↑", 0x5F: "#",
              0x60: "―", 0x7B: "¼", 0x7C: "‖", 0x7D: "¾", 0x7E: "÷", 0x7F: "■"}


def ttx_cell_rows(g):
    """Teletext cell g as 12 x 20."""
    if g >= 192:
        return mosaic_rows(g - 192, True)
    if g >= 128:
        return mosaic_rows(g - 128, False)
    if g < 32:
        return [0] * 20
    return TTX.get(ord(SCREEN_TTX.get(g, chr(g))), [0] * 20)


def slice_cols(rows, width, first, n):
    """Columns first .. first + n - 1 of rows (width bits, the left the top bit) as n-bit rows."""
    return [(r >> (width - first - n)) & ((1 << n) - 1) for r in rows]


def pua_pieces():
    """{code point: (rows, w, h)} for every piece."""
    out = {PUA_UDG + v: ([((v >> (2 * r)) & 1) << 1 | ((v >> (2 * r + 1)) & 1) for r in range(4)], 2, 4)
           for v in range(256)}
    for c in range(128):
        rows = MOS.get(c, [0] * 8)
        for h in range(2):
            out[PUA_MOS2 + 2 * c + h] = (slice_cols(rows, 8, 4 * h, 4), 4, 8)
        for q in range(4):
            out[PUA_MOS4 + 4 * c + q] = (slice_cols(rows, 8, 2 * q, 2), 2, 8)
    for g in range(256):
        full = ttx_cell_rows(g)
        for v, rows in ((0, full), (1, [full[y // 2] for y in range(20)]), (2, [full[10 + y // 2] for y in range(20)])):
            for h in range(2):
                out[PUA_TTX + 2 * (256 * v + g) + h] = (slice_cols(rows, 12, 6 * h, 6), 6, 20)
    return out


# ---- the fonts ----------------------------------------------------------------------------------------------------
# name, family, advance (font units: the cell's width, 1024 high), base glyphs, their grid (w, h), font-mode pieces
FONT_SET = [
    ("BBCMicroMOS", "BBC Micro MOS", 512, MOS, (8, 8), True),            # MODE 0 and 3: cells 1:2
    ("BBCMicroMOS40", "BBC Micro MOS 40", 1024, MOS, (8, 8), False),     # MODE 1, 4 and 6: cells 1:1
    ("BBCMicroMOS20", "BBC Micro MOS 20", 2048, MOS, (8, 8), False),     # MODE 2 and 5: cells 2:1
    ("BBCMicroTeletext", "BBC Micro Teletext", 922, TTX, (12, 20), False),  # MODE 7: cells 9:10, as measured
]


def glyph_from_bitmap(rows, adv, gw, gh):
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    pen = TTGlyphPen(None)
    top = 1 << (gw - 1)
    for r, bits in enumerate(rows):
        y1 = round(ASC - r * (ASC - DESC) / gh)
        y0 = round(ASC - (r + 1) * (ASC - DESC) / gh)
        c = 0
        while c < gw:
            if bits & (top >> c):
                s = c
                while c < gw and bits & (top >> c):
                    c += 1
                x0, x1 = round(s * adv / gw), round(c * adv / gw)
                run = sum(top >> k for k in range(s, c))
                # overlap the row below where it is lit too: rows that only meet leave a hairline seam when a
                # terminal draws the font anti-aliased at a fractional size
                y0b = y0 - 8 if r + 1 < len(rows) and rows[r + 1] & run else y0
                pen.moveTo((x0, y0b)); pen.lineTo((x0, y1)); pen.lineTo((x1, y1)); pen.lineTo((x1, y0b)); pen.closePath()
            else:
                c += 1
    return pen.glyph()


def glyph_from_sextant(v, adv, sep=False):
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    pen = TTGlyphPen(None)
    cols, rows = (SEP_COLS, SEP_ROWS) if sep else (MOSAIC_COLS, MOSAIC_ROWS)
    for i in range(6):
        if v & (1 << i):
            (c0, c1), (r0, r1) = cols[i % 2], rows[i // 2]
            x0, x1 = round(c0 * adv / 12), round(c1 * adv / 12)
            y1 = round(ASC - r0 * (ASC - DESC) / 20)
            y0 = round(ASC - r1 * (ASC - DESC) / 20)
            pen.moveTo((x0, y0)); pen.lineTo((x0, y1)); pen.lineTo((x1, y1)); pen.lineTo((x1, y0)); pen.closePath()
    return pen.glyph()


def make_ttf(path, fname, family, adv, base, grid, pieces, version=VERSION, with_shade=True):
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    from fontTools.ttLib.tables.O_S_2f_2 import Panose
    gw, gh = grid
    rule_made = quadrant_pieces()
    if with_shade:
        rule_made[0x2592] = shade(gw, gh)
    if pieces:
        rule_made.update(pua_pieces())
    order, cmap, glyphs = [".notdef"], {}, {".notdef": TTGlyphPen(None).glyph()}
    for cp, (rows, pw, ph) in sorted(rule_made.items()):
        n = "u%04X" % cp
        order.append(n); cmap[cp] = n; glyphs[n] = glyph_from_bitmap(rows, adv, pw, ph)
    for cp in sorted(base):
        n = "space" if cp == 32 else "u%04X" % cp
        order.append(n); cmap[cp] = n; glyphs[n] = glyph_from_bitmap(base[cp], adv, gw, gh)
    for cp in sorted(MOSAIC):
        n = "u%05X" % cp
        order.append(n); cmap[cp] = n; glyphs[n] = glyph_from_sextant(MOSAIC[cp], adv)
    for v in range(1, 64):                                        # separated: Unicode 16, U+1CE51-U+1CE8F
        cp = 0x1CE50 + v
        n = "u%05X" % cp
        order.append(n); cmap[cp] = n; glyphs[n] = glyph_from_sextant(v, adv, sep=True)
    fb = FontBuilder(UPM, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap(cmap)
    fb.setupGlyf(glyphs)
    # each glyph's left side bearing is its own left edge: renderers place the outline by it
    glyf = fb.font["glyf"]
    lsb = {}
    for n in order:
        g = glyf[n]
        g.recalcBounds(glyf)
        lsb[n] = g.xMin if g.numberOfContours else 0
    fb.setupHorizontalMetrics({n: (adv, lsb[n]) for n in order})
    fb.setupHorizontalHeader(ascent=ASC, descent=DESC)
    fb.setupNameTable({"familyName": family, "styleName": "Regular", "psName": fname + "-Regular",
                       "fullName": family + " Regular", "uniqueFontIdentifier": "%s-%s" % (fname, version),
                       "version": "Version " + version})
    # Latin 1 in the code page range and the Regular bit: without them Windows' GDI will not match the font for a
    # program that asks for the ANSI character set (PuTTY does) and draws Courier New instead
    fb.setupOS2(sTypoAscender=ASC, sTypoDescender=DESC, sTypoLineGap=0, usWinAscent=ASC, usWinDescent=-DESC,
                xAvgCharWidth=adv, fsSelection=0x40, ulCodePageRange1=1, fsType=0)
    pn = Panose()
    for k in ("bFamilyType", "bSerifStyle", "bWeight", "bProportion", "bContrast", "bStrokeVariation", "bArmStyle",
              "bLetterForm", "bMidline", "bXHeight"):
        setattr(pn, k, 0)
    pn.bFamilyType, pn.bProportion = 2, 9                         # monospaced
    fb.font["OS/2"].panose = pn
    fb.setupPost(isFixedPitch=1)
    fb.updateHead(created=STAMP, modified=STAMP)
    fb.save(path)
    return len(order)


def make(where, version=VERSION, with_shade=True):
    os.makedirs(where, exist_ok=True)
    for fname, family, adv, base, grid, pieces in FONT_SET:
        make_ttf(os.path.join(where, fname + ".ttf"), fname, family, adv, base, grid, pieces, version, with_shade)


def mos_table_matches_framebuffer():
    """MOS_ROM against the framebuffer's MOS character set (bbc.fnt.h, make-fonts.py's source): the characters that
    differ (none expected)."""
    def text(fn):
        t = open(os.path.join(FONTS, fn)).read()
        return re.sub(r'#include\s+"([^"]+)"', lambda m: text(m.group(1)), t)
    t = re.sub(r"//[^\n]*|/\*.*?\*/", "", text("bbc.fnt.h"), flags=re.S)
    data = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)", t[t.index("{") + 1:t.rindex("}")])]
    return [c for c, a in MOS_ROM.items() if mos_rows(a) != data[c * 8:c * 8 + 8]]


if __name__ == "__main__":
    if sys.argv[1:] == ["--check"]:
        differ = mos_table_matches_framebuffer()
        with tempfile.TemporaryDirectory() as tmp:
            make(tmp)
            stale = [n for n in sorted(os.listdir(tmp)) if not os.path.exists(os.path.join(OUT, n)) or
                     open(os.path.join(tmp, n), "rb").read() != open(os.path.join(OUT, n), "rb").read()]
        if differ:
            sys.exit("MOS_ROM differs from bbc.fnt.h: " + " ".join("%d" % c for c in differ))
        if stale:
            sys.exit("out of date: " + " ".join(stale) + " (run make-fonts-telnetd.py)")
        print("fonts up to date; MOS_ROM matches bbc.fnt.h")
    elif not sys.argv[1:]:
        make(OUT)
    else:
        sys.exit("usage: make-fonts-telnetd.py [--check]")
