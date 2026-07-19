#!/usr/bin/env python3
"""Lockstep integration test: the patched ANFS ROM bytes execute in a
6502 emulator whose FRED/JIM hooks talk to the REAL Pi-side econet C
code (harness subprocess). A scripted AUN peer validates the wire
format and plays fileserver."""
import os, subprocess, sys

# Paths default to files alongside this script; override with env vars.
#   ECO_ROM     - the patched ANFS ROM image (anfs-4.18-pi1mhz-fixed.rom)
#   ECO_SYMS    - "name hexaddr" lines for the engine entry points the
#                 tests call (svc5_irq_check, eco_rx_pump, tx_begin, the
#                 eih_*/erp_* labels, ...); see README
#   ECO_HARNESS - the compiled harness binary
HERE = os.path.dirname(os.path.abspath(__file__))
ROM_PATH = os.environ.get('ECO_ROM', os.path.join(HERE, 'anfs-4.18-pi1mhz.rom'))
SYM_PATH = os.environ.get('ECO_SYMS', os.path.join(HERE, 'syms.txt'))
HARNESS  = os.environ.get('ECO_HARNESS', os.path.join(HERE, 'harness'))

ROM = open(ROM_PATH,'rb').read()
SYM = dict((k, int(v,16)) for k,v in (l.split() for l in open(SYM_PATH)))

# ---------------- harness link ----------------
H = subprocess.Popen([HARNESS], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, text=True, bufsize=1)
udp_out = []          # captured outbound datagrams (ip, port, bytes)
def hx(cmd):
    H.stdin.write(cmd + '\n')
    while True:
        line = H.stdout.readline().strip()
        if line.startswith('O '):
            _, ip, port, data = line.split()
            udp_out.append((int(ip,16), int(port), bytes.fromhex(data)))
        elif line.startswith('K '):
            return int(line.split()[1], 16)
        elif line.startswith('V '):
            return int(line.split()[1], 16)
        else:
            raise RuntimeError(f'harness said {line!r}')

# ---------------- 6502 emulator ----------------
# 65C02-only opcodes (undefined on the NMOS 6502 in a stock BBC B). The 4.18
# AUNFS path must never execute one; the model-B guard in CPU.step() trips on
# any of these. (The 4.21 Master build uses several of them - phx/phy/plx/ply,
# trb, bra, inc a/dec a, stz, jmp (abs,x) - but runs under a different harness.)
C02_ONLY_OPCODES = frozenset([
    0x1a, 0x3a,                                      # inc a / dec a
    0x5a, 0x7a, 0xda, 0xfa,                          # phy / ply / phx / plx
    0x80,                                            # bra
    0x89,                                            # bit #imm
    0x04, 0x0c, 0x14, 0x1c,                          # tsb / trb
    0x64, 0x74, 0x9c, 0x9e,                          # stz
    0x12, 0x32, 0x52, 0x72, 0x92, 0xb2, 0xd2, 0xf2,  # (zp) zero-page indirect
    0x7c,                                            # jmp (abs,x)
])

