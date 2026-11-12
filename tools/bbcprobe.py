#!/usr/bin/env python3
"""Drive a BBC through the Pi1MHz RAM probe (docs/dev/ramprobe.md).

   bbcprobe.py [--host Pi1MHz.local] COMMAND ...
     status                        what the probe says
     halt [--nmi]                  stop the machine; prints the registers.  Rung 0 (the vsync
                                   event), then rung 1 (nIRQ); the NMI only with --nmi
     continue [pc=0x2000 a=1 ...]  let it go on, optionally somewhere else
     peek ADDR LEN [--bank N] [-o FILE]   read memory: hex, or into a file
     peek ADDR:LEN ADDR:LEN ...    several ranges in one stop: a line of hex for each
     poke ADDR HEX | --file FILE   write memory
     poke ADDR:HEX ADDR:HEX ...    several ranges in one stop
     jsr ADDR [a=1 x=2 y=3]        call code; prints what it returned
     run HEX [a=1 x=2 y=3]         run a little code from the Pi's memory
     rehook                        point EVNTV and IRQ2V at the probe's hooks again, after a
                                   program put them back (disc Elite's loader does): by the NMI
                                   if the machine is running, so it needs &0D00 intact (not
                                   after an Acorn DFS disc access; MMFS leaves it alone)
     shared-read OFFSET LEN [-o FILE]       read shared memory, with no stop: hex, or into a file
     shared-write OFFSET HEX | --file FILE  write shared memory, with no stop

Halt first, then peek, poke, jsr or run.  Shared memory needs no halt: it is in the Pi1MHz, and
the BBC's own code reads and writes it while it runs.  Numbers may be 0x2000 or 8192.
"""
import argparse
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

CHUNK = 512
HOOKS = ((0x0220, 0xFC8F), (0x0206, 0xFC6F))     # EVNTV and IRQ2V, and each one's hook in FRED


def rehook_code():
    """For each vector not already pointing into FRED (&FCxx): its value into the hook's JMP
    (the two bytes after the hook's first), then the vector to the hook.  The check keeps a hook
    from ever going on to itself.  59 bytes, run from the agent's page."""
    code = b""
    for vec, hook in HOOKS:
        old = hook + 1
        code += bytes([
            0xAD, (vec + 1) & 0xFF, vec >> 8,                   # LDA vec+1
            0xC9, 0xFC,                                         # CMP #&FC
            0xF0, 0x16,                                         # BEQ next
            0xAD, vec & 0xFF, vec >> 8,                         # LDA vec
            0x8D, old & 0xFF, old >> 8,                         # STA hook+1
            0xAD, (vec + 1) & 0xFF, vec >> 8,                   # LDA vec+1
            0x8D, (old + 1) & 0xFF, old >> 8,                   # STA hook+2
            0xA9, hook & 0xFF,                                  # LDA #<hook
            0x8D, vec & 0xFF, vec >> 8,                         # STA vec
            0xA9, hook >> 8,                                    # LDA #>hook
            0x8D, (vec + 1) & 0xFF, vec >> 8,                   # STA vec+1
        ])                                                      # .next
    return code + b"\x60"                                       # RTS


