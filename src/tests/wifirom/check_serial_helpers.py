#!/usr/bin/env python3
"""Run helpers 19 (serial modem) and 20 (telnet daemon) from 6502code.bin on
a 6502 and check how they hook the Pi's serial stub into INSV and IRQ1V.

Each vector is taken on its own (the TelnetD ROM's hookin rule): one that is
the stub's already is left and its stash kept; any other is stashed in the
stub and pointed at it.  An IRQ1V into FRED or JIM that is not the stub is
another program's hook, and nothing at all is written.  So a stub of ours
left in IRQ1V (a software BREAK) is taken back, where the helpers used to
refuse it as busy.  Unhooking puts both vectors back and tells the Pi.

The stub is at its default place, &FCCC, and its offset is patched into the
page's LDY # at &FD06 as src/helpers.c does.  The MOS is modelled only as
far as the helpers reach: OSBYTE 134/135/156/3, OSWRCH and OSNEWL.

usage: check_serial_helpers.py 6502code.bin
"""
import os
import sys

try:
    from py65.devices.mpu6502 import MPU
except ImportError:
    print("py65 not installed (pip install py65): skipped the serial helper checks")
    sys.exit(1 if os.environ.get("CI") else 0)

OSWRCH, OSNEWL, OSBYTE = 0xFFEE, 0xFFE7, 0xFFF4
HELPER_WINDOW = 0xFC88           # PAGERTS: LDX #&FF : JMP &FC88
RETURN_TRAP = 0xFF00
INSV, IRQ1V = 0x022A, 0x0204
BASE = 0xCC                      # Serial_addr's default: the stub at &FCCC
STUB = 0xFC00 + BASE
TXOLD, RXOLD, EXOLD, CTRL = STUB + 0x0A, STUB + 0x0D, STUB + 0x2B, STUB + 0x28
LDY_OFF = 0x06                   # SERIAL_HELPER_LDY in src/helpers.c
MOS_INSV, MOS_IRQ1V = 0xE4B3, 0xDC93
MODE, COL, ROW = 3, 5, 9

checks = fails = 0


def check(cond, what):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
        print("  FAIL:", what)


class Stop(Exception):
    pass


class Memory:
    def __init__(self, page):
        self.ram = bytearray(0x10000)
        self.ram[0xFD00:0xFE00] = page
        self.ctrl = []               # writes to the stub's control port
        self.stub_writes = []        # any other write into FRED

    def __getitem__(self, a):
        return self.ram[a]

    def __setitem__(self, a, v):
        if a == CTRL:
            self.ctrl.append(v)
        elif 0xFC00 <= a < 0xFD00:
            self.stub_writes.append(a)
        if 0xFD00 <= a < 0xFE00:
            raise AssertionError(f"write to the helper page &{a:04X}")
        self.ram[a] = v

    def __len__(self):
        return 0x10000

    def word(self, a):
        return self.ram[a] | self.ram[a + 1] << 8

    def setword(self, a, v):
        self.ram[a], self.ram[a + 1] = v & 0xFF, v >> 8


def page_of(image, n, base=BASE):
    page = bytearray(image[n * 256:n * 256 + 256])
    assert page[LDY_OFF] == 0xA0, f"helper {n}: no LDY # at &FD{LDY_OFF:02X} for the Pi to patch"
    page[LDY_OFF + 1] = base
    return page


def setup(mem, insv=MOS_INSV, irq1v=MOS_IRQ1V, stale=False):
    mem.ram[STUB:STUB + 3] = bytes([0xE0, 0x02, 0xD0])   # CPX #2 / BNE: what the Pi writes
    mem.setword(INSV, insv)
    mem.setword(IRQ1V, irq1v)
    if stale:                                          # a stub hooked before a soft BREAK
        mem.setword(RXOLD, MOS_IRQ1V)
        mem.setword(EXOLD, MOS_IRQ1V)


def run(mem, entry, i_flag=False):
    cpu = MPU(memory=mem)
    cpu.sp = 0xFF
    cpu.p = (cpu.p | 0x04) if i_flag else (cpu.p & ~0x04)
    cpu.stPushWord(RETURN_TRAP - 1)
    cpu.pc = entry
    out = bytearray()
    osbytes = []
    for _ in range(100_000):
        pc = cpu.pc
        if pc == HELPER_WINDOW:
            check(cpu.x == 0xFF, f"leaves through &FC88 with X=&{cpu.x:02X}")
            ret = cpu.stPopWord() + 1
            check(ret == RETURN_TRAP and cpu.sp == 0xFF, f"stack at exit: return &{ret:04X}, SP &{cpu.sp:02X}")
            check(bool(cpu.p & 0x04) == i_flag, "the interrupt flag as it came")
            return out.decode("latin-1"), osbytes
        if pc == OSWRCH:
            out.append(cpu.a)
        elif pc == OSNEWL:
            out += b"\n\r"
        elif pc == OSBYTE:
            osbytes.append((cpu.a, cpu.x, cpu.y))
            if cpu.a == 135:
                cpu.y = MODE
            elif cpu.a == 134:
                cpu.x, cpu.y = COL, ROW
        if pc in (OSWRCH, OSNEWL, OSBYTE):
            cpu.pc = (cpu.stPopWord() + 1) & 0xFFFF
            continue
        if pc >= 0xFE00 or pc < 0xFD00:
            raise AssertionError(f"call to &{pc:04X}")
        cpu.step()
    raise AssertionError("runaway")