class CPU:
    def __init__(self):
        self.mem = bytearray(0x10000)
        self.mem[0x8000:0xc000] = ROM
        self.a = self.x = self.y = 0
        self.sp = 0xfd
        self.n = self.v = self.z = self.c = self.i = self.d = False
        self.pc = 0
        self.jim_addr = 0
        self.tube_in = b''; self.tube_in_pos = 0
        self.tube_out = bytearray(); self.tube_r4 = bytearray()
        self.tube_claim_addr = None     # 4-byte address of the last Tube claim
        self.fred_aa = 0
        self.instructions = 0
        self.master_hits = 0   # model-B: count of ACCCON/INTOFF (&FE34/&FE38) accesses
        self.c02_hits = 0      # model-B: count of 65C02-only opcodes fetched

    # FRED-aware memory access (mirrors discaccess/econet semantics)
    def rd(self, a):
        a &= 0xffff
        if a == 0xfe34 or a == 0xfe38:   # model-B: ACCCON/INTOFF are Master-only
            self.master_hits += 1
        if a == 0xfca6: return self.jim_addr & 0xff
        if a == 0xfca7: return (self.jim_addr >> 8) & 0xff
        if a == 0xfca8: return (self.jim_addr >> 16) & 0xff
        if a == 0xfca9:
            v = hx(f'G {self.jim_addr:x}')
            self.jim_addr = (self.jim_addr + 1) & 0xffffff
            return v
        if a == 0xfcaa: return self.fred_aa
        if a == 0xfcab: return hx('F')
        if a == 0xfee5:                 # Tube R3 data: parasite -> host
            v = self.tube_in[self.tube_in_pos] if self.tube_in_pos < len(self.tube_in) else 0
            self.tube_in_pos += 1
            return v
        if a == 0xfee6: return 0xc0     # Tube R4/R1 status: always ready
        return self.mem[a]
    def wr(self, a, v):
        a &= 0xffff; v &= 0xff
        if a == 0xfe34 or a == 0xfe38:   # model-B: ACCCON/INTOFF are Master-only
            self.master_hits += 1
        if a == 0xfca6: self.jim_addr = (self.jim_addr & 0xffff00) | v; return
        if a == 0xfca7: self.jim_addr = (self.jim_addr & 0xff00ff) | (v<<8); return
        if a == 0xfca8: self.jim_addr = (self.jim_addr & 0x00ffff) | (v<<16); return
        if a == 0xfca9:
            hx(f'P {self.jim_addr:x} {v:x}')
            self.jim_addr = (self.jim_addr + 1) & 0xffffff
            return
        if a == 0xfee5:                 # Tube R3 data: host -> parasite
            self.tube_out.append(v); return
        if a == 0xfee7:                 # Tube R4 data: claim command stream
            self.tube_r4.append(v); return
        if a == 0xfcaa:
            self.fred_aa = hx(f'C {v:x}')   # echo skipped: result is final
            return
        if a >= 0x8000: raise RuntimeError(f'ROM write &{a:04x} at pc=&{self.pc:04x}')
        self.mem[a] = v

    def push(self, v): self.mem[0x100 + self.sp] = v & 0xff; self.sp = (self.sp - 1) & 0xff
    def pop(self): self.sp = (self.sp + 1) & 0xff; return self.mem[0x100 + self.sp]
    def setnz(self, v): v &= 0xff; self.n = v >= 0x80; self.z = v == 0; return v
    def flags(self):
        return ((0x80 if self.n else 0)|(0x40 if self.v else 0)|0x20|0x10|
                (8 if self.d else 0)|(4 if self.i else 0)|(2 if self.z else 0)|(1 if self.c else 0))
    def setflags(self, p):
        self.n = bool(p&0x80); self.v = bool(p&0x40); self.d = bool(p&8)
        self.i = bool(p&4); self.z = bool(p&2); self.c = bool(p&1)

    def adc(self, m):
        r = self.a + m + (1 if self.c else 0)
        self.v = (~(self.a ^ m) & (self.a ^ r) & 0x80) != 0
        self.c = r > 0xff
        self.a = self.setnz(r)
    def sbc(self, m): self.adc(m ^ 0xff)
    def cmpv(self, r, m):
        t = (r - m) & 0x1ff
        self.c = r >= m; self.setnz((r - m) & 0xff)

    def step(self):
        # Stubs for the relocated OS Tube routines (not present in the
        # lockstep image, which skips ROM relocation): claim succeeds
        # (C=1), release is a no-op. This isolates the econet Tube code.
        if self.pc == 0x0406:           # tube_addr_data_dispatch
            # entry: A=claim type, (Y:X) points at the 4-byte transfer address.
            # Capture it so tests can check WHICH parasite address was claimed.
            p = ((self.y << 8) | self.x) & 0xffff
            self.tube_claim_addr = bytes(self.mem[p:p+4])
            self.c = True
            lo = self.pop(); hi = self.pop(); self.pc = ((hi<<8)|lo)+1; return
        if self.pc == 0x0414:           # tube_release_claim
            lo = self.pop(); hi = self.pop(); self.pc = ((hi<<8)|lo)+1; return
        self.instructions += 1
        op = self.rd(self.pc); pc = self.pc + 1
        # model-B: a stock BBC B has an NMOS 6502 - no 65C02 opcodes. The 4.18
        # AUNFS path must never fetch one (the 4.21 Master build legitimately
        # does, but that runs under a different harness). Fail loudly if it does.
        if op in C02_ONLY_OPCODES:
            self.c02_hits += 1
            raise RuntimeError(f'model-B violation: 65C02 opcode &{op:02x} at &{self.pc:04x}')
        def b():  # operand byte
            nonlocal pc; v = self.rd(pc); pc += 1; return v
        def w():
            nonlocal pc; v = self.rd(pc) | (self.rd(pc+1) << 8); pc += 2; return v
        # addressing helpers returning effective address
        def zp(): return b()
        def zpx(): return (b() + self.x) & 0xff
        def zpy(): return (b() + self.y) & 0xff
        def ab(): return w()
        def abx(): return (w() + self.x) & 0xffff
        def aby(): return (w() + self.y) & 0xffff
        def indx(): z = (b() + self.x) & 0xff; return self.mem[z] | (self.mem[(z+1)&0xff] << 8)
        def indy(): z = b(); return ((self.mem[z] | (self.mem[(z+1)&0xff] << 8)) + self.y) & 0xffff
        def br(cond):
            nonlocal pc
            off = b()
            if cond: pc = (pc + (off - 256 if off >= 128 else off)) & 0xffff
        M = {  # opcode: (kind, mode)
        }
        # --- decode by opcode groups (compact) ---
        o = op
        if o == 0xea: pass
        elif o == 0x18: self.c = False
        elif o == 0x38: self.c = True
        elif o == 0x58: self.i = False
        elif o == 0x78: self.i = True
        elif o == 0xd8: self.d = False
        elif o == 0xf8: self.d = True
        elif o == 0xb8: self.v = False
        elif o == 0x48: self.push(self.a)
        elif o == 0x68: self.a = self.setnz(self.pop())
        elif o == 0x08: self.push(self.flags())
        elif o == 0x28: self.setflags(self.pop())
        elif o == 0xaa: self.x = self.setnz(self.a)
        elif o == 0x8a: self.a = self.setnz(self.x)
        elif o == 0xa8: self.y = self.setnz(self.a)
        elif o == 0x98: self.a = self.setnz(self.y)
        elif o == 0xba: self.x = self.setnz(self.sp)
        elif o == 0x9a: self.sp = self.x
        elif o == 0xe8: self.x = self.setnz(self.x + 1)
        elif o == 0xc8: self.y = self.setnz(self.y + 1)
        elif o == 0xca: self.x = self.setnz(self.x - 1)
        elif o == 0x88: self.y = self.setnz(self.y - 1)
        elif o == 0x60:  # rts
            lo = self.pop(); hi = self.pop(); pc = ((hi << 8) | lo) + 1
        elif o == 0x20:  # jsr
            t = w(); ret = pc - 1
            self.push((ret >> 8) & 0xff); self.push(ret & 0xff); pc = t
        elif o == 0x4c: pc = w()
        elif o == 0x6c:
            a = w(); pc = self.rd(a) | (self.rd((a & 0xff00) | ((a+1) & 0xff)) << 8)
        elif o == 0x40:  # rti
            self.setflags(self.pop()); lo = self.pop(); hi = self.pop(); pc = (hi << 8) | lo
        elif o in (0x10,0x30,0x50,0x70,0x90,0xb0,0xd0,0xf0):
            br({0x10:not self.n,0x30:self.n,0x50:not self.v,0x70:self.v,
                0x90:not self.c,0xb0:self.c,0xd0:not self.z,0xf0:self.z}[o])
        else:
            grp = {  # (loader, store) per mode for each ALU op family
            }
            modes = {0xa9:('lda','imm'),0xa5:('lda','zp'),0xb5:('lda','zpx'),0xad:('lda','ab'),0xbd:('lda','abx'),0xb9:('lda','aby'),0xa1:('lda','indx'),0xb1:('lda','indy'),
                     0xa2:('ldx','imm'),0xa6:('ldx','zp'),0xb6:('ldx','zpy'),0xae:('ldx','ab'),0xbe:('ldx','aby'),
                     0xa0:('ldy','imm'),0xa4:('ldy','zp'),0xb4:('ldy','zpx'),0xac:('ldy','ab'),0xbc:('ldy','abx'),
                     0x85:('sta','zp'),0x95:('sta','zpx'),0x8d:('sta','ab'),0x9d:('sta','abx'),0x99:('sta','aby'),0x81:('sta','indx'),0x91:('sta','indy'),
                     0x86:('stx','zp'),0x96:('stx','zpy'),0x8e:('stx','ab'),
                     0x84:('sty','zp'),0x94:('sty','zpx'),0x8c:('sty','ab'),
                     0x69:('adc','imm'),0x65:('adc','zp'),0x75:('adc','zpx'),0x6d:('adc','ab'),0x7d:('adc','abx'),0x79:('adc','aby'),0x61:('adc','indx'),0x71:('adc','indy'),
                     0xe9:('sbc','imm'),0xe5:('sbc','zp'),0xf5:('sbc','zpx'),0xed:('sbc','ab'),0xfd:('sbc','abx'),0xf9:('sbc','aby'),0xe1:('sbc','indx'),0xf1:('sbc','indy'),
                     0x29:('and','imm'),0x25:('and','zp'),0x35:('and','zpx'),0x2d:('and','ab'),0x3d:('and','abx'),0x39:('and','aby'),0x21:('and','indx'),0x31:('and','indy'),
                     0x09:('ora','imm'),0x05:('ora','zp'),0x15:('ora','zpx'),0x0d:('ora','ab'),0x1d:('ora','abx'),0x19:('ora','aby'),0x01:('ora','indx'),0x11:('ora','indy'),
                     0x49:('eor','imm'),0x45:('eor','zp'),0x55:('eor','zpx'),0x4d:('eor','ab'),0x5d:('eor','abx'),0x59:('eor','aby'),0x41:('eor','indx'),0x51:('eor','indy'),
                     0xc9:('cmp','imm'),0xc5:('cmp','zp'),0xd5:('cmp','zpx'),0xcd:('cmp','ab'),0xdd:('cmp','abx'),0xd9:('cmp','aby'),0xc1:('cmp','indx'),0xd1:('cmp','indy'),
                     0xe0:('cpx','imm'),0xe4:('cpx','zp'),0xec:('cpx','ab'),
                     0xc0:('cpy','imm'),0xc4:('cpy','zp'),0xcc:('cpy','ab'),
                     0x24:('bit','zp'),0x2c:('bit','ab'),
                     0xe6:('inc','zp'),0xf6:('inc','zpx'),0xee:('inc','ab'),0xfe:('inc','abx'),
                     0xc6:('dec','zp'),0xd6:('dec','zpx'),0xce:('dec','ab'),0xde:('dec','abx'),
                     0x0a:('asl','acc'),0x06:('asl','zp'),0x16:('asl','zpx'),0x0e:('asl','ab'),0x1e:('asl','abx'),
                     0x4a:('lsr','acc'),0x46:('lsr','zp'),0x56:('lsr','zpx'),0x4e:('lsr','ab'),0x5e:('lsr','abx'),
                     0x2a:('rol','acc'),0x26:('rol','zp'),0x36:('rol','zpx'),0x2e:('rol','ab'),0x3e:('rol','abx'),
                     0x6a:('ror','acc'),0x66:('ror','zp'),0x76:('ror','zpx'),0x6e:('ror','ab'),0x7e:('ror','abx')}
            if o not in modes: raise RuntimeError(f'opcode &{o:02x} at &{self.pc:04x}')
            name, mode = modes[o]
            if mode == 'imm':
                m = b(); ea = None
            elif mode == 'acc':
                m = self.a; ea = 'A'
            else:
                ea = {'zp':zp,'zpx':zpx,'zpy':zpy,'ab':ab,'abx':abx,'aby':aby,'indx':indx,'indy':indy}[mode]()
                m = None
            def load():
                return m if ea is None or ea == 'A' else self.rd(ea)
            if name == 'lda': self.a = self.setnz(load())
            elif name == 'ldx': self.x = self.setnz(load())
            elif name == 'ldy': self.y = self.setnz(load())
            elif name == 'sta': self.wr(ea, self.a)
            elif name == 'stx': self.wr(ea, self.x)
            elif name == 'sty': self.wr(ea, self.y)
            elif name == 'adc': self.adc(load())
            elif name == 'sbc': self.sbc(load())
            elif name == 'and': self.a = self.setnz(self.a & load())
            elif name == 'ora': self.a = self.setnz(self.a | load())
            elif name == 'eor': self.a = self.setnz(self.a ^ load())
            elif name == 'cmp': self.cmpv(self.a, load())
            elif name == 'cpx': self.cmpv(self.x, load())
            elif name == 'cpy': self.cmpv(self.y, load())
            elif name == 'bit':
                v = load(); self.z = (self.a & v) == 0; self.n = bool(v & 0x80); self.v = bool(v & 0x40)
            elif name in ('inc','dec'):
                v = (load() + (1 if name=='inc' else -1)) & 0xff
                self.wr(ea, self.setnz(v))
            else:  # shifts
                v = load()
                if name == 'asl': self.c = bool(v & 0x80); v = (v << 1) & 0xff
                elif name == 'lsr': self.c = bool(v & 1); v >>= 1
                elif name == 'rol': c0 = self.c; self.c = bool(v & 0x80); v = ((v << 1) | (1 if c0 else 0)) & 0xff
                else: c0 = self.c; self.c = bool(v & 1); v = (v >> 1) | (0x80 if c0 else 0)
                v = self.setnz(v)
                if ea == 'A': self.a = v
                else: self.wr(ea, v)
        self.pc = pc & 0xffff

    def call(self, addr, limit=2_000_000):
        """JSR addr, run until matching RTS."""
        sentinel = 0xff42
        self.push((sentinel - 1) >> 8); self.push((sentinel - 1) & 0xff)
        self.pc = addr
        n = 0
        while self.pc != sentinel:
            self.step(); n += 1
            if n > limit: raise RuntimeError(f'runaway at &{self.pc:04x}')
        return n

# ---------------- helpers ----------------
def aun(t, port, ctrl, seq, data=b''):
    return bytes([t, port, ctrl, 0, seq & 0xff, (seq>>8)&0xff, (seq>>16)&0xff, (seq>>24)&0xff]) + data

IP10 = 0x0a01a8c0   # 192.168.1.10 network order in u32-le
ok = 0
def check(cond, what):
    global ok
    assert cond, what
    ok += 1
    print(f'  ok: {what}')

# Drops leftover rx/park/IRQ residue between logically independent test
# groups sharing the one engine instance (the 'D' harness hook), keeping the
# ROM's open funnel block armed. See the 4816341 commit message.
def reset(): hx('D')

# ---------------- scenarios ----------------
print('== setup ==')
hx('S aun_station=1.32 aun_map=1.254=192.168.1.10')
hx('R 1')
cpu = CPU()

print('== 1: eco_ensure_init ==')
cpu.call(SYM['eco_ensure_init'])
check(not cpu.c, 'init returns carry clear')
check(cpu.y == 32, f'station 32 from cmdline (got {cpu.y})')
check(cpu.mem[0x0d22] == 32, 'tx_src_stn cached')
check(cpu.mem[0x0d23] == 0x80, 'eco_init_done latched')

print('== 2: transmit + ACK (data, port &99 to 1.254) ==')
payload = b'HELLO'
for i, ch in enumerate(payload): cpu.mem[0x3000+i] = ch
txcb = 0xc0
for off, v in enumerate([0x80, 0x99, 254, 1, 0x00, 0x30, 0xff, 0xff, 0x05, 0x30, 0xff, 0xff]):
    cpu.mem[txcb+off] = v
