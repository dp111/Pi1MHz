#!/usr/bin/env python3
"""Run the SD explorer's two filename prompts (6502code.bin pages EXP_GET1
"Copy as:" and EXP_PUT1 "Put file:") on a 6502 and check that Escape at the
prompt cancels it, as the footer's ESC promises.

The explorer runs with *FX229,1 so its key loop reads ESC as ASCII 27.  With
that left on, OSWORD 0 takes ESC as a character outside the 32-126 range the
prompt allows and ignores it, so it never returns C=1: "Copy as:" could only
be left with RETURN, which copies under the SD name.  Each prompt must turn
Escape back on for the line, and afterwards put ESC back to ASCII 27 and
acknowledge the Escape (OSBYTE 126), so the key loop is as it was.

The pages run as the firmware serves them: a write to &FC88 shows another
page in the &FD00 window, and the transfer page's stubs run from its
template page.  The MOS is modelled only as far as the prompts reach.

usage: check_explorer.py 6502code.bin
"""
import os
import sys

try:
    from py65.devices.mpu6502 import MPU
except ImportError:
    print("py65 not installed (pip install py65): skipped the explorer checks")
    sys.exit(1 if os.environ.get("CI") else 0)

OSWRCH, OSWORD, OSBYTE, OSFIND = 0xFFEE, 0xFFF1, 0xFFF4, 0xFFCE
RETURN_TRAP = 0xFF00
EXP_KEY, EXP_GET1, EXP_GET2, EXP_PUT1, EXP_XFERT = 31, 36, 37, 39, 46
XFER_SEL = 0xFE
ENTRY_INSTALLED = 4          # both prompt pages: transfer page ready, prompt
ESC = 27


class Stop(Exception):
    """The run left the prompt: where it went."""


class Memory:
    def __init__(self, image, explorer):
        self.ram = bytearray(0x10000)
        self.image = image
        self.ex = explorer

    def show(self, page):
        src = EXP_XFERT if page == XFER_SEL else page
        self.ram[0xFD00:0xFE00] = self.image[src * 256:src * 256 + 256]

    def __getitem__(self, a):
        if 0xFC00 <= a < 0xFD00:
            return 0                  # discaccess reads: an empty SD name
        return self.ram[a]

    def __setitem__(self, a, v):
        if a == 0xFC88:
            self.ex.page_switch(v)
        elif 0xFC00 <= a < 0xFD00:
            pass                      # discaccess writes
        else:
            self.ram[a] = v

    def __len__(self):
        return 0x10000


class Explorer:
    def __init__(self, image, keys):
        self.mem = Memory(image, self)
        self.cpu = MPU(memory=self.mem)
        self.keys = list(keys)
        self.fx229 = 1               # as the explorer's entry page sets it
        self.escape = False          # the MOS Escape flag
        self.acks = 0                # OSBYTE 126 calls that cleared it
        self.fx229_at_osword = []
        self.osword_carry = []
        self.page = None

    def page_switch(self, page):
        if page not in (EXP_GET1, EXP_PUT1, XFER_SEL):
            # The trailer pushes &FD, Y and RTSes: Y+1 is the entry.
            raise Stop((page, self.cpu.y + 1))
        self.page = page
        self.mem.show(page)

    def osbyte(self, cpu):
        if cpu.a == 229:
            old = self.fx229
            self.fx229 = (old & cpu.y) ^ cpu.x
            cpu.x = old
        elif cpu.a == 126:
            cpu.x = 0xFF if self.escape else 0
            self.acks += self.escape
            self.escape = False
        else:
            raise AssertionError(f"OSBYTE {cpu.a} not modelled")

    def osword0(self, cpu):
        """MOS 1.20 line input: Escape returns C=1 only while ESC is the
        Escape key (*FX229,0); otherwise it is code 27, outside the range
        and ignored, like any other out-of-range character."""
        assert cpu.a == 0, f"OSWORD {cpu.a}"
        blk = cpu.x | cpu.y << 8
        buf = self.mem[blk] | self.mem[blk + 1] << 8
        maxlen, lo, hi = self.mem[blk + 2], self.mem[blk + 3], self.mem[blk + 4]
        self.fx229_at_osword.append(self.fx229)
        n = 0
        while True:
            if not self.keys:
                raise Stop("line input still waiting for a key")
            k = self.keys.pop(0)
            if k == ESC and self.fx229 == 0:
                self.escape = True
                cpu.p |= 1
                cpu.y = n
                break
            if k == 13:
                self.mem[buf + n] = 13
                cpu.p &= ~1
                cpu.y = n
                break
            if lo <= k <= hi and n < maxlen:
                self.mem[buf + n] = k
                n += 1
        self.osword_carry.append(cpu.p & 1)

    def run(self, page):
        cpu = self.cpu
        self.page = page
        self.mem.show(page)
        cpu.sp = 0xFF
        cpu.stPushWord(RETURN_TRAP - 1)
        cpu.stPush(0x30)              # the PLP at the entry
        cpu.pc = 0xFD00 + ENTRY_INSTALLED
        try:
            for _ in range(200_000):
                pc = cpu.pc
                if pc >= 0xFF00:
                    if pc == OSWRCH:
                        pass
                    elif pc == OSBYTE:
                        self.osbyte(cpu)
                    elif pc == OSWORD:
                        self.osword0(cpu)
                    elif pc == OSFIND:
                        raise Stop("OSFIND")
                    else:
                        raise AssertionError(f"call to &{pc:04X}")
                    cpu.pc = (cpu.stPopWord() + 1) & 0xFFFF
                    continue
                cpu.step()
        except Stop as s:
            return s.args[0]
        raise AssertionError("runaway")


checks = fails = 0


def check(ok, what):
    global checks, fails
    checks += 1
    if not ok:
        fails += 1
        print(f"FAIL: {what}")


CASES = [
    # (prompt page, keys, where it must go)
    (EXP_GET1, [ESC, 13], (EXP_KEY, 1)),            # cancelled
    (EXP_GET1, [ord("A"), ord("B"), 13], (EXP_GET2, 1)),
    (EXP_GET1, [13], (EXP_GET2, 1)),                # the SD name, as before
    (EXP_PUT1, [ESC, 13], (EXP_KEY, 1)),
    (EXP_PUT1, [ord("A"), ord("B"), 13], "OSFIND"),
    (EXP_PUT1, [13], (EXP_KEY, 1)),                 # empty: cancelled, as before
]


def main(args):
    if len(args) != 1:
        print(__doc__)
        return 2
    image = open(args[0], "rb").read()
    for page, keys, want in CASES:
        name = {EXP_GET1: "Copy as:", EXP_PUT1: "Put file:"}[page]
        what = f"{name} keys {keys}"
        ex = Explorer(image, keys)
        try:
            got = ex.run(page)
        except AssertionError as e:
            check(False, f"{what}: {e}")
            continue
        check(got == want, f"{what}: went to {got}, not {want}")
        check(ex.fx229_at_osword == [0],
              f"{what}: OSWORD 0 ran with *FX229 {ex.fx229_at_osword}, not [0]")
        check(ex.fx229 == 1, f"{what}: left *FX229 {ex.fx229}, not the explorer's 1")
        check(not ex.escape, f"{what}: left an Escape condition pending")
        check(ex.acks == (ESC in keys),
              f"{what}: acknowledged {ex.acks} Escapes")
    print(f"\n{checks} checks, {fails} failures")
    print("EXPLORER PROMPT TESTS FAILED" if fails else "EXPLORER PROMPT TESTS PASSED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
