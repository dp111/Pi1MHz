#!/usr/bin/env python3
"""Build a minimal old-map ADFS ("Hugo") disc image for a Pi1MHz VFS
directory - the scsi0.dat that VFS mounts and boots (*OPT 4,3 style).

The format is the classic Acorn ADFS old map: sectors 0/1 = free-space
map, sectors 2-6 = the 1280-byte root directory, files contiguous from
sector 7.  Only what VFS actually reads is generated; the image is
truncated after the last file sector (the .cfg/.dsc supply geometry).

Usage:
  make_vfs_menu.py out.dat NAME=path[,load,exec] ...
e.g.
  make_vfs_menu.py scsi0.dat '!BOOT=boot.txt' 'MENU=menu.txt'

Verified byte-identical (bar sequence numbers) against the hand-built
menu image this replaces.
"""
import sys

SECTOR = 256
DISC_SECTORS = 0x1080          # nominal 1 MB disc, as the original image
DIR_START = 2
DIR_SECTORS = 5
FILE_START = 7


def checksum(data255):
    """Acorn ADFS map checksum: add bytes 254..0 with carry, seeded 255."""
    a = 255
    for b in reversed(data255):
        if a > 255:
            a = (a & 255) + 1
        a += b
    return a & 255


def build(files, disc_sectors=DISC_SECTORS, boot_opt=3, seq=0x10):
    """files: list of (name, data, load, exec) tuples, in directory order
    (ADFS requires the directory sorted case-insensitively)."""
    if len(files) > 47:
        raise ValueError('old-map ADFS root holds at most 47 entries')
    for name, *_ in files:
        if any(ord(c) > 0x7F or ord(c) < 0x20 for c in name):
            raise ValueError('non-ASCII name would alias attribute bits: %r' % name)
    # --- lay files out contiguously from FILE_START
    layout = []
    sec = FILE_START
    for name, data, load, exe in files:
        nsec = max(1, (len(data) + SECTOR - 1) // SECTOR)
        layout.append((name, data, load, exe, sec, nsec))
        sec += nsec
    end = sec

    img = bytearray(end * SECTOR)

    # --- free space map: one entry, everything after the files
    s0 = img[0:SECTOR]
    s0[0:3] = (end).to_bytes(3, 'little')                 # FreeStart[0]
    s0[0xFC:0xFF] = disc_sectors.to_bytes(3, 'little')    # disc size
    s0[0xFF] = checksum(s0[0:255])
    img[0:SECTOR] = s0

    s1 = bytearray(SECTOR)
    s1[0:3] = (disc_sectors - end).to_bytes(3, 'little')  # FreeLen[0]
    s1[0xFB] = 0                                          # disc id
    s1[0xFC] = 0
    s1[0xFD] = boot_opt
    s1[0xFE] = 3                                          # free list end (1 entry * 3)
    s1[0xFF] = checksum(s1[0:255])
    img[SECTOR:2 * SECTOR] = s1

    # --- root directory ("Hugo", 1280 bytes at sector 2)
    d = bytearray(DIR_SECTORS * SECTOR)
    d[0] = seq
    d[1:5] = b'Hugo'
    p = 5
    for name, data, load, exe, start, nsec in layout:
        if len(name) > 10:
            raise ValueError('name too long: ' + name)
        nm = bytearray((name + '\r').ljust(10, '\0').encode('latin1')[:10])
        nm[0] |= 0x80                                     # R attribute
        nm[1] |= 0x80                                     # W attribute
        d[p:p + 10] = nm
        d[p + 10:p + 14] = load.to_bytes(4, 'little')
        d[p + 14:p + 18] = exe.to_bytes(4, 'little')
        d[p + 18:p + 22] = len(data).to_bytes(4, 'little')
        d[p + 22:p + 25] = start.to_bytes(3, 'little')
        d[p + 25] = seq
        p += 26
    # directory tail: root is named "$"
    d[0x4CC:0x4CC + 2] = b'$\r'                           # dir name
    d[0x4D9:0x4D9 + 2] = b'$\r'                           # dir title
    d[0x4FA] = seq
    d[0x4FB:0x4FF] = b'Hugo'
    # Old-map ADFS: the root is its own parent (*DIR ^ / *BACK from root
    # otherwise read sector 0 as a directory -> "Broken directory")
    d[0x4D6:0x4D9] = DIR_START.to_bytes(3, 'little')
    img[DIR_START * SECTOR:(DIR_START + DIR_SECTORS) * SECTOR] = d

    # --- file data
    for name, data, load, exe, start, nsec in layout:
        img[start * SECTOR:start * SECTOR + len(data)] = data

    return bytes(img)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out = sys.argv[1]
    files = []
    for spec in sys.argv[2:]:
        name, rest = spec.split('=', 1)
        parts = rest.split(',')
        path = parts[0]
        load = int(parts[1], 16) if len(parts) > 1 else 0
        exe = int(parts[2], 16) if len(parts) > 2 else load
        with open(path, 'rb') as f:
            files.append((name, f.read(), load, exe))
    files.sort(key=lambda t: t[0].upper())                # ADFS dir order
    img = build(files)
    with open(out, 'wb') as f:
        f.write(img)
    print('%s: %d bytes, %d files' % (out, len(img), len(files)))


if __name__ == '__main__':
    main()