cpu.mem[0xa0] = txcb; cpu.mem[0xa1] = 0
udp_out.clear()
import threading
# the ACK must arrive while the 6502 spins in TX_POLL: drive it from a hook
orig_wr = CPU.wr
state = {'acked': False}
def wr_hook(self, a, v):
    orig_wr(self, a, v)
    if (a & 0xffff) == 0xfcaa and not state['acked']:
        for ip, port, data in list(udp_out):
            if data[0] == 2:                      # our DATA datagram seen
                check(ip == IP10 and port == 32768, 'datagram to mapped peer 192.168.1.10:32768')
                check(data[1] == 0x99 and data[2] == 0x00, 'AUN header port &99, ctrl bit7 stripped')
                check(data[8:] == payload, 'payload intact')
                hx('U %x %d %s' % (IP10, 32768, aun(3, 0x99, 0, int.from_bytes(data[4:8],"little")).hex()))
                state['acked'] = True
CPU.wr = wr_hook
cpu.call(SYM['tx_begin'])
CPU.wr = orig_wr
check(state['acked'], 'DATA datagram was emitted and ACKed')
check(cpu.mem[txcb] == 0x00, f'TXCB result &00 success (got &{cpu.mem[txcb]:02x})')
check(cpu.mem[0x0d60] == 0x80, 'tx_complete_flag set')

print('== 3: transmit NAK -> not listening (&41) ==')
cpu.mem[txcb] = 0x80
state = {'acked': False}
def wr_hook2(self, a, v):
    orig_wr(self, a, v)
    if (a & 0xffff) == 0xfcaa and not state['acked']:
        for ip, port, data in list(udp_out):
            if data[0] == 2 and not state['acked']:
                hx('U %x %d %s' % (IP10, 32768, aun(4, 0x99, 0, int.from_bytes(data[4:8],"little")).hex()))
                state['acked'] = True
udp_out.clear()
CPU.wr = wr_hook2
cpu.call(SYM['tx_begin'])
CPU.wr = orig_wr
check(cpu.mem[txcb] == 0x41, f'TXCB result &41 not listening (got &{cpu.mem[txcb]:02x})')

print('== 3b: jittered/delayed ACK - TX_POLL tolerates a late ACK ==')
# Test 2 injected the ACK on the very first TX_POLL read - an idealised zero-
# latency network. Real UDP latency varies: the ACK can land several poll
# cycles later. Sweep the delay (in poll cycles); every value within the ROM's
# per-handshake liveness budget must still complete the transmit with &00, and
# none may trip the F7 hang guard early. Payload HELLO is still at &3000 and
# the 1.254 -> 192.168.1.10 peer mapping from test 2 is still live here.
for poll_delay in (1, 5, 20, 60):
    for off, v in enumerate([0x80, 0x99, 254, 1, 0x00, 0x30, 0xff, 0xff, 0x05, 0x30, 0xff, 0xff]):
        cpu.mem[txcb+off] = v
    cpu.mem[txcb] = 0x80
    cpu.mem[0xa0] = txcb; cpu.mem[0xa1] = 0
    udp_out.clear()
    st = {'polls': 0, 'acked': False}
    def wr_hook_jit(self, a, v, st=st, pd=poll_delay):
        orig_wr(self, a, v)
        if (a & 0xffff) == 0xfcaa and not st['acked']:
            st['polls'] += 1
            if st['polls'] >= pd:
                for ip, port, data in list(udp_out):
                    if data[0] == 2:                  # our DATA datagram seen
                        hx('U %x %d %s' % (IP10, 32768,
                           aun(3, 0x99, 0, int.from_bytes(data[4:8], "little")).hex()))
                        st['acked'] = True
    CPU.wr = wr_hook_jit
    cpu.call(SYM['tx_begin'])
    CPU.wr = orig_wr
    check(st['acked'] and cpu.mem[txcb] == 0x00,
          'ACK delayed %d poll cycle(s): transmit still completed &00 (got &%02x)'
          % (poll_delay, cpu.mem[txcb]))

print('== 3c: dropped DATA -> engine NOT_LISTENING -> NFS layer re-issues ==')
# True packet loss, end to end. The first DATA datagram is emitted but no ACK or
# NAK ever returns. The Pi engine times out after AUN_NORESP_TIMEOUT_MS and
# reports NOT_LISTENING (&41); the stock (unpatched) ANFS NFS retry loop
# send_net_packet (&983F -> poll_adlc_tx_status -> tx_begin) classifies &41 as
# retryable and re-issues the whole transmit under a FRESH sequence number
# (matching the bridge's new-seq retry expectation). We drop attempt 1 (advance
# the engine clock past its silence deadline) and ACK attempt 2, proving the ROM
# re-sends rather than surfacing 'Not listening' on the first loss. This closes
# the last loss path: the engine owns lost-ACK dedup + NAK retransmit, while the
# Pi's send-side whole-transaction retry lives here in the NFS layer.
for off, v in enumerate([0x80, 0x99, 254, 1, 0x00, 0x30, 0xff, 0xff, 0x05, 0x30, 0xff, 0xff]):
    cpu.mem[txcb+off] = v
cpu.mem[txcb] = 0x80
cpu.mem[0x9a] = txcb; cpu.mem[0x9b] = 0       # net_tx_ptr -> TXCB
cpu.mem[0x0d6d] = 3                            # tx_retry_count = 3 (bound the test)
cpu.mem[0x0d60] = 0x80                         # tx_complete_flag primed for the first poll
udp_out.clear()
st = {'handled': 0}
def wr_hook_drop(self, a, v, st=st):
    orig_wr(self, a, v)
    if (a & 0xffff) != 0xfcaa:
        return
    datas = [d for _, _, d in udp_out if d[0] == 2]
    if len(datas) > st['handled']:            # a new DATA datagram just went out
        st['handled'] = len(datas)
        seq = int.from_bytes(datas[-1][4:8], "little")
        if st['handled'] == 1:
            hx('T 1001')                      # attempt 1: silence past the deadline
        else:
            hx('U %x %d %s' % (IP10, 32768, aun(3, 0x99, 0, seq).hex()))  # attempt 2: ACK
CPU.wr = wr_hook_drop
cpu.call(SYM['send_net_packet'])
CPU.wr = orig_wr
datas = [d for _, _, d in udp_out if d[0] == 2]
check(len(datas) >= 2,
      'first DATA dropped -> NFS layer re-issued the transmit (%d DATA datagrams)' % len(datas))
check(int.from_bytes(datas[0][4:8], 'little') != int.from_bytes(datas[1][4:8], 'little'),
      'retry uses a fresh sequence number (not a same-seq resend)')
check(cpu.mem[txcb] == 0x00,
      'retry was ACKed -> transmit completed &00 (got &%02x)' % cpu.mem[txcb])

print('== 4: rx pump delivers a fileserver reply into the &00C0 CB ==')
for off, v in enumerate([0x7f, 0x90, 0, 0, 0x00, 0x31, 0xff, 0xff, 0x7f, 0x31, 0xff, 0xff]):
    cpu.mem[txcb+off] = v                      # open receive, port &90, buffer &3100
cpu.mem[0x0d61] = 0x80                         # econet_flags: &00C0 list active
reply = b'\x05\x00FS-REPLY'
udp_out.clear()
hx('U %x %d %s' % (IP10, 32768, aun(2, 0x90, 0x00, 0x1000, reply).hex()))
acks = [d for _,_,d in udp_out if d[0] == 3]
check(len(acks) == 1 and int.from_bytes(acks[0][4:8],'little') == 0x1000,
      'ACK sent at receipt, seq echoed (ack-on-receipt)')
cpu.call(SYM['eco_rx_pump'])
check(len([d for _,_,d in udp_out if d[0] == 3]) == 1,
      'no second ACK at delivery: it was already acked on receipt')
check(cpu.mem[txcb] == 0x80, 'CB ctrl: bit7 set, wire ctrl restored')
check(cpu.mem[txcb+1] == 0x90 and cpu.mem[txcb+2] == 254 and cpu.mem[txcb+3] == 1, 'CB port/src station/src net')
got = bytes(cpu.mem[0x3100:0x3100+len(reply)])
check(got == reply, 'payload copied into Beeb memory')
end = cpu.mem[txcb+8] | (cpu.mem[txcb+9] << 8)
check(end == 0x3100 + len(reply), f'CB end pointer = past last byte (&{end:04x})')

print('== 4b: rx queue - both frames ACKed, delivered in order ==')
for off, v in enumerate([0x7f, 0x90, 0, 0, 0x00, 0x31, 0xff, 0xff, 0x7f, 0x31, 0xff, 0xff]):
    cpu.mem[txcb+off] = v                      # re-open the CB
frameA, frameB = b'FRAME-A', b'FRAME-B'
udp_out.clear()
hx('U %x %d %s' % (IP10, 32768, aun(2, 0x90, 0x00, 0x2000, frameA).hex()))
hx('U %x %d %s' % (IP10, 32768, aun(2, 0x90, 0x00, 0x2004, frameB).hex()))
check(len([d for _,_,d in udp_out if d[0] == 3]) == 2, 'both frames ACKed at receipt')
check(not [d for _,_,d in udp_out if d[0] == 4], 'neither frame NAKed')
cpu.call(SYM['eco_rx_pump'])                   # delivers A from the queue
check(bytes(cpu.mem[0x3100:0x3100+7]) == frameA, 'frame A delivered intact')
for off, v in enumerate([0x7f, 0x90, 0, 0, 0x00, 0x31, 0xff, 0xff, 0x7f, 0x31, 0xff, 0xff]):
    cpu.mem[txcb+off] = v                      # re-open for the next frame
cpu.call(SYM['eco_rx_pump'])                   # B comes straight from the queue
check(bytes(cpu.mem[0x3100:0x3100+7]) == frameB, 'frame B delivered from the queue, no retransmission')
check(len([d for _,_,d in udp_out if d[0] == 3]) == 2, 'still just the two receipt ACKs; none added at delivery')

print('== 5: unlistened port -> funnel ACKs at receipt, ROM demux drops above ==')
# ANFS opens a wildcard funnel (handle 0) through which every inbound frame
# passes, so the engine ACKs any port the instant it lands - fast enough to
# beat the fileserver's reply-ACK timeout. A frame whose port has no control
# block is therefore taken by the funnel and ACKed, then dropped by the ROM
# demux above the AUN layer (and, for a reply arriving a beat early, held by
# park-and-retry). It is NOT NAKed: ack-on-receipt replaced the old
# deferred-verdict NAK. The genuine "cannot take it" NAK now comes only from
# a full/oversize funnel (see test 5b).
cpu.mem[txcb] = 0x00                           # Beeb CB closed
udp_out.clear()
hx('U %x %d %s' % (IP10, 32768, aun(2, 0x77, 0x00, 0x2100, b'XX').hex()))
acks = [d for _,_,d in udp_out if d[0] == 3]
check(len(acks) == 1 and int.from_bytes(acks[0][4:8],'little') == 0x2100,
      'funnel ACKs the unlistened-port frame at receipt (seq echoed)')
