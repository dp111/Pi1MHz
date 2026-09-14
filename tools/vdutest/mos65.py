#!/usr/bin/env python3
"""A small 65C12 interpreter that runs the Master MOS 3.20 graphics code.

An oracle for the circle family: it executes the ROM's own circle, arc, chord
and sector routines on a memory image, with the row fill (&DAE8), the point
plot and the line routine hooked, so thousands of shapes can be checked in
seconds.  Nothing here goes into the firmware; the ROM images are read from
the beebjit tree at run time.  See docs/dev/vdu-rom-conformance.md.
"""
import os
import sys

ROMDIR = os.path.join(os.environ.get("BEEBJIT", "/mnt/c/Archlinux/beebjit"), "roms/mos3.20")

class CPU:
    def __init__(self):
        self.mem = bytearray(0x10000)
        term = open(f"{ROMDIR}/terminal.rom", "rb").read()
        mos = open(f"{ROMDIR}/mos.rom", "rb").read()
        self.mem[0x8000:0xC000] = term
        self.mem[0xC000:0x10000] = mos
        for a in range(0x8000, 0x9000):       # ANDY private RAM paged over the ROM
            self.mem[a] = 0
        self.a = self.x = self.y = 0
        self.s = 0xFF
        self.c = self.z = self.i = self.d = self.v = self.n = 0
        self.pc = 0
        self.hooks = {}
        self.steps = 0

    # ---- memory ----
    def rd(self, a):
        return self.mem[a & 0xFFFF]

    def wr(self, a, v):
        a &= 0xFFFF
        if a >= 0x9000:
            raise RuntimeError(f"write to ROM {a:04X} at pc {self.pc:04X}")
        self.mem[a] = v & 0xFF

    def rd16(self, a):
        return self.rd(a) | self.rd(a + 1) << 8

    def push(self, v):
        self.mem[0x100 + self.s] = v & 0xFF
        self.s = (self.s - 1) & 0xFF

    def pull(self):
        self.s = (self.s + 1) & 0xFF
        return self.mem[0x100 + self.s]

    def getp(self):
        return (self.n << 7) | (self.v << 6) | 0x30 | (self.d << 3) | (self.i << 2) | (self.z << 1) | self.c

    def setp(self, p):
        self.n = p >> 7 & 1; self.v = p >> 6 & 1; self.d = p >> 3 & 1
        self.i = p >> 2 & 1; self.z = p >> 1 & 1; self.c = p & 1

    def nz(self, v):
        v &= 0xFF
        self.n = v >> 7
        self.z = int(v == 0)
        return v

    # ---- addressing ----
    def fetch(self):
        v = self.mem[self.pc]
        self.pc = (self.pc + 1) & 0xFFFF
        return v

    def fetch16(self):
        lo = self.fetch()
        return lo | self.fetch() << 8

    def ea(self, mode):
        if mode == 'zp':  return self.fetch()
        if mode == 'zpx': return (self.fetch() + self.x) & 0xFF
        if mode == 'zpy': return (self.fetch() + self.y) & 0xFF
        if mode == 'abs': return self.fetch16()
        if mode == 'abx': return (self.fetch16() + self.x) & 0xFFFF
        if mode == 'aby': return (self.fetch16() + self.y) & 0xFFFF
        if mode == 'izx':
            z = (self.fetch() + self.x) & 0xFF
            return self.rd(z) | self.rd((z + 1) & 0xFF) << 8
        if mode == 'izy':
            z = self.fetch()
            return ((self.rd(z) | self.rd((z + 1) & 0xFF) << 8) + self.y) & 0xFFFF
        if mode == 'izp':
            z = self.fetch()
            return self.rd(z) | self.rd((z + 1) & 0xFF) << 8
        raise ValueError(mode)

    # ---- arithmetic ----
    def adc(self, v):
        if self.d:
            raise RuntimeError("decimal mode")
        r = self.a + v + self.c
        self.v = int(((self.a ^ r) & (v ^ r) & 0x80) != 0)
        self.c = int(r > 0xFF)
        self.a = self.nz(r)

    def sbc(self, v):
        self.adc(v ^ 0xFF)

    def cmp(self, reg, v):
        r = reg - v
        self.c = int(r >= 0)
        self.nz(r)

    def branch(self, cond):
        off = self.fetch()
        if cond:
            self.pc = (self.pc + (off - 256 if off & 0x80 else off)) & 0xFFFF

    def call(self, addr, sentinel=0xFFF0):
        """JSR addr, run until it returns to the sentinel."""
        self.push((sentinel - 1) >> 8); self.push((sentinel - 1) & 0xFF)
        self.pc = addr
        while self.pc != sentinel:
            self.step()
            self.steps += 1
            if self.steps > getattr(self, 'max_steps', 50_000_000):
                raise RuntimeError("runaway")

    def rts(self):
        lo = self.pull(); hi = self.pull()
        self.pc = ((hi << 8 | lo) + 1) & 0xFFFF

    def step(self):
        h = self.hooks.get(self.pc)
        if h is not None:
            h(self)
            self.rts()
            return
        op = self.fetch()
        f = OPS.get(op)
        if f is None:
            raise RuntimeError(f"unimplemented opcode {op:02X} at {self.pc-1:04X}")
        f(self)


