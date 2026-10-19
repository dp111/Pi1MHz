#!/usr/bin/env python3
"""Run telnetd.rom's service entry on an NMOS 6502 (py65) and check what every
machine with the ROM fitted depends on:

1. reset (service 1) with the Pi's serial stub at &FCCC and telnetd on (the byte the
   Pi sets in the ROM's workspace) hooks the Beeb in, once, as helper 20 would; with
   telnetd off or the stub moved it touches nothing; either way it gives A, X and Y
   back as they came, with the stack as it was, so the MOS still has its slot number
   and the call is left unclaimed for the ROMs below.  The MOS's routines the claims
   go on to are kept in the workspace in the Pi's FRED page, never in Beeb RAM;
2. after a POWER-ON reset it waits for the Pi, which is booting for about two
   seconds while the Beeb runs its ROM scan: it polls the window until it
   answers (up to a bound), then runs the helper; if no Pi ever answers it gives
   up, runs nothing, and still gives everything back;
3. after any other reset (soft BREAK, CTRL-BREAK) it does not wait: the Pi is
   already up;
4. every other reason code is passed on untouched, with no OS call;
5. the image has the header the MOS and the helpers' own loader look for: a
   service ROM with a (C) string, 16 KiB;
6. only NMOS 6502 instructions are executed, so it runs on a B and a Master.

usage: check_rom.py ROM
"""
import re
import sys

try:
    from py65.devices.mpu6502 import MPU
    from py65.memory import ObservableMemory
except ImportError:
    import os
    print("py65 not installed (pip install py65): skipped the telnetd.rom checks")
    sys.exit(1 if os.environ.get("CI") else 0)

ROM_BASE = 0x8000
SERVICE = ROM_BASE + 3
HELPER = 0xFC88
OSBYTE = 0xFFF4
RETURN_TRAP = 0xFF00
WAIT_TURNS = 22            # the ROM's bound on polls after a power-on
# Every opcode the ROM may execute (the service entry's JMP included).
ALLOWED = {0x4C, 0x48, 0x68, 0x8A, 0x98, 0xA8, 0xAA,     # JMP PHA PLA TXA TYA TAY TAX
           0xAD, 0xA9, 0xA2, 0xA0, 0xC9, 0xE0,           # LDA abs/imm LDX# LDY# CMP# CPX#
           0xD0, 0xF0, 0x88, 0xCA, 0x38, 0xE9,           # BNE BEQ DEY DEX SEC SBC#
           0xBD, 0x8D, 0x99, 0xAC,                       # LDA abs,X  STA abs  STA abs,Y  LDY abs
           0xB1, 0xDD, 0x29, 0xB0, 0xC8, 0xE8,           # LDA (zp),Y  CMP abs,X  AND#  BCS  INY  INX
           0xBA, 0xAE, 0x9D, 0x18,                       # TSX  LDX abs  STA abs,X  CLC (claim REMV)
           0x08, 0x28, 0x78, 0xA5, 0x85, 0xCD,           # PHP  PLP  SEI  LDA zp  STA zp  CMP abs (claim KEYV)
           0x20, 0x60, 0x09}                             # JSR RTS  ORA# (the hook-in's reports)

checks = fails = 0


def check(cond, what):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
        print("  FAIL:", what)


STUB = 0xFCCC
WS = STUB - 21                 # the ROM's workspace, kept by the Pi below the stub's escape routine
REMVOLD, KEYVOLD, RDCHVOLD, SHIFTFLAG, TELNETON = WS, WS + 2, WS + 4, WS + 6, WS + 7
STUB_SIG = {STUB: 0xE0, STUB + 1: 0x02, STUB + 2: 0xD0, STUB + 0x30: 0x60}   # what stubok looks at


def call(rom, a, x, y, pi_after=0, reset_type=2, pokes=None, watch=(), key_after=None, window=None,
         stub_after="pi", telnet_on=1):
    """Run the service entry as the MOS would.

    pi_after   the helper window answers (&8E) from the read after this many
               polls; None: never.  0 means it is up already.
    stub_after the Pi's serial stub at &FCCC reads as itself from the look after this many;
               None: never (a moved stub, or no Pi).  "pi": as pi_after.
    telnet_on  the workspace byte the Pi sets when telnetd is on (TELNETON).
    reset_type what OSBYTE &FD returns in X: 0 soft BREAK, 1 power-on, 2 CTRL-BREAK.
    pokes      {address: byte} set in memory before the call (the MOS vectors, a stub).
    watch      addresses whose writes are recorded in out["writes"], in order.
    Returns a dict of what happened."""
    if stub_after == "pi":
        stub_after = pi_after
    mem = ObservableMemory()
    out = {"helper_writes": [], "osbytes": [], "reads": 0, "writes": [], "stub_reads": 0}

    def on_stub_read(addr):
        if addr == STUB:
            out["stub_reads"] += 1
        up = stub_after is not None and out["stub_reads"] > stub_after
        return STUB_SIG[addr] if up else 0xFF

    mem.subscribe_to_read(list(STUB_SIG), on_stub_read)
    mem._subject[TELNETON] = telnet_on

    def on_helper_write(addr, value):
        out["helper_writes"].append(value)
        return 0x8E                      # the window's opcode stays in place

    def on_helper_read(addr):
        out["reads"] += 1
        up = pi_after is not None and out["reads"] > pi_after
        return 0x8E if up else 0xFF      # a bus nobody drives reads as 0xFF

    mem.subscribe_to_write([HELPER], on_helper_write)
    mem.subscribe_to_read([HELPER], on_helper_read)
    if watch:
        mem.subscribe_to_write(list(watch), lambda addr, value: out["writes"].append((addr, value)))
    for i, b in enumerate(rom):
        mem[ROM_BASE + i] = b
    for addr, b in (pokes or {}).items():
        mem._subject[addr] = b
    # The window after a page select, and a helper that, like the real one,
    # leaves A, X and Y changed (it ends LDX #&FF, and prints with A and Y):
    # STX &FC88 / LDA #&55 / LDY #&66 / LDX #&FF / RTS
    for i, b in enumerate((0x8E, 0x88, 0xFC, 0xA9, 0x55, 0xA0, 0x66, 0xA2, 0xFF, 0x60)):
        mem._subject[HELPER + i] = b
    for i, b in enumerate(window or ()):     # a bus that is not a Pi's window after its first byte
        mem._subject[HELPER + 1 + i] = b
    mem._subject[OSBYTE] = 0x60          # the MOS's OSBYTE: modelled below, then RTS
    mem._subject[0xFFEE] = 0x60          # OSWRCH and OSNEWL: the name printed after a direct hook-in
    mem._subject[0xFFE7] = 0x60
    mem._subject[RETURN_TRAP] = 0xEA
    mpu = MPU(memory=mem, pc=SERVICE)
    mpu.a, mpu.x, mpu.y = a, x, y
    mpu.sp = 0xF0
    ret = RETURN_TRAP - 1                # the MOS's JSR: the ROM's RTS lands on RETURN_TRAP
    mem._subject[0x100 + mpu.sp] = ret >> 8
    mem._subject[0x100 + mpu.sp - 1] = ret & 0xFF
    mpu.sp -= 2
    out["sp_before"] = mpu.sp + 2
    opcodes = []
    for _ in range(40_000_000):
        if mpu.pc == RETURN_TRAP:
            break
        if mpu.pc == HELPER:
            out["jumped"] = True         # the ROM went into the helper window
        if mpu.pc == OSBYTE:             # OSBYTE &FD,0,&FF: X = the last reset type
            out["osbytes"].append((mpu.a, mpu.x, mpu.y))
            if mpu.a == 0xFD:
                mpu.x = reset_type
            elif mpu.a == 0x79:              # the keyboard scan from key 16: &FF none, else the key
                mpu.x = 0x62 if key_after is not None and out["reads"] >= key_after else 0xFF
            elif mpu.a == 135:           # the character at the cursor (X) and the MODE (Y)
                mpu.x, mpu.y = 0x20, 7
            elif mpu.a == 134:           # the text cursor: X column, Y row
                mpu.x, mpu.y = 5, 9
        elif ROM_BASE <= mpu.pc < ROM_BASE + 0x4000:
            opcodes.append(mem._subject[mpu.pc])
        mpu.step()
    else:
        raise RuntimeError("the ROM did not return")
    out.update(regs=(mpu.a, mpu.x, mpu.y), sp_after=mpu.sp, opcodes=opcodes, mem=mem)
    return out