check(not [d for _,_,d in udp_out if d[0] == 4], 'no NAK: ack-on-receipt, not deferred verdict')
cpu.call(SYM['eco_rx_pump'])
check(cpu.mem[txcb] == 0x00, 'closed Beeb CB untouched')

print('== 5b: 4K datagram delivered; frame too big for the Beeb CB is dropped ==')
big = bytes((i & 0xff) for i in range(4096))
for off, v in enumerate([0x7f, 0x90, 0, 0, 0x00, 0x30, 0xff, 0xff, 0x00, 0x41, 0xff, 0xff]):
    cpu.mem[txcb+off] = v                      # CB buffer &3000-&40FF (cap 4352)
udp_out.clear()
hx('U %x %d %s' % (IP10, 32768, aun(2, 0x90, 0x00, 0x5000, big).hex()))
cpu.call(SYM['eco_rx_pump'])
check(bytes(cpu.mem[0x3000:0x4000]) == big, '4096-byte payload delivered intact')
end = cpu.mem[txcb+8] | (cpu.mem[txcb+9] << 8)
check(end == 0x4000, 'CB end pointer past the 4K payload')
check(len([d for _,_,d in udp_out if d[0] == 3]) == 1, '4K frame ACKed at delivery')
# A frame that fits the 8K funnel but not the (tiny) Beeb CB is ACKed by the
# funnel on receipt, then dropped by the ROM demux when it will not fit the
# CB - it must NOT overrun the CB buffer. (The engine-level oversize NAK,
# len > AUN_MAX_DATA, is covered by test_aun.c case 21.)
for off, v in enumerate([0x7f, 0x90, 0, 0, 0x00, 0x30, 0xff, 0xff, 0x10, 0x30, 0xff, 0xff]):
    cpu.mem[txcb+off] = v                      # tiny CB: capacity 16 bytes
guard = bytes(cpu.mem[0x3000:0x3010])          # snapshot to prove no overrun
udp_out.clear()
hx('U %x %d %s' % (IP10, 32768, aun(2, 0x90, 0x00, 0x5004, b'X'*100).hex()))
check(len([d for _,_,d in udp_out if d[0] == 3]) == 1, 'funnel ACKs the 100-byte frame at receipt')
cpu.call(SYM['eco_rx_pump'])
check(cpu.mem[txcb] == 0x7f, 'tiny CB control byte untouched')
check(bytes(cpu.mem[0x3000:0x3010]) == guard, 'tiny CB buffer not overrun by the oversize payload')

print('== 6: inbound machine peek answered by the Pi ==')
udp_out.clear()
hx('U %x %d %s' % (IP10, 32768, aun(5, 0, 0x08, 0x3000).hex()))
reps = [d for _,_,d in udp_out if d[0] == 6]
check(len(reps) == 1 and len(reps[0]) == 12, 'immediate reply emitted, 4-byte machine id')

print('== 7: outbound machine peek (TXCB ctrl &88, port 0) ==')
for off, v in enumerate([0x88, 0x00, 254, 1, 0x00, 0x32, 0xff, 0xff, 0x04, 0x32, 0xff, 0xff,
                          0,0,0,0]):
    cpu.mem[txcb+off] = v                      # reply buffer &3200-&3203
state = {'acked': False}
def wr_hook3(self, a, v):
    orig_wr(self, a, v)
    if (a & 0xffff) == 0xfcaa and not state['acked']:
        for ip, port, data in list(udp_out):
            if data[0] == 5 and not state['acked']:
                check(data[2] == 0x08, 'imm ctrl &88 -> wire &08')
                hx('U %x %d %s' % (IP10, 32768,
                    aun(6, 0, 0x08, int.from_bytes(data[4:8],'little'), b'\xaa\xbb\xcc\xdd').hex()))
                state['acked'] = True
udp_out.clear()
CPU.wr = wr_hook3
cpu.call(SYM['tx_begin'])
CPU.wr = orig_wr
check(state['acked'], 'IMMEDIATE datagram emitted and answered')
check(cpu.mem[txcb] == 0x00, 'TXCB result success')
check(bytes(cpu.mem[0x3200:0x3204]) == b'\xaa\xbb\xcc\xdd', 'peek reply landed in TXCB buffer')

print('== 7b: outbound immediate reply to a Tube buffer streams to R3 (F4) ==')
# Same machine peek as test 7, but the reply buffer is flagged as PARASITE:
# the extended-address bytes +6/+7 are &0000, not the &FFFF host sentinel.
# The original ANFS streamed an immediate reply to a Tube buffer (its rx and
# immediate-data NMI paths were all Tube-aware); the AUN eti_reply dropped
# that (F4). With the fix, eti_reply mirrors eco_rx_pump: a parasite-flagged
# reply streams to Tube R3 and the host buffer is left untouched.
cpu.mem[0x0d63] = 0x01                          # tube_present
cpu.tube_out = bytearray()
for off, v in enumerate([0x88, 0x00, 254, 1, 0x00, 0x32, 0x00, 0x00, 0x04, 0x32, 0x00, 0x00,
                          0,0,0,0]):
    cpu.mem[txcb+off] = v                       # reply buf &3200, +6/+7=&0000 -> parasite
for i in range(4): cpu.mem[0x3200+i] = 0        # clear the host buffer
state = {'acked': False}
def wr_f4(self, a, v):
    orig_wr(self, a, v)
    if (a & 0xffff) == 0xfcaa and not state['acked']:
        for ip, port, data in list(udp_out):
            if data[0] == 5 and not state['acked']:
                hx('U %x %d %s' % (IP10, 32768,
                    aun(6, 0, 0x08, int.from_bytes(data[4:8],'little'), b'\xaa\xbb\xcc\xdd').hex()))
                state['acked'] = True
udp_out.clear()
CPU.wr = wr_f4
cpu.call(SYM['tx_begin'])
CPU.wr = orig_wr
check(state['acked'], 'Tube-buffer immediate emitted and answered')
_reply = b'\xaa\xbb\xcc\xdd'
check(bytes(cpu.tube_out) == _reply and bytes(cpu.mem[0x3200:0x3204]) != _reply,
      'F4 FIXED: reply streamed to Tube R3, host buffer untouched')
cpu.mem[0x0d63] = 0

print('== 7c: outbound POKE carries its data block, not just 4 args (R4) ==')
# The original streamed the local buffer as the poke data; the AUN port sent
# only the 4 argument bytes, so a peer's inbound POKE wrote 0 bytes. With R4
# the payload is [4 dest bytes][buffer data] and tx length = 4 + data length.
cpu.mem[0x0d63] = 0                              # no 2nd proc: data from host RAM
cpu.mem[0x3300:0x3306] = b'POKED!'              # local data to send
# TXCB: ctrl &82 POKE, port 0, dest 254.1, buffer &3300..&3306 (6 data bytes),
# args (+12..15) = remote dest &FFFF4000
for off, v in enumerate([0x82, 0x00, 254, 1, 0x00, 0x33, 0xff, 0xff, 0x06, 0x33, 0xff, 0xff,
                          0x00, 0x40, 0xff, 0xff]):
    cpu.mem[txcb+off] = v
state = {'acked': False}
def wr_poke(self, a, v):
    orig_wr(self, a, v)
    if (a & 0xffff) == 0xfcaa and not state['acked']:
        for ip, port, data in list(udp_out):
            if data[0] == 5 and not state['acked']:
                check(data[2] == 0x02, 'imm ctrl &82 -> wire &02')
                check(data[8:] == bytes([0x00, 0x40, 0xff, 0xff]) + b'POKED!',
                      'outbound POKE payload = 4 dest bytes + the data block (R4)')
                hx('U %x %d %s' % (IP10, 32768,
                    aun(6, 0, 0x02, int.from_bytes(data[4:8], 'little'), b'').hex()))
                state['acked'] = True
udp_out.clear()
CPU.wr = wr_poke
cpu.call(SYM['tx_begin'])
CPU.wr = orig_wr
check(state['acked'], 'POKE immediate emitted and answered')

print('== 8: broadcast (dest station &FF) completes without handshake ==')
for off, v in enumerate([0x80, 0x9c, 0xff, 0xff, 0x00, 0x30, 0xff, 0xff, 0x04, 0x30, 0xff, 0xff]):
    cpu.mem[txcb+off] = v
udp_out.clear()
cpu.call(SYM['tx_begin'])
bc = [(ip,d) for ip,_,d in udp_out if d[0] == 1]
check(len(bc) == 2 and all(d[1] == 0x9c for _,d in bc), 'BROADCAST to mapped peer + subnet broadcast, port &9C')
check(any(ip == 0xff01a8c0 for ip,_ in bc), 'subnet broadcast address 192.168.1.255 included')
check(cpu.mem[txcb] == 0x00, 'broadcast result success')

# ---------------------------------------------------------------------------
# Tests 9+ (IRQ pump, immediates, Tube, page-crossing copies, service gate)
# share ONE engine instance with tests 1-8. Each test sets up its own control
# block and frames, and the pump drains any frame an earlier test parked, so
# the shared state does not leak false fails - keep new tests self-contained.
print('== 9: IRQ-driven reception via service call 5 ==')
reset()
for off, v in enumerate([0x7f, 0x90, 0, 0, 0x00, 0x31, 0xff, 0xff, 0x7f, 0x31, 0xff, 0xff]):
    cpu.mem[txcb+off] = v                      # open receive CB, port &90