OPS = {}

def load_group(base_codes, name, fn):
    for code, mode in base_codes:
        def mk(mode=mode):
            if mode == 'imm':
                return lambda c: fn(c, c.fetch())
            return lambda c: fn(c, c.rd(c.ea(mode)))
        OPS[code] = mk()

G1 = lambda b: [(b + 0x09, 'imm'), (b + 0x05, 'zp'), (b + 0x15, 'zpx'), (b + 0x0D, 'abs'), (b + 0x1D, 'abx'), (b + 0x19, 'aby'), (b + 0x01, 'izx'), (b + 0x11, 'izy'), (b + 0x12, 'izp')]

def _ora(c, v): c.a = c.nz(c.a | v)
def _and(c, v): c.a = c.nz(c.a & v)
def _eor(c, v): c.a = c.nz(c.a ^ v)
def _adc(c, v): c.adc(v)
def _lda(c, v): c.a = c.nz(v)
def _cmp(c, v): c.cmp(c.a, v)
def _sbc(c, v): c.sbc(v)
load_group(G1(0x00), 'ora', _ora)
load_group(G1(0x20), 'and', _and)
load_group(G1(0x40), 'eor', _eor)
load_group(G1(0x60), 'adc', _adc)
load_group(G1(0xA0), 'lda', _lda)
load_group(G1(0xC0), 'cmp', _cmp)
load_group(G1(0xE0), 'sbc', _sbc)

for code, mode in [(0x85, 'zp'), (0x95, 'zpx'), (0x8D, 'abs'), (0x9D, 'abx'), (0x99, 'aby'), (0x81, 'izx'), (0x91, 'izy'), (0x92, 'izp')]:
    OPS[code] = (lambda mode: lambda c: c.wr(c.ea(mode), c.a))(mode)
for code, mode in [(0x86, 'zp'), (0x96, 'zpy'), (0x8E, 'abs')]:
    OPS[code] = (lambda mode: lambda c: c.wr(c.ea(mode), c.x))(mode)
for code, mode in [(0x84, 'zp'), (0x94, 'zpx'), (0x8C, 'abs')]:
    OPS[code] = (lambda mode: lambda c: c.wr(c.ea(mode), c.y))(mode)
for code, mode in [(0x64, 'zp'), (0x74, 'zpx'), (0x9C, 'abs'), (0x9E, 'abx')]:
    OPS[code] = (lambda mode: lambda c: c.wr(c.ea(mode), 0))(mode)

