#!/usr/bin/env python3
"""Run helper 16 (the 1MHz-WiFi ROM loader) on a 6502 with sixteen sideways
banks, and check which bank it chooses to load into.

After CTRL-BREAK the ROM that an earlier load put in sideways RAM is still
there.  The loader used to skip every bank that holds a ROM, its own earlier
copy included, so with no other sideways RAM it answered "No SWR" and a new
build of the ROM (the firmware may have changed on a reboot) could not be put
in.  A bank that holds a ROM with this ROM's title, whatever its version, is
now the one to load into; any other ROM is left alone, and so is a real ROM
that cannot be written.

Helper 16 is two pages: page 16 loads, page 20 finds the bank.  They run in
the same &FD00 window and hand over through the page switch at the end of each,
which this model carries out as the firmware does (a write to &FC88 shows
another page).  The run stops at the loader's first access to the disc
registers (&FCA6-&FCAF): the bank selected then is the one it will load into.

usage: check_swr_slot.py 6502code.bin 1mhz-wicfs.rom
"""
import os
import sys

try:
    from py65.devices.mpu6502 import MPU
except ImportError:
    print("py65 not installed (pip install py65): skipped the SWR slot checks")
    sys.exit(1 if os.environ.get("CI") else 0)

HELPER_PAGE = 16
OSWRCH = 0xFFEE
RETURN_TRAP = 0xFF00
SAVED_F4 = 3                     # the ROM selected when the helper was called


class Stop(Exception):
    """The run is over: ("load", bank) or ("brk", message)."""


class Memory:
    def __init__(self, image, banks):
        self.image = image
        self.ram = bytearray(0x10000)
        self.banks = banks             # [(bytearray, writable)] x 16
        self.bank = SAVED_F4
        self.ram[0xF4] = SAVED_F4

    def show(self, page):
        self.ram[0xFD00:0xFE00] = self.image[page * 256:page * 256 + 256]

    def __getitem__(self, a):
        if 0x8000 <= a < 0xC000:
            return self.banks[self.bank][0][a - 0x8000]
        if 0xFCA6 <= a < 0xFCB0:
            raise Stop(("load", self.bank))
        return self.ram[a]

    def __setitem__(self, a, v):
        if 0x8000 <= a < 0xC000:
            data, writable = self.banks[self.bank]
            if writable:
                data[a - 0x8000] = v
        elif a == 0xFE30:
            self.bank = v & 15
        elif a == 0xFC88:
            self.show(v)
        elif 0xFCA6 <= a < 0xFCB0:
            raise Stop(("load", self.bank))
        else:
            self.ram[a] = v

    def __len__(self):
        return 0x10000


def run(image, banks):
    """Call helper 16; ("load", bank, dots, stack depth) or ("brk", text, ...)."""
    mem = Memory(image, banks)
    cpu = MPU(memory=mem)
    mem.show(HELPER_PAGE)
    cpu.sp = 0xFF
    cpu.stPushWord(RETURN_TRAP - 1)
    cpu.pc = 0xFD00
    dots = 0
    try:
        for _ in range(200_000):
            pc = cpu.pc
            if pc == OSWRCH:
                dots += cpu.a == ord(".")
                cpu.pc = (cpu.stPopWord() + 1) & 0xFFFF
                continue
            if pc >= 0xFF00:
                raise AssertionError(f"call to &{pc:04X}")
            if mem.ram[pc] == 0 and 0xFD00 <= pc < 0xFE00:
                text = bytearray()
                a = pc + 2                      # BRK, error number, message
                while mem.ram[a]:
                    text.append(mem.ram[a])
                    a += 1
                raise Stop(("brk", text.decode(), mem.bank, mem.ram[0xF4]))
            cpu.step()
    except Stop as s:
        kind = s.args[0]
        if kind[0] == "load":
            return ("load", kind[1], dots, 0xFF - cpu.sp, mem.ram[0x1FD])
        return ("brk", kind[1], dots, mem.bank, mem.ram[0xF4])
    raise AssertionError("runaway")