OSWRCH = 0xFFEE
OSNEWL = 0xFFE7
COLS = (80, 40, 20, 80, 40, 20, 40, 40)
ROWS = (32, 32, 32, 25, 32, 32, 25, 25)


def run_command(rom, line, mode=1, screen=None, cur=(3, 4), hooked=True, stream0=1, slot=6, y0=0, reason=4):
    """Run service call 4 with a command line, against a model of the MOS: OSBYTE 135 (the
    character at the text cursor, Y the mode), 134 (the cursor), 3 (the output streams) and
    OSWRCH (VDU 31 moves the cursor on the screen; what is sent while bit 0 of the stream is
    set is what the Pi receives)."""
    screen = screen or {}
    mem = ObservableMemory()
    for i, b in enumerate(rom):
        mem[ROM_BASE + i] = b
    text = line.encode() + b"\r"
    for i, b in enumerate(text):
        mem._subject[0x300 + i] = b
    mem._subject[0xF2], mem._subject[0xF3] = 0x00, 0x03
    if hooked:
        mem._subject[0x022A], mem._subject[0x022B] = 0xCC, 0xFC
    for i, b in enumerate((0x8E, 0x88, 0xFC, 0x60)):
        mem._subject[HELPER + i] = b
    helper = []
    mem.subscribe_to_write([HELPER], lambda addr, value: (helper.append(value), 0x8E)[1])
    for addr in (OSBYTE, OSWRCH, OSNEWL, RETURN_TRAP):
        mem._subject[addr] = 0x60
    mem._subject[RETURN_TRAP] = 0xEA
    st = {"stream": stream0, "cx": cur[0], "cy": cur[1], "args": [], "pi": [], "streams": [], "calls": 0, "printed": []}
    mpu = MPU(memory=mem, pc=SERVICE)
    mpu.a, mpu.x, mpu.y = reason, slot, y0
    mpu.sp = 0xF0
    ret = RETURN_TRAP - 1
    mem._subject[0x100 + mpu.sp] = ret >> 8
    mem._subject[0x100 + mpu.sp - 1] = ret & 0xFF
    mpu.sp -= 2
    sp_before = mpu.sp + 2
    opcodes = []
    for _ in range(40_000_000):
        if mpu.pc == RETURN_TRAP:
            break
        if mpu.pc == OSBYTE:
            st["calls"] += 1
            if mpu.a == 135:
                mpu.x = screen.get((st["cx"], st["cy"]), 32)
                mpu.y = mode
            elif mpu.a == 134:
                mpu.x, mpu.y = st["cx"], st["cy"]
            elif mpu.a == 3:
                old = st["stream"]
                st["stream"] = mpu.x
                st["streams"].append(mpu.x)
                mpu.x = old
        elif mpu.pc == OSNEWL:
            st["printed"] += [10, 13]
        elif mpu.pc == OSWRCH:
            c = mpu.a
            st["printed"].append(c)
            if st["stream"] & 1:
                st["pi"].append(c)
            if not st["stream"] & 2:                     # the screen drivers are on
                if st["args"]:
                    st["args"].append(c)
                    if len(st["args"]) == 3:
                        st["cx"], st["cy"] = st["args"][1], st["args"][2]
                        st["args"] = []
                elif c == 31:
                    st["args"] = [31]
        elif ROM_BASE <= mpu.pc < ROM_BASE + 0x4000:
            opcodes.append(mem._subject[mpu.pc])
        mpu.step()
    else:
        raise RuntimeError("the ROM did not return")
    st.update(regs=(mpu.a, mpu.x, mpu.y), sp_after=mpu.sp, sp_before=sp_before, helper=helper, opcodes=opcodes)
    return st


def run_remv(rom, x, v, c, old_y, old_c, screen=None, cur=(3, 4), hooked=True, a=0x5A, mode=1, win=None, pokes=None, flaky=None):
    """Call the REMV handler the way the MOS's extended vector entry does, with the MOS's own
    routine modelled by a stub that returns Y=old_y and C=old_c.  Returns what happened."""
    boot = call(rom, 1, 6, 0, pi_after=0, reset_type=2)
    handler = boot["mem"]._subject[0x0DE1] | (boot["mem"]._subject[0x0DE2] << 8)
    mem = ObservableMemory()
    for i, b in enumerate(rom):
        mem[ROM_BASE + i] = b
    screen = screen or {}
    mem._subject[REMVOLD], mem._subject[REMVOLD + 1] = 0x00, 0x03    # the MOS's routine at &0300, as the claim keeps it
    old = (0xA0, old_y, 0x38 if old_c else 0x18, 0xEE, 0x10, 0x03, 0x60)   # LDY #y / SEC or CLC / INC &0310 / RTS
    for i, b in enumerate(old):
        mem._subject[0x300 + i] = b
    mem._subject[0x310] = 0
    if hooked:
        mem._subject[0x022A], mem._subject[0x022B] = 0xCC, 0xFC
    for addr, b in (pokes or {}).items():
        mem._subject[addr] = b
    for addr in (OSBYTE, OSWRCH, RETURN_TRAP):
        mem._subject[addr] = 0x60
    mem._subject[RETURN_TRAP] = 0xEA
    wl, wb, wr, wt = win if win else (0, ROWS[mode] - 1, COLS[mode] - 1, 0)
    for i, v8 in enumerate((wl, wb, wr, wt)):
        mem._subject[0x308 + i] = v8                    # the MOS's VDU variables 8-11: the text window
    # cur is relative to the window, as OSBYTE 134 gives it; the model keeps it absolute
    st = {"stream": 1, "cx": wl + cur[0], "cy": wt + cur[1], "args": [], "pi": [], "calls": 0, "ctl": []}
    mem.subscribe_to_write([0xFCF4], lambda addr, value: st["ctl"].append(value))   # the stub's control port
    mpu = MPU(memory=mem, pc=handler)
    mpu.a, mpu.x, mpu.y = a, x, 7
    mpu.p = (mpu.p & ~0x41) | (0x40 if v else 0) | (1 if c else 0)
    mpu.sp = 0xF0
    ret = RETURN_TRAP - 1
    mem._subject[0x100 + mpu.sp] = ret >> 8
    mem._subject[0x100 + mpu.sp - 1] = ret & 0xFF
    mpu.sp -= 2
    sp_before = mpu.sp + 2
    for _ in range(40_000_000):
        if mpu.pc == RETURN_TRAP:
            break
        if mpu.pc == OSBYTE:
            st["calls"] += 1
            if mpu.a == 135:
                mpu.x = screen.get((st["cx"], st["cy"]), 32)
                mpu.y = mode
                if flaky and flaky.get((st["cx"], st["cy"]), 0) > 0:     # a read that comes back unrecognised
                    flaky[(st["cx"], st["cy"])] -= 1
                    mpu.x = 0
            elif mpu.a == 134:
                mpu.x, mpu.y = st["cx"] - mem._subject[0x308], st["cy"] - mem._subject[0x30B]
            elif mpu.a == 160:
                mpu.x, mpu.y = mem._subject[0x300 + mpu.x], 0
            elif mpu.a == 3:
                old_stream = st["stream"]
                st["stream"] = mpu.x
                mpu.x = old_stream
        elif mpu.pc == OSWRCH:
            if st["stream"] & 1:
                st["pi"].append(mpu.a)
            if not st["stream"] & 2:
                if st["args"]:
                    st["args"].append(mpu.a)
                    if len(st["args"]) == 3:
                        l, bt, r, t = (mem._subject[0x308 + i] for i in range(4))
                        ax, ay = l + st["args"][1], t + st["args"][2]
                        if ax <= r and ay <= bt:                # outside the window: ignored, as the MOS does
                            st["cx"], st["cy"] = ax, ay
                        st["args"] = []
                elif mpu.a == 31:
                    st["args"] = [31]
        mpu.step()
    else:
        raise RuntimeError("the handler did not return")
    st["win_after"] = tuple(mem._subject[0x308 + i] for i in range(4))
    st.update(a=mpu.a, x=mpu.x, y=mpu.y, c=mpu.p & 1, v=(mpu.p >> 6) & 1, sp_after=mpu.sp, sp_before=sp_before,
              old_calls=mem._subject[0x310], mem=mem)
    return st