def _ldx(c, v): c.x = c.nz(v)
def _ldy(c, v): c.y = c.nz(v)
load_group([(0xA2, 'imm'), (0xA6, 'zp'), (0xB6, 'zpy'), (0xAE, 'abs'), (0xBE, 'aby')], 'ldx', _ldx)
load_group([(0xA0, 'imm'), (0xA4, 'zp'), (0xB4, 'zpx'), (0xAC, 'abs'), (0xBC, 'abx')], 'ldy', _ldy)
load_group([(0xE0, 'imm'), (0xE4, 'zp'), (0xEC, 'abs')], 'cpx', lambda c, v: c.cmp(c.x, v))
load_group([(0xC0, 'imm'), (0xC4, 'zp'), (0xCC, 'abs')], 'cpy', lambda c, v: c.cmp(c.y, v))

def _bit(c, v):
    c.z = int((c.a & v) == 0); c.n = v >> 7 & 1; c.v = v >> 6 & 1
load_group([(0x24, 'zp'), (0x2C, 'abs'), (0x34, 'zpx'), (0x3C, 'abx')], 'bit', _bit)
OPS[0x89] = lambda c: setattr(c, 'z', int((c.a & c.fetch()) == 0))   # BIT #imm: Z only

def rmw(codes, fn):
    for code, mode in codes:
        if mode == 'acc':
            OPS[code] = (lambda: lambda c: setattr(c, 'a', fn(c, c.a)))()
        else:
            def mk(mode=mode):
                def f(c):
                    a = c.ea(mode)
                    c.wr(a, fn(c, c.rd(a)))
                return f
            OPS[code] = mk()

def _asl(c, v): c.c = v >> 7; return c.nz(v << 1)
def _lsr(c, v): c.c = v & 1; return c.nz(v >> 1)
def _rol(c, v): r = (v << 1) | c.c; c.c = v >> 7; return c.nz(r)
def _ror(c, v): r = (v >> 1) | (c.c << 7); c.c = v & 1; return c.nz(r)
def _inc(c, v): return c.nz(v + 1)
def _dec(c, v): return c.nz(v - 1)
RMW = lambda b: [(b + 0x0A, 'acc'), (b + 0x06, 'zp'), (b + 0x16, 'zpx'), (b + 0x0E, 'abs'), (b + 0x1E, 'abx')]
rmw(RMW(0x00), _asl); rmw(RMW(0x40), _lsr); rmw(RMW(0x20), _rol); rmw(RMW(0x60), _ror)
rmw([(0xE6, 'zp'), (0xF6, 'zpx'), (0xEE, 'abs'), (0xFE, 'abx'), (0x1A, 'acc')], _inc)
rmw([(0xC6, 'zp'), (0xD6, 'zpx'), (0xCE, 'abs'), (0xDE, 'abx'), (0x3A, 'acc')], _dec)

def _tsb(c, v): c.z = int((c.a & v) == 0); return (v | c.a) & 0xFF
def _trb(c, v): c.z = int((c.a & v) == 0); return (v & ~c.a) & 0xFF
def tsbtrb(codes, fn):
    for code, mode in codes:
        def mk(mode=mode):
            def f(c):
                a = c.ea(mode)
                c.wr(a, fn(c, c.rd(a)))
            return f
        OPS[code] = mk()
tsbtrb([(0x04, 'zp'), (0x0C, 'abs')], _tsb)
tsbtrb([(0x14, 'zp'), (0x1C, 'abs')], _trb)