cpu.mem[0x0d61] = 0x80
unsolicited = b'NOTIFY!'
hx('U %x %d %s' % (IP10, 32768, aun(2, 0x90, 0x00, 0x3000, unsolicited).hex()))
hx('L')                                        # Pi main-loop turn: IRQ update
check(hx('I') == 1, 'nIRQ asserted while a frame is queued')
check(hx('F') & 0x80, 'FRED &FCAB bit 7 set (frame waiting)')
cpu.call(SYM['svc5_irq_check'])                # MOS unrecognised-IRQ service
check(cpu.a == 0, 'svc5 claimed the interrupt (A=0)')
check(bytes(cpu.mem[0x3100:0x3100+7]) == unsolicited, 'unsolicited frame delivered by the IRQ pump')
check(cpu.mem[txcb] == 0x80, 'CB completed (bit 7 set)')
hx('L')
check(hx('I') == 0, 'nIRQ released once the queue is empty')
check(hx('F') == 0, '&FCAB cleared')

print('== 9b: Econet receive event (&FE) fires like the old NMI path ==')
# EVNTV stub at &2000: sty &71 / inc &70 / rts ; vector at &0220/1
for i, b in enumerate([0x84, 0x71, 0xe6, 0x70, 0x60]): cpu.mem[0x2000+i] = b
cpu.mem[0x220], cpu.mem[0x221] = 0x00, 0x20
cpu.mem[0x70] = cpu.mem[0x71] = 0
cpu.mem[0x0d6c] = 0x01                         # fs_flags bit 0: events enabled
for off, v in enumerate([0x7f, 0x90, 0, 0, 0x00, 0x31, 0xff, 0xff, 0x7f, 0x31, 0xff, 0xff]):
    cpu.mem[txcb+off] = v                      # &00C0 CB -> slot &C0/12 = 16
hx('U %x %d %s' % (IP10, 32768, aun(2, 0x90, 0x00, 0x4000, b'EVT').hex()))
hx('L')
cpu.call(SYM['svc5_irq_check'])                # IRQ -> pump -> deferred event
check(cpu.mem[0x70] == 1, 'EVNTV called exactly once')
check(cpu.mem[0x71] == 16, f'Y = slot 16, as the original passed it (got {cpu.mem[0x71]})')
cpu.mem[0x0d6c] = 0x00                         # events disabled
for off, v in enumerate([0x7f, 0x90, 0, 0, 0x00, 0x31, 0xff, 0xff, 0x7f, 0x31, 0xff, 0xff]):
    cpu.mem[txcb+off] = v
hx('U %x %d %s' % (IP10, 32768, aun(2, 0x90, 0x00, 0x4004, b'EVT').hex()))
hx('L')
cpu.call(SYM['svc5_irq_check'])
check(cpu.mem[0x70] == 1, 'no event when fs_flags bit 0 is clear')
check(cpu.mem[txcb] == 0x80, 'frame still delivered either way')

print('== 9c: remote immediates - peek, poke, JSR via svc5 ==')
# remote PEEK of &3000-&300F (payload: start addr, end addr)
for i in range(16): cpu.mem[0x3000+i] = 0xA0 + i
args = bytes([0x00,0x30,0xff,0xff, 0x10,0x30,0xff,0xff])
udp_out.clear()
hx('U %x %d %s' % (IP10, 32768, aun(5, 0, 0x01, 0x6000, args).hex()))
hx('L')
check(hx('F') & 0x40, '&FCAB bit 6 set: immediate pending')
cpu.call(SYM['svc5_irq_check'])
reps = [d for _,_,d in udp_out if d[0] == 6]
check(len(reps) == 1 and reps[0][8:] == bytes(cpu.mem[0x3000:0x3010]),
      'PEEK reply carries our memory contents')
check(int.from_bytes(reps[0][4:8],'little') == 0x6000, 'reply echoes the request seq')
# remote POKE of &3200 (payload: dest addr + data)
poke = bytes([0x00,0x32,0xff,0xff]) + b'POKED!'
udp_out.clear()
hx('U %x %d %s' % (IP10, 32768, aun(5, 0, 0x02, 0x6004, poke).hex()))
hx('L')
cpu.call(SYM['svc5_irq_check'])
check(bytes(cpu.mem[0x3200:0x3206]) == b'POKED!', 'POKE wrote our memory')
check([d for _,_,d in udp_out if d[0] == 6], 'POKE acknowledged with an empty reply')
# remote JSR: stub at &2100 increments &72
for i, b in enumerate([0xe6, 0x72, 0x60]): cpu.mem[0x2100+i] = b
cpu.mem[0x72] = 0
hx('U %x %d %s' % (IP10, 32768, aun(5, 0, 0x03, 0x6008, bytes([0x00,0x21])).hex()))
hx('L')
cpu.call(SYM['svc5_irq_check'])
check(cpu.mem[0x72] == 1, 'remote JSR executed the target routine')

print('== 9c2: *Prot-ed inbound immediate is refused (R3) ==')
# prot_status (&0D68) bit n disables op &81+n, exactly as the original
# .immediate_op gate did. bit2 => JSR (&83) disabled: a remote JSR must not run.
cpu.mem[0x0d68] = 0x04                           # *Prot JSR
for i, b in enumerate([0xe6, 0x73, 0x60]): cpu.mem[0x2100+i] = b   # stub: inc &73
cpu.mem[0x73] = 0
udp_out.clear()
hx('U %x %d %s' % (IP10, 32768, aun(5, 0, 0x03, 0x6100, bytes([0x00,0x21])).hex()))
hx('L')
cpu.call(SYM['svc5_irq_check'])
check(cpu.mem[0x73] == 0, 'protected JSR refused: target NOT executed (R3)')
check([d for _,_,d in udp_out if d[0] == 6], 'refused immediate still gets an (empty) reply')
cpu.mem[0x0d68] = 0                              # *Unprot
udp_out.clear()
hx('U %x %d %s' % (IP10, 32768, aun(5, 0, 0x03, 0x6104, bytes([0x00,0x21])).hex()))
hx('L')
cpu.call(SYM['svc5_irq_check'])
check(cpu.mem[0x73] == 1, 'unprotected JSR runs again')

print('== 9c3: remote UserProc (op 4) and OSProc (op 5) via svc5 (R2) ==')
# The original ANFS ran two more immediates the Pi build had dropped:
#   op 4 UserProc -> OSEVENT 8, op 5 OSProc -> dir_op_dispatch. Both defer
# their execution to foreground (after the reply), exactly like remote JSR.
cpu.mem[0x0d68] = 0                              # *Unprot: ops enabled
# op 4: UserProc -> oseven (&FFBF). Stub records A(=args hi)/X(=args lo)/Y(=8).
for i, b in enumerate([0x85, 0x74, 0x86, 0x75, 0x84, 0x76, 0x60]): cpu.mem[0xffbf+i] = b
cpu.mem[0x74] = cpu.mem[0x75] = cpu.mem[0x76] = 0
udp_out.clear()
hx('U %x %d %s' % (IP10, 32768, aun(5, 0, 0x04, 0x7000, bytes([0x34, 0x12])).hex())); hx('L')
cpu.call(SYM['svc5_irq_check'])
check(cpu.mem[0x76] == 8, 'UserProc raised OSEVENT with event number 8 (R2)')
check(cpu.mem[0x75] == 0x34 and cpu.mem[0x74] == 0x12, 'UserProc passed args X=lo, A=hi')
check([d for _, _, d in udp_out if d[0] == 6], 'UserProc immediate acknowledged')
# op 5: OSProc -> dir_op_dispatch. Stub records X(=args lo)/Y(=args hi).
_dod = SYM['dir_op_dispatch']
_save = bytes(cpu.mem[_dod:_dod+7])
for i, b in enumerate([0x86, 0x78, 0x84, 0x79, 0xe6, 0x77, 0x60]): cpu.mem[_dod+i] = b
cpu.mem[0x77] = cpu.mem[0x78] = cpu.mem[0x79] = 0
udp_out.clear()
hx('U %x %d %s' % (IP10, 32768, aun(5, 0, 0x05, 0x7004, bytes([0x56, 0x78])).hex())); hx('L')
cpu.call(SYM['svc5_irq_check'])
check(cpu.mem[0x77] == 1, 'OSProc invoked dir_op_dispatch (R2)')
check(cpu.mem[0x78] == 0x56 and cpu.mem[0x79] == 0x78, 'OSProc passed args X=lo, Y=hi')
check([d for _, _, d in udp_out if d[0] == 6], 'OSProc immediate acknowledged')
cpu.mem[_dod:_dod+7] = _save                    # restore the ROM entry point

print('== 9c4: remote PEEK/POKE of the second processor go over the Tube (R1) ==')
# A remote peek/poke whose target address selects the parasite (ext bytes
# not the &FFxx host sentinel) must hit parasite memory over R3, as the
# original ANFS did - not host RAM. Only reachable with a 2nd proc fitted.
cpu.mem[0x0d68] = 0                              # *Unprot: ops enabled
cpu.mem[0x0d63] = 0x01                           # tube_present
# PEEK &00003000..&00003010: the 16 reply bytes must come from R3, not host.
cpu.tube_in = bytes(range(0x40, 0x50)); cpu.tube_in_pos = 0
cpu.mem[0x3000:0x3010] = b'\xee' * 16            # poison host memory (must be ignored)
udp_out.clear()
peek = bytes([0x00, 0x30, 0x00, 0x00, 0x10, 0x30])   # start &00003000, end &00003010
hx('U %x %d %s' % (IP10, 32768, aun(5, 0, 0x01, 0x7200, peek).hex())); hx('L')
cpu.call(SYM['svc5_irq_check'])
reps = [d for _, _, d in udp_out if d[0] == 6]
check(reps and reps[0][8:8+16] == bytes(range(0x40, 0x50)),
      'parasite PEEK reply came from Tube R3, not host memory (R1)')
# POKE &00003200 + 6 bytes: the data must go to R3, host RAM untouched.
cpu.tube_out = bytearray()
cpu.mem[0x3200:0x3206] = b'\xee' * 6
udp_out.clear()
poke = bytes([0x00, 0x32, 0x00, 0x00]) + b'PARA!!'    # dest &00003200 + data
hx('U %x %d %s' % (IP10, 32768, aun(5, 0, 0x02, 0x7204, poke).hex())); hx('L')
cpu.call(SYM['svc5_irq_check'])
check(bytes(cpu.tube_out) == b'PARA!!', 'parasite POKE streamed data to Tube R3 (R1)')
check(bytes(cpu.mem[0x3200:0x3206]) == b'\xee' * 6, 'parasite POKE left host memory untouched (R1)')
cpu.mem[0x0d63] = 0                              # 2nd proc absent again for later groups

