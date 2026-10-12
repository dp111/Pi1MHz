#!/usr/bin/env python3
"""Run the ROM loader helpers (3-16 and 18) on a 6502 with sixteen sideways
banks, and check which bank each loads into.

After CTRL-BREAK a ROM that an earlier load put in sideways RAM is still
there.  The loaders used to skip every bank holding a ROM, their own earlier
copy included, so with no other sideways RAM they answered "No SWR" and a new
build of the ROM could not be put in without switching off.  Now a RAM bank
holding a ROM with the incoming ROM's title, whatever its version, is the one
to load into, ahead of any free bank; a free RAM bank is the next choice; any
other ROM, and any bank that cannot be written, is left alone.  The title is
taken from the file being loaded, so this holds for every loader.

A loader is two pages: its own reads the file into the Pi's buffer, page 0
(swrinstall) picks the bank and copies it in.  They run in the same &FD00
window and hand over through the page switch at the end of each, which this
model carries out as the firmware does (a write to &FC88 shows another page).
The disc registers at &FCA6-&FCAA are modelled as the FAT service: a
pointer, an auto-incrementing data byte, and a command/status byte; the
commands used are open (2), read (4) and close (3).  A run ends at the
loader's reset (JMP (&FFFC)) or at a BRK.

Based on the slot test in Mark Usher's PR #30.

usage: check_swr_slot.py 6502code.bin
"""
import os
import sys

try:
    from py65.devices.mpu6502 import MPU
except ImportError:
    print("py65 not installed (pip install py65): skipped the SWR slot checks")
    sys.exit(1 if os.environ.get("CI") else 0)

LOADERS = list(range(3, 17)) + [18]
OSWRCH = 0xFFEE
OSBYTE = 0xFFF4
RESET_TRAP = 0xFF80
RETURN_TRAP = 0xFF00
SAVED_F4 = 3                     # the ROM selected when the helper was called
DISC = 0xFCA6                    # pointer (3), data, command/status
CMD_BLOCK = 0xFFFF00             # where the loader writes its commands
BUFFER = 0xF00000                # where a read puts the file


class Stop(Exception):
    """The run is over: ("reset",) or ("brk", message)."""


class Memory:
    def __init__(self, image, banks, files):
        self.image = image
        self.ram = bytearray(0x10000)
        self.banks = banks             # [(bytearray, writable)] x 16
        self.files = files             # name -> bytes
        self.bank = SAVED_F4
        self.ram[0xF4] = SAVED_F4
        self.ram[0xFFFC:0xFFFE] = RESET_TRAP.to_bytes(2, "little")
        self.ptr = 0
        self.pi = {}                   # the Pi's side of the disc registers
        self.status = 0
        self.open = None               # name of the open file
        self.opened = []               # every name asked for
        self.closed = 0
        self.writes = {}               # RAM bank -> bytes written to it
        self.cpu = None
        self.irq_on_write = False      # a RAM bank was written with I clear
        self.os_after_write = []       # OS calls after the first such write
        self.busy = False              # the next status read says busy

    def show(self, page):
        self.ram[0xFD00:0xFE00] = self.image[page * 256:page * 256 + 256]
        if page == 0:
            self.ram[0xFD01] = 0x03    # as helpers.c: release the waitloop

    def command(self):
        block = bytes(self.pi.get(CMD_BLOCK + i, 0) for i in range(64))
        cmd = block[0]
        if cmd == 2:                    # open: 2, 0, 1, name, 0
            name = block[3:block.index(0, 3)].decode()
            self.opened.append(name)
            if name in self.files:
                self.open = name
                self.status = 0
            else:
                self.status = 4
        elif cmd == 4:                  # read 16K to &F00000
            assert self.open, "read with no file open"
            data = self.files[self.open]
            for i, b in enumerate(data[:0x4000]):
                self.pi[BUFFER + i] = b
            self.status = 20 if len(data) < 0x4000 else 0   # as fat_service.c: EOF
        elif cmd == 3:                  # close
            self.open = None
            self.closed += 1
            self.status = 0
        else:
            raise AssertionError(f"unknown command {cmd}")
        self.busy = True                # the Pi answers at its next poll

    def __getitem__(self, a):
        if 0x8000 <= a < 0xC000:
            return self.banks[self.bank][0][a - 0x8000]
        if a == DISC + 3:
            v = self.pi.get(self.ptr, 0)
            self.ptr = (self.ptr + 1) & 0xFFFFFF
            return v
        if a == DISC + 4:
            if self.busy:
                self.busy = False
                return 0x80
            return self.status
        return self.ram[a]

    def __setitem__(self, a, v):
        if 0x8000 <= a < 0xC000:
            data, writable = self.banks[self.bank]
            if writable:               # a write to ROM changes nothing
                if self.writes.get(self.bank, 0) >= 1 and not (self.cpu.p & 0x04):
                    self.irq_on_write = True   # past the RAM probe: the copy
                self.writes[self.bank] = self.writes.get(self.bank, 0) + 1
                data[a - 0x8000] = v
        elif a == 0xFE30:
            self.bank = v & 15
        elif a == 0xFC88:
            self.show(v)
        elif DISC <= a < DISC + 3:
            shift = 8 * (a - DISC)
            self.ptr = (self.ptr & ~(0xFF << shift)) | (v << shift)
        elif a == DISC + 3:
            self.pi[self.ptr] = v
            self.ptr = (self.ptr + 1) & 0xFFFFFF
        elif a == DISC + 4:
            self.command()
        else:
            self.ram[a] = v

    def __len__(self):
        return 0x10000


