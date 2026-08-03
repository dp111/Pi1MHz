#!/usr/bin/env python3
# Build an Acorn DFS single-sided .ssd disc image (200 KB, 80 track).
#
# Purpose: pack the Pi1MHz net test programs onto a disc so the BBC Master
# can *EXEC them straight off the SD card instead of typing them in over the
# serial link (which is slow and drops characters).  MMFS2 (Pi1MHz ROM slot 5)
# serves loose .ssd files by name - drop the image on the SD root, *DIN it,
# then *EXEC the program.  See docs/user/mmfs.md.
#
# Usage:  python3 mkssd.py            # rebuilds NET.ssd next to this script
#
# The DFS catalogue layout implemented here is the standard two-sector
# (512-byte) catalogue; files follow from sector 2, each sector-aligned.

import os
import sys

SECTOR = 256
TOTAL_SECTORS = 800            # single sided, 80 track = 204800 bytes
CAT_SECTORS = 2               # sectors 0 and 1 hold the catalogue
MAX_FILES = 31


def to_cr(text):
    """BBC text files use CR line endings; strip leading listing whitespace."""
    out = []
    for line in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        out.append(line.lstrip())
    return "\r".join(out).encode("latin1")


class File:
    def __init__(self, name, data, load=0, exec_=0, directory="$", locked=False):
        assert 1 <= len(name) <= 7, "DFS names are 1..7 chars: %r" % name
        self.name = name.upper()
        self.directory = directory
        self.locked = locked
        self.data = data
        self.load = load & 0xFFFFFF
        self.exec = exec_ & 0xFFFFFF


def build(title, files, boot_option=0):
    assert len(files) <= MAX_FILES
    img = bytearray(TOTAL_SECTORS * SECTOR)

    # --- disc title, split across the two catalogue sectors (8 + 4 chars) ---
    t = title.encode("latin1")[:12].ljust(12, b"\x00")
    img[0:8] = t[0:8]
    img[256:260] = t[8:12]

    # --- place files, sector-aligned, from sector 2 upward ---
    next_sector = CAT_SECTORS
    placed = []
    for f in files:
        nsec = (len(f.data) + SECTOR - 1) // SECTOR
        if next_sector + nsec > TOTAL_SECTORS:
            raise ValueError("disc full placing %s" % f.name)
        off = next_sector * SECTOR
        img[off:off + len(f.data)] = f.data
        placed.append((f, next_sector, len(f.data)))
        next_sector += nsec

    # --- catalogue: DFS lists files in DESCENDING start-sector order ---
    placed.sort(key=lambda p: p[1], reverse=True)

    img[261] = len(placed) * 8
    img[262] = ((boot_option & 3) << 4) | ((TOTAL_SECTORS >> 8) & 3)
    img[263] = TOTAL_SECTORS & 0xFF

    for i, (f, start, length) in enumerate(placed):
        n = f.name.ljust(7)[:7].encode("latin1")
        dchar = f.directory.encode("latin1")[0]
        if f.locked:
            dchar |= 0x80
        o = 8 + i * 8
        img[o:o + 7] = n
        img[o + 7] = dchar

        m = 256 + 8 + i * 8
        img[m + 0] = f.load & 0xFF
        img[m + 1] = (f.load >> 8) & 0xFF
        img[m + 2] = f.exec & 0xFF
        img[m + 3] = (f.exec >> 8) & 0xFF
        img[m + 4] = length & 0xFF
        img[m + 5] = (length >> 8) & 0xFF
        img[m + 6] = (((f.exec >> 16) & 3) << 6 |
                      ((length >> 16) & 3) << 4 |
                      ((f.load >> 16) & 3) << 2 |
                      ((start >> 8) & 3))
        img[m + 7] = start & 0xFF

    return bytes(img)


def parse(img):
    """Read a DFS catalogue back - used to self-check build()."""
    title = (img[0:8] + img[256:260]).decode("latin1").rstrip("\x00 ")
    nfiles = img[261] // 8
    boot = (img[262] >> 4) & 3
    out = []
    for i in range(nfiles):
        o = 8 + i * 8
        name = img[o:o + 7].decode("latin1").rstrip()
        dchar = img[o + 7]
        m = 256 + 8 + i * 8
        length = img[m + 4] | (img[m + 5] << 8) | (((img[m + 6] >> 4) & 3) << 16)
        start = img[m + 7] | (((img[m + 6]) & 3) << 8)
        out.append((chr(dchar & 0x7F) + "." + name, start, length))
    return title, boot, out


def main():
    here = os.path.dirname(os.path.abspath(__file__))

    def read(name):
        with open(os.path.join(here, name), "r", encoding="latin1") as fh:
            return fh.read()

    boot_txt = (
        "*BASIC\r"
        'PRINT"Pi1MHz net test disc"\r'
        'PRINT"*EXEC NETDEMO  - raw sockets"\r'
        'PRINT"*EXEC NETHTTP  - N: HTTP GET"\r'
        'PRINT"(set net_enable=1 in Pi1MHz.cfg)"\r'
    )

    files = [
        File("!BOOT", boot_txt.encode("latin1")),
        File("NETDEMO", to_cr(read("NETDEMO.BAS"))),
        File("NETHTTP", to_cr(read("NETHTTP.BAS"))),
    ]

    img = build("Pi1MHz NET", files, boot_option=3)   # *OPT4,3 = EXEC !BOOT

    # self-check: parse it back, confirm every file survived intact
    title, boot, cat = parse(img)
    names = {c[0].split(".", 1)[1] for c in cat}
    expected = {"!BOOT", "NETDEMO", "NETHTTP"}
    assert names == expected, "round-trip mismatch: %r" % names
    assert boot == 3, "boot option lost"

    # byte-exact check of each file's data region
    for f in files:
        entry = next(c for c in cat if c[0].endswith("." + f.name))
        _, start, length = entry
        assert length == len(f.data), "%s length %d != %d" % (
            f.name, length, len(f.data))
        got = img[start * SECTOR:start * SECTOR + length]
        assert got == f.data, "%s data mismatch" % f.name

    out = os.path.join(here, "NET.ssd")
    with open(out, "wb") as fh:
        fh.write(img)

    print("wrote %s (%d bytes)  title=%r boot=%d" %
          (out, len(img), title, boot))
    for name, start, length in sorted(cat):
        print("  %-12s sector %3d  %5d bytes" % (name, start, length))


if __name__ == "__main__":
    sys.exit(main())