class Probe:
    def __init__(self, host, timeout=10.0):
        self.base = f"http://{host}"
        self.timeout = timeout

    def _url(self, path, **params):
        query = urllib.parse.urlencode(params, safe="")
        return f"{self.base}{path}" + (f"?{query}" if query else "")

    def _go(self, path, post, body=None, **params):
        req = urllib.request.Request(self._url(path, **params), method="POST" if post else "GET",
                                     data=(body or b"") if post else None)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return r.read().decode()
        except urllib.error.HTTPError as e:
            body = e.read().decode()
            try:
                raise SystemExit(f"{path}: {json.loads(body).get('error') or e}")
            except json.JSONDecodeError:
                raise SystemExit(f"{path}: {e}")

    def status(self):
        return json.loads(self._go("/bbc", False))

    def act(self, path, body=None, **params):
        """Start something and wait until it's over."""
        s = self.attempt(path, body, **params)
        if s["error"]:
            raise SystemExit(f"{path}: {s['error']}")
        return s

    def attempt(self, path, body=None, **params):
        """As act, but a failure comes back in the status rather than ending the program."""
        self._go(path, True, body, **params)
        deadline = time.monotonic() + self.timeout
        while True:
            s = self.status()
            if not s["busy"]:
                return s
            if time.monotonic() > deadline:
                raise SystemExit(f"{path}: still {s['state']} after {self.timeout} s")
            time.sleep(0.01)

    def halt(self, nmi=False):
        """The gentlest rung that answers: 0, then 1, then (only if asked) the NMI."""
        tries = [{"rung": 0}, {"rung": 1}] + ([{"rung": 2, "nmi": 1}] if nmi else [])
        for t in tries:
            s = self.attempt("/bbc/halt", **t)
            if not s["error"]:
                return s
        raise SystemExit(f"halt: {s['error']}" + ("" if nmi else " (--nmi tries the NMI too)"))

    def rehook(self):
        """Hook EVNTV and IRQ2V again; a running machine is stopped by the NMI and let go."""
        stopped = self.status()["state"] == "halted"
        if not stopped:
            s = self.attempt("/bbc/halt", rung=2, nmi=1)
            if s["error"]:
                raise SystemExit(f"rehook: {s['error']}")
        self.act("/bbc/run", hex=rehook_code().hex())
        if not stopped:
            self.act("/bbc/continue")

    def peek(self, addr, length, bank=None):
        params = {"addr": addr, "len": length}
        if bank is not None:
            params["bank"] = bank
        self.act("/bbc/peek", **params)
        return self._data(length)

    def peek_ranges(self, ranges, bank=None, rung=None):
        """[(addr, len), ...] read in one stop (and, with a rung, on a running machine: in,
        read, out): their bytes, one bytes object each."""
        params = {"ranges": _ranges(ranges)}
        if bank is not None:
            params["bank"] = bank
        if rung is not None:
            params["rung"] = rung
        self.act("/bbc/peek", **params)
        data, out = self._data(sum(n for _, n in ranges)), []
        for _, n in ranges:
            out.append(data[:n])
            data = data[n:]
        return out

    def _data(self, length):
        out = bytearray()
        while len(out) < length:
            text = self._go("/bbc/data", False, offset=len(out), len=4096).strip()
            if not text:
                raise SystemExit("peek: short read")
            out += bytes.fromhex(text)
        return bytes(out)

    def shared_read(self, offset, length):
        out = bytearray()
        while len(out) < length:
            n = min(4096, length - len(out))
            out += bytes.fromhex(self._go("/bbc/shared", False, offset=offset + len(out), len=n).strip())
        return bytes(out)

    def shared_write(self, offset, data):
        """Any length that fits, in one request: the body goes straight into the Pi's memory."""
        return json.loads(self._go("/bbc/shared", True, data, offset=offset))

    def poke(self, addr, data):
        for at in range(0, len(data), CHUNK):
            self.act("/bbc/poke", addr=addr + at, hex=data[at:at + CHUNK].hex())

    def poke_ranges(self, writes, rung=None):
        """[(addr, bytes), ...] written in one stop, the bytes as the request's body."""
        params = {"ranges": _ranges([(a, len(d)) for a, d in writes])}
        if rung is not None:
            params["rung"] = rung
        self.act("/bbc/poke", b"".join(d for _, d in writes), **params)


def _ranges(ranges):
    return ",".join(f"0x{a:X}:{n}" for a, n in ranges)


def regs(args):
    out = {}
    for a in args:
        k, v = a.split("=", 1)
        out[k] = int(v, 0)
    return out


def show(r, pc=True):
    if r is None:
        return "in the OS's vsync event (rung 0): no program registers"
    line = f"A={r['a']:02X} X={r['x']:02X} Y={r['y']:02X} P={r['p']:02X}"
    return line + (f" PC={r['pc']:04X} S={r['s']:02X}" if pc else "")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="Pi1MHz.local")
    ap.add_argument("command")
    ap.add_argument("args", nargs="*")
    ap.add_argument("--bank", type=lambda s: int(s, 0))
    ap.add_argument("-o", "--output")
    ap.add_argument("--file")
    ap.add_argument("--nmi", action="store_true", help="halt may use the NMI (see docs/dev/ramprobe.md)")
    o = ap.parse_args()
    p = Probe(o.host)
    c, a = o.command, o.args
    if c == "status":
        print(json.dumps(p.status(), indent=2))
    elif c == "halt":
        print(show(p.halt(o.nmi)["stopped"]))
    elif c == "continue":
        print(p.act("/bbc/continue", **regs(a))["state"])
    elif c == "peek" and a and ":" in a[0]:
        ranges = [(int(x, 0), int(y, 0)) for x, y in (r.split(":") for r in a)]
        for data in p.peek_ranges(ranges, o.bank):
            print(data.hex())
    elif c == "poke" and a and ":" in a[0]:
        p.poke_ranges([(int(x, 0), bytes.fromhex(y)) for x, y in (w.split(":") for w in a)])
    elif c == "peek":
        data = p.peek(int(a[0], 0), int(a[1], 0), o.bank)
        if o.output:
            with open(o.output, "wb") as f:
                f.write(data)
        else:
            print(data.hex())
    elif c == "poke":
        if o.file:
            with open(o.file, "rb") as f:
                data = f.read()
        else:
            data = bytes.fromhex(a[1])
        p.poke(int(a[0], 0), data)
    elif c == "jsr":
        print(show(p.act("/bbc/jsr", addr=int(a[0], 0), **regs(a[1:]))["result"], pc=False))
    elif c == "run":
        print(show(p.act("/bbc/run", hex=a[0], **regs(a[1:]))["result"], pc=False))
    elif c == "rehook":
        p.rehook()
    elif c == "shared-read":
        data = p.shared_read(int(a[0], 0), int(a[1], 0))
        if o.output:
            with open(o.output, "wb") as f:
                f.write(data)
        else:
            print(data.hex())
    elif c == "shared-write":
        if o.file:
            with open(o.file, "rb") as f:
                data = f.read()
        else:
            data = bytes.fromhex(a[1])
        p.shared_write(int(a[0], 0), data)
    else:
        ap.error(f"no command {c}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