def run(image, page, banks, files, machine=1):
    """Call helper `page`; returns (result, mem, dots, stack depth)."""
    mem = Memory(image, banks, files)
    cpu = MPU(memory=mem)
    mem.cpu = cpu
    mem.show(page)
    cpu.sp = 0xFF
    cpu.stPushWord(RETURN_TRAP - 1)
    cpu.pc = 0xFD00
    dots = 0
    try:
        for _ in range(2_000_000):
            pc = cpu.pc
            if pc in (OSWRCH, OSBYTE) and any(n > 1 for n in mem.writes.values()):
                mem.os_after_write.append(pc)
            if pc == OSWRCH:
                dots += cpu.a == ord(".")
                cpu.pc = (cpu.stPopWord() + 1) & 0xFFFF
                continue
            if pc == OSBYTE:
                if cpu.a == 0:
                    cpu.x = machine
                cpu.pc = (cpu.stPopWord() + 1) & 0xFFFF
                continue
            if pc == RESET_TRAP:
                raise Stop(("reset",))
            if pc >= 0xFF00:
                raise AssertionError(f"call to &{pc:04X}")
            if mem.ram[pc] == 0 and 0xFD00 <= pc < 0xFE00:
                text = bytearray()
                a = pc + 2                      # BRK, error number, message
                while mem.ram[a]:
                    text.append(mem.ram[a])
                    a += 1
                raise Stop(("brk", text.decode()))
            cpu.step()
    except Stop as s:
        return s.args[0], mem, dots, 0xFF - cpu.sp
    raise AssertionError("runaway")


def rom_image(title, version=b"1.00", fill=0x00):
    """A 16K bank holding a ROM header: title, version, then "(C)" as the
    MOS expects (the copyright offset points at the zero before it)."""
    img = bytearray([fill]) * 0x4000
    body = title + b"\0" + version + b"\0"
    img[9:9 + len(body)] = body
    img[7] = 9 + len(body) - 1
    img[9 + len(body):9 + len(body) + 3] = b"(C)"
    img[6] = 0x82
    for i in range(0x100, 0x4000):     # a body that a short copy would miss
        img[i] = (i * 7 + len(title)) & 0xFF
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


def ram(img=None):
    return (bytearray(img) if img is not None else bytearray(0x4000), True)


def loader_name(image, page):
    """The file a loader page opens, read from its open command."""
    pg = image[page * 256:page * 256 + 256]
    i = pg.index(b"\x02\x00\x01")
    return pg[i + 3:pg.index(0, i + 3)].decode()


