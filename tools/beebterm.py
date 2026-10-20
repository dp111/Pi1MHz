#!/usr/bin/env python3
"""beebterm - drive the BBC's keyboard and read its screen through the Pi1MHz
telnet daemon (docs/user/telnetd.md), from a script or from the command line.

    beebterm.py HOST [--port 23] [--send "PRINT 6*7"]... [--wait 1.0]
                     [--user U --password-env VAR] [--rows 25 --cols 80]

Each --send is typed on the Beeb followed by RETURN; --key sends raw bytes
(escape sequences as \\x1b[A, \\r ...).  After each send it waits until the Beeb
has been quiet for --wait seconds, then prints the screen as plain text.  The
screen is rebuilt from the ANSI the daemon sends (cursor movement, CLS, TAB),
so a program that positions text with VDU 31 reads back as a screen.

As a module:

    from beebterm import BeebTerm
    with BeebTerm("192.168.1.20") as b:
        b.type("PRINT 6*7")
        print(b.screen_text())
"""
import argparse
import os
import socket
import sys
import time

# The daemon sends UTF-8: MODE 7's teletext characters (up arrow for ^, one half for \ ...), mosaics, the
# pound sign.  A Windows console is cp1252 and cannot print most of them: print as UTF-8 instead, and
# replace (never crash on) anything the console still cannot show.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240
ECHO, SGA = 1, 3


class Screen:
    """A text screen fed with the ANSI the daemon produces, behaving as xterm does for every
    sequence the daemon sends: cursor moves (H A B C D G), erase (J K), the scroll region
    (r, which also puts the cursor home), scrolling (S T), colours and reverse video (m), and
    the cursor shown or hidden (?25).  Each cell keeps its colours, for colour checks."""

    def __init__(self, rows=25, cols=80):
        self.rows, self.cols = rows, cols
        self.fg, self.bg, self.rev = 7, 0, False
        self.cursor_on = True
        self.clear()
        self._esc = None

    def _blank(self):
        return [" "] * self.cols, [(7, 0, False)] * self.cols

    def clear(self):
        self.grid = [[" "] * self.cols for _ in range(self.rows)]
        self.attr = [[(7, 0, False)] * self.cols for _ in range(self.rows)]
        self.lattr = ["5"] * self.rows          # ESC # 3/4: the top/bottom of a double height line, 5: single
        self.r = self.c = 0
        self.top, self.bot = 0, self.rows - 1

    def _scroll_up(self, n=1):
        for _ in range(n):
            del self.grid[self.top]
            del self.attr[self.top]
            g, a = self._blank()
            self.grid.insert(self.bot, g)
            self.attr.insert(self.bot, a)

    def _scroll_down(self, n=1):
        for _ in range(n):
            del self.grid[self.bot]
            del self.attr[self.bot]
            g, a = self._blank()
            self.grid.insert(self.top, g)
            self.attr.insert(self.top, a)

    def _newline(self):
        if self.r == self.bot:
            self._scroll_up()
        elif self.r < self.rows - 1:
            self.r += 1

    def feed(self, data):
        text = data.decode("utf-8", "replace")
        for ch in text:
            if self._esc is not None:
                self._esc += ch
                if len(self._esc) == 3 and self._esc[1] in "#(":   # ESC # n: line size (DECDHL/DECSWL); ESC ( x: charset
                    if self._esc[1] == "#" and ch in "3456":
                        self.lattr[self.r] = ch
                    self._esc = None
                elif len(self._esc) == 2 and ch in "#(":
                    pass
                elif len(self._esc) == 2 and ch != "[":
                    self._esc = None
                elif len(self._esc) > 2 and "@" <= ch <= "~":
                    self._csi(self._esc[2:-1], ch)
                    self._esc = None
                continue
            if ch == "\x1b":
                self._esc = ch
            elif ch == "\r":
                self.c = 0
            elif ch == "\n":
                self._newline()
            elif ch == "\b":
                self.c = max(0, self.c - 1)
            elif ch == "\x07" or ch < " ":
                pass
            else:
                if self.c >= self.cols:
                    self.c = 0
                    self._newline()
                self.grid[self.r][self.c] = ch
                self.attr[self.r][self.c] = (self.fg, self.bg, self.rev)
                self.c += 1

    def _csi(self, params, final):
        if params.startswith("?"):
            if params[1:] == "25":
                self.cursor_on = final == "h"
            return
        nums = [int(p) if p.isdigit() else 0 for p in params.split(";")] if params else []
        n = nums[0] if nums and nums[0] else 1
        if final in "Hf":
            self.r = min(self.rows - 1, (nums[0] if nums and nums[0] else 1) - 1)
            self.c = min(self.cols - 1, (nums[1] if len(nums) > 1 and nums[1] else 1) - 1)
        elif final == "A":
            self.r = max(self.top if self.r >= self.top else 0, self.r - n)
        elif final == "B":
            self.r = min(self.bot if self.r <= self.bot else self.rows - 1, self.r + n)
        elif final == "C":
            self.c = min(self.cols - 1, self.c + n)
        elif final == "D":
            self.c = max(0, self.c - n)
        elif final == "G":
            self.c = min(self.cols - 1, n - 1)
        elif final == "J":
            mode = nums[0] if nums else 0
            if mode == 2:
                keep = (self.r, self.c)
                self.grid = [[" "] * self.cols for _ in range(self.rows)]
                self.attr = [[(7, 0, False)] * self.cols for _ in range(self.rows)]
                self.r, self.c = keep                   # xterm: 2J leaves the cursor where it is
            elif mode == 0:
                for x in range(self.c, self.cols):
                    self.grid[self.r][x] = " "
                for y in range(self.r + 1, self.rows):
                    self.grid[y], self.attr[y] = self._blank()
            elif mode == 1:
                for x in range(0, self.c + 1):
                    self.grid[self.r][x] = " "
                for y in range(0, self.r):
                    self.grid[y], self.attr[y] = self._blank()
        elif final == "K":
            mode = nums[0] if nums else 0
            xs = range(self.c, self.cols) if mode == 0 else range(0, self.c + 1) if mode == 1 else range(self.cols)
            for x in xs:
                self.grid[self.r][x] = " "
        elif final == "r":
            t = (nums[0] if nums and nums[0] else 1) - 1
            bt = (nums[1] if len(nums) > 1 and nums[1] else self.rows) - 1
            if 0 <= t < bt < self.rows:
                self.top, self.bot = t, bt
            self.r = self.c = 0                         # xterm: setting the region homes the cursor
        elif final == "S":
            self._scroll_up(n)
        elif final == "T":
            self._scroll_down(n)
        elif final == "m":
            for v in (nums or [0]):
                if v == 0:
                    self.fg, self.bg, self.rev = 7, 0, False
                elif v == 7:
                    self.rev = True
                elif v == 27:
                    self.rev = False
                elif 30 <= v <= 37:
                    self.fg = v - 30
                elif 40 <= v <= 47:
                    self.bg = v - 40
                elif 90 <= v <= 97:
                    self.fg = v - 90 + 8
                elif 100 <= v <= 107:
                    self.bg = v - 100 + 8

    def text(self):
        lines = ["".join(row).rstrip() for row in self.grid]
        while lines and not lines[-1]:
            lines.pop()
        return "\n".join(lines)

    def fg_at(self, row, col):
        return self.attr[row][col][0]


