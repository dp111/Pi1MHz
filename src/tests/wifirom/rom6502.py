"""Run a sideways ROM's service entry on an NMOS 6502 with a stub MOS.

Enough of a BBC Micro / Electron for the paths that run on every machine
whether or not the Pi answers: reset (service 1), unrecognised commands
(service 4) and *HELP (service 9).  The ROM sits in a bank that is writable
(sideways RAM) or not (an EPROM), so both can be checked.

SimPi is the other end of the 1MHz bus for the commands that talk to the Pi
(*WGET): the FCA6-FCAA service mailbox, answering each command number with
whatever result the test scripts.

The MOS is modelled only as far as those paths reach.  Anything else the ROM
calls - an OSWORD, an OSBYTE below &A6 that is not listed, a jump into the OS
ROM - raises Unmodelled, so a test can never pass because the model quietly
did nothing.  An opcode the NMOS 6502 does not have raises too: py65 would
step over it, and a Model B or an Electron would not.

Needs py65 (pip install py65).
"""
import re

from py65.devices.mpu6502 import MPU

ROM_BASE = 0x8000
ROM_END = 0xC000
SERVICE = ROM_BASE + 3
LINE_BUF = 0x0700          # where a test puts the command line; (&F2) -> here
RETURN_TRAP = 0xFF00       # the MOS's side of the service call
BRK_TRAP = 0xFF02

OSRDCH, OSASCI, OSNEWL, OSWRCH = 0xFFE0, 0xFFE3, 0xFFE7, 0xFFEE
OSWORD, OSBYTE, OSCLI = 0xFFF1, 0xFFF4, 0xFFF7
OS_ENTRIES = {0xFFB9: "OSRDRM", 0xFFCE: "OSFIND", 0xFFD1: "OSGBPB",
              0xFFD4: "OSBPUT", 0xFFD7: "OSBGET", 0xFFDA: "OSARGS",
              0xFFDD: "OSFILE", OSRDCH: "OSRDCH", 0xFFE9: "OSFSC",
              OSWORD: "OSWORD", OSCLI: "OSCLI"}

MACHINE_ID = {"electron": 0x01, "bbc": 0xFF, "master": 0xF5}   # OSBYTE &81,0,&FF


class Unmodelled(Exception):
    """The ROM did something this model does not cover."""


class RomError(Exception):
    """The ROM raised a MOS error (BRK)."""
    def __init__(self, number, message):
        super().__init__(f"BRK &{number:02X} \"{message}\"")
        self.number = number
        self.message = message


class SimPi:
    """The Pi's side of the services mailbox at &FCA6-&FCAA.

    &FCA6-&FCA8 hold a 24-bit JIM address, &FCA9 reads and writes the byte
    there and steps the address on, and a write of &F0-&FF to &FCAA runs the
    command whose number is in the first byte of the page it names (&F0 the
    page at &FFF000, the service driver's &FF the one at &FFFF00).  The
    result comes back in &FCAA with bit 7 clear.  &FCFD-&FCFF select the
    page of the same JIM memory that &FD00-&FDFF shows, which is where the
    ROM's driver keeps a reply for read_buffer.  `results` maps a command
    number to the result code (a list is consumed one entry per command,
    the last one repeating); a command not listed answers 0.  `replies`
    maps a command number to {JIM address: byte} the Pi publishes with it.
    """
    def __init__(self, results=None, replies=None):
        self.results = {k: (list(v) if isinstance(v, (list, tuple)) else [v])
                        for k, v in (results or {}).items()}
        self.replies = replies or {}
        self.jim = {}
        self.addr = 0
        self.result = 0
        self.commands = []         # every command number run, in order
        self.select = {0xFD: 0, 0xFE: 0, 0xFF: 0}   # JIM page selectors

    def _window(self, a):
        s = self.select
        return s[0xFD] << 24 | s[0xFE] << 16 | s[0xFF] << 8 | (a & 0xFF)

    def read(self, a):
        reg = a - 0xFC00
        if reg == 0xA6:
            return self.addr & 0xFF
        if reg == 0xA7:
            return (self.addr >> 8) & 0xFF
        if reg == 0xA8:
            return (self.addr >> 16) & 0xFF
        if reg == 0xA9:
            v = self.jim.get(self.addr, 0)
            self.addr = (self.addr + 1) & 0xFFFFFF
            return v
        if reg == 0xAA:
            return self.result
        if 0xFD00 <= a < 0xFE00:
            return self.jim.get(self._window(a), 0)
        raise Unmodelled(f"read of I/O &{a:04X}")

    def write(self, a, v):
        reg = a - 0xFC00
        if reg == 0xA6:
            self.addr = (self.addr & 0xFFFF00) | v
        elif reg == 0xA7:
            self.addr = (self.addr & 0xFF00FF) | v << 8
        elif reg == 0xA8:
            self.addr = (self.addr & 0x00FFFF) | v << 16
        elif reg == 0xA9:
            self.jim[self.addr] = v
            self.addr = (self.addr + 1) & 0xFFFFFF
        elif reg == 0xAA:
            if v < 0xF0:
                raise Unmodelled(f"service command &{v:02X}, not a dispatch &F0-&FF")
            self._dispatch(0xFF0000 | v << 8)
        elif reg in (0xFD, 0xFE, 0xFF):
            self.select[reg] = v
        elif 0xFD00 <= a < 0xFE00:
            self.jim[self._window(a)] = v
        else:
            raise Unmodelled(f"write &{v:02X} to I/O &{a:04X}")

    def _dispatch(self, page):
        number = self.jim.get(page, 0)
        self.commands.append(number)
        queue = self.results.get(number, [0])
        self.result = queue.pop(0) if len(queue) > 1 else queue[0]
        for addr, byte in self.replies.get(number, {}).items():
            self.jim[addr] = byte


