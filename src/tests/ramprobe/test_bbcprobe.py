#!/usr/bin/env python3
"""tools/bbcprobe.py, run as a person runs it, against the web API on an emulated BBC.

     test_bbcprobe.py BBCSIM ROMS BBCPROBE                  bbcsim: kc-beeb, a Master 128
"""
import json
import os
import random
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

SIM, ROMS, CLI = sys.argv[1:4]
BASIC_BANK = 12
fails = checks = 0


def ok(cond, what):
    global fails, checks
    checks += 1
    print(f"  {'ok' if cond else 'FAIL'}: {what}")
    if not cond:
        fails += 1


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


port = free_port()
host = f"127.0.0.1:{port}"
sim = subprocess.Popen([SIM, ROMS, str(port)], stdout=subprocess.PIPE, text=True)
assert sim.stdout.readline().strip() == "ready"


def cli(*args, want=0):
    pre = ["--host", host]
    r = subprocess.run([sys.executable, "-I", CLI] + pre + [str(a) for a in args], capture_output=True, text=True, timeout=30)
    if want is not None and r.returncode != want:
        print(f"  bbcprobe {args}: exit {r.returncode}: {r.stdout}{r.stderr}")
    return r


def sim_post(path, **params):
    q = urllib.parse.urlencode(params)
    req = urllib.request.Request(f"http://{host}{path}?{q}", method="POST", data=b"")
    return urllib.request.urlopen(req, timeout=60).read().decode()


def screen():
    return urllib.request.urlopen(f"http://{host}/sim/screen", timeout=10).read().decode()