KEYV_TABLE = 0xD940            # the MOS's table of default vectors (OS 1.20), as &FFB7 says
MOS_KEYV = 0x0400              # a stand-in for the MOS's KEYV routine
MODEL_REMV = 0x0500            # ... and for REMV, with a one-byte keyboard buffer
KEYV_OPCODES = {0x70, 0x2C, 0xB8, 0x6C, 0x09, 0x90, 0xC0}   # BVS BIT abs CLV JMP (ind) ORA# BCC CPY#


def asm(mem, org, src):
    from py65.assembler import Assembler
    a = Assembler(MPU(memory=mem))
    for line in src.strip().splitlines():
        line = line.split(";")[0].strip()
        if not line:
            continue
        b = a.assemble(line, org)
        for i, v in enumerate(b):
            mem._subject[org + i] = v
        org += len(b)


def model_remv(mem, buf):
    """REMV (via &22C) as a one-byte keyboard buffer holding buf (None: empty), as the MOS's
    answers: V set looks, with the byte in A and Y its offset in the buffer (OS 1.20 &E472,
    &E491); V clear takes it, the byte in Y; C set when empty.  &531 is the count, &532 the
    bytes taken."""
    mem._subject[0x022C], mem._subject[0x022D] = MODEL_REMV & 0xFF, MODEL_REMV >> 8
    asm(mem, MODEL_REMV, """
        BVS $0510
        LDA $0531
        BEQ $0520
        DEC $0531
        LDY $0530
        INC $0532
        CLC
        RTS
    """)
    asm(mem, 0x0510, """
        LDA $0531
        BEQ $0520
        LDA $0530
        LDY #$E5
        CLC
        RTS
    """)
    asm(mem, 0x0520, """
        SEC
        RTS
    """)
    mem._subject[0x0530], mem._subject[0x0531] = (buf, 1) if buf is not None else (0, 0)


def keyv_pokes(keyv=(0x02, 0xEF)):
    """The MOS's default vector table at &D940 with KEYV's default &EF02, KEYV as given."""
    return {0xFFB7: KEYV_TABLE & 0xFF, 0xFFB8: KEYV_TABLE >> 8, KEYV_TABLE + 0x28: 0x02, KEYV_TABLE + 0x29: 0xEF,
            0x0228: keyv[0], 0x0229: keyv[1]}




def run_keyv(rom, v, c, answer=0x00, buf=None, a=0x11, x=0x22, y=0x33, zp=(0x5A, 0xA5), flag=0):
    """Call the KEYV handler the way the MOS's extended vector entry does: the MOS's own KEYV
    (found through the table) is a stand-in that records the registers and C, V it was given
    and answers A=answer; REMV is the one-byte buffer of model_remv; SHIFTFLAG holds flag."""
    boot = call(rom, 1, 6, 0, pi_after=0, reset_type=2, pokes=keyv_pokes())
    bm = boot["mem"]._subject
    handler = bm[0x0D9F + 60] | (bm[0x0D9F + 61] << 8)
    mem = ObservableMemory()
    for i, b in enumerate(rom):
        mem[ROM_BASE + i] = b
    for addr, b in keyv_pokes((0x3C, 0xFF)).items():
        mem._subject[addr] = b
    mem._subject[KEYV_TABLE + 0x28], mem._subject[KEYV_TABLE + 0x29] = MOS_KEYV & 0xFF, MOS_KEYV >> 8
    mem._subject[KEYVOLD], mem._subject[KEYVOLD + 1] = MOS_KEYV & 0xFF, MOS_KEYV >> 8   # as the claim keeps it
    asm(mem, MOS_KEYV, f"""
        PHP
        STA $0420
        STX $0421
        STY $0422
        PLA
        STA $0423
        INC $0424
        LDA #${answer:02X}
        LDX #$EE
        LDY #$DD
        RTS
    """)
    model_remv(mem, buf)
    mem._subject[SHIFTFLAG] = flag
    mem._subject[0xA8], mem._subject[0xA9] = zp
    mem._subject[RETURN_TRAP] = 0xEA
    mpu = MPU(memory=mem, pc=handler)
    mpu.a, mpu.x, mpu.y = a, x, y
    mpu.p = (mpu.p & ~0x41) | (0x40 if v else 0) | (1 if c else 0)
    mpu.sp = 0xF0
    ret = RETURN_TRAP - 1
    mem._subject[0x100 + mpu.sp] = ret >> 8
    mem._subject[0x100 + mpu.sp - 1] = ret & 0xFF
    mpu.sp -= 2
    sp_before = mpu.sp + 2
    opcodes = []
    zp_writes = []
    mem.subscribe_to_write(list(range(0x100)), lambda addr, value: zp_writes.append(addr))
    for _ in range(100_000):
        if mpu.pc == RETURN_TRAP:
            break
        if ROM_BASE <= mpu.pc < ROM_BASE + 0x4000:
            opcodes.append(mem._subject[mpu.pc])
        mpu.step()
    else:
        raise RuntimeError("the KEYV handler did not return")
    m = mem._subject
    return {"a": mpu.a, "sp_after": mpu.sp, "sp_before": sp_before, "calls": m[0x0424],
            "given": (m[0x0420], m[0x0421], m[0x0422]), "given_cv": (m[0x0423] & 1, (m[0x0423] >> 6) & 1),
            "left": m[0x0531], "taken": m[0x0532], "zp": (m[0xA8], m[0xA9]), "opcodes": opcodes,
            "flag": m[SHIFTFLAG], "zp_writes": zp_writes}


def run_mos_page_wait(rom, mos, buf, shift_after=None):
    """With the real MOS 1.20 (its image named by MOS12): KEYV claimed by this ROM, through the
    MOS's own extended vector entry and its own KEYV routine; buf (a key, or None) put in the
    keyboard buffer by the MOS's own INSV; then the paged-mode wait (OS 1.20 &CB0D: CLC, JSR
    &E9D9, SEC, BPL &CB0E).  shift_after: the Pi's trip sets SHIFTFLAG after that many
    instructions of waiting (None: never).  Returns (the wait ended, buffer not empty, the flag
    after)."""
    boot = call(rom, 1, 6, 0, pi_after=0, reset_type=2, pokes=keyv_pokes())
    bm = boot["mem"]._subject
    mem = ObservableMemory()
    for i, b in enumerate(mos):
        mem._subject[0xC000 + i] = b
    for i, b in enumerate(rom):
        mem._subject[ROM_BASE + i] = b
    for addr in (0x0228, 0x0229, 0x0D9F + 60, 0x0D9F + 61, 0x0D9F + 62, KEYVOLD, KEYVOLD + 1):
        mem._subject[addr] = bm[addr]
    tab = 0xD940
    for v in (0x2A, 0x2C):                     # INSV and REMV: the MOS's own routines
        mem._subject[0x200 + v], mem._subject[0x201 + v] = mos[tab + v - 0xC000], mos[tab + v + 1 - 0xC000]
    start = mos[0xE447 - 0xC000]               # the keyboard buffer empty, as a reset leaves it
    mem._subject[0x02D8] = mem._subject[0x02E1] = start
    insert = buf if buf is not None else 0
    asm(mem, 0x2000, f"""
        LDA #${insert:02X}
        LDX #$00
        JSR $2010
        JSR $CB0D
        NOP
    """)
    if buf is None:
        asm(mem, 0x2010, "RTS")
    else:
        asm(mem, 0x2010, "JMP ($022A)")
    mem._subject[0xFF] = 0                     # no ESCAPE
    mem._subject[SHIFTFLAG] = 0
    mpu = MPU(memory=mem, pc=0x2000)
    mpu.sp = 0xF0
    for i in range(200_000):
        if shift_after is not None and i == shift_after:
            mem._subject[SHIFTFLAG] = 1
        if mpu.pc == 0x200A:
            return True, mem._subject[0x02D8] != mem._subject[0x02E1], mem._subject[SHIFTFLAG]
        mpu.step()
    return False, mem._subject[0x02D8] != mem._subject[0x02E1], mem._subject[SHIFTFLAG]