class Memory:
    """64 KiB; writes to the ROM bank are dropped unless it is writable.
    With a SimPi, &FC00-&FDFF is the 1MHz bus; without one it is Unmodelled."""
    def __init__(self, image, writable, pi=None):
        self.ram = bytearray(0x10000)
        self.ram[ROM_BASE:ROM_BASE + len(image)] = image
        self.writable = writable
        self.pi = pi

    def __getitem__(self, a):
        if self.pi and 0xFC00 <= a < 0xFE00:
            return self.pi.read(a)
        return self.ram[a]

    def __setitem__(self, a, v):
        if ROM_BASE <= a < ROM_END and not self.writable:
            return
        if self.pi and 0xFC00 <= a < 0xFE00:
            self.pi.write(a, v)
            return
        if a >= ROM_END:
            raise Unmodelled(f"write &{v:02X} to OS ROM / I/O &{a:04X}")
        self.ram[a] = v

    def __len__(self):
        return 0x10000


class Beeb:
    def __init__(self, image, writable=True, machine="bbc", reset_type=2,
                 max_steps=2_000_000, pi=None):
        if len(image) != 0x4000:
            raise ValueError(f"ROM image is {len(image)} bytes, not 16384")
        self.pi = pi
        self.mem = Memory(image, writable, pi)
        self.cpu = MPU(memory=self.mem)
        self.machine = machine
        self.max_steps = max_steps
        # OSBYTE &A6-&FF read/write the MOS variables: new = (old AND Y) EOR X.
        self.osvars = {0xD7: 0x80, 0xFD: reset_type}
        self.osbytes = []          # (A, X, Y) of every OSBYTE, in order
        self.text = ""
        self._vdu_skip = 0
        self.mem.ram[0xFFFE] = BRK_TRAP & 0xFF
        self.mem.ram[0xFFFF] = BRK_TRAP >> 8

    # ---- output -------------------------------------------------------
    def _wrch(self, c):
        if self._vdu_skip:
            self._vdu_skip -= 1
        elif c == 23:                      # VDU 23,n,8 bytes: a glyph
            self._vdu_skip = 9
        elif c == 13:
            self.text += "\n"
        elif 32 <= c < 127:
            self.text += chr(c)
        elif c in (7, 10):
            pass
        else:
            self.text += f"<{c}>"

    # ---- the stub MOS ---------------------------------------------------
    def _osbyte(self, cpu):
        a, x, y = cpu.a, cpu.x, cpu.y
        self.osbytes.append((a, x, y))
        if a >= 0xA6:
            old = self.osvars.get(a, 0)
            self.osvars[a] = (old & y) ^ x
            cpu.x = old
            cpu.y = self.osvars.get((a + 1) & 0xFF, 0)
        elif a == 0x81 and x == 0 and y == 0xFF:
            cpu.x = cpu.y = MACHINE_ID[self.machine]
        elif a == 0x87:                    # character at cursor, screen mode
            cpu.x, cpu.y = 32, 7
        elif a == 0x13:                    # wait for vertical sync
            pass
        else:
            raise Unmodelled(f"OSBYTE &{a:02X},&{x:02X},&{y:02X}")

    def _trap(self, pc):
        cpu = self.cpu
        if pc == OSWRCH:
            self._wrch(cpu.a)
        elif pc == OSASCI:
            if cpu.a == 13:
                self._wrch(10)
            self._wrch(cpu.a)
        elif pc == OSNEWL:
            self._wrch(10)
            self._wrch(13)
        elif pc == OSBYTE:
            self._osbyte(cpu)
        elif pc == BRK_TRAP:
            cpu.stPop()                    # P
            ret = cpu.stPopWord()          # BRK pushes its address + 2
            number = self.mem[ret - 1]
            msg = bytearray()
            a = ret
            while self.mem[a] != 0 and len(msg) < 255:
                msg.append(self.mem[a])
                a += 1
            raise RomError(number, msg.decode("latin-1"))
        else:
            raise Unmodelled(f"call to {OS_ENTRIES.get(pc, f'&{pc:04X}')}")
        # every modelled call returns like an RTS from the OS
        cpu.pc = (cpu.stPopWord() + 1) & 0xFFFF

    # ---- running a service call ---------------------------------------
    def service(self, reason, slot=5, y=0, line=None):
        """Offer service call `reason` as the MOS does; return (A, X, Y).

        `line` is the command text for calls 4 and 9: it goes in LINE_BUF,
        (&F2) points there and Y indexes it, as the MOS leaves them."""
        if line is not None:
            data = line.encode("latin-1") + b"\r"
            self.mem.ram[LINE_BUF:LINE_BUF + len(data)] = data
            self.mem.ram[0xF2] = LINE_BUF & 0xFF
            self.mem.ram[0xF3] = LINE_BUF >> 8
        self.mem.ram[0xF4] = slot          # the MOS's current-ROM copy
        self.cpu.sp = 0xFF
        return self.call(SERVICE, reason, slot, y)

    def call(self, address, a=0, x=0, y=0):
        """JSR to `address` in the ROM; return (A, X, Y) at its RTS.
        The flags it returned with are left in self.cpu.p."""
        cpu = self.cpu
        cpu.a, cpu.x, cpu.y = a, x, y
        cpu.p = 0x34                       # I set, D clear: as the MOS calls
        ret = RETURN_TRAP - 1
        cpu.stPushWord(ret)
        cpu.pc = address
        for _ in range(self.max_steps):
            pc = cpu.pc
            if pc == RETURN_TRAP:
                self.sp = cpu.sp           # back where it was before the JSR
                return cpu.a, cpu.x, cpu.y
            if pc >= ROM_END:
                self._trap(pc)
                continue
            op = self.mem[pc]
            if MPU.instruct[op] is MPU.inst_not_implemented:
                raise Unmodelled(f"opcode &{op:02X} at &{pc:04X} is not NMOS 6502")
            cpu.step()
        raise Unmodelled(f"runaway: no return after {self.max_steps} steps, pc &{cpu.pc:04X}")


def command_names(image):
    """The ROM's command table, read from the image itself: names in ASCII,
    each followed by its handler address high byte first (bit 7 set ends the
    name), a bare address ending the table.  Found by the first entry, WGET."""
    at = image.find(b"WGET")
    if at < 0 or image[at + 4] < 0x80:
        raise ValueError("command table not found")
    names = []
    while image[at] < 0x80:
        end = at
        while image[end] < 0x80:
            end += 1
        names.append(image[at:end].decode("ascii"))
        at = end + 2
    return names


def read_buffer_address(image):
    """Where the driver's read_buffer starts: PHP, SEI, LDA abs (the page
    shadow), JSR (select the page), LDA &FD00,X, PLP.  write_buffer pushes
    A before its LDA, so only read_buffer has this shape."""
    hits = [m.start() for m in re.finditer(
        rb"\x08\x78\xAD..\x20..\xBD\x00\xFD\x28", image, re.DOTALL)]
    if len(hits) != 1:
        raise ValueError(f"read_buffer found {len(hits)} times, not once")
    return ROM_BASE + hits[0]