print('== 9c5: remote JSR delivers its parameter block to the port buffer (R5) ==')
# The original rx_imm_exec received the JSR/Proc parameter data into the port
# buffer (net_rx_ptr) before jumping; the AUN port read only the 2-byte target
# and discarded the params. R5 copies the payload past the target to
# (net_rx_ptr) so the called routine finds them there.
cpu.mem[0x0d68] = 0                              # *Unprot
cpu.mem[0x009c] = 0x00; cpu.mem[0x009d] = 0x2a  # net_rx_ptr -> &2A00 port buffer
cpu.mem[0x2a00:0x2a04] = b'\x00\x00\x00\x00'
# JSR stub at &2200: ldy #0 / lda (net_rx_ptr),y / sta &76 / inc &75 / rts
for i, b in enumerate([0xa0, 0x00, 0xb1, 0x9c, 0x85, 0x76, 0xe6, 0x75, 0x60]):
    cpu.mem[0x2200+i] = b
cpu.mem[0x75] = cpu.mem[0x76] = 0
udp_out.clear()
# wire layout is [4-byte addr/arg field][param block]: target in the low two
# bytes, two address-extension bytes, then params at offset 4 (as the original
# ANFS delivered them and as our own etb_imm emits an outbound JSR).
jsrp = bytes([0x00, 0x22, 0x00, 0x00]) + b'PRM'   # target &2200, ext, then params
hx('U %x %d %s' % (IP10, 32768, aun(5, 0, 0x03, 0x7300, jsrp).hex())); hx('L')
cpu.call(SYM['svc5_irq_check'])
check(cpu.mem[0x75] == 1, 'remote JSR with params executed')
check(cpu.mem[0x76] == ord('P'), 'JSR routine read its params from net_rx_ptr (R5)')
check(bytes(cpu.mem[0x2a00:0x2a03]) == b'PRM', 'full param block landed in the port buffer (R5)')

print('== 9c6: JSR immediate round-trips (etb_imm out == eih_defer_op in) ==')
# Strongest self-consistency check: capture the ROM's OWN outbound JSR
# datagram, then feed it straight back in as an inbound JSR. If the outbound
# encoder (etb_imm, R4) and inbound decoder (eih_defer_op, R5) disagree by so
# much as one byte, the params land wrong and the stub reads garbage.
cpu.mem[0x0d68] = 0                              # *Unprot
cpu.mem[0x0d63] = 0                              # no 2nd proc: params from host RAM
cpu.mem[0x009c] = 0x00; cpu.mem[0x009d] = 0x2a  # net_rx_ptr -> &2A00 port buffer
cpu.mem[0x2a00:0x2a04] = b'\x00\x00\x00\x00'
cpu.mem[0x75] = cpu.mem[0x76] = 0
for i, b in enumerate([0xa0, 0x00, 0xb1, 0x9c, 0x85, 0x76, 0xe6, 0x75, 0x60]):
    cpu.mem[0x2200+i] = b                        # same JSR stub as 9c5, at &2200
cpu.mem[0x3400:0x3403] = b'RTP'                 # local param block to transmit
# TXCB: ctrl &83 JSR, port 0, dest 254.1, param buffer &3400..&3403 (3 bytes),
# args (+12..15) = remote target &0000_2200 (the stub, ext bytes 0)
for off, v in enumerate([0x83, 0x00, 254, 1, 0x00, 0x34, 0xff, 0xff, 0x03, 0x34, 0xff, 0xff,
                          0x00, 0x22, 0x00, 0x00]):
    cpu.mem[txcb+off] = v
captured = {}
def wr_rt(self, a, v):
    orig_wr(self, a, v)
    if (a & 0xffff) == 0xfcaa and 'payload' not in captured:
        for ip, port, data in list(udp_out):
            if data[0] == 5:
                check(data[2] == 0x03, 'imm ctrl &83 -> wire &03')
                check(data[8:] == bytes([0x00, 0x22, 0x00, 0x00]) + b'RTP',
                      'outbound JSR payload = 4 addr bytes + param block')
                captured['payload'] = data[8:]
                hx('U %x %d %s' % (IP10, 32768,
                    aun(6, 0, 0x03, int.from_bytes(data[4:8], 'little'), b'').hex()))
                break
udp_out.clear()
CPU.wr = wr_rt
cpu.call(SYM['tx_begin'])
CPU.wr = orig_wr
check('payload' in captured, 'outbound JSR immediate emitted and answered')
# now replay the very bytes the ROM sent as an INBOUND JSR immediate
udp_out.clear()
hx('U %x %d %s' % (IP10, 32768, aun(5, 0, 0x03, 0x7400, captured['payload']).hex())); hx('L')
cpu.call(SYM['svc5_irq_check'])
check(cpu.mem[0x75] == 1, 'round-tripped JSR executed the target')
check(bytes(cpu.mem[0x2a00:0x2a03]) == b'RTP',
      'round-tripped param block arrived intact at the port buffer')

print('== 9c7: PEEK immediate round-trips (8-byte [start][end] descriptor, R6) ==')
# The original calc_peek_poke_size emitted [remote start][remote end] with
# end = start + local reply-buffer count; the AUN port had dropped the end,
# sending 4 bytes only, so a peer never learned how many bytes to return.
# Capture the ROM's own outbound PEEK and replay it inbound: the inbound
# handler must read exactly `count` bytes, proving the descriptor is whole.
cpu.mem[0x0d63] = 0                              # no 2nd proc: host memory
# outbound TXCB: ctrl &81 PEEK, port 0, dest 254.1, reply buffer &3500..&3510
# (count = 16), args (+12..15) = remote start &FFFF5000 (ext FFFF = host)
for off, v in enumerate([0x81, 0x00, 254, 1, 0x00, 0x35, 0xff, 0xff, 0x10, 0x35, 0xff, 0xff,
                          0x00, 0x50, 0xff, 0xff]):
    cpu.mem[txcb+off] = v
cap = {}
def wr_pk(self, a, v):
    orig_wr(self, a, v)
    if (a & 0xffff) == 0xfcaa and 'd' not in cap:
        for ip, port, data in list(udp_out):
            if data[0] == 5:
                check(data[2] == 0x01, 'imm ctrl &81 -> wire &01')
                check(data[8:] == bytes([0x00,0x50,0xff,0xff, 0x10,0x50,0xff,0xff]),
                      'outbound PEEK descriptor = [start][start+count], 8 bytes (R6)')
                cap['d'] = data[8:]
                hx('U %x %d %s' % (IP10, 32768,
                    aun(6, 0, 0x01, int.from_bytes(data[4:8],'little'), b'\x11'*16).hex()))
                break
udp_out.clear()
CPU.wr = wr_pk
cpu.call(SYM['tx_begin'])
CPU.wr = orig_wr
check('d' in cap, 'outbound PEEK immediate emitted and answered')
# replay the captured descriptor inbound; the inbound PEEK must read 16 bytes
cpu.mem[0x5000:0x5010] = bytes(range(0x60, 0x70))
udp_out.clear()
hx('U %x %d %s' % (IP10, 32768, aun(5, 0, 0x01, 0x5600, cap['d']).hex())); hx('L')
cpu.call(SYM['svc5_irq_check'])
reps = [d for _, _, d in udp_out if d[0] == 6]
check(reps and reps[0][8:8+16] == bytes(range(0x60, 0x70)),
      'round-tripped PEEK read exactly `count` bytes from [start,end) (R6)')

print('== 9c8: inbound parasite PEEK/POKE claim the immediate target address (R7) ==')
# The Tube claim must use the immediate's own peek/poke address, NOT the stale
# outbound TXCB the last transmit left behind. Poison TXCB+4..7 so a claim that
# wrongly reads it is caught, then check the claimed 4-byte address.
cpu.mem[0x0d63] = 0x01                            # 2nd proc fitted
cpu.mem[txcb+4:txcb+8] = bytes([0xDE,0xAD,0xBE,0xEF])
cpu.tube_in = bytes(range(0x40,0x50)); cpu.tube_in_pos = 0
cpu.tube_claim_addr = None
# PEEK &00003456..&00003466 (ext 00,00 selects the parasite), count 16
peek = bytes([0x56,0x34,0x00,0x00, 0x66,0x34])
udp_out.clear()
hx('U %x %d %s' % (IP10, 32768, aun(5, 0, 0x01, 0x7700, peek).hex())); hx('L')
cpu.call(SYM['svc5_irq_check'])
check(cpu.tube_claim_addr == bytes([0x56,0x34,0x00,0x00]),
      'parasite PEEK claimed target &00003456, not the stale TXCB (R7)')
# POKE &00003800 + data (ext 00,00 selects the parasite)
cpu.tube_out = bytearray(); cpu.tube_claim_addr = None
cpu.mem[txcb+4:txcb+8] = bytes([0xDE,0xAD,0xBE,0xEF])
poke = bytes([0x00,0x38,0x00,0x00]) + b'HW!'
udp_out.clear()
hx('U %x %d %s' % (IP10, 32768, aun(5, 0, 0x02, 0x7704, poke).hex())); hx('L')
cpu.call(SYM['svc5_irq_check'])
check(cpu.tube_claim_addr == bytes([0x00,0x38,0x00,0x00]),
      'parasite POKE claimed target &00003800, not the stale TXCB (R7)')
cpu.mem[0x0d63] = 0

print('== 9d: Tube receive - frame streamed to R3, not host memory ==')
# CB buffer address with +6/+7 = &FE/&FF marks a Tube target
cpu.mem[0x0d63] = 0x01                          # tube_present (set by adlc_init via OSBYTE &EA)
cpu.mem[0x0d61] = 0x80
# parasite target &00003000: +4=00 +5=30 +6=00 +7=00 (high byte != &FF)
for off, v in enumerate([0x7f, 0x90, 0, 0, 0x00, 0x30, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00]):
    cpu.mem[txcb+off] = v