RDCHV_TABLE_OFF = 0x10          # RDCHV's place in the MOS's table of default vectors
MOS_RDCHV = 0x0600              # a stand-in for the MOS's RDCHV routine: A=&EE, C set, counted at &0610


def run_rdchv(rom, buf, escape=False):
    """Call the RDCHV handler as the MOS's extended vector entry does, with REMV a buffer of the bytes
    in buf (look: A = the next, take: Y = it; C set when empty) and the MOS's RDCHV a stand-in."""
    boot = call(rom, 1, 6, 0, pi_after=0, reset_type=2,
                pokes={**keyv_pokes(), KEYV_TABLE + RDCHV_TABLE_OFF: 0x00, KEYV_TABLE + RDCHV_TABLE_OFF + 1: 0x06,
                       0x0210: 0x00, 0x0211: 0x06})
    bm = boot["mem"]._subject
    handler = bm[0x0D9F + 24] | (bm[0x0D9F + 25] << 8)
    mem = ObservableMemory()
    for i, b in enumerate(rom):
        mem[ROM_BASE + i] = b
    mem._subject[RDCHVOLD], mem._subject[RDCHVOLD + 1] = MOS_RDCHV & 0xFF, MOS_RDCHV >> 8
    asm(mem, MOS_RDCHV, """
        INC $0610
        LDA #$EE
        SEC
        RTS
    """)
    # REMV: a buffer at &0700 (count at &06FF), V set looks (A = byte, Y = junk), V clear takes (Y = byte)
    mem._subject[0x022C], mem._subject[0x022D] = 0x00, 0x05
    asm(mem, 0x0500, """
        BVS $0520
        LDA $06FF
        BEQ $0540
        LDY $0700
        LDX #$00
        LDA $0701
        STA $0700
        LDA $0702
        STA $0701
        DEC $06FF
        LDX #$00
        TYA
        CLC
        RTS
    """)
    asm(mem, 0x0520, """
        LDA $06FF
        BEQ $0540
        LDA $0700
        LDY #$E5
        CLC
        RTS
    """)
    asm(mem, 0x0540, """
        SEC
        RTS
    """)
    for i, b in enumerate(buf):
        mem._subject[0x0700 + i] = b
    mem._subject[0x06FF] = len(buf)
    mem._subject[0xFF] = 0x80 if escape else 0
    mem._subject[RETURN_TRAP] = 0xEA
    mpu = MPU(memory=mem, pc=handler)
    mpu.a, mpu.x, mpu.y = 0x11, 0x22, 0x33
    mpu.sp = 0xF0
    ret = RETURN_TRAP - 1
    mem._subject[0x100 + mpu.sp] = ret >> 8
    mem._subject[0x100 + mpu.sp - 1] = ret & 0xFF
    mpu.sp -= 2
    sp_before = mpu.sp + 2
    for _ in range(100_000):
        if mpu.pc == RETURN_TRAP:
            break
        mpu.step()
    else:
        raise RuntimeError("the RDCHV handler did not return")
    m = mem._subject
    return {"a": mpu.a, "x": mpu.x, "y": mpu.y, "c": mpu.p & 1, "sp_after": mpu.sp, "sp_before": sp_before,
            "mos": m[0x0610], "left": m[0x06FF], "boot": boot}


