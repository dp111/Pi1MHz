#!/usr/bin/env python3
"""Page 0 of 6502code.bin, the help printer, on a 6502 (py65).

It prints the screen the Pi left at &FFE000 and pages it itself: after a
text window's worth of lines, less one, it waits for any key at OSRDCH
(which a Beeb driven down the serial redirect or over telnet can send;
the MOS's paged mode wants SHIFT at the keyboard).  Paged mode is off
while it prints and comes back as it was; Escape ends the screen.
"""
import sys
from py65.devices.mpu65c02 import MPU

OSRDCH, OSWRCH, OSBYTE, SELECT = 0xFFE0, 0xFFEE, 0xFFF4, 0xFC88
DISC_DATA = 0xFCA9                     # discaccess+3: read, auto-increment

fails = 0


def check(ok, what):
    global fails
    print(("  ok: " if ok else "  FAIL: ") + what)
    fails += 0 if ok else 1


def lines(n):
    return b"".join(b"line %d\r\n" % i for i in range(n))


def run(page, text, paged_was_on=False, top=0, bottom=24, escape_at=0):
    """-> (bytes printed, line feeds printed before each key wait, OSBYTE
    126 calls, X and SP at the page's exit); None if it never exits."""
    mem = bytearray(0x10000)
    mem[0xFD00:0xFE00] = page
    mem[0xFD01] = 0x03                 # the Pi has released the wait loop
    for a in (OSRDCH, OSWRCH, OSBYTE):
        mem[a] = 0x60
    cpu = MPU(memory=mem, pc=0xFD00)
    cpu.sp = 0xFD
    text, out, waits, acks, pos = text + b"\0", [], [], 0, 0
    for _ in range(200000):
        if cpu.pc == SELECT:
            return bytes(out), waits, acks, cpu.x, cpu.sp
        if cpu.pc == OSWRCH:
            out.append(cpu.a)
        elif cpu.pc == OSRDCH:
            waits.append(out.count(10))
            if len(waits) == escape_at:
                cpu.a, cpu.p = 27, cpu.p | 1
            else:
                cpu.a, cpu.p = 32, cpu.p & ~1
        elif cpu.pc == OSBYTE:
            if cpu.a == 117:
                cpu.x = 0x04 if paged_was_on else 0x00
            elif cpu.a == 126:
                acks += 1
            elif cpu.a == 160:         # VDU variables &300+X, &301+X
                var = {8: 0, 9: bottom, 10: 39, 11: top}
                cpu.x, cpu.y = var.get(cpu.x, 0xEE), var.get(cpu.x + 1, 0xEE)
        if pos < len(text):
            mem[DISC_DATA] = text[pos]
        before = cpu.pc
        operand = mem[before + 1] | (mem[before + 2] << 8)
        if mem[before] in (0xAD, 0xAE) and operand == DISC_DATA:
            pos += 1                   # LDA or LDX abs of the data register
        cpu.step()
    return None


def main(path):
    page = open(path, "rb").read()[0:256]

    for was_on in (False, True):
        r = run(page, lines(10), paged_was_on=was_on)
        want = b"\x0f" + lines(10) + (b"\x0e" if was_on else b"")
        check(r is not None and r[0] == want and r[1] == [] and r[3:] == (0xFF, 0xFD),
              "a short screen: VDU 15, the text, no wait, paged mode %s as it was"
              % ("on" if was_on else "off"))

    r = run(page, lines(60), paged_was_on=True)
    check(r is not None and r[0] == b"\x0f" + lines(60) + b"\x0e" and r[1] == [24, 48]
          and r[2] == 0 and r[3:] == (0xFF, 0xFD),
          "60 lines on 25 rows: a key after 24 and after 48, all of it printed")

    r = run(page, lines(24))
    check(r is not None and r[1] == [] and r[0] == b"\x0f" + lines(24),
          "exactly a page: no wait with nothing left to show")

    r = run(page, lines(20), top=10, bottom=14)
    check(r is not None and r[1] == [4, 8, 12, 16] and r[0] == b"\x0f" + lines(20),
          "a 5 row text window (rows 10-14): a key every 4 lines")

    r = run(page, lines(60), paged_was_on=True, escape_at=2)
    check(r is not None and r[0] == b"\x0f" + lines(48) + b"\x0e" and r[1] == [24, 48]
          and r[2] == 1 and r[3:] == (0xFF, 0xFD),
          "Escape at the second wait: acknowledged, nothing more printed, paged mode back")

    print("%s" % ("HELP PAGE TESTS PASSED" if not fails else "%d failures" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