OPS[0xE8] = lambda c: setattr(c, 'x', c.nz(c.x + 1))
OPS[0xCA] = lambda c: setattr(c, 'x', c.nz(c.x - 1))
OPS[0xC8] = lambda c: setattr(c, 'y', c.nz(c.y + 1))
OPS[0x88] = lambda c: setattr(c, 'y', c.nz(c.y - 1))
OPS[0xAA] = lambda c: setattr(c, 'x', c.nz(c.a))
OPS[0x8A] = lambda c: setattr(c, 'a', c.nz(c.x))
OPS[0xA8] = lambda c: setattr(c, 'y', c.nz(c.a))
OPS[0x98] = lambda c: setattr(c, 'a', c.nz(c.y))
OPS[0xBA] = lambda c: setattr(c, 'x', c.nz(c.s))
OPS[0x9A] = lambda c: setattr(c, 's', c.x)
OPS[0x48] = lambda c: c.push(c.a)
OPS[0x68] = lambda c: setattr(c, 'a', c.nz(c.pull()))
OPS[0xDA] = lambda c: c.push(c.x)
OPS[0xFA] = lambda c: setattr(c, 'x', c.nz(c.pull()))
OPS[0x5A] = lambda c: c.push(c.y)
OPS[0x7A] = lambda c: setattr(c, 'y', c.nz(c.pull()))
OPS[0x08] = lambda c: c.push(c.getp())
OPS[0x28] = lambda c: c.setp(c.pull())
OPS[0x18] = lambda c: setattr(c, 'c', 0)
OPS[0x38] = lambda c: setattr(c, 'c', 1)
OPS[0x58] = lambda c: setattr(c, 'i', 0)
OPS[0x78] = lambda c: setattr(c, 'i', 1)
OPS[0xB8] = lambda c: setattr(c, 'v', 0)
OPS[0xD8] = lambda c: setattr(c, 'd', 0)
OPS[0xF8] = lambda c: setattr(c, 'd', 1)
OPS[0xEA] = lambda c: None
OPS[0x10] = lambda c: c.branch(not c.n)
OPS[0x30] = lambda c: c.branch(c.n)
OPS[0x50] = lambda c: c.branch(not c.v)
OPS[0x70] = lambda c: c.branch(c.v)
OPS[0x90] = lambda c: c.branch(not c.c)
OPS[0xB0] = lambda c: c.branch(c.c)
OPS[0xD0] = lambda c: c.branch(not c.z)
OPS[0xF0] = lambda c: c.branch(c.z)
OPS[0x80] = lambda c: c.branch(True)
def _jsr(c):
    t = c.fetch16()
    ret = (c.pc - 1) & 0xFFFF
    c.push(ret >> 8); c.push(ret & 0xFF)
    c.pc = t
OPS[0x20] = _jsr
OPS[0x4C] = lambda c: setattr(c, 'pc', c.fetch16())
OPS[0x6C] = lambda c: setattr(c, 'pc', c.rd16(c.fetch16()))
OPS[0x7C] = lambda c: setattr(c, 'pc', c.rd16((c.fetch16() + c.x) & 0xFFFF))
OPS[0x60] = lambda c: c.rts()


# ---------------------------------------------------------------------------
# The oracle

def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v

class Unhandled(Exception):
    pass

def master_plot(kind, mode, centre, start, end, window=None, trace=None, max_steps=50_000_000):
    """Run the Master's PLOT &A0/&A8/&B0 code; return list of (y, x1, x2) rows and points."""
    c = CPU()
    rows, points = [], []
    w = window or (0, 0, 319 if mode in (1, 4) else (639 if mode in (0, 3) else 159), 255)
    def put16(a, v):
        c.mem[a] = v & 0xFF; c.mem[a + 1] = (v >> 8) & 0xFF
    put16(0x300, w[0]); put16(0x302, w[1]); put16(0x304, w[2]); put16(0x306, w[3])
    put16(0x314, centre[0]); put16(0x316, centre[1])
    put16(0x324, start[0]); put16(0x326, start[1])
    put16(0x320, end[0]); put16(0x322, end[1])
    c.mem[0x355] = mode
    def fill(cpu):
        X, Y = cpu.x, cpu.y
        yy = s16(cpu.rd16(0x302 + X)); x1 = s16(cpu.rd16(0x300 + X)); x2 = s16(cpu.rd16(0x300 + Y))
        rows.append((yy, x1, x2))
    def plotpoint(cpu):
        X = cpu.x
        points.append((s16(cpu.rd16(0x300 + X)), s16(cpu.rd16(0x302 + X))))
    def unhandled(name):
        def h(cpu):
            raise Unhandled(name)
        return h
    c.hooks[0xDAE8] = fill
    # plotPointInternal: find its address from the arc code (L99DB: jmp plotPointInternal)
    c.hooks[c.rd16(0x99DC)] = plotpoint
    def line(cpu):
        points.append(('line', (s16(cpu.rd16(0x324)), s16(cpu.rd16(0x326))), (s16(cpu.rd16(0x320)), s16(cpu.rd16(0x322)))))
    c.hooks[0xD8A9] = line
    entry = {'arc': 0x9999, 'segment': 0x9935, 'sector': 0x9923}[kind]
    c.s = 0xEF
    c.max_steps = max_steps
    c.call(entry)
    return rows, points



