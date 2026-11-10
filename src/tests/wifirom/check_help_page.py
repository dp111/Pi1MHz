#!/usr/bin/env python3
"""Page 0 of 6502code.bin, the help printer, on a 6502 (py65).

It prints the screen the Pi left at &FFE000 in paged mode and puts paged
mode back as it found it.  The Pi may make its VDU 14 a VDU 15 (helpers.c,
HELP_PAGED_OP: a Beeb driven down the serial redirect, nothing to press
SHIFT), and paged mode must still come back as it was - on as well as off.
"""
import sys
from py65.devices.mpu65c02 import MPU

OSWRCH, OSBYTE, SELECT = 0xFFEE, 0xFFF4, 0xFC88
DISC_DATA = 0xFCA9                     # discaccess+3: read, auto-increment
PAGED_OP = 0x0B                        # HELP_PAGED_OP in helpers.c
TEXT = b"Pi1MHz help\r\nline two\r\n"

fails = 0


def check(ok, what):
    global fails
    print(("  ok: " if ok else "  FAIL: ") + what)
    fails += 0 if ok else 1


def run(page, op, paged_was_on):
    mem = bytearray(0x10000)
    mem[0xFD00:0xFE00] = page
    mem[0xFD01] = 0x03                 # the Pi has released the wait loop
    mem[0xFD00 + PAGED_OP] = op
    for a in (OSWRCH, OSBYTE):
        mem[a] = 0x60
    cpu = MPU(memory=mem, pc=0xFD00)
    cpu.sp = 0xFD
    text, out, pos = TEXT + b"\0", [], 0
    for _ in range(20000):
        if cpu.pc == SELECT:
            return out, cpu.x, cpu.sp
        if cpu.pc == OSWRCH:
            out.append(cpu.a)
        elif cpu.pc == OSBYTE and cpu.a == 117:
            cpu.x = 0x04 if paged_was_on else 0x00
        if pos < len(text):
            mem[DISC_DATA] = text[pos]
        before = cpu.pc
        operand = mem[before + 1] | (mem[before + 2] << 8)
        if mem[before] == 0xAD and operand == DISC_DATA:
            pos += 1                   # LDA abs of the data register
        cpu.step()
    return None, None, None


def main(path):
    image = open(path, "rb").read()
    page = image[0:256]
    check(page[PAGED_OP - 1] == 0xA9 and page[PAGED_OP] == 14,
          "LDA #14 where helpers.c patches it (HELP_PAGED_OP)")
    body = list(TEXT) + [0]
    for op in (14, 15):
        for was_on in (False, True):
            out, x, sp = run(page, op, was_on)
            want = [op] + body + [14 if was_on else 15]
            check(out == want and x == 0xFF and sp == 0xFD,
                  "VDU %d first, the screen, then paged mode %s as it was"
                  % (op, "on" if was_on else "off"))
    print("%s" % ("HELP PAGE TESTS PASSED" if not fails else "%d failures" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