def helper(image, n):
    telnet = n == 20
    name = f"helper {n}"
    on = "TelnetD\n\r" if telnet else " Serial: Pi\r\n"
    off = " Off\n\r" if telnet else " Serial: 6850\r\n"
    busy = " ?\n\r" if telnet else " IRQ1V busy\r\n"
    notin = " ?\n\r" if telnet else " Not hooked\r\n"
    none = " ?\n\r" if telnet else " No port\r\n"
    report = [0x80 + COL, 0xE0 + ROW, 0x10 + MODE] if telnet else [1]
    fx3_on = [(3, 1, 0)] if telnet else []
    fx3_off = [(3, 0, 0)] if telnet else []

    def hooked(mem):
        return mem.word(INSV) == STUB and mem.word(IRQ1V) == STUB + 0x0C

    def stash_ok(mem):
        return (mem.word(TXOLD) == MOS_INSV and mem.word(RXOLD) == MOS_IRQ1V
                and mem.word(EXOLD) == MOS_IRQ1V)

    # 1. the MOS's own vectors: both stashed, both into the stub, the Pi told
    for i_flag in (False, True):
        mem = Memory(page_of(image, n))
        setup(mem)
        text, ob = run(mem, 0xFD00, i_flag)
        check(hooked(mem) and stash_ok(mem), f"{name}: MOS vectors hooked (I={i_flag})")
        check(mem.ctrl == report, f"{name}: control port {mem.ctrl}")
        reads = [135, 134] if telnet else []          # MODE, then the cursor
        check([o[0] for o in ob[:len(reads)]] == reads and ob[len(reads):] == [(156, 0x40, 0x1F)] + fx3_on,
              f"{name}: OSBYTEs {ob}")
        check(text == on, f"{name}: says {text!r}")

    # 2. again, already hooked: the vectors stay, the stash is not overwritten with the stub
    text, ob = run(mem, 0xFD00)
    check(hooked(mem) and stash_ok(mem), f"{name}: hooked twice keeps the stash")
    check(mem.ctrl == report * 2 and text == on, f"{name}: hooked twice: {mem.ctrl} {text!r}")

    # 3. our stub left in IRQ1V (a software BREAK put INSV back): taken back, not refused
    mem = Memory(page_of(image, n))
    setup(mem, irq1v=STUB + 0x0C, stale=True)
    text, ob = run(mem, 0xFD00)
    check(hooked(mem) and stash_ok(mem) and text == on and mem.ctrl == report,
          f"{name}: a stale stub in IRQ1V is accepted: {text!r}, INSV &{mem.word(INSV):04X}, "
          f"stash &{mem.word(RXOLD):04X}")

    # 4. another program's IRQ1V in FRED or JIM: refused, nothing written
    for foreign in (0xFC50, 0xFD20, STUB + 0x0D):
        mem = Memory(page_of(image, n))
        setup(mem, irq1v=foreign)
        text, ob = run(mem, 0xFD00)
        check(mem.word(INSV) == MOS_INSV and mem.word(IRQ1V) == foreign and not mem.stub_writes
              and not mem.ctrl and text == busy and not ob,
              f"{name}: foreign IRQ1V &{foreign:04X} refused: {text!r}, writes {mem.stub_writes}")

    # 5. unhook: both back, the Pi told first
    mem = Memory(page_of(image, n))
    setup(mem)
    run(mem, 0xFD00)
    text, ob = run(mem, 0xFD03)
    check(mem.word(INSV) == MOS_INSV and mem.word(IRQ1V) == MOS_IRQ1V, f"{name}: unhooked")
    check(mem.ctrl == report + [0] and ob == fx3_off and text == off, f"{name}: unhook: {mem.ctrl} {ob} {text!r}")

    # 6. unhook when not hooked: nothing changes
    text, ob = run(mem, 0xFD03)
    check(mem.word(INSV) == MOS_INSV and mem.ctrl == report + [0] and text == notin and not ob,
          f"{name}: not hooked: {text!r}")

    # 7. no stub (Serial_addr=-1: the Pi patches 0, or the stub missing)
    for base, sig in ((0, True), (BASE, False)):
        mem = Memory(page_of(image, n, base))
        setup(mem)
        if not sig:
            mem.ram[STUB] = 0
        for entry in (0xFD00, 0xFD03):
            text, ob = run(mem, entry)
            check(mem.word(INSV) == MOS_INSV and mem.word(IRQ1V) == MOS_IRQ1V and not mem.ctrl
                  and text == none and not ob, f"{name}: no stub (base {base}, entry &{entry:04X}): {text!r}")


def main(path):
    image = open(path, "rb").read()
    helper(image, 19)
    helper(image, 20)
    print(f"{checks} checks, {fails} failures")
    print("SERIAL HELPER TESTS FAILED" if fails else "SERIAL HELPER TESTS PASSED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