def rom_image(title, version=b"0.1.67", fill=0x00):
    """A 16K bank holding a ROM header: title, version, then "(C)" as the
    MOS expects (the copyright offset points at the zero before it)."""
    img = bytearray([fill]) * 0x4000
    body = title + b"\0" + version + b"\0"
    img[9:9 + len(body)] = body
    img[7] = 9 + len(body) - 1
    img[9 + len(body):9 + len(body) + 3] = b"(C)"
    img[6] = 0x82
    return img


checks = fails = 0


def check(ok, what):
    global checks, fails
    checks += 1
    if not ok:
        fails += 1
        print(f"FAIL: {what}")


def banks_of(**kw):
    """Sixteen empty read-only banks, with `kw` (b15=..., b9=...) swapped in:
    each value is (bytearray, writable)."""
    banks = [(bytearray(0x4000), False) for _ in range(16)]
    for k, v in kw.items():
        banks[int(k[1:])] = v
    return banks


def main(args):
    if len(args) != 2:
        print(__doc__)
        return 2
    image = open(args[0], "rb").read()
    shipped = bytearray(open(args[1], "rb").read())
    assert shipped[9:18] == b"1MHz-WiFi", "not the 1MHz-WiFi ROM"
    old = (bytearray(shipped), True)                       # as loaded last time
    older = (bytearray(shipped.replace(b"0.1.67", b"0.1.66")), True)
    mmfs = rom_image(b"MMFS", b"2.00")
    longer = rom_image(b"1MHz-WiFiX")

    # The case that failed: our own ROM in the only sideways RAM.
    r = run(image, banks_of(b15=old))
    check(r[:2] == ("load", 15), f"own ROM in the only RAM bank: {r}")
    # two bytes are the harness's return address, one the loader's saved ROM number
    check(r[3] == 3 and r[4] == SAVED_F4, f"the saved ROM number is on the stack once: {r}")
    check(r[2] == 1, f"it stopped looking at the first bank: {r[2]} banks tried")

    # A test build with an unchanged version number, or an older version:
    # the title decides, not the version.
    r = run(image, banks_of(b15=older))
    check(r[:2] == ("load", 15), f"same title, another version: {r}")

    # Lower banks are looked at when the higher ones are real ROMs.
    r = run(image, banks_of(b15=(bytearray(mmfs), False), b14=(bytearray(mmfs), False), b9=older))
    check(r[:2] == ("load", 9), f"our ROM in bank 9 behind two ROMs: {r}")

    # Another ROM in the only RAM is never replaced.
    mm = (bytearray(mmfs), True)
    r = run(image, banks_of(b15=mm))
    check(r[0] == "brk" and r[1] == "No SWR", f"another ROM in the only RAM bank: {r}")
    check(mm[0] == mmfs, "and that ROM is untouched")
    check(r[3] == SAVED_F4 and r[4] == SAVED_F4, f"and the ROM selected is put back: {r}")
    check(r[2] == 16, f"all sixteen banks were tried: {r[2]}")

    # A title that merely starts the same is another ROM.
    ln = (bytearray(longer), True)
    r = run(image, banks_of(b15=ln))
    check(r[0] == "brk" and ln[0] == longer, f"1MHz-WiFiX is not 1MHz-WiFi: {r}")

    # The same title in a bank that cannot be written (an EPROM) is not
    # something to load into.
    ep = (bytearray(shipped), False)
    r = run(image, banks_of(b15=ep))
    check(r[0] == "brk" and ep[0] == shipped, f"our ROM in an EPROM: {r}")

    # Free sideways RAM is used first when the old copy is lower down, so
    # the new copy is the higher-priority one.
    r = run(image, banks_of(b15=(bytearray(0x4000), True), b9=older))
    check(r[:2] == ("load", 15), f"free RAM above the old copy: {r}")

    # Nothing but empty read-only slots.
    r = run(image, banks_of())
    check(r[0] == "brk" and r[1] == "No SWR", f"no sideways RAM at all: {r}")

    print(f"{checks} checks, {fails} failures")
    print("SWR SLOT TESTS FAILED" if fails else "SWR SLOT TESTS PASSED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
