#!/usr/bin/env python3
"""Compare Pi1MHz's VDU driver against a real BBC OS 1.20 + GXR 1.20.

Both sides are driven from the one test table below: each case is a list of
VDU byte sequences.  The Beeb side runs them under beebjit (headless, driven
over the emulated serial line) and reads the result back with POINT; the host
side runs the very same bytes through src/framebuffer/* built for the PC.
Both emit "L <name>" / "P x y" records, which are then diffed.

  ./vdutest.py                 run every case
  ./vdutest.py circle ellipse  run only cases whose name matches

See docs/dev/vdu-rom-conformance.md for the method and its traps.
"""
import os, re, subprocess, sys, tempfile

HERE     = os.path.dirname(os.path.abspath(__file__))
ROOT     = os.path.abspath(os.path.join(HERE, "..", ".."))
BEEBJIT  = os.environ.get("BEEBJIT", "/mnt/c/Archlinux/beebjit")
MODE     = 4          # MODE 4: 1 pixel = 4 OS units in both axes
U        = 4          # OS units per pixel in MODE 4
XL, YL   = 30, 24     # pixel box that gets scanned and compared

def P(n):                      # a 16-bit OS-unit coordinate, as VDU bytes
    n &= 0xFFFF
    return [n & 0xFF, n >> 8]

def plot(k, x, y):             # VDU 25,k,x;y;  with x,y in pixels
    return [25, k] + P(x * U) + P(y * U)

MOVE = 4                       # PLOT 4 = move absolute

def case(name, *cmds):
    return (name, list(cmds))

CASES = [
    # --- lines: the family already fixed, kept as a regression guard --------
    case("line-5-solid",       plot(MOVE,0,0),  plot(5,26,7)),
    case("line-13-omitlast",   plot(MOVE,0,0),  plot(13,26,7)),
    case("line-21-dotted",     plot(MOVE,0,0),  plot(21,26,7)),
    case("line-37-omitfirst",  plot(MOVE,0,0),  plot(37,26,7)),
    case("line-5-reverse",     plot(MOVE,26,7), plot(5,0,0)),
    case("line-5-steep",       plot(MOVE,0,0),  plot(5,7,24)),

    # --- points ------------------------------------------------------------
    case("point-69",           plot(69,13,11)),
    case("point-69-edge",      plot(69,0,0)),

    # --- triangles ---------------------------------------------------------
    case("tri-85-a",  plot(MOVE,1,1),  plot(MOVE,27,5),  plot(85,9,23)),
    case("tri-85-b",  plot(MOVE,3,20), plot(MOVE,25,2),  plot(85,14,22)),
    case("tri-85-flat-bottom", plot(MOVE,2,2), plot(MOVE,26,2), plot(85,14,20)),
    case("tri-85-flat-top",    plot(MOVE,2,20), plot(MOVE,26,20), plot(85,14,3)),
    case("tri-85-degenerate",  plot(MOVE,4,4), plot(MOVE,20,4), plot(85,26,4)),

    # --- rectangles and parallelograms -------------------------------------
    case("rect-101",   plot(MOVE,3,3),  plot(101,25,20)),
    case("rect-101-r", plot(MOVE,25,20), plot(101,3,3)),
    case("para-117",   plot(MOVE,2,2),  plot(MOVE,18,2),  plot(117,26,18)),
    case("para-117-b", plot(MOVE,4,18), plot(MOVE,20,22), plot(117,26,6)),

    # --- circles -----------------------------------------------------------
    case("circ-149-r10", plot(MOVE,14,12), plot(149,24,12)),
    case("circ-149-r7",  plot(MOVE,14,12), plot(149,21,12)),
    case("circ-149-r4",  plot(MOVE,14,12), plot(149,18,12)),
    case("circ-149-r1",  plot(MOVE,14,12), plot(149,15,12)),
    case("circ-157-fill-r10", plot(MOVE,14,12), plot(157,24,12)),
    case("circ-157-fill-r5",  plot(MOVE,14,12), plot(157,19,12)),

    # --- ellipses ----------------------------------------------------------
    case("ell-197-wide", plot(MOVE,14,12), plot(MOVE,26,12), plot(197,14,20)),
    case("ell-197-tall", plot(MOVE,14,12), plot(MOVE,20,12), plot(197,14,23)),
    case("ell-205-fill", plot(MOVE,14,12), plot(MOVE,26,12), plot(205,14,20)),
    case("ell-197-shear",plot(MOVE,14,12), plot(MOVE,24,12), plot(197,18,21)),

    # --- arcs, chords, sectors --------------------------------------------
    case("arc-165",    plot(MOVE,14,12), plot(MOVE,26,12), plot(165,14,24)),
    case("chord-173",  plot(MOVE,14,12), plot(MOVE,26,12), plot(173,14,24)),
    case("sector-181", plot(MOVE,14,12), plot(MOVE,26,12), plot(181,14,24)),

    # --- horizontal line fills (need something on screen first) ------------
    case("hfill-77",  plot(MOVE,4,4), plot(85,26,4), plot(MOVE,14,12), plot(77,14,12)),
    case("hfill-93",  plot(MOVE,2,2), plot(101,26,20), plot(MOVE,14,12), plot(93,14,12)),

    # --- flood fills -------------------------------------------------------
    case("flood-133", plot(MOVE,4,4), plot(MOVE,26,6), plot(85,14,22),
                      plot(MOVE,14,10), plot(133,14,10)),

    # --- move/copy rectangle ----------------------------------------------
    case("copy-189", plot(MOVE,2,2), plot(101,10,10),
                     plot(MOVE,2,2), plot(MOVE,10,10), plot(189,16,12)),
    case("move-185", plot(MOVE,2,2), plot(101,10,10),
                     plot(MOVE,2,2), plot(MOVE,10,10), plot(185,16,12)),
]

