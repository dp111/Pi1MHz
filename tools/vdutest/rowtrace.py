#!/usr/bin/env python3
"""Record the Master's row fills for arc/chord/sector PLOTs.

A beebjit breakpoint on the MOS 3.20 fill-row routine (&DAE8) dumps the
registers and page 3 at every row fill.  X and Y index the two end points in
the VDU variables: the row is point X's y, from point X's x to point Y's x.

Three traps, each of which silently lost every row:
* the drawing must run with the VDU driver on (*FX3,0), results with it off;
* the Beeb's serial output is delayed by the emulated baud rate, so text
  markers never line up with the dumps: each case writes its index to &70;
* beebjit's own stdout is buffered: run it under stdbuf -o0.
An interrupt landing on the breakpointed instruction reports a row twice.

  rowtrace.py out.json            run the built-in random case set
Cases are (name, mode, plotcode, centre, start, end) in pixels of the mode.
"""
import json, os, random, re, subprocess, sys

BEEBJIT = os.path.join(os.environ.get("BEEBJIT", "/mnt/c/Archlinux/beebjit"), "beebjit")
TREE = os.path.join(__import__("tempfile").gettempdir(), "vdutest-master")   # built by vdutest.py
UNITS = {0: (2, 4), 1: (4, 4), 2: (8, 4), 4: (4, 4), 5: (8, 4)}

def run(cases):
    lines = ["NEW", "10 MODE 128", "20 *FX3,3", "25 PRINT"]
    n = 30
    cur = None
    for idx, (name, mode, code, c, s, e) in enumerate(cases):
        ux, uy = UNITS[mode]
        stmt = []
        if mode != cur:
            stmt.append(f"*FX3,0")
            lines.append(f"{n} *FX3,0"); n += 1
            lines.append(f"{n} VDU 22,{128+mode}"); n += 1
            lines.append(f"{n} *FX3,3"); n += 1
            cur = mode
        lines.append(f'{n} ?&70={(idx+1) & 255}:?&71={(idx+1) >> 8}:*FX3,0'); n += 1
        lines.append(f"{n} MOVE {c[0]*ux},{c[1]*uy}:MOVE {s[0]*ux},{s[1]*uy}:PLOT {code},{e[0]*ux},{e[1]*uy}"); n += 1
        lines.append(f"{n} *FX3,3"); n += 1
    lines.append(f'{n} PRINT "ENDOFTEST"'); n += 1
    lines.append("RUN")
    prog = "".join(l + "\r" for l in lines)
    p = subprocess.Popen(["stdbuf", "-o0", BEEBJIT, "-master", "-headless", "-terminal", "-fast", "-debug",
                          "-cycles", "900000000000",
                          "-commands", "b 0xdae8 commands 'm 0x70;m 0x300;m 0x340;c';c"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, cwd=TREE)
    p.stdin.write(prog.encode("latin-1")); p.stdin.flush()
    out = bytearray()
    while True:
        chunk = p.stdout.read1(1 << 16)
        if not chunk:
            break
        out += chunk
        if re.search(rb"[\r\n]ENDOFTEST", out[-200:]):
            break
    p.kill(); p.wait()
    text = out.decode("latin-1").replace("\0", "")
    res = {c[0]: [] for c in cases}
    mem = {}
    reg = None
    for ln in re.split(r"[\r\n]+", text):
        m = re.match(r"\[ITRP\] DAE8: .*X=([0-9A-F]{2}) Y=([0-9A-F]{2})", ln)
        if m:
            reg = (int(m.group(1), 16), int(m.group(2), 16)); mem = {}; continue
        m = re.match(r"(?:\(6502db\) )?([0-9A-F]{4}): ((?:[0-9A-F]{2} ){16})", ln)
        if m and reg is not None:
            base = int(m.group(1), 16)
            for i, b in enumerate(m.group(2).split()):
                mem[base + i] = int(b, 16)
            if 0x37F in mem and 0x71 in mem:
                w = lambda a: (mem[a] | mem[a + 1] << 8) - (0x10000 if mem[a + 1] & 0x80 else 0)
                idx = mem[0x70] | mem[0x71] << 8
                X, Y = reg
                if 1 <= idx <= len(cases):
                    res[cases[idx - 1][0]].append((w(0x302 + X), w(0x300 + X), w(0x300 + Y)))
                reg = None
    return res

def rows_to_pixels(rows):
    px = set()
    for y, a, b in rows:
        for x in range(min(a, b), max(a, b) + 1):
            px.add((x, y))
    return px

def random_cases(count, seed, modes=(4,)):
    rnd = random.Random(seed)
    out = []
    for i in range(count):
        mode = rnd.choice(modes)
        cx, cy = 40, 40
        r = rnd.randint(3, 22)
        a0 = rnd.uniform(0, 2 * 3.14159265)
        sx, sy = cx + round(r * __import__('math').cos(a0)), cy + round(r * __import__('math').sin(a0))
        a1 = rnd.uniform(0, 2 * 3.14159265)
        rr = rnd.uniform(0.3, 1.6) * r
        ex, ey = cx + round(rr * __import__('math').cos(a1)), cy + round(rr * __import__('math').sin(a1))
        code = rnd.choice((173, 181))
        out.append((f"r{seed}-{i}-{code}-m{mode}", mode, code, (cx, cy), (sx, sy), (ex, ey)))
    return out

if __name__ == "__main__":
    outp = sys.argv[1]
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    modes = tuple(int(m) for m in sys.argv[4].split(",")) if len(sys.argv) > 4 else (4,)
    cases = random_cases(count, seed, modes)
    res = run(cases)
    data = {"cases": cases, "rows": res}
    json.dump(data, open(outp, "w"))
    print(f"{len(res)} cases traced, {sum(len(v) for v in res.values())} rows")