def main(path):
    rom = open(path, "rb").read()

    check(len(rom) == 16384, "the image is 16 KiB")
    check(rom[0:3] == b"\0\0\0", "no language entry")
    check(rom[3] == 0x4C, "service entry is a JMP")
    check(rom[6] == 0x82, "type byte: a service ROM, no language")
    off = rom[7]
    check(rom[off] == 0 and rom[off + 1:off + 4] == b"(C)",
          "the copyright offset points at the 0 before (C), as the helpers' loader tests")
    check(rom[8] == 1, "binary version byte")
    check(rom[9:16] == b"TelnetD", "title")
    ver = rom[17:rom.index(0, 17)]
    check(re.fullmatch(rb"\d+\.\d+ b\d+", ver) is not None, f"*HELP shows a version and a build number: {ver!r}")

    # 1. reset with the Pi already up (CTRL-BREAK): the stub at &FCCC is hooked in by the ROM itself
    # (*FX3,1 and the vectors into the stub); helper 20 is never run
    for a, x, y in ((1, 5, 3), (1, 15, 0), (1, 0, 255), (1, 7, 7)):
        r = call(rom, a, x, y, pi_after=0, reset_type=2)
        m = r["mem"]._subject
        check(r["helper_writes"] == [] and not r.get("jumped"), f"CTRL-BREAK ({x},{y}): no helper is run, got {r['helper_writes']}")
        check((3, 1, 0) in r["osbytes"] and (m[0x22A], m[0x22B]) == (0xCC, 0xFC), f"CTRL-BREAK ({x},{y}): hooked in (*FX3,1, INSV at the stub): {r['osbytes']}")
        check(r["regs"] == (a, x, y), f"CTRL-BREAK: A,X,Y given back as {(a, x, y)}, got {r['regs']}")
        check(r["sp_after"] == r["sp_before"], f"CTRL-BREAK: stack balanced ({r['sp_before']:#x} -> {r['sp_after']:#x})")
        check(set(r["opcodes"]) <= ALLOWED, f"only NMOS opcodes: {sorted(set(r['opcodes']) - ALLOWED)}")
        check(r["stub_reads"] == 1 and r["reads"] == 0, f"CTRL-BREAK: no waiting - one look at the stub, got {r['stub_reads']} (window {r['reads']})")

    # 1a. PR review: the ROM touches nothing unless the Pi says telnetd is on (TELNETON), and
    # nothing with the stub moved (Serial_addr=, the workspace not where the ROM keeps it)
    for what, kw in (("telnetd off", dict(telnet_on=0)), ("stub moved", dict(stub_after=None))):
        r = call(rom, 1, 9, 2, pi_after=0, reset_type=2, watch=(0x022A, 0x022B, 0x0204, 0x0205, 0x022C, 0x0228, 0x0210), **kw)
        check(r["writes"] == [] and r["osbytes"] == [(0xFD, 0, 0xFF)] and r["helper_writes"] == [] and not r.get("jumped"),
              f"{what}: no vector, no *FX3 or *FX156, no helper: {r['writes']} {r['osbytes']}")
        check(r["regs"] == (1, 9, 2) and r["sp_after"] == r["sp_before"], f"{what}: registers and stack given back")

    # 1b. a software BREAK can leave the interrupt vector at the stub the helper installed
    # before it: a stub is recognised and the vector it replaced put back, so the helper
    # does not mistake the old hook for somebody else's; and the Pi is told it was a reset.
    stub = {0xFCCC: 0xE0, 0xFCCD: 0x02, 0xFCD9: 0x34, 0xFCDA: 0xDC}      # the stub at &CC, keeping &DC34 at +0D
    r = call(rom, 1, 4, 0, pi_after=0, reset_type=0,
             pokes={**stub, 0x0204: 0xD8, 0x0205: 0xFC}, watch=(0x0204, 0x0205))
    check(r["writes"][:2] == [(0x0204, 0x34), (0x0205, 0xDC)], f"a stale stub vector is put back first: {r['writes']}")
    check(r["regs"] == (1, 4, 0) and r["sp_after"] == r["sp_before"],
          "... and the hook-in still happens and the registers and stack come back")
    r = call(rom, 1, 4, 0, pi_after=0, reset_type=0,
             pokes={0x0204: 0x50, 0x0205: 0xFC, 0xFC44: 0xA9, 0xFC45: 0x00}, watch=(0x0204, 0x0205))
    check(r["writes"] == [], f"a vector into FRED that is not our stub is left alone: {r['writes']}")
    r = call(rom, 1, 4, 0, pi_after=0, reset_type=0, pokes={0x0204: 0x00, 0x0205: 0xD0}, watch=(0x0204, 0x0205))
    check(r["writes"] == [(0x0204, 0xD8), (0x0205, 0xFC)], f"an ordinary vector is not taken for a stale stub: only the hook-in moves it: {r['writes']}")
    r = call(rom, 1, 4, 0, pi_after=0, reset_type=0, pokes={0x022A: 0xCC, 0x022B: 0xFC}, watch=(0xFCF4,))
    check(r["writes"][-1:] == [(0xFCF4, 0x26)], f"a hooked-in stub is told: the reset marker, &26 'can be asked for the screen, tells every key taken, reads LITKEY bytes as they are', goes to its control port last: {r['writes']}")
    r = call(rom, 1, 4, 0, pi_after=0, reset_type=0, pokes={0x022A: 0xCC, 0x022B: 0xFC, 0x022D: 0xFF}, watch=(0xFCF4,))
    check(r["writes"][-1:] == [(0xFCF4, 0x20)], f"REMV already somebody's extended vector: the marker says it cannot: {r['writes']}")

    # 1c. the claim of REMV: the vector goes through the MOS's extended entry, the table says
    # where, and the MOS's own routine is kept in the ROM's workspace in the Pi's FRED page
    # (PR review: not in USERV's slot of the table, nor any other RAM the ROM does not own)
    r = call(rom, 1, 6, 0, pi_after=0, reset_type=2, pokes={0x022C: 0x34, 0x022D: 0xE5},
             watch=(0x022C, 0x022D, REMVOLD, REMVOLD + 1, 0x0D9F, 0x0DA0, 0x0DA1, 0x0DE1, 0x0DE2, 0x0DE3))
    w = dict(r["writes"])
    check(w.get(0x022C) == 0x42 and w.get(0x022D) == 0xFF, f"REMV points at the MOS's extended entry for vector 22 (&FF42): {r['writes']}")
    check(w.get(REMVOLD) == 0x34 and w.get(REMVOLD + 1) == 0xE5, "the MOS's own REMV is kept in the workspace")
    check(not ({0x0D9F, 0x0DA0, 0x0DA1} & set(w)), f"USERV's slot of the table (&0D9F-&0DA1) is not touched: {r['writes']}")
    check(w.get(0x0DE3) == 6 and 0x80 <= w.get(0x0DE2, 0) < 0xC0, "the table entry names this ROM's slot and an address in it")
    r = call(rom, 1, 6, 0, pi_after=0, reset_type=2, pokes={0x022C: 0x42, 0x022D: 0xFF}, watch=(0x022C, 0x022D, REMVOLD, 0x0DE1))
    check(r["writes"] == [], f"already an extended vector: left alone, nothing kept over: {r['writes']}")
    r = call(rom, 1, 6, 0, pi_after=None, reset_type=2, watch=(0x022C, 0x022D))
    check(r["writes"] == [], "no Pi: no vector is touched")
    r = call(rom, 1, 4, 0, pi_after=0, reset_type=0, telnet_on=0, pokes={0x022A: 0xCC, 0x022B: 0xFC}, watch=(0xFCF4,))
    check(r["writes"] == [], f"telnetd off: no marker either: {r['writes']}")
    r = call(rom, 1, 4, 0, pi_after=None, reset_type=2, pokes={0x022A: 0xCC, 0x022B: 0xFC}, watch=(0xFCF4,))
    check(r["writes"] == [], "no Pi: nothing is written")

    # 1b. the Pi's serial stub at &FCCC (any firmware, with or without 6502code.bin): the ROM hooks
    # it in itself, as helper 20 would, and no helper runs
    STUBA = 0xFCCC
    stub = {STUBA: 0xE0, STUBA + 1: 0x02, STUBA + 2: 0xD0, STUBA + 0x30: 0x60,
            0x22A: 0x34, 0x22B: 0x12, 0x204: 0x78, 0x205: 0x56, 0x22C: 0xA0, 0x22D: 0xE4}
    for pi_after in (0, None):                 # the helper window up, or no 6502code.bin at all
        r = call(rom, 1, 9, 2, pi_after=pi_after, stub_after=0, reset_type=2, pokes=stub, watch=(STUBA + 0x28,))
        m = r["mem"]._subject
        check(r["helper_writes"] == [], f"stub at &FCCC (window {'up' if pi_after == 0 else 'absent'}): no helper is run")
        check((m[0x22A], m[0x22B], m[0x204], m[0x205]) == (0xCC, 0xFC, 0xD8, 0xFC),
              f"... INSV and IRQ1V point into the stub, got INSV &{m[0x22B]:02X}{m[0x22A]:02X} IRQ1V &{m[0x205]:02X}{m[0x204]:02X}")
        check((m[STUBA + 0x0A], m[STUBA + 0x0B], m[STUBA + 0x0D], m[STUBA + 0x0E], m[STUBA + 0x2B], m[STUBA + 0x2C])
              == (0x34, 0x12, 0x78, 0x56, 0x78, 0x56), "... the old vectors kept in the stub first")
        ctl = [v for a_, v in r["writes"]]
        check(ctl[:3] == [0x80 + 5, 0xE0 + 9, 0x10 + 7] and ctl[3:] == [0x26],
              f"... the Pi told column, row, MODE (telnet), then the reset marker: got {[hex(v) for v in ctl]}")
        ob = [o[0] for o in r["osbytes"]]
        check(156 in ob and (3, 1, 0) in r["osbytes"], f"... the 6850 quietened and *FX3,1: {r['osbytes']}")
        check(r["regs"] == (1, 9, 2) and r["sp_after"] == r["sp_before"], f"... registers and stack given back, got {r['regs']}")
    # already hooked (a second reset with the vectors still in the stub): only the report
    hooked = dict(stub)
    hooked.update({0x22A: 0xCC, 0x22B: 0xFC, 0x204: 0xD8, 0x205: 0xFC, STUBA + 0x0A: 0x34, STUBA + 0x0B: 0x12})
    r = call(rom, 1, 9, 2, pi_after=0, reset_type=0, pokes=hooked, watch=(STUBA + 0x28,))
    m = r["mem"]._subject
    check(m[STUBA + 0x0A] == 0x34 and (m[0x22A], m[0x22B]) == (0xCC, 0xFC) and [v for a_, v in r["writes"]][:3] == [0x85, 0xE9, 0x17],
          "stub already hooked: the stashed vectors are not overwritten, and the Pi still gets the report")

    # 1c. a soft reset (CALL !-4, the telnet F12) resets a second processor as the BREAK key would:
    # the Tube ULA's parasite reset flag on and off; power-on and CTRL-BREAK reset it in hardware
    for rt, want in ((0, [(0xFEE0, 0xA0), (0xFEE0, 0x20)]), (1, []), (2, [])):
        r = call(rom, 1, 9, 2, pi_after=0, reset_type=rt, watch=(0xFEE0,))
        check(r["writes"] == want, f"reset type {rt}: the parasite reset pulse {'given' if want else 'not given'} ({r['writes']})")
        check(r["regs"] == (1, 9, 2) and r["sp_after"] == r["sp_before"], f"reset type {rt}: registers and stack given back")

    # 2. power-on: the Pi comes up while the ROM waits, and the ROM hooks it in once
    for after in (0, 1, 7, WAIT_TURNS - 1):
        r = call(rom, 1, 9, 2, pi_after=after, reset_type=1)
        check(r["helper_writes"] == [] and r["osbytes"].count((3, 1, 0)) == 1,
              f"power-on, Pi ready after {after} polls: hooked in once, no helper: {r['osbytes']}")
        check(r["regs"] == (1, 9, 2) and r["sp_after"] == r["sp_before"], f"power-on, Pi after {after}: registers and stack intact, got {r['regs']}")

    # ... and when it never does, the ROM gives up after its bound and carries on
    r = call(rom, 1, 9, 2, pi_after=None, reset_type=1)
    check(r["helper_writes"] == [], "power-on, no Pi ever: the helper is not run")
    check(r["regs"] == (1, 9, 2) and r["sp_after"] == r["sp_before"], f"power-on, no Pi: registers and stack intact, got {r['regs']} sp {r['sp_before']:#x}->{r['sp_after']:#x}")
    check(WAIT_TURNS <= r["reads"] <= WAIT_TURNS + 3, f"power-on, no Pi: gave up after about {WAIT_TURNS} polls, not {r['reads']}")
    # a key pressed while it waits ends the wait at once (a machine with no Pi1MHz need not sit through it)
    r = call(rom, 1, 9, 2, pi_after=None, reset_type=1, key_after=3)
    check(r["helper_writes"] == [] and 3 <= r["reads"] <= 5 and r["regs"] == (1, 9, 2) and r["sp_after"] == r["sp_before"],
          f"power-on, no Pi, a key after 3 polls: the wait ends there ({r['reads']} polls), registers and stack intact")
    check((0x79, 0x10, r["osbytes"][-1][2]) in r["osbytes"], f"... the keyboard read from key 16 (SHIFT and CTRL not counted): {r['osbytes'][:4]}")
    # a bus that reads &8E at &FC88 but not the rest of STX &FC88 is not a Pi: never jumped into
    for rt in (0, 1, 2):
        r = call(rom, 1, 9, 2, pi_after=0, reset_type=rt, window=(0xFC, 0xFC))
        check(not r.get("jumped") and r["regs"] == (1, 9, 2) and r["sp_after"] == r["sp_before"],
              f"reset type {rt}: &8E alone at &FC88 (not 88 FC after it) is not a Pi: the window not called (jumped {r.get('jumped')})")

    # 3. a soft BREAK or CTRL-BREAK with no Pi: no waiting at all
    for rt in (0, 2):
        r = call(rom, 1, 9, 2, pi_after=None, reset_type=rt)
        check(r["helper_writes"] == [] and r["stub_reads"] == 1 and r["reads"] == 0,
              f"reset type {rt}, no Pi: one look and no wait (stub {r['stub_reads']}, window {r['reads']})")
        check(r["regs"] == (1, 9, 2) and r["sp_after"] == r["sp_before"], f"reset type {rt}, no Pi: registers and stack intact")

    # 4. everything else is passed on, with no OS call and no look at the Pi
    # A ROM filing system operation (service &0D) is passed on untouched too, and with the stub
    # hooked in (INSV in it) the Pi is told, &22 to the stub's control port at +28.
    stub = {0x22A: 0xCC, 0x22B: 0xFC}
    for hooked in (True, False):
        r = call(rom, 0x0D, 6, 0x0B, pi_after=0, reset_type=1, pokes=stub if hooked else None, watch=(0xFCF4,))
        check(r["helper_writes"] == [] and r["osbytes"] == [], f"service &0D (hooked={hooked}): no helper, no OS call")
        check(r["regs"] == (0x0D, 6, 0x0B) and r["sp_after"] == r["sp_before"], f"service &0D (hooked={hooked}): passed on untouched, got {r['regs']}")
        check(r["writes"] == ([(0xFCF4, 0x22)] if hooked else []), f"service &0D (hooked={hooked}): the Pi told once, got {r['writes']}")
    for reason in (0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 17, 254, 255):
        r = call(rom, reason, 6, 4, pi_after=0, reset_type=1)
        check(r["helper_writes"] == [] and r["osbytes"] == [] and r["reads"] == 0, f"service {reason}: no helper, no OS call, no look at the Pi")
        check(r["regs"] == (reason, 6, 4) and r["sp_after"] == r["sp_before"], f"service {reason}: passed on untouched")

    # 5. the request: the screen goes to the Pi, character by character, and nothing else changes
    scr = {(0, 0): 0x41, (5, 0): 0x42, (39, 31): 0x43, (2, 7): 0x44, (3, 3): 0x20, (9, 9): 0x00}
    for mode in range(8):
        cols, rows = COLS[mode], ROWS[mode]
        s2 = {k: v for k, v in scr.items() if k[0] < cols and k[1] < rows}
        r = run_remv(rom, 0, 0, 0, 0xFF, 0, screen=s2, cur=(1, 2), mode=mode)
        expect = [26, 12]
        for y in range(rows):
            for x in range(cols):
                ch = s2.get((x, y), 32)
                if ch not in (0, 32):
                    expect += [31, x, y, ch]
        expect += [28, 0, rows - 1, cols - 1, 0, 31, 1, 2]
        check(r["pi"] == expect, f"mode {mode}: the Pi gets VDU 26 and CLS, VDU 31,x,y,char for each character, then the window and the cursor (got {len(r['pi'])} bytes, expected {len(expect)})")
        check((r["cx"], r["cy"]) == (1, 2), f"mode {mode}: the cursor is back where it was on the screen")
        check(r["stream"] == 1, f"mode {mode}: the output streams are put back (stream {r['stream']})")
        check(r["c"] == 1 and r["a"] == 0x5A and r["x"] == 0 and r["sp_after"] == r["sp_before"], f"mode {mode}: reported empty, A and X and the stack come back")

    # 5b. with a text window set: cells outside it are read too, and the window comes back
    scr = {(0, 0): 0x41, (39, 31): 0x42, (6, 9): 0x43}
    r = run_remv(rom, 0, 0, 0, 0xFF, 0, screen=scr, cur=(2, 3), mode=1, win=(5, 20, 30, 8))
    expect = [26, 12, 31, 0, 0, 0x41, 31, 6, 9, 0x43, 31, 39, 31, 0x42, 28, 5, 20, 30, 8, 31, 2, 3]
    check(r["pi"] == expect, f"a text window: every cell is read, then the Pi gets the window and the cursor in it (got {r['pi']})")
    check(r["win_after"] == (5, 20, 30, 8), f"... and the Beeb's window is put back: {r['win_after']}")
    check((r["cx"], r["cy"]) == (5 + 2, 8 + 3), f"... with its cursor where it was: {(r['cx'], r['cy'])}")

    # 5c. *HELP: the title, version and build on a plain *HELP; nothing for a topic; never claimed
    r = run_command(rom, "", reason=9, slot=6, y0=0)
    text = bytes(r["printed"]).decode("latin-1")
    check(re.search(r"TelnetD \d+\.\d+ b\d+", text) is not None, f"*HELP prints the title, version and build: {text!r}")
    check(r["regs"] == (9, 6, 0) and r["sp_after"] == r["sp_before"], f"*HELP is passed on with A, X, Y and the stack as they came: {r['regs']}")
    r = run_command(rom, "ADFS", reason=9, slot=6, y0=0)
    check(r["printed"] == [] and r["regs"] == (9, 6, 0), "*HELP ADFS: nothing printed, passed on")

    # 6. the REMV handler: a byte from the keyboard buffer passes through untouched, except the
    # one the Pi uses to ask for the screen, which is swallowed (nothing for the program) and
    # answered by sending the screen
    scr = {(0, 0): 0x41, (5, 2): 0x42}
    r = run_remv(rom, 0, 0, 0, 0x41, 0)
    check(r["y"] == 0x41 and r["c"] == 0 and r["a"] == 0x5A and r["x"] == 0 and r["old_calls"] == 1 and r["pi"] == [] and r["calls"] == 0,
          f"an ordinary key goes through: Y={r['y']:#x} C={r['c']}, one call to the MOS, no screen sent")
    r = run_remv(rom, 0, 0, 1, 0x00, 1)
    check(r["c"] == 1 and r["old_calls"] == 1 and r["pi"] == [] and r["calls"] == 0, "an empty buffer is reported empty and nothing is sent")
    r = run_remv(rom, 0, 0, 0, 0xFE, 0)
    check(r["y"] == 0xFE and r["c"] == 0 and r["pi"] == [], "&FE is a key like any other")
    r = run_remv(rom, 0, 0, 0, 0xFF, 0, screen=scr, cur=(3, 1))
    check(r["c"] == 1 and r["a"] == 0x5A and r["x"] == 0, f"the request is swallowed: the buffer is reported empty, A and X kept (C={r['c']})")
    check(r["pi"][:2] == [26, 12] and 31 in r["pi"] and 0x41 in r["pi"] and 0x42 in r["pi"] and r["pi"][-3:] == [31, 3, 1],
          f"... and the screen is sent: CLS, the characters, the cursor back ({len(r['pi'])} bytes)")
    check(r["stream"] == 1 and (r["cx"], r["cy"]) == (3, 1) and r["sp_after"] == r["sp_before"], "... with the output streams and the cursor as they were, and the stack balanced")
    fl = {(0, 0): 2, (5, 2): 9}
    r = run_remv(rom, 0, 0, 0, 0xFF, 0, screen={(0, 0): 0x41, (5, 2): 0x42}, cur=(3, 1), flaky=fl)
    check(0x41 in r["pi"] and 0x42 not in r["pi"] and r["sp_after"] == r["sp_before"],
          "a cell read as nothing is read again (2 misses: sent; more than the tries: left out), stack balanced")
    r = run_remv(rom, 0, 0, 0, 0xFF, 0, screen={(0, 14): 0x01, (1, 14): 0x45, (2, 14): 0x83}, cur=(3, 1), mode=7)
    cells = [r["pi"][i + 3] for i in range(len(r["pi"]) - 3) if r["pi"][i] == 31 and r["pi"][i + 2] == 14]
    check(cells == [0x81, 0x45, 0x83],
          f"MODE 7: a control code stored below 32 (WordWise-Plus's &01) is sent as &81, never as a VDU code ({cells})")
    r = run_remv(rom, 0, 1, 0, 0xFF, 0, screen=scr)
    check(r["y"] == 0xFF and r["c"] == 0 and r["pi"] == [] and r["old_calls"] == 1, "looking at the byte (V set) leaves it, and sends nothing")
    # A keyboard read gives the 6850 back from the ROM filing system (&EA = 1 with &247 = 2),
    # and only from it: not from the cassette, not the RS423 timeout count, not on a look.
    for fs, ea, v, want in ((2, 1, 0, 0), (2, 0, 0, 0), (2, 0xE7, 0, 0xE7), (0, 1, 0, 1), (2, 1, 1, 1)):
        r = run_remv(rom, 0, v, 0, 0x41, 0, pokes={0x247: fs, 0xEA: ea})
        check(r["mem"]._subject[0xEA] == want and r["y"] == 0x41 and r["a"] == 0x5A and r["sp_after"] == r["sp_before"],
              f"&247={fs} &EA={ea:#x} V={v}: &EA left {r['mem']._subject[0xEA]:#x}, want {want:#x}, the key and A kept")
    # The vectors put back behind the Pi's back (a second processor's start, a Master's MOS) while
    # the stub is in telnet mode (its LDX # at +15 is 0): a keyboard read hooks them in again, with
    # the old ones kept, and *FX3 bit 0 set again; nothing is reported to the Pi.
    ST = 0xFCCC
    lost = {ST: 0xE0, ST + 1: 0x02, ST + 2: 0xD0, ST + 0x30: 0x60, ST + 0x15: 0xA2, ST + 0x16: 0x00,
            0x22A: 0x43, 0x22B: 0xEA, 0x204: 0xFF, 0x205: 0xE5}
    r = run_remv(rom, 0, 0, 0, 0x41, 0, hooked=False, pokes=lost)
    m = r["mem"]._subject
    check((m[0x22A], m[0x22B], m[0x204], m[0x205]) == (0xCC, 0xFC, 0xD8, 0xFC),
          f"lost vectors are hooked in again on a keyboard read: INSV &{m[0x22B]:02X}{m[0x22A]:02X} IRQ1V &{m[0x205]:02X}{m[0x204]:02X}")
    check((m[ST + 0x0A], m[ST + 0x0B], m[ST + 0x0D], m[ST + 0x0E]) == (0x43, 0xEA, 0xFF, 0xE5),
          "... the MOS's vectors kept in the stub to chain to")
    check(m[ST + 0x28] == 0x25 and r["y"] == 0x41 and r["a"] == 0x5A and r["x"] == 0 and r["sp_after"] == r["sp_before"],
          "... the Pi told only the key taken (&25, build 13), and the key, A, X and the stack as they were")
    off = dict(lost)
    off[ST + 0x16] = 0x01                       # the modem's buffer: telnet is off (unhooked on purpose)
    r = run_remv(rom, 0, 0, 0, 0x41, 0, hooked=False, pokes=off)
    m = r["mem"]._subject
    check((m[0x22A], m[0x22B]) == (0x43, 0xEA), "not in telnet mode: the vectors are left alone")
    ours = dict(lost)
    ours.update({0x22A: 0xCC, 0x22B: 0xFC, 0x204: 0xD8, 0x205: 0xFC, ST + 0x0A: 0x43, ST + 0x0B: 0xEA})
    r = run_remv(rom, 0, 0, 0, 0x41, 0, hooked=False, pokes=ours)
    m = r["mem"]._subject
    check(m[ST + 0x0A] == 0x43 and r["calls"] == 0, "hooked in already: nothing done, no OS call (the stub never keeps itself)")
    half = dict(lost)
    half.update({0x22A: 0xCC, 0x22B: 0xFC, ST + 0x0A: 0x43, ST + 0x0B: 0xEA})      # INSV still ours, IRQ1V lost
    r = run_remv(rom, 0, 0, 0, 0x41, 0, hooked=False, pokes=half)
    m = r["mem"]._subject
    check((m[ST + 0x0A], m[ST + 0x0B]) == (0x43, 0xEA) and (m[0x204], m[0x205]) == (0xD8, 0xFC),
          "only IRQ1V lost: it is hooked in again and INSV's kept vector is not overwritten with the stub")
    for x in (1, 2, 3, 9):
        r = run_remv(rom, x, 0, 0, 0xFF, 0, screen=scr)
        check(r["y"] == 0xFF and r["c"] == 0 and r["pi"] == [] and r["old_calls"] == 1 and r["x"] == x,
              f"buffer {x} is not the keyboard: passed to the MOS untouched")
    r = run_remv(rom, 0, 0, 0, 0xFF, 0, screen=scr, hooked=False)
    check(r["c"] == 1 and r["pi"] == [], "with no Pi hooked in, the request byte is still not shown to the program, and nothing is sent")
    for x, v, c in ((0, 0, 0), (0, 0, 1), (0, 1, 0), (2, 0, 1), (3, 1, 1)):
        r = run_remv(rom, x, v, c, 0x41, 0 if c == 0 else 1)
        check(r["sp_after"] == r["sp_before"], f"REMV X={x} V={v} C={c}: stack balanced")

    # 8. ROM build 11: a cursor key, COPY or RETURN that a program takes out of the keyboard
    # buffer is told to the Pi (&24 to the stub's control port), so the copy cursor it draws moves
    # only when the Beeb's does; A, X, Y and the result are as the MOS gave them
    for key in (0x8B, 0x8C, 0x8D, 0x8E, 0x8F, 13):
        r = run_remv(rom, 0, 0, 0, key, 0)
        check(r["ctl"] == [0x24] and r["y"] == key and r["c"] == 0 and r["a"] == 0x5A and r["x"] == 0
              and r["sp_after"] == r["sp_before"],
              f"key &{key:02X} taken: the Pi told (&24) once, Y, C, A, X kept: ctl {r['ctl']} Y={r['y']:#x} C={r['c']}")
    for key in (0x41, 0x8A, 0x90, 0x80, 0x7F, 0xFC, 0xFB):
        r = run_remv(rom, 0, 0, 0, key, 0)
        check(r["ctl"] == [0x25] and r["y"] == key and r["c"] == 0 and r["a"] == 0x5A and r["x"] == 0,
              f"key &{key:02X} taken: told as any other key (&25), Y, C, A, X kept: {r['ctl']}")
    r = run_remv(rom, 0, 1, 0, 0x8B, 0)
    check(r["ctl"] == [], "looking at a COPY (V set) is not taking it: nothing told")
    r = run_remv(rom, 2, 0, 0, 0x8B, 0)
    check(r["ctl"] == [], "a byte of another buffer: nothing told")
    r = run_remv(rom, 0, 0, 1, 0x00, 1)
    check(r["ctl"] == [] and r["c"] == 1, "an empty buffer: nothing told")
    r = run_remv(rom, 0, 0, 0, 0x8B, 0, hooked=False)
    check(r["ctl"] == [] and r["y"] == 0x8B, "no stub hooked in: nothing told, the key given as it is")

    # 9. ROM build 13: RDCHV claimed (while the MOS's own), so a byte after LITKEY is read as it is
    r = run_rdchv(rom, [])
    bm = r["boot"]["mem"]._subject
    check((bm[0x0210], bm[0x0211]) == (0x18, 0xFF), f"RDCHV points at the MOS's extended entry for vector 8 (&FF18): &{bm[0x0211]:02X}{bm[0x0210]:02X}")
    check((bm[RDCHVOLD], bm[RDCHVOLD + 1]) == (0x00, 0x06), "... the MOS's RDCHV kept in the workspace")
    for byte in (0xC5, 0x8B, 0xFF, 0x81, 0xFB):
        r = run_rdchv(rom, [0xFB, byte])
        check(r["a"] == byte and r["c"] == 0 and (r["x"], r["y"]) == (0x22, 0x33) and r["mos"] == 0 and r["left"] == 0
              and r["sp_after"] == r["sp_before"],
              f"LITKEY then &{byte:02X}: read as it is (A={r['a']:#04x} C={r['c']}), X and Y kept, both taken, the MOS's read not used")
    for buf in ([], [0x41], [0xC5], [0x41, 0xFB, 0xC5]):
        r = run_rdchv(rom, buf)
        check(r["mos"] == 1 and r["a"] == 0xEE and r["left"] == len(buf) and (r["x"], r["y"]) == (0x22, 0x33)
              and r["sp_after"] == r["sp_before"],
              f"buffer {[hex(b) for b in buf]}: the MOS's read as usual, nothing taken here")
    r = run_rdchv(rom, [0xFB], escape=True)
    check(r["mos"] == 1 and r["sp_after"] == r["sp_before"], "LITKEY with no byte after it and ESCAPE pending: the MOS's read (which reports the ESCAPE)")
    r = call(rom, 1, 6, 0, pi_after=0, reset_type=2, pokes={**keyv_pokes(), KEYV_TABLE + RDCHV_TABLE_OFF: 0x00,
             KEYV_TABLE + RDCHV_TABLE_OFF + 1: 0x06, 0x0210: 0x34, 0x0211: 0x12}, watch=(0x0210, 0x0211))
    check(r["writes"] == [], f"RDCHV somebody else's: left alone {r['writes']}")

    # 7. SHIFT for paged mode: KEYV is claimed through the MOS's extended entry for vector 20
    # (&FF3C) while it is the MOS's own routine (the default table at &FFB7 says which)
    r = call(rom, 1, 6, 0, pi_after=0, reset_type=2, pokes=keyv_pokes(),
             watch=(0x0228, 0x0229, 0x0D9F + 60, 0x0D9F + 61, 0x0D9F + 62))
    w = dict(r["writes"])
    check(w.get(0x0228) == 0x3C and w.get(0x0229) == 0xFF, f"KEYV points at the MOS's extended entry for vector 20 (&FF3C): {r['writes']}")
    check(w.get(0x0D9F + 62) == 6 and 0x80 <= w.get(0x0D9F + 61, 0) < 0xC0, "the table entry names this ROM's slot and an address in it")
    check(r["regs"] == (1, 6, 0) and r["sp_after"] == r["sp_before"] and set(r["opcodes"]) <= ALLOWED,
          f"... A, X, Y and the stack given back, NMOS opcodes only: {sorted(set(r['opcodes']) - ALLOWED)}")
    check(r["mem"]._subject[0xA8] == 0 and r["mem"]._subject[0xA9] == 0, "... &A8 and &A9 put back")
    m = r["mem"]._subject
    check((m[KEYVOLD], m[KEYVOLD + 1]) == (0x02, 0xEF), f"... the MOS's KEYV (&EF02) kept in the workspace: &{m[KEYVOLD + 1]:02X}{m[KEYVOLD]:02X}")
    for kv in ((0x3C, 0xFF), (0x00, 0x90)):
        r = call(rom, 1, 6, 0, pi_after=0, reset_type=2, pokes=keyv_pokes(kv), watch=(0x0228, 0x0229, 0x0D9F + 60))
        check(r["writes"] == [], f"KEYV &{kv[1]:02X}{kv[0]:02X}, not the MOS's: left alone, {r['writes']}")
    r = call(rom, 1, 6, 0, pi_after=None, reset_type=2, pokes=keyv_pokes(), watch=(0x0228, 0x0229))
    check(r["writes"] == [], "no Pi: KEYV is not touched")

    r = call(rom, 1, 6, 0, pi_after=0, reset_type=2, pokes={**keyv_pokes(), SHIFTFLAG: 1}, watch=(SHIFTFLAG,))
    check(r["writes"] == [(SHIFTFLAG, 0)], f"the claim clears a SHIFT left over from before the reset: {r['writes']}")

    # the handler: every call but "is SHIFT or CTRL pressed" (C and V clear) goes to the MOS's
    # routine with A, X, Y and C, V as they came, and the flag is not looked at
    for v, c in ((1, 0), (1, 1), (0, 1)):
        r = run_keyv(rom, v, c, flag=1)
        check(r["calls"] == 1 and r["given"] == (0x11, 0x22, 0x33) and r["given_cv"] == (c, v),
              f"KEYV V={v} C={c}: passed on as it came, got {r['given']} C,V={r['given_cv']}")
        check(r["flag"] == 1 and r["zp"] == (0x5A, 0xA5) and r["sp_after"] == r["sp_before"],
              "... the SHIFT flag not taken, &A8/&A9 kept, stack balanced")
        check(r["zp_writes"] == [], f"... and no zero page written at all (an NMI could see it; T-14): {r['zp_writes']}")
        check(set(r["opcodes"]) <= ALLOWED | KEYV_OPCODES, f"... NMOS opcodes only: {sorted(set(r['opcodes']) - ALLOWED - KEYV_OPCODES)}")
    for ans in (0x00, 0x80, 0x40, 0xC0):
        r = run_keyv(rom, 0, 0, answer=ans)
        check(r["a"] == ans and r["calls"] == 1 and r["given_cv"] == (0, 0) and r["sp_after"] == r["sp_before"],
              f"the SHIFT/CTRL test, no SHIFT asked for: the MOS's answer {ans:#04x} as it is, got {r['a']:#04x}")
    for ans in (0x00, 0x80):
        r = run_keyv(rom, 0, 0, answer=ans, flag=1)
        check(r["a"] == ans | 0x40 and r["flag"] == 0 and r["zp"] == (0x5A, 0xA5) and r["sp_after"] == r["sp_before"],
              f"SHIFT asked for: SHIFT added to {ans:#04x} (got {r['a']:#04x}) and the flag cleared")
        r = run_keyv(rom, 0, 0, answer=ans, flag=1, buf=0x8B)
        check(r["a"] == ans | 0x40 and r["left"] == 1 and r["taken"] == 0,
              f"... with a COPY waiting in the buffer too: SHIFT all the same, the COPY left where it is")
    r = run_keyv(rom, 0, 0, flag=1)
    r2 = run_keyv(rom, 0, 0, flag=r["flag"])
    check(r["a"] == 0x40 and r2["a"] == 0x00, "SHIFT once: the next test says not pressed")

    # REMV gives every byte to the program (no byte stands for SHIFT any more)
    r = run_remv(rom, 0, 0, 0, 0xFC, 0)
    check(r["y"] == 0xFC and r["c"] == 0, "REMV: &FC is a key like any other")

    # with the real MOS 1.20, when its image is given: the paged-mode wait ends when the Pi's
    # trip sets the flag, whatever is in the keyboard buffer
    import os
    mos_path = os.environ.get("MOS12")
    if mos_path:
        mos = open(mos_path, "rb").read()
        done, left, flag = run_mos_page_wait(rom, mos, None, shift_after=20_000)
        check(done and flag == 0, f"MOS 1.20: the paged-mode wait goes on when SHIFT is asked for during it, the flag cleared (ended {done}, flag {flag})")
        done, left, flag = run_mos_page_wait(rom, mos, 0x8B, shift_after=20_000)
        check(done and left, f"MOS 1.20: ... also with a COPY waiting in the buffer, which stays there (ended {done}, left {left})")
        done, left, flag = run_mos_page_wait(rom, mos, 0x8B)
        check(not done and left, f"MOS 1.20: a COPY alone does not end the wait (ended {done})")
        done, left, flag = run_mos_page_wait(rom, mos, None)
        check(not done, "MOS 1.20: with nothing it keeps waiting")
    else:
        print("MOS12 not set: skipped the checks against the real MOS 1.20")

    print(f"{checks} checks, {fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