cpu.tube_out = bytearray()
tube_payload = bytes(range(0, 64))
cpu.mem[0x3000:0x3040] = b'\xee'*64            # poison host memory
hx('U %x %d %s' % (IP10, 32768, aun(2, 0x90, 0x00, 0x7000, tube_payload).hex()))
cpu.call(SYM['eco_rx_pump'])
check(bytes(cpu.tube_out) == tube_payload, 'rx payload streamed to Tube R3 in order')
check(cpu.mem[0x3000] == 0xee, 'host memory untouched (data went to the Tube)')
acks = [d for _,_,d in udp_out if d[0] == 3]
check(acks, 'Tube-delivered frame ACKed')

print('== 9e: Tube transmit - payload fetched from R3 ==')
cpu.tube_in = bytes(range(100, 100+32)); cpu.tube_in_pos = 0
cpu.tube_out = bytearray()
# TXCB: ctrl &80, port &99, dest 254.1, buffer start &0000 end &0020,
# ext bytes &FE/&FF marking Tube source
for off, v in enumerate([0x80, 0x99, 254, 1, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00]):
    cpu.mem[txcb+off] = v
cpu.mem[0xa0] = txcb; cpu.mem[0xa1] = 0
cpu.mem[0x0d63] = 0x01                          # tube_present
state = {'acked': False}
def wr_tube(self, a, v):
    orig_wr(self, a, v)
    if (a & 0xffff) == 0xfcaa and not state['acked']:
        for ip, port, data in list(udp_out):
            if data[0] == 2 and not state['acked']:
                check(data[8:8+32] == bytes(range(100,132)), 'tx payload came from Tube R3')
                hx('U %x %d %s' % (IP10, 32768, aun(3, 0x99, 0, int.from_bytes(data[4:8],'little')).hex()))
                state['acked'] = True
udp_out.clear()
CPU.wr = wr_tube
cpu.call(SYM['tx_begin'])
CPU.wr = orig_wr
check(state['acked'], 'Tube-sourced transmit completed and was ACKed')
check(cpu.mem[txcb] == 0x00, 'TXCB result success')

reset()
print('== 10: svc5 passes on a non-econet interrupt ==')
cpu.call(SYM['svc5_irq_check'])
check(cpu.a == 5, 'A=5 returned: interrupt passed to the next ROM')

reset()
print('== 11: page-crossing copy loops (>=256 bytes everywhere) ==')
# 11a: 1KB transmit through esb_page (etb_mem -> eco_stage_buf)
big_tx = bytes((i*7) & 0xff for i in range(1024))
cpu.mem[0x4000:0x4400] = big_tx
for off, v in enumerate([0x80, 0x99, 254, 1, 0x00, 0x40, 0xff, 0xff, 0x00, 0x44, 0xff, 0xff]):
    cpu.mem[txcb+off] = v
cpu.mem[0xa0] = txcb; cpu.mem[0xa1] = 0
cpu.mem[0x0d63] = 0                              # no tube: local path
state = {'acked': False}
def wr_big(self, a, v):
    orig_wr(self, a, v)
    if (a & 0xffff) == 0xfcaa and not state['acked']:
        for ip, port, data in list(udp_out):
            if data[0] == 2 and not state['acked']:
                check(data[8:] == big_tx, '1KB tx payload intact through the page loop')
                hx('U %x %d %s' % (IP10, 32768, aun(3, 0x99, 0, int.from_bytes(data[4:8],'little')).hex()))
                state['acked'] = True
udp_out.clear()
CPU.wr = wr_big
cpu.call(SYM['tx_begin'])
CPU.wr = orig_wr
check(cpu.mem[txcb] == 0x00, '1KB tx success')

# 11b: 600-byte Tube receive (etso_page) and Tube transmit (etsi_page)
cpu.mem[0x0d63] = 0x01
cpu.mem[0x0d61] = 0x80
for off, v in enumerate([0x7f, 0x90, 0, 0, 0x00, 0x30, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00]):
    cpu.mem[txcb+off] = v
cpu.tube_out = bytearray()
tp600 = bytes((i*3) & 0xff for i in range(600))
hx('U %x %d %s' % (IP10, 32768, aun(2, 0x90, 0x00, 0x8000, tp600).hex()))
cpu.call(SYM['eco_rx_pump'])
check(bytes(cpu.tube_out) == tp600, '600-byte rx payload streamed to Tube (page loop)')
cpu.tube_in = bytes((i*5) & 0xff for i in range(600)); cpu.tube_in_pos = 0
for off, v in enumerate([0x80, 0x99, 254, 1, 0x00, 0x00, 0x00, 0x00, 0x58, 0x02, 0x00, 0x00]):
    cpu.mem[txcb+off] = v                        # tube source, len &258=600
state = {'acked': False}
def wr_t600(self, a, v):
    orig_wr(self, a, v)
    if (a & 0xffff) == 0xfcaa and not state['acked']:
        for ip, port, data in list(udp_out):
            if data[0] == 2 and not state['acked']:
                check(data[8:] == bytes((i*5) & 0xff for i in range(600)), '600-byte Tube tx fetched via page loop')
                hx('U %x %d %s' % (IP10, 32768, aun(3, 0x99, 0, int.from_bytes(data[4:8],'little')).hex()))
                state['acked'] = True
udp_out.clear()
CPU.wr = wr_t600
cpu.call(SYM['tx_begin'])
CPU.wr = orig_wr
check(cpu.mem[txcb] == 0x00, '600-byte Tube tx success')
cpu.mem[0x0d63] = 0

# 11c: 600-byte remote PEEK and POKE (esb_page / eub_page)
for i in range(600): cpu.mem[0x4800+i] = (i*11) & 0xff
args = bytes([0x00,0x48,0xff,0xff, 0x58,0x4a,0xff,0xff])   # &4800-&4A58
udp_out.clear()
hx('U %x %d %s' % (IP10, 32768, aun(5, 0, 0x01, 0x9000, args).hex()))
hx('L')
cpu.call(SYM['svc5_irq_check'])
reps = [d for _,_,d in udp_out if d[0] == 6]
check(len(reps) == 1 and reps[0][8:] == bytes(cpu.mem[0x4800:0x4800+600]), '600-byte PEEK reply (page loop)')
poke = bytes([0x00,0x4c,0xff,0xff]) + bytes((i^0x5a)&0xff for i in range(600))
udp_out.clear()
hx('U %x %d %s' % (IP10, 32768, aun(5, 0, 0x02, 0x9004, poke).hex()))
hx('L')
cpu.call(SYM['svc5_irq_check'])
check(bytes(cpu.mem[0x4c00:0x4c00+600]) == poke[4:], '600-byte POKE written (page loop)')

reset()
print('== 12: workspace-slot scan (erp_next_slot / erp_try_ws) ==')
cpu.mem[0x0d61] = 0xc0                           # both lists active
cpu.mem[0x9f]  = 0x52                            # nfs_workspace_hi -> page &5200
for off, v in enumerate([0x7f, 0x91, 0, 0, 0,0,0,0, 0,0,0,0]):
    cpu.mem[txcb+off] = v                        # &00C0 CB: port &91 (mismatch)
for off, v in enumerate([0x7f, 0x92, 0, 0, 0,0,0,0, 0,0,0,0]):
    cpu.mem[0x5200+off] = v                      # ws slot 0: port &92 (mismatch)
for off, v in enumerate([0x7f, 0x90, 0, 0, 0x00, 0x53, 0xff, 0xff, 0x40, 0x53, 0xff, 0xff]):
    cpu.mem[0x520c+off] = v                      # ws slot 1: port &90, buf &5300
hx('U %x %d %s' % (IP10, 32768, aun(2, 0x90, 0x00, 0x9100, b'SLOT2!').hex()))
cpu.call(SYM['eco_rx_pump'])
check(bytes(cpu.mem[0x5300:0x5306]) == b'SLOT2!', 'frame delivered to the 2nd workspace slot')
check(cpu.mem[0x520c] == 0x80, 'slot 1 completed; slot 0 and &00C0 CB skipped')
check(cpu.mem[txcb] == 0x7f and cpu.mem[0x5200] == 0x7f, 'mismatching CBs untouched')

print('== 12b: empty &00C0 reply-CB falls through to the workspace list (F9) ==')
# With bit7 (&00C0 list) AND bit6 (workspace list) both active but the &00C0
# reply CB empty, the old scanner dropped the frame at the empty &00C0 entry
# instead of trying the workspace slots - so a frame a workspace slot would
# take was lost. Now an empty &00C0 list falls through to the workspace list.
cpu.mem[0x0d61] = 0xc0                            # &00C0 (bit7) + workspace (bit6) active
cpu.mem[0x9f]  = 0x52                             # workspace page &5200
cpu.mem[0xc0]  = 0x00                             # &00C0 reply CB EMPTY
for off, v in enumerate([0x7f, 0x90, 0, 0, 0x00, 0x53, 0xff, 0xff, 0x40, 0x53, 0xff, 0xff]):
    cpu.mem[0x5200+off] = v                       # ws slot 0: port &90 -> buf &5300
for i in range(6): cpu.mem[0x5300+i] = 0
hx('U %x %d %s' % (IP10, 32768, aun(2, 0x90, 0x00, 0x9b00, b'WSONLY').hex()))
cpu.call(SYM['eco_rx_pump'])
check(bytes(cpu.mem[0x5300:0x5306]) == b'WSONLY',
      'empty &00C0 CB: frame delivered to the workspace slot, not dropped (F9)')
check(cpu.mem[0x5200] == 0x80, 'workspace slot CB completed')
cpu.mem[0x0d61] = 0x80

reset()
print('== 13: hooks, bad ctrl, not-ready ==')
# eco_pump_dec2: pump + DEC of the wait_net_tx_ack middle counter
for off, v in enumerate([0x7f, 0x90, 0, 0, 0x00, 0x31, 0xff, 0xff, 0x7f, 0x31, 0xff, 0xff]):
    cpu.mem[txcb+off] = v
