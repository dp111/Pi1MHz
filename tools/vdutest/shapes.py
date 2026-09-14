#!/usr/bin/env python3
"""Random PLOTs through src/framebuffer against the Master's own VDU driver.

Each case is a screen MODE and a short random sequence of VDU commands: GCOL
modes and colours (sometimes an ECF pattern), a few shapes to give the fills
something to meet, then the PLOT under test.  The same bytes go through the
host build of the driver and through MOS 3.20 itself, run by mos65.py, and the
whole screen is compared pixel by pixel and colour by colour.

  ./shapes.py [count] [seed] [family...]

Families: line point triangle rectangle parallelogram hfill flood circle disc
arc chord sector copy move ellipse text text16 (text and cursor control,
windows and VDU 4/5; text16 adds VDU 23,16 cursor movement flags) are
opt-in: name them to run them.  Cases that hang the real Master (some
tiny sectors in MODE 2 and 5) are skipped; the driver bounds them instead.
Needs gcc, python3 and the beebjit tree (for the ROM images and the snapshots
the interpreter starts from); not part of the build.
"""
import os, random, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mos65, vdutest

MODES = (0, 1, 2, 4, 5)
UNITS = {0: (2, 4), 1: (4, 4), 2: (8, 4), 4: (4, 4), 5: (8, 4)}
WIDTH = {0: 640, 1: 320, 2: 160, 4: 320, 5: 160}
COLOURS = {0: 2, 1: 4, 2: 16, 4: 2, 5: 4}

LINES = (5, 13, 21, 29, 37, 45, 53, 61, 1, 7, 15)
FAMILIES = {
    'line': LINES, 'point': (69, 71, 65), 'triangle': (85,), 'rectangle': (101,),
    'parallelogram': (117,), 'hfill': (77, 93, 109, 125), 'flood': (133, 141),
    'circle': (149,), 'disc': (157,), 'arc': (165,), 'chord': (173,), 'sector': (181,),
    'copy': (189,), 'move': (185,), 'ellipse': (197, 205),
    'text': (), 'text16': (),
}

# Not run by default: the text engine still differs from the Master (see
# docs/dev/vdu-rom-conformance.md), and VDU 23,16 is not implemented.
OPT_IN = ('text', 'text16')

TEXT_COLS = {0: 80, 1: 40, 2: 20, 4: 40, 5: 20}

def rnd_text(rnd, mode, flags):
    """A random run of text and cursor control, optionally with VDU 23,16."""
    cols, rows = TEXT_COLS[mode], 32
    ux, uy = UNITS[mode]
    ncol = COLOURS[mode]
    out = []
    if flags:
        out += [23, 16, rnd.randrange(0, 128), 0, 0, 0, 0, 0, 0, 0]
    for _ in range(rnd.randint(15, 60)):
        r = rnd.random()
        if r < 0.35:
            out += [rnd.randrange(33, 127) for _ in range(rnd.randint(1, 12))]
        elif r < 0.55:
            out.append(rnd.choice((8, 9, 10, 11, 13, 127, 8, 9, 10, 11, 13, 30)))
        elif r < 0.63:
            out += [31, rnd.randrange(0, cols + 2), rnd.randrange(0, rows + 2)]
        elif r < 0.69:
            l = rnd.randrange(0, cols); rr = rnd.randrange(l, min(cols, l + 12) + (1 if rnd.random() < 0.1 else 0))
            tp = rnd.randrange(0, rows); b = rnd.randrange(tp, min(rows, tp + 8) + (1 if rnd.random() < 0.1 else 0))
            out += [28, l, b, rr, tp]
        elif r < 0.72:
            out.append(26)
        elif r < 0.75:
            out.append(12)
        elif r < 0.80:
            out.append(rnd.choice((4, 5)))
            if out[-1] == 5:
                out += [25, 4] + P(rnd.randrange(0, 1280)) + P(rnd.randrange(0, 1024))
        elif r < 0.85:
            out += [17, rnd.randrange(0, ncol) | rnd.choice((0, 128))]
        elif r < 0.88:
            out += [18, rnd.choice((0, 0, 3)), rnd.randrange(1, ncol)]
        elif r < 0.90:
            x0, y0 = rnd.randrange(0, 1000), rnd.randrange(0, 800)
            out += [24] + P(x0) + P(y0) + P(x0 + rnd.randrange(64, 400)) + P(y0 + rnd.randrange(64, 300))
        elif r < 0.93 and flags:
            out += [23, 16, rnd.randrange(0, 128), rnd.choice((0, 0, 255, rnd.randrange(0, 256))), 0, 0, 0, 0, 0, 0]
        else:
            out += [rnd.randrange(33, 127) for _ in range(rnd.randint(cols - 3, cols + 3))]
    return out

def P(n):
    n &= 0xFFFF
    return [n & 0xFF, n >> 8]