def master_circle(fill, mode, centre, point, window=None, max_steps=50_000_000):
    """Run the Master's PLOT &90 (outline) or &98 (filled) circle."""
    c = CPU()
    rows, points = [], []
    w = window or (0, 0, 319 if mode in (1, 4) else (639 if mode in (0, 3) else 159), 255)
    def put16(a, v):
        c.mem[a] = v & 0xFF; c.mem[a + 1] = (v >> 8) & 0xFF
    put16(0x300, w[0]); put16(0x302, w[1]); put16(0x304, w[2]); put16(0x306, w[3])
    put16(0x324, centre[0]); put16(0x326, centre[1])
    put16(0x320, point[0]); put16(0x322, point[1])
    c.mem[0x355] = mode
    c.hooks[0xDAE8] = lambda k: rows.append((s16(k.rd16(0x302 + k.x)), s16(k.rd16(0x300 + k.x)), s16(k.rd16(0x300 + k.y))))
    c.hooks[c.rd16(0x99DC)] = lambda k: points.append((s16(k.rd16(0x300 + k.x)), s16(k.rd16(0x302 + k.x))))
    c.s = 0xEF
    c.max_steps = max_steps
    c.call(0x9944 if fill else 0x99A4)
    return rows, points


# ---------------------------------------------------------------------------
# The whole VDU driver: feed bytes to the Master's own OSWRCH VDU path and read
# the pixels back out of screen memory.
#
# The memory image is a snapshot taken under beebjit right after MODE m, at the
# VDU 25 handler (&C69B) of a MOVE: screen cleared, VDU queue empty, ANDY and
# the utilities ROM paged in over &8000-&BFFF and the MOS at &C000.  make_snapshots()
# rebuilds them.  Only the non-shadow graphics modes are supported.

SNAPDIR = os.path.join(__import__("tempfile").gettempdir(), "vdutest-snap")
OUTPUT_TO_VDU = 0xC027          # outputToVDU: A = character
SCREEN_BASE = {0: 0x3000, 1: 0x3000, 2: 0x3000, 4: 0x5800, 5: 0x5800}
BYTES_PER_ROW = {0: 80, 1: 80, 2: 80, 4: 40, 5: 40}
PIXELS_PER_BYTE = {0: 8, 1: 4, 2: 2, 4: 8, 5: 4}

def make_snapshots(modes=(0, 1, 2, 4, 5)):
    import subprocess, tempfile
    os.makedirs(SNAPDIR, exist_ok=True)
    bj = os.path.join(os.environ.get("BEEBJIT", "/mnt/c/Archlinux/beebjit"), "beebjit")
    tree = os.path.join(tempfile.gettempdir(), "vdutest-master")   # vdutest.master_tree()
    for m in modes:
        out = os.path.join(SNAPDIR, f"mode{m}.bin")
        prog = f"10 MODE {m}\r20 MOVE 0,0\r30 PRINT \"ENDOFTEST\"\rRUN\r".encode()
        subprocess.run([bj, "-master", "-headless", "-terminal", "-fast", "-debug",
                        "-cycles", "20000000000",
                        "-commands", f"b 0xc69b commands 'savemem {out} 0 ffff;db 0;c';c"],
                       input=prog, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       cwd=tree, timeout=300)