hx('U %x %d %s' % (IP10, 32768, aun(2, 0x90, 0x00, 0x9200, b'HOOK').hex()))
cpu.sp = 0xf0; cpu.x = 0xf0                      # X = SP as wait_net_tx_ack's TSX
cpu.mem[0x01f2] = 5                              # middle counter at &0102+X
cpu.call(SYM_PUMPDEC) if False else None
cpu.push(0xff); cpu.push(0x41)                   # manual JSR frame for call()
cpu.pc = 0  # (placeholder; use call helper instead)
cpu.sp = 0xf0
cpu.x = 0xf0
cpu.call(SYM['eco_pump_dec2'])
check(bytes(cpu.mem[0x3100:0x3104]) == b'HOOK', 'eco_pump_dec2 pumped the frame')
check(cpu.mem[0x01f2] == 4, 'eco_pump_dec2 decremented the middle counter')
# eco_osw12_shim: pump + displaced instructions
cpu.mem[0x9d] = 0x77                             # net_rx_ptr_hi
cpu.call(SYM['eco_osw12_shim'])
check(cpu.mem[0xab] == 0x77, 'osw12 shim ran the displaced lda/sta')
# bad control byte (port 0, ctrl &8C -> out of immediate range)
for off, v in enumerate([0x8c, 0x00, 254, 1, 0x00, 0x30, 0xff, 0xff, 0x04, 0x30, 0xff, 0xff]):
    cpu.mem[txcb+off] = v
cpu.call(SYM['tx_begin'])
check(cpu.mem[txcb] == 0x44, 'ctrl &8C port 0 -> &44 bad control block')
# Pi not ready: INIT fails -> placeholder station, C=1
hx('R 0')
cpu.mem[0x0d23] = 0                              # force re-init
cpu.call(SYM['eco_ensure_init'])
check(cpu.c and cpu.y == 0xfe, 'Pi not ready: C=1, placeholder station &FE')
hx('R 1')
cpu.mem[0x0d23] = 0
cpu.call(SYM['eco_ensure_init'])
check(not cpu.c and cpu.y == 32, 'recovers once the network is back')

reset()
print('== 14: outbound immediate with >256-byte reply (eti_rpage) ==')
for off, v in enumerate([0x81, 0x00, 254, 1, 0x00, 0x56, 0xff, 0xff, 0x58, 0x58, 0xff, 0xff,
                          0,0,0,0]):
    cpu.mem[txcb+off] = v                        # peek, reply buffer &5600 size 600
state = {'acked': False}
rep600 = bytes((i*13) & 0xff for i in range(600))
def wr_imm6(self, a, v):
    orig_wr(self, a, v)
    if (a & 0xffff) == 0xfcaa and not state['acked']:
        for ip, port, data in list(udp_out):
            if data[0] == 5 and not state['acked']:
                hx('U %x %d %s' % (IP10, 32768, aun(6, 0, 0x01, int.from_bytes(data[4:8],'little'), rep600).hex()))
                state['acked'] = True
udp_out.clear()
CPU.wr = wr_imm6
cpu.call(SYM['tx_begin'])
CPU.wr = orig_wr
check(cpu.mem[txcb] == 0x00, 'immediate with 600-byte reply succeeded')
check(bytes(cpu.mem[0x5600:0x5600+600]) == rep600, '600-byte reply landed (page loop)')

reset()
print('== 15: remote HALT spins in svc5 until CONTINUE ==')
udp_out.clear()
hx('U %x %d %s' % (IP10, 32768, aun(5, 0, 0x06, 0xa000).hex()))    # HALT
hx('L')
inj = {'reads': 0, 'done': False}
orig_rd = CPU.rd
def rd_inject(self, a):
    if (a & 0xffff) == 0xfcab:
        inj['reads'] += 1
        if inj['reads'] == 30 and not inj['done']:
            inj['done'] = True
            hx('U %x %d %s' % (IP10, 32768, aun(5, 0, 0x07, 0xa004).hex()))  # CONTINUE
            hx('L')
    return orig_rd(self, a)
CPU.rd = rd_inject
cpu.call(SYM['svc5_irq_check'], limit=5_000_000)
CPU.rd = orig_rd
check(inj['done'], 'machine stayed halted across many polls')
check(cpu.a == 0, 'svc5 returned claimed after CONTINUE released the halt')
check(cpu.mem[0x0d37] == 0, 'halt flag cleared')
reps = [d for _,_,d in udp_out if d[0] == 6]
check(len(reps) == 2, 'both HALT and CONTINUE were acknowledged')

reset()
print('== 16: adlc_init (BREAK-time entry) and pump bail paths ==')
cpu.mem[0xfff4] = 0x60                           # stub MOS OSBYTE: RTS
cpu.mem[0x0d23] = 0x80                           # already inited: ensure_init fast path
cpu.call(SYM['adlc_init'])
check(cpu.mem[0x0d60] == 0x80 and cpu.mem[0x0d62] == 0x80,
      'adlc_init sets tx_complete_flag and econet_init_flag')
check(cpu.mem[0x0d23] == 0x80, 'engine re-validated through ensure_init')
# erp_bail: pump is a no-op before INIT
cpu.mem[0x0d23] = 0
cpu.a, cpu.x, cpu.y = 0x12, 0x34, 0x56
cpu.call(SYM['eco_rx_pump'])
check((cpu.a, cpu.x, cpu.y) == (0x12, 0x34, 0x56), 'pump bails cleanly before INIT, registers preserved')
cpu.mem[0x0d23] = 0x80

reset()
print('== 17: service gate (check_adlc_flag) never declines (the *HELP/*Net bug) ==')
# Regression for the bug where ALL service calls were declined when
# rom_ws_pages[slot] bit7 was set (no *HELP, *Net = Bad command).
# Drive check_adlc_flag directly with the absent-flag bit SET and
# confirm it falls through to handle_vectors_claimed (&8a62) instead
# of doing the decline RTS.
GATE = 0x8a5a; HANDLE = 0x8a62
for flag in (0x00, 0x80, 0xff):          # incl. bit7 set = old failure
    cpu.mem[0x0df0] = flag               # rom_ws_pages[slot 0]
    cpu.x = 0
    cpu.sp = 0xf0
    cpu.push(9)                          # service number, as dispatch_service pha'd
    cpu.pc = GATE
    declined = False
    for _ in range(12):
        if cpu.pc == HANDLE: break
        # a decline is an RTS that pops our pushed byte+return -> pc leaves the block
        if cpu.pc < GATE or cpu.pc > HANDLE+1:
            declined = True; break
        cpu.step()
    check(cpu.pc == HANDLE and not declined,
          'gate continues with rom_ws_pages=&%02X (was the decline bug)' % flag)

reset()
print('== 18: service 15 must NOT clear our ROM type-table entry (the *HELP de-service bug) ==')
# Regression for the bug fixed by patch P12. On a Master (OSBYTE 0 returns
# X>=3) service_handler ran "sta rom_type_table,x" with X=romsel+1, zeroing
# this ROM's cached type byte at &02A1+slot. The Master offers service calls
# from that cache, so AUNFS was silently dropped right after service 15 -
# *ROMS still listed it (live &8006 intact) but *HELP/*Net never reached it.
# P12 NOPs the 5 bytes at &8A33-&8A37. Drive service_handler with service 15,
# stub OSBYTE 0 to report a Master, and confirm the entry is left intact.
SVCH = 0x8a15
cpu.mem[0xfff4] = 0xa2; cpu.mem[0xfff5] = 0x03; cpu.mem[0xfff6] = 0x60  # OSBYTE stub: LDX #3 (Master) / RTS
cpu.mem[0xf4]   = 0x00          # romsel_copy = slot 0
cpu.mem[0x02a1] = 0x82          # our cached ROM type byte (service + language bits)
cpu.sp = 0xf0
cpu.a  = 0x0f                   # service 15 (vectors claimed)
cpu.pc = SVCH
for _ in range(40):
    if cpu.pc == 0x8a38:        # reached restore_rom_slot, just past the (NOPed) clear
        break
    cpu.step()
check(cpu.pc == 0x8a38, 'service_handler reached restore_rom_slot after service 15')
check(cpu.mem[0x02a1] == 0x82, 'service 15 left rom_type_table[slot] intact (P12 fix)')

reset()
print('== 19: aun_station=ip / ip.ip derive station (and net) from our IPv4 ==')
# The harness pins our address to 192.168.1.20 (the R command), so the last
# octet is 20 and the third octet is 1.  Drive INIT (station byte 0 = "use
# the Pi-side configuration") + STATUS straight through the command interface
# and read back the station/net the engine resolved.
CP = 0xff0000 | (0xe0 << 8)            # command page 0xE0 (DISC_RAM_BASE == 0)
def init_and_status(cmdline):
    hx('S ' + cmdline)
    for off in range(16):              # clear the command block
        hx('P %x 0' % (CP + off))
    hx('P %x %x' % (CP, 30))           # AUN_CMD_INIT, station byte 0 = use Pi cfg
    hx('C e0')
    hx('P %x %x' % (CP, 31))           # AUN_CMD_STATUS
    hx('C e0')
    return hx('G %x' % (CP + 4)), hx('G %x' % (CP + 5))   # (station, net)

stn, net = init_and_status('aun_station=ip')
check(stn == 20, 'aun_station=ip -> station = last IP octet 20 (got %d)' % stn)
check(net == 0,  'aun_station=ip -> net 0 (got %d)' % net)
stn, net = init_and_status('aun_station=ip.ip')
check(stn == 20, 'aun_station=ip.ip -> station 20 (got %d)' % stn)
check(net == 1,  'aun_station=ip.ip -> net = third IP octet 1 (got %d)' % net)

print('== 20: model-B (NMOS 6502) compatibility across the whole suite ==')
# Standing invariants accumulated over every test above: the 4.18 AUNFS path
# must run on a stock BBC B, so across all init/tx/rx-pump/svc5/immediate/tube
# exercises it must never touch a Master-only register or execute a 65C02 op.
check(cpu.master_hits == 0,
      'no ACCCON/INTOFF (&FE34/&FE38) access in %d instructions' % cpu.instructions)
check(cpu.c02_hits == 0,
      'no 65C02-only opcode fetched (run aborts on first if any)')

H.stdin.write('Q\n')
print(f'\nALL {ok} LOCKSTEP CHECKS PASSED  ({cpu.instructions} instructions executed)')