try:
    print("== before the install line")
    s = json.loads(cli("status").stdout)
    ok(s["installed"] is False, "status: not installed")
    ok(s["state"] == "waiting", f"and waiting, not running: {s['state']}")
    r = cli("halt", want=1)
    ok(r.returncode == 1 and "not installed" in r.stderr, f"halt says so: {r.stderr.strip()}")

    print("== install, typed at the keyboard")
    sim_post("/sim/type", text="X%=23:CALL &FC88\r")
    s = json.loads(cli("status").stdout)
    ok(s["installed"] is True and s["state"] == "running", f"status: installed, {s['state']}")
    ok("Pi1MHz probe on" in screen(), "the BBC says so")
    ok(s["api"] == 2 and s["build"] and isinstance(s["resets"], int), f"status: api {s['api']}, build {s['build']}, resets {s['resets']}")

    print("== halt, peek")
    r = cli("halt")
    ok(r.returncode == 0 and "PC=" in r.stdout, f"halt: {r.stdout.strip()}")
    r = cli("peek", 0x0D00, 1)
    ok(r.stdout.strip().upper() == "4C", f"the NMI's JMP at &0D00: {r.stdout.strip()}")

    print("== poke, read it back")
    r = cli("poke", 0x2000, "A9428E0030AA60")
    ok(r.returncode == 0, "poke &2000")
    r = cli("peek", 0x2000, 7)
    ok(r.stdout.strip() == "a9428e0030aa60", f"peek &2000: {r.stdout.strip()}")

    print("== jsr, run")
    r = cli("poke", 0x2000, "A94260")                # LDA #&42 : RTS
    r = cli("jsr", 0x2000, "x=7", "y=9")
    ok(r.returncode == 0 and "A=42 X=07 Y=09" in r.stdout, f"jsr: {r.stdout.strip()}")
    r = cli("run", "A9558A60", "x=3")                # LDA #&55 : TXA : RTS
    ok(r.returncode == 0 and "A=03" in r.stdout, f"run: {r.stdout.strip()}")

    print("== more than one request's worth")
    blob = bytes((i * 7 + 3) & 0xFF for i in range(5000))
    with tempfile.TemporaryDirectory() as d:
        src, back = os.path.join(d, "in"), os.path.join(d, "out")
        with open(src, "wb") as f:
            f.write(blob)
        r = cli("poke", 0x3000, "--file", src)
        ok(r.returncode == 0, "poke 5000 bytes from a file, in chunks")
        r = cli("peek", 0x3000, 5000, "-o", back)
        with open(back, "rb") as f:
            ok(f.read() == blob, "peek 5000 bytes into a file, the same")

    print("== several ranges in one request")
    r = cli("poke", "0x2100:0102", "0x22FE:0A0B0C0D", "0x2400:" + "EE" * 300)
    ok(r.returncode == 0, "poke 3 ranges, one across a page, in one request")
    r = cli("peek", "0x2100:2", "0x22FE:4", "0x2400:300", "0x8009:5", "--bank", BASIC_BANK)
    lines = r.stdout.split()
    ok(r.returncode == 0 and lines[:3] == ["0102", "0a0b0c0d", "ee" * 300] and bytes.fromhex(lines[3]) == b"BASIC",
       f"peek 4 ranges in one request, the last from bank {BASIC_BANK}: {[l[:12] for l in lines]}")

    print("== sideways ROM")
    r = cli("peek", 0x8007, 1, "--bank", BASIC_BANK)  # BASIC's copyright offset
    ok(r.returncode == 0 and len(r.stdout.strip()) == 2, f"peek bank {BASIC_BANK}: {r.stdout.strip()}")
    r = cli("peek", 0x8009, 5, "--bank", BASIC_BANK)
    ok(bytes.fromhex(r.stdout.strip()) == b"BASIC", f"bank {BASIC_BANK} says {bytes.fromhex(r.stdout.strip())!r}")

    print("== refusals, and then it still works")
    r = cli("poke", 0x0D00, "00", want=1)
    ok(r.returncode == 1 and r.stderr.strip() != "", f"poke on the NMI's JMP: {r.stderr.strip()}")
    r = cli("peek", 0x0D00, 1)
    ok(r.stdout.strip().upper() == "4C", "the JMP is unharmed")
    r = cli("poke", 0x2000, "ZZ", want=None)
    ok(r.returncode != 0, "not hex: refused")
    r = cli("nonsense", want=2)
    ok(r.returncode == 2, "no such command")

    print("== continue: BASIC carries on")
    r = cli("continue")
    ok(r.stdout.strip() == "running", f"continue: {r.stdout.strip()}")
    sim_post("/sim/type", text="PRINT 6*7\r")
    ok(" 42" in screen() or "42" in screen(), "PRINT 6*7 gives 42")

    print("== the ladder over HTTP: rungs 0 and 1, no NMI")
    for rung in (0, 1):
        r = urllib.request.urlopen(urllib.request.Request(f"http://{host}/bbc/halt?rung={rung}", method="POST", data=b""), timeout=10)
        deadline = time.time() + 2
        while True:
            s = json.loads(cli("status").stdout)
            if not s["busy"] or time.time() > deadline:
                break
            time.sleep(0.02)
        ok(s["state"] == "halted" and s["rung"] == rung, f"halt?rung={rung}: {s['state']}, by rung {s['rung']} {s['error']}")
        cli("continue")
    urllib.request.urlopen(urllib.request.Request(f"http://{host}/bbc/peek?addr=0x0D00&len=3&rung=0", method="POST", data=b""), timeout=10)
    time.sleep(0.5)
    s = json.loads(cli("status").stdout)
    got = urllib.request.urlopen(f"http://{host}/bbc/data?offset=0&len=3", timeout=10).read().decode().strip()
    ok(s["state"] == "running" and got == "4C74FC", f"peek?rung=0 on a running BBC: in, read, out ({s['state']}, {got})")
    sim_post("/sim/type", text="PRINT 6*7\r")
    ok("42" in screen(), "BASIC none the worse")
    req = urllib.request.Request(f"http://{host}/bbc/peek?ranges=0x0D00%3A3%2C0x0D00%3A1&rung=1&wait=1", method="POST", data=b"")
    s = json.loads(urllib.request.urlopen(req, timeout=10).read())
    got = urllib.request.urlopen(f"http://{host}/bbc/data?offset=0&len=5", timeout=10).read().decode().strip()
    ok(s["state"] == "running" and s["peeked"] == 4 and got == "4C74FC4C",
       f"two ranges by rung 1 on a running BBC, escaped as a client sends them: in, read both, out ({s['state']}, {got})")

    print("== a poke's bytes as the body, answered when done (wait=1)")
    pat = bytes(random.randrange(256) for _ in range(3000))
    req = urllib.request.Request(f"http://{host}/bbc/poke?addr=0x3000&rung=0&wait=1", method="POST", data=pat,
                                 headers={"Content-Type": "application/octet-stream"})
    s = json.loads(urllib.request.urlopen(req, timeout=20).read())
    ok(s["state"] == "running" and not s["busy"] and s["error"] == "", f"3000 bytes in one request, the answer once done: {s['state']} {s['error']}")
    req = urllib.request.Request(f"http://{host}/bbc/peek?addr=0x3000&len=3000&rung=0&wait=1", method="POST", data=b"")
    json.loads(urllib.request.urlopen(req, timeout=20).read())
    got = b""
    while len(got) < 3000:
        got += bytes.fromhex(urllib.request.urlopen(f"http://{host}/bbc/data?offset={len(got)}&len=4096", timeout=10).read().decode().strip())
    ok(got == pat, "read back, the same")
    try:
        urllib.request.urlopen(urllib.request.Request(f"http://{host}/bbc/poke?addr=0x3000&rung=0", method="POST",
                               data=bytes(40000), headers={"Content-Type": "application/octet-stream"}), timeout=20)
        ok(False, "a 40000-byte body refused")
    except urllib.error.HTTPError as e:
        ok(e.code == 400, f"a 40000-byte body refused: {e.code}")

    print("== stopped in a loop with interrupts off, and sent somewhere else")
    sim_post("/sim/type", text="?&2000=120:?&2001=76:?&2002=1:?&2003=32\r")   # SEI : JMP &2001
    # a separate line, as the keyboard stops with it
    sim_post("/sim/type", text="CALL &2000\r")
    r = cli("halt", want=1)
    ok(r.returncode == 1 and "--nmi" in r.stderr, f"interrupts off: halt alone gives up at rung 1: {r.stderr.strip()}")
    r = cli("halt", "--nmi")
    pc = int(r.stdout.split("PC=")[1].split()[0], 16)
    ok(0x2001 <= pc <= 0x2003, f"stopped in the loop: {r.stdout.strip()}")
    cli("poke", 0x2000, "5860")                      # CLI : RTS
    r = cli("continue", "pc=0x2000")
    ok(r.returncode == 0, "sent to CLI : RTS")
    sim_post("/sim/type", text="PRINT 5+6\r")
    ok("11" in screen(), "back in BASIC, printing")

    print("== a program puts EVNTV and IRQ2V back, as disc Elite's loader does: rehook")
    cli("halt")
    # the vectors to what the hooks go on to, the OS's own: LDA &FC90 : STA &0220 ... RTS
    cli("run", "AD90FC8D2002AD91FC8D2102AD70FC8D0602AD71FC8D070260")
    cli("continue")
    r = cli("halt", want=1)
    ok(r.returncode == 1 and "--nmi" in r.stderr, f"rungs 0 and 1 can't reach it now: {r.stderr.strip()}")
    r = cli("rehook")
    ok(r.returncode == 0, f"rehook, by the NMI: {r.stderr.strip()}")
    for _ in range(2):                               # twice: the second changes nothing
        r = cli("halt")
        ok(r.returncode == 0, f"rung 0 or 1 reaches it again: {r.stdout.strip()}")
        vecs = cli("peek", "0x0220:2", "0x0206:2").stdout.split()
        hops = cli("run", "AD91FC60").stdout.strip()   # LDA &FC91 : RTS, the EVNTV hook's old vector, high byte
        ok(vecs == ["8ffc", "6ffc"] and not hops.startswith("A=FC"),
           f"EVNTV and IRQ2V at the hooks, which go on to the OS's: {vecs}, {hops}")
        cli("continue")
        cli("rehook")
    sim_post("/sim/type", text="PRINT 2+3\r")
    ok("5" in screen(), "BASIC carries on")

finally:
    try:
        sim_post("/sim/quit")
    except OSError:
        pass
    try:
        sim.wait(timeout=5)
    except subprocess.TimeoutExpired:
        sim.kill()

print(f"\n{checks} checks, {fails} failures")
if fails:
    sys.exit(1)
print("BBCPROBE OVER HTTP: TESTS PASSED")
