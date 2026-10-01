"""Run a sideways ROM's service entry on an NMOS 6502 with a stub MOS.

Enough of a BBC Micro / Electron for the paths that run on every machine
whether or not the Pi answers: reset (service 1), unrecognised commands
(service 4) and *HELP (service 9).  The ROM sits in a bank that is writable
(sideways RAM) or not (an EPROM), so both can be checked.

The MOS is modelled only as far as those paths reach.  Anything else the ROM
calls - an OSWORD, an OSBYTE below &A6 that is not listed, a jump into the OS
ROM - raises Unmodelled, so a test can never pass because the model quietly
did nothing.  An opcode the NMOS 6502 does not have raises too: py65 would
step over it, and a Model B or an Electron would not.

Needs py65 (pip install py65).
"""
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


class Memory:
    """64 KiB; writes to the ROM bank are dropped unless it is writable."""
    def __init__(self, image, writable):
        self.ram = bytearray(0x10000)
        self.ram[ROM_BASE:ROM_BASE + len(image)] = image
        self.writable = writable

    def __getitem__(self, a):
        return self.ram[a]

    def __setitem__(self, a, v):
        if ROM_BASE <= a < ROM_END and not self.writable:
            return
        if a >= ROM_END:
            raise Unmodelled(f"write &{v:02X} to OS ROM / I/O &{a:04X}")
        self.ram[a] = v

    def __len__(self):
        return 0x10000


class Beeb:
    def __init__(self, image, writable=True, machine="bbc", reset_type=2,
                 max_steps=2_000_000):
        if len(image) != 0x4000:
            raise ValueError(f"ROM image is {len(image)} bytes, not 16384")
        self.mem = Memory(image, writable)
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
        cpu = self.cpu
        if line is not None:
            data = line.encode("latin-1") + b"\r"
            self.mem.ram[LINE_BUF:LINE_BUF + len(data)] = data
            self.mem.ram[0xF2] = LINE_BUF & 0xFF
            self.mem.ram[0xF3] = LINE_BUF >> 8
        self.mem.ram[0xF4] = slot          # the MOS's current-ROM copy
        cpu.a, cpu.x, cpu.y = reason, slot, y
        cpu.sp = 0xFF
        cpu.p = 0x34                       # I set, D clear: as the MOS calls
        ret = RETURN_TRAP - 1
        cpu.stPushWord(ret)
        cpu.pc = SERVICE
        for _ in range(self.max_steps):
            pc = cpu.pc
            if pc == RETURN_TRAP:
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