def main(args):
    if len(args) != 1:
        print(__doc__)
        return 2
    image = open(args[0], "rb").read()

    new = rom_image(b"Test ROM", b"2.00")
    old = rom_image(b"Test ROM", b"1.99")             # the copy loaded last time
    other = rom_image(b"MMFS", b"2.00")
    longer = rom_image(b"Test ROMX", b"2.00")

    for page in LOADERS:
        name = loader_name(image, page)
        files = {name: bytes(new)}
        what = f"helper {page} ({name})"

        # The case that failed: an earlier copy in the only sideways RAM.
        b15 = ram(old)
        r, mem, dots, depth = run(image, page, banks_of(b15=b15), files)
        check(r == ("reset",), f"{what}: earlier copy in the only RAM bank: {r}")
        check(b15[0] == new, f"{what}: and it is replaced by the new one")
        check(mem.closed == 1 and mem.open is None, f"{what}: the file is closed once")
        # two bytes are the harness's return address, one the saved ROM number
        check(depth == 3 and mem.ram[0x1FD] == SAVED_F4,
              f"{what}: the saved ROM number is on the stack once: {depth}")
        check(not mem.irq_on_write and not mem.os_after_write,
              f"{what}: the copy over a live ROM runs with interrupts off and "
              f"calls no OS routine: {mem.irq_on_write} {mem.os_after_write}")

        # Another ROM in the only RAM is never replaced.
        b15 = ram(other)
        r, mem, dots, depth = run(image, page, banks_of(b15=b15), files)
        check(r == ("brk", "No SWR"), f"{what}: another ROM in the only RAM bank: {r}")
        check(b15[0] == other, f"{what}: and that ROM is untouched")
        check(mem.bank == SAVED_F4 and mem.ram[0xF4] == SAVED_F4,
              f"{what}: and the ROM selected is put back")
        check(mem.closed == 1 and mem.open is None, f"{what}: and the file is closed")
        check(dots == 32, f"{what}: both passes tried all sixteen banks: {dots}")

        # No file: nothing is touched.
        b15 = ram()
        r, mem, dots, depth = run(image, page, banks_of(b15=b15), {})
        check(r == ("brk", "No ROM"), f"{what}: no file: {r}")
        check(mem.opened == [name] and not mem.writes, f"{what}: and no bank is written")
        check(mem.bank == SAVED_F4 and mem.ram[0xF4] == SAVED_F4,
              f"{what}: and the ROM selected is put back")

    # The rest of the cases are the shared page-0 half, so one loader will do.
    page = 16
    files = {loader_name(image, page): bytes(new)}

    # Free RAM into an empty bank, byte for byte.
    b12 = ram()
    r, mem, dots, depth = run(image, page, banks_of(b12=b12), files)
    check(r == ("reset",) and b12[0] == new, "free RAM: the ROM is copied byte for byte")
    check(set(mem.writes) == {12}, f"free RAM: only that bank is written: {mem.writes}")

    # An 8K ROM: the read ends early (status 20) and that is not an error.
    short = bytes(new[:0x2000])
    b12 = ram()
    r, mem, dots, depth = run(image, page, banks_of(b12=b12), {loader_name(image, page): short})
    check(r == ("reset",) and b12[0][:0x2000] == short, f"an 8K ROM loads: {r}")

    # Lower banks are looked at when the higher ones are real ROMs.
    b9 = ram(old)
    r, mem, dots, depth = run(image, page, banks_of(
        b15=(bytearray(other), False), b14=(bytearray(other), False), b9=b9), files)
    check(r == ("reset",) and b9[0] == new, f"earlier copy in bank 9 behind two ROMs: {r}")

    # The earlier copy is replaced even with free RAM above it, so the
    # machine is not left with two copies.
    b15, b9 = ram(), ram(old)
    r, mem, dots, depth = run(image, page, banks_of(b15=b15, b9=b9), files)
    check(r == ("reset",) and b9[0] == new, "free RAM above the earlier copy: the copy is replaced")
    check(b15[0] == bytearray(0x4000) and set(mem.writes) == {9},
          f"and the free bank is left alone: {sorted(mem.writes)}")

    # A title that merely starts the same is another ROM.
    b15 = ram(longer)
    r, mem, dots, depth = run(image, page, banks_of(b15=b15), files)
    check(r == ("brk", "No SWR") and b15[0] == longer, f"Test ROMX is not Test ROM: {r}")

    # ... and so is a shorter one: the file's title must end where the bank's does.
    files_x = {loader_name(image, page): bytes(longer)}
    b15 = ram(new)
    r, mem, dots, depth = run(image, page, banks_of(b15=b15), files_x)
    check(r == ("brk", "No SWR") and b15[0] == new, f"Test ROM is not Test ROMX: {r}")

    # The same title in a bank that cannot be written (an EPROM) is not
    # something to load into, nor is it written.
    ep = (bytearray(old), False)
    r, mem, dots, depth = run(image, page, banks_of(b15=ep), files)
    check(r == ("brk", "No SWR") and ep[0] == old and not mem.writes,
          f"earlier copy in an EPROM: {r}, writes {mem.writes}")

    # Nothing but empty read-only slots.
    r, mem, dots, depth = run(image, page, banks_of(), files)
    check(r == ("brk", "No SWR"), f"no sideways RAM at all: {r}")

    # Helper 18 asks for the Master build on a Master.
    pg18 = loader_name(image, 18)
    m_name = pg18[:-len("B.rom")] + "M.rom"
    b15 = ram()
    r, mem, dots, depth = run(image, 18, banks_of(b15=b15), {m_name: bytes(new)}, machine=3)
    check(r == ("reset",) and mem.opened == [m_name], f"helper 18 on a Master: {mem.opened}")

    print(f"{checks} checks, {fails} failures")
    print("SWR SLOT TESTS FAILED" if fails else "SWR SLOT TESTS PASSED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
