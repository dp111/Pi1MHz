#!/usr/bin/env python3
"""Build the Pi1MHz test-suite hard disc (a BeebSCSI jukebox set).

  build.py build [outdir]      tokenise bas/*.txt with basictool, write
                               outdir/scsi0.dat (default: out/)
  build.py root <scsi0.dat>    list the ADFS root directory of an image
  build.py verify-wrote <dat>  check the file TDISC leaves behind (WROTE:
                               16 KB, word I EOR &5A3C0F1E) reached the image
                               and that SEQ was deleted; exit 1 on mismatch
  build.py dfs <image.ssd>     list a DFS catalogue (the MMFS cross-check)

Each test program gets bas/common.txt appended (the PASS/FAIL printer and
the CHAIN list), is tokenised with basictool (the tool of record, ROM-exact)
and round-trip checked.  The image is an old-map ADFS disc built with
tools/make_vfs_menu.py, padded to 2 MB so ADFS can write to it; the
firmware works the geometry out from the file size (no .dsc/.cfg needed).
See README.md for how run.sh uses it.
"""
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
import make_vfs_menu as adfs  # noqa: E402

BASICTOOL = (os.environ.get('BASICTOOL') or shutil.which('basictool')
             or '/mnt/c/Archlinux/basictool/basictool')
TESTS = ['TINFO', 'TFRED', 'TRAM', 'TDISC', 'TVDU', 'TNET', 'TM5000', 'TTTX', 'TAUN', 'TBURST', 'TFAT', 'TFATJ']
PROGS = ['RUNALL'] + TESTS
DISC_SECTORS = 2 * 128 * 33          # 8448 sectors = 2 MB, a whole number of 33-sector tracks
TITLE = 'PI1MHZTEST'
BASIC_LOAD, BASIC_EXEC = 0xFFFF0E00, 0xFFFF8023
WROTE_LEN, WROTE_KEY = 16384, 0x5A3C0F1E


def read_text(name):
    with open(os.path.join(HERE, 'bas', name), encoding='latin1') as f:
        return f.read().replace('\r\n', '\n').replace('\r', '\n')


def program_source(name):
    """Test source plus the common footer, one program listing."""
    return read_text(name + '.txt').rstrip('\n') + '\n' + read_text('common.txt')


def tokenise(name, workdir):
    src = os.path.join(workdir, name + '.txt')
    tok = os.path.join(workdir, name + '.tok')
    text = program_source(name)
    with open(src, 'w', encoding='latin1', newline='\n') as f:
        f.write(text)
    subprocess.run([BASICTOOL, '-t', src, tok], check=True)
    back = subprocess.run([BASICTOOL, '--input-tokenised', '-a', tok, '-'],
                          check=True, capture_output=True).stdout.decode('latin1')
    norm = lambda s: [l.strip() for l in s.replace('\r', '\n').split('\n') if l.strip()]
    if norm(back) != norm(text):
        for a, b in zip(norm(back), norm(text)):
            if a != b:
                sys.exit('%s: round-trip mismatch:\n  got  %s\n  want %s' % (name, a, b))
        sys.exit('%s: round-trip mismatch (line count)' % name)
    with open(tok, 'rb') as f:
        return f.read()



def build(outdir):
    os.makedirs(outdir, exist_ok=True)
    files = [('!BOOT', read_text('BOOT.txt').replace('\n', '\r').encode('latin1'), 0, 0)]
    for name in PROGS:
        files.append((name, tokenise(name, outdir), BASIC_LOAD, BASIC_EXEC))
    files.sort(key=lambda t: t[0].upper())
    img = adfs.build(files, disc_sectors=DISC_SECTORS, boot_opt=3, title=TITLE)
    img = img.ljust(DISC_SECTORS * adfs.SECTOR, b'\0')
    dat = os.path.join(outdir, 'scsi0.dat')
    with open(dat, 'wb') as f:
        f.write(img)
    print('%s: %d bytes, %d files: %s' % (dat, len(img), len(files),
                                         ' '.join(n for n, *_ in files)))


def parse_root(img):
    """Old-map root directory -> list of dicts (name, load, exec, length, start)."""
    d = img[2 * adfs.SECTOR:7 * adfs.SECTOR]
    if d[1:5] != b'Hugo':
        raise ValueError('no Hugo root directory')
    out, p = [], 5
    while p + 26 <= 0x4CB and d[p]:
        e = d[p:p + 26]
        name = bytes(c & 0x7F for c in e[:10]).split(b'\r')[0].split(b'\0')[0].decode('latin1')
        out.append(dict(name=name,
                        load=int.from_bytes(e[10:14], 'little'),
                        exec=int.from_bytes(e[14:18], 'little'),
                        length=int.from_bytes(e[18:22], 'little'),
                        start=int.from_bytes(e[22:25], 'little')))
        p += 26
    title = bytes(d[0x4D9:0x4D9 + 19]).split(b'\r')[0].decode('latin1')
    return title, out


def verify_wrote(path):
    with open(path, 'rb') as f:
        img = f.read()
    title, entries = parse_root(img)
    names = {e['name'].upper(): e for e in entries}
    ok = True
    if 'SEQ' in names:
        print('FAIL: SEQ still in the directory (TDISC *DELETE did not reach the image)')
        ok = False
    w = names.get('WROTE')
    if not w:
        print('FAIL: WROTE not in the directory')
        return False
    if w['length'] != WROTE_LEN:
        print('FAIL: WROTE length %d, expected %d' % (w['length'], WROTE_LEN))
        ok = False
    data = img[w['start'] * adfs.SECTOR:w['start'] * adfs.SECTOR + WROTE_LEN]
    expect = b''.join(struct.pack('<I', i ^ WROTE_KEY) for i in range(0, WROTE_LEN, 4))
    bad = sum(1 for a, b in zip(data, expect) if a != b)
    if bad:
        print('FAIL: WROTE content: %d of %d bytes differ' % (bad, WROTE_LEN))
        ok = False
    print('%s: WROTE at sector %d, %d bytes, %s' % (path, w['start'], w['length'],
                                                   'content OK' if ok else 'MISMATCH'))
    return ok


def main(argv):
    if len(argv) < 2 or argv[1] not in ('build', 'root', 'verify-wrote', 'dfs'):
        sys.exit(__doc__)
    if argv[1] == 'dfs':
        sys.path.insert(0, os.path.join(HERE, '..', '..', 'beeb', 'net'))
        import mkssd
        with open(argv[2], 'rb') as f:
            title, boot, files = mkssd.parse(f.read())
        print('title %r boot %d' % (title, boot))
        for name, start, length in files:
            print('%-9s %6d @%d' % (name, length, start))
        return
    if argv[1] == 'build':
        build(argv[2] if len(argv) > 2 else os.path.join(HERE, 'out'))
    elif argv[1] == 'root':
        with open(argv[2], 'rb') as f:
            title, entries = parse_root(f.read())
        print('title %r' % title)
        for e in entries:
            print('%-10s %08X %08X %7d @%d' % (e['name'], e['load'], e['exec'], e['length'], e['start']))
    else:
        sys.exit(0 if verify_wrote(argv[2]) else 1)


if __name__ == '__main__':
    main(sys.argv)