def rnd_case(rnd, family):
    mode = rnd.choice(MODES)
    if family in ('text', 'text16'):
        return mode, rnd_text(rnd, mode, family == 'text16')
    ux, uy = UNITS[mode]
    w = WIDTH[mode]
    ncol = COLOURS[mode]
    out = []
    def plot(k, x, y):
        out.extend([25, k] + P(x * ux) + P(y * uy))
    def pt(r=40):
        cx, cy = rnd.randint(w // 4, 3 * w // 4), rnd.randint(60, 196)
        return cx + rnd.randint(-r, r), cy + rnd.randint(-r, r)
    def gcol():
        kind = rnd.random()
        if kind < 0.12 and ncol > 2:
            out.extend([18, rnd.choice((16, 32, 48, 64)), 0])          # an ECF pattern
        else:
            out.extend([18, rnd.choice((0, 0, 0, 1, 2, 3, 4)), rnd.randrange(1, ncol)])
    # something on the screen for the fills, copies and EOR plots to meet
    for _ in range(rnd.randint(0 if family in ('line', 'point') else 1, 3)):
        out.extend([18, 0, rnd.randrange(1, ncol)])
        c = rnd.choice((85, 101, 149, 157, 5))
        a, b, d = pt(), pt(), pt()
        plot(4, *a); plot(4, *b); plot(c, *d)
    gcol()
    code = rnd.choice(FAMILIES[family])
    r = rnd.choice((0, 1, 2, 3, rnd.randint(0, 12), rnd.randint(0, 60)))
    c = pt()
    s = (c[0] + rnd.randint(-r, r), c[1] + rnd.randint(-r, r))
    e = (c[0] + rnd.randint(-2 * r - 2, 2 * r + 2), c[1] + rnd.randint(-2 * r - 2, 2 * r + 2))
    if family in ('hfill', 'flood'):
        plot(code, *c)
    elif family in ('copy', 'move'):
        plot(4, *c); plot(4, *s); plot(code, *e)
    elif code in (1, 9):                           # relative lines
        plot(4, *c)
        out.extend([25, code] + P((e[0] - c[0]) * ux) + P((e[1] - c[1]) * uy))
    else:
        plot(4, *c); plot(4, *s); plot(code, *e)
    return mode, out

def rom_screen(mode, data):
    v = mos65.MasterVDU(mode)
    v.vdu(data, max_steps=12_000_000)          # a full-screen MODE 0 flood takes ~5M
    return {(x, y, c) for (x, y), c in v.pixels().items()}

def host_screens(cases):
    exe = vdutest.build_host()
    script = []
    for name, mode, data in cases:
        script += [f"V 22 {mode}", "V 23 1 0 0 0 0 0 0 0 0", f"L {name}"]
        for i in range(0, len(data), 256):             # vduhost reads 4096-character lines
            script.append("V " + " ".join(map(str, data[i:i + 256])))
        script.append(f"DUMP 0 0 {WIDTH[mode] - 1} 255")
    r = subprocess.run([exe], input="\n".join(script) + "\n", capture_output=True, text=True)
    return vdutest.parse(r.stdout)

def main():
    args = sys.argv[1:]
    n = int(args.pop(0)) if args and args[0].isdigit() else 300
    seed = int(args.pop(0)) if args and args[0].isdigit() else 1
    families = args or [f for f in FAMILIES if f not in OPT_IN]
    rnd = random.Random(seed)
    cases, want = [], {}
    t0 = time.time()
    for i in range(n):
        family = families[i % len(families)]
        mode, data = rnd_case(rnd, family)
        name = f"{family}-{i}"
        try:
            want[name] = rom_screen(mode, data)
        except RuntimeError:
            want[name] = None                              # hangs the Master
        cases.append((name, mode, data))
    got = {}
    for i in range(0, n, 100):
        got.update(host_screens(cases[i:i + 100]))
    stats = {}
    shown = 0
    for name, mode, data in cases:
        family = name.rsplit('-', 1)[0]
        st = stats.setdefault(family, [0, 0, 0])
        if want[name] is None:
            st[2] += 1
            continue
        st[0] += 1
        g = got.get(name, set())
        if g == want[name]:
            st[1] += 1
        elif shown < 10:
            shown += 1
            print(f"  DIFFER {name} MODE {mode}: host-only {sorted(g - want[name])[:5]} rom-only {sorted(want[name] - g)[:5]}")
            print(f"         VDU {','.join(map(str, data))}")
    total = sum(s[0] for s in stats.values())
    good = sum(s[1] for s in stats.values())
    for family in families:
        t, ok, hung = stats.get(family, [0, 0, 0])
        print(f"  {family:14s} {ok}/{t}" + (f"  ({hung} hang the Master, skipped)" if hung else ""))
    print(f"{good}/{total} random cases match the Master ({time.time() - t0:.0f}s)")
    return 0 if good == total else 1

if __name__ == "__main__":
    sys.exit(main())