# --------------------------------------------------------------------------

def parse(text):
    """-> {name: set of (x,y)} from L/P records."""
    out, cur = {}, None
    for ln in text.splitlines():
        f = ln.split()
        if not f:
            continue
        if f[0] == "L":
            cur = " ".join(f[1:]); out[cur] = set()
        elif f[0] == "P" and cur is not None and len(f) == 3:
            try:    out[cur].add((int(f[1]), int(f[2])))
            except ValueError: pass
    return out

def run_host(cases):
    exe = os.path.join(tempfile.gettempdir(), "vduhost")
    src = ["vduhost.c", "stubs.c"]
    fb  = ["framebuffer.c", "screen_modes.c", "primitives.c", "fonts.c", "teletext.c"]
    cmd = (["gcc", "-O1", "-I", f"{ROOT}/src/framebuffer", "-I", f"{ROOT}/src", "-o", exe]
           + [os.path.join(HERE, s) for s in src]
           + [os.path.join(ROOT, "src/framebuffer", s) for s in fb] + ["-lm"])
    subprocess.run(cmd, check=True, stderr=subprocess.DEVNULL)
    script = [f"V 22 {MODE}", "V 28 10 31 39 0"]
    for name, cmds in cases:
        script += ["CLG", f"L {name}"]
        for c in cmds:
            script.append("V " + " ".join(str(b) for b in c))
        script.append(f"DUMP 0 0 {XL} {YL}")
    r = subprocess.run([exe], input="\n".join(script) + "\n",
                       capture_output=True, text=True)
    return parse(r.stdout)

def run_beeb(cases):
    """Drive the same bytes through a real OS 1.20 + GXR under beebjit."""
    lines = [ "NEW",
             f"10 MODE {MODE}",
              "15 VDU 28,10,31,39,0",
              "20 READ NT",
              "30 FOR T=1 TO NT",
              "40 READ N$,NB",
              "50 CLG",
              "60 FOR I=1 TO NB:READ B:VDU B:NEXT",
              "70 PRINT'\"L \";N$",
             f"80 FOR X=0 TO {XL}:FOR Y=0 TO {YL}",
              "90 IF POINT(X*4,Y*4)>0 THEN PRINT \"P \";X;\" \";Y",
              "100 NEXT:NEXT",
              "110 NEXT",
              "120 PRINT'\"ENDOFTEST\"",
              "130 END",
             f"500 DATA {len(cases)}" ]
    n = 501
    for name, cmds in cases:
        by = [b for c in cmds for b in c]
        lines.append(f'{n} DATA "{name}",{len(by)}'); n += 1
        for i in range(0, len(by), 16):
            lines.append(f"{n} DATA " + ",".join(str(b) for b in by[i:i+16])); n += 1
    lines.append("RUN")
    prog = "".join(l + "\r" for l in lines)
    r = subprocess.run([os.path.join(BEEBJIT, "beebjit"),
                        "-headless", "-terminal", "-fast",
                        "-rom", "f", os.path.join(BEEBJIT, "roms/gxr.rom"),
                        "-cycles", "900000000000"],
                       input=prog.encode(), capture_output=True, cwd=BEEBJIT)
    text = r.stdout.decode("latin-1").replace("\r", "")
    if "ENDOFTEST" not in text.split("RUN", 1)[-1]:
        print("WARNING: the Beeb run did not reach ENDOFTEST - results truncated",
              file=sys.stderr)
    return parse(text)

def main():
    pats = sys.argv[1:]
    cases = [c for c in CASES if not pats or any(p in c[0] for p in pats)]
    if not cases:
        sys.exit("no cases match")
    print(f"running {len(cases)} cases ...", file=sys.stderr)
    beeb = run_beeb(cases)
    host = run_host(cases)
    bad = 0
    for name, _ in cases:
        b, h = beeb.get(name), host.get(name)
        if b is None:
            print(f"  {name:22s} NO BEEB RESULT"); bad += 1; continue
        if h is None:
            print(f"  {name:22s} NO HOST RESULT"); bad += 1; continue
        if b == h:
            print(f"  {name:22s} match ({len(b)} px)")
        else:
            bad += 1
            print(f"  {name:22s} DIFFER  beeb={len(b)} host={len(h)} "
                  f"host-only={len(h-b)} beeb-only={len(b-h)}")
            for tag, s in (("host-only", sorted(h - b)), ("beeb-only", sorted(b - h))):
                if s:
                    print(f"      {tag}: {s[:12]}{' ...' if len(s) > 12 else ''}")
    print(f"\n{len(cases)-bad}/{len(cases)} cases match the ROM")
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