class BeebTerm:
    def __init__(self, host, port=23, user=None, password=None, rows=25, cols=80,
                 timeout=10.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(0.05)
        self.screen = Screen(rows, cols)
        self.raw = b""
        self._state, self._cmd, self._seen = "data", 0, set()
        if user is not None:
            self._login(user, password or "", timeout)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    # -- receiving ---------------------------------------------------------
    def _telnet(self, data):
        out = bytearray()
        for b in data:
            if self._state == "data":
                if b == IAC:
                    self._state = "iac"
                else:
                    out.append(b)
            elif self._state == "iac":
                if b in (WILL, WONT, DO, DONT):
                    self._cmd, self._state = b, "opt"
                elif b == SB:
                    self._state = "sb"
                elif b == IAC:
                    out.append(IAC)
                    self._state = "data"
                else:
                    self._state = "data"
            elif self._state == "opt":
                self._state = "data"
                # We take the server's ECHO and SGA, and refuse everything else once.
                if self._cmd == WILL and b in (ECHO, SGA):
                    self._reply(DO, b)
                elif self._cmd == WILL:
                    self._reply(DONT, b)
                elif self._cmd == DO and b == SGA:
                    self._reply(WILL, b)
                elif self._cmd == DO:
                    self._reply(WONT, b)
            elif self._state == "sb":
                if b == IAC:
                    self._state = "sbiac"
            elif self._state == "sbiac":
                self._state = "data" if b == SE else "sb"
        return bytes(out)

    def _reply(self, cmd, opt):
        if (cmd, opt) not in self._seen:
            self._seen.add((cmd, opt))
            self.sock.sendall(bytes([IAC, cmd, opt]))

    def pump(self):
        """Read what has arrived, without blocking.  Returns the bytes of text."""
        got = b""
        try:
            while True:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                got += self._telnet(chunk)
        except (socket.timeout, BlockingIOError):
            pass
        if got:
            self.raw += got
            self.screen.feed(got)
        return got

    def wait_quiet(self, quiet=1.0, limit=15.0):
        """Return once nothing has arrived for `quiet` seconds (or `limit`)."""
        start = last = time.monotonic()
        while time.monotonic() - last < quiet and time.monotonic() - start < limit:
            if self.pump():
                last = time.monotonic()
        return self.screen_text()

    def wait_for(self, text, limit=10.0):
        """True once `text` appears on the screen."""
        end = time.monotonic() + limit
        while time.monotonic() < end:
            self.pump()
            if text in self.screen.text():
                return True
        return False

    def _login(self, user, password, timeout):
        if not self.wait_for("login:", timeout):
            raise RuntimeError("no login prompt")
        self.send_keys(user + "\r")
        if not self.wait_for("Password:", timeout):
            raise RuntimeError("no password prompt")
        self.send_keys(password + "\r")
        self.wait_quiet(0.5, 5.0)
        if "incorrect" in self.screen.text():
            raise RuntimeError("login refused")

    # -- sending -----------------------------------------------------------
    def send_keys(self, keys):
        data = keys.encode("latin-1") if isinstance(keys, str) else keys
        self.sock.sendall(data.replace(bytes([IAC]), bytes([IAC, IAC])))

    def type(self, line, quiet=1.0):
        """Type a line and RETURN, wait for the Beeb to go quiet, return the screen."""
        self.send_keys(line + "\r")
        return self.wait_quiet(quiet)

    def screen_text(self):
        return self.screen.text()


def _selftest():
    s = Screen(5, 20)
    s.feed(b"\x1b[2J\x1b[HHello\r\n>PRINT 6*7\r\n        42\r\n>")
    assert s.text() == "Hello\n>PRINT 6*7\n        42\n>", repr(s.text())
    s.feed(b"\x1b[2;6H\x1b[31mX\x1b[0m")      # TAB: row 2, column 6, a colour on the way
    assert s.text().split("\n")[1] == ">PRINX 6*7", repr(s.text())
    s = Screen(3, 10)
    s.feed(b"a\r\nb\r\nc\r\nd")                 # scrolls
    assert s.text() == "b\nc\nd", repr(s.text())
    s = Screen(3, 10)
    s.feed(b"ab\x08 \x08c")                      # the daemon's DELETE: back, blank, back
    assert s.text() == "ac", repr(s.text())

    # A fake daemon: greeting, login, a screen, and a reply to what is typed.
    import threading
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    got = []

    def line_from(c):
        import re
        buf = b""
        while b"\r" not in buf:
            buf += c.recv(64)
        return re.sub(rb"\xff[\xfb-\xfe].", b"", buf)    # drop the client's negotiation

    def serve():
        c, _ = srv.accept()
        c.sendall(bytes([IAC, WILL, ECHO, IAC, WILL, SGA, IAC, DO, SGA]) + b"\r\nlogin: ")
        got.append(line_from(c))
        c.sendall(b"\r\nPassword: ")
        got.append(line_from(c))
        c.sendall(b"\r\n\r\n[BBC attached]\r\n")
        got.append(line_from(c))
        c.sendall(b">PRINT 6*7\r\n        42\r\n>")
        time.sleep(0.3)
        c.close()

    t = threading.Thread(target=serve, daemon=True)
    t.start()
    with BeebTerm("127.0.0.1", srv.getsockname()[1], user="u", password="p") as b:
        out = b.type("PRINT 6*7", quiet=0.2)
    t.join(2)
    srv.close()
    assert got == [b"u\r", b"p\r", b"PRINT 6*7\r"], got
    assert "42" in out and "[BBC attached]" in out, repr(out)
    print("beebterm self-test ok")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("host", nargs="?")
    ap.add_argument("--port", type=int, default=23)
    ap.add_argument("--send", action="append", default=[], metavar="LINE",
                    help="type LINE and RETURN (repeatable)")
    ap.add_argument("--key", action="append", default=[], metavar="BYTES",
                    help=r"send raw bytes, e.g. '\x1b[A' (repeatable, after --send)")
    ap.add_argument("--wait", type=float, default=1.0, help="quiet seconds to wait after each send")
    ap.add_argument("--user")
    ap.add_argument("--password-env", metavar="VAR",
                    help="environment variable holding the password (never on the command line)")
    ap.add_argument("--rows", type=int, default=25)
    ap.add_argument("--cols", type=int, default=80)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        _selftest()
        return 0
    if not a.host:
        ap.error("host is required")
    password = os.environ.get(a.password_env) if a.password_env else None
    with BeebTerm(a.host, a.port, a.user, password, a.rows, a.cols) as b:
        b.wait_quiet(a.wait)
        for line in a.send:
            b.type(line, a.wait)
        for k in a.key:
            b.send_keys(k.encode("latin-1").decode("unicode_escape").encode("latin-1"))
            b.wait_quiet(a.wait)
        print(b.screen_text())
    return 0


if __name__ == "__main__":
    sys.exit(main())