# beebjit's MOS 3.20 set: sideways slot -> image.  Slot 15 holds the MOS
# utilities (and the terminal); slot 14 (beebjit's "view.rom") carries the
# MOS extension - ellipses, move/copy - after VIEW.
SLOTS = {9: "dfs", 10: "viewsht", 11: "edit", 12: "basic4", 13: "adfs", 14: "view", 15: "terminal"}
ROMSEL = 0xFE30

class MasterVDU(CPU):
    def __init__(self, mode):
        super().__init__()
        path = os.path.join(SNAPDIR, f"mode{mode}.bin")
        if not os.path.exists(path):
            make_snapshots((mode,))
        snap = open(path, "rb").read()
        self.mem[:len(snap)] = snap
        self.mode = mode
        self.slots = {n: open(os.path.join(ROMDIR, f"{f}.rom"), "rb").read() for n, f in SLOTS.items()}
        self.andy = bytearray(snap[0x8000:0x9000])   # the snapshot was taken with ANDY paged in
        self.romsel = 0x8F
        self.mem[ROMSEL] = 0x8F

    def page(self, v):
        if self.romsel & 0x80:
            self.andy[:] = self.mem[0x8000:0x9000]
        self.romsel = v
        rom = self.slots.get(v & 0x0F, bytes(0x4000))
        self.mem[0x9000:0xC000] = rom[0x1000:0x4000]
        self.mem[0x8000:0x9000] = self.andy if v & 0x80 else rom[0:0x1000]

    def wr(self, a, v):
        a &= 0xFFFF
        v &= 0xFF
        if a < 0x8000 or 0xFC00 <= a < 0xFF00:     # RAM, and the I/O area as plain memory
            self.mem[a] = v
            if a == ROMSEL:
                self.page(v)
            return
        if a < 0x9000 and self.romsel & 0x80:      # ANDY
            self.mem[a] = v
            return
        raise RuntimeError(f"write to ROM {a:04X} at pc {self.pc:04X}")

    def vdu(self, data, max_steps=20_000_000):
        for b in data:
            self.a = b & 0xFF
            self.s = 0xE0
            self.steps = 0
            self.max_steps = max_steps
            self.call(OUTPUT_TO_VDU)

    def pixels(self):
        """{(x, y): logical colour} for every non-zero pixel, y up as POINT counts."""
        m = self.mode
        base, bpr, ppb = SCREEN_BASE[m], BYTES_PER_ROW[m], PIXELS_PER_BYTE[m]
        size = 0x8000 - base
        top = self.rd16(0x350)
        out = {}
        # BBC video forms the address from the 6845's character address times
        # 8 plus the raster line, so memory runs a character cell at a time:
        # each byte column of a character row holds 8 bytes, one per raster.
        for row in range(256):
            y = 255 - row
            for i in range(bpr):
                off = (((row >> 3) * bpr + i) << 3) | (row & 7)
                b = self.mem[base + ((top - base + off) % size)]
                if not b:
                    continue
                for p in range(ppb):
                    if ppb == 8:
                        c = (b >> (7 - p)) & 1
                    elif ppb == 4:
                        c = ((b >> (7 - p)) & 1) << 1 | ((b >> (3 - p)) & 1)
                    else:
                        c = ((b >> (7 - p)) & 1) << 3 | ((b >> (5 - p)) & 1) << 2 | ((b >> (3 - p)) & 1) << 1 | ((b >> (1 - p)) & 1)
                    if c:
                        out[(i * ppb + p, y)] = c
        return out
