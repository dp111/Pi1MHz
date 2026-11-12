/* ramprobe.c - the Pi's side of the RAM probe.  See ramprobe.h and
   docs/dev/ramprobe.md.

   The BBC's side of every step runs on the bus alone: the agent polls bytes in
   the &FD00 window and answers through the services port, whose FIQ handlers
   are always there.  So nothing here needs its own FIQ; the main-loop poll
   writes the mailbox and watches the report in JIM.

   The window holds the agent's page for as long as the machine is stopped, and
   only then does this code write to it. */
#include <stdlib.h>
#include <string.h>

#include "Pi1MHz.h"
#include "helpers.h"
#include "ramprobe.h"
#include "services.h"
#include "rpi/asm-helpers.h"

#define WINDOW(off)   (Pi1MHz_MEM_PAGE + (uint32_t)(off))
#define MB_CODE       0xA0u
#define MB_COMMAND    0xF0u
#define MB_SEQ        0xF1u
#define MB_A          0xF3u
#define MB_X          0xF4u
#define MB_Y          0xF5u
#define CMD_RESUME    1u
#define CMD_CALL      2u
#define RP_DATA       (RP_REPORT + 0x100u)     /* peek and poke bytes */
#define RP_SEQ_AT     (RP_CALL_AT + 4u)
#define CHUNK         256u

#define NMI_EVERY_US  40000u      /* the NMI again, if it went unanswered */
#define NMI_TRIES     3u
#define LEAVE_US      1000u       /* after a continue, the agent's last instructions: no NMI yet */
#define EVENT_WAIT_US 300000u     /* rung 0: the vsync event comes every 20 ms */
#define IRQ_WAIT_US   40000u      /* rung 1: an IRQ is taken at once, unless interrupts are off */
#define STEP_LIMIT_US 500000u     /* a step of work */
#define CALL_LIMIT_US 5000000u    /* a call of someone else's code */

typedef enum { OP_NONE, OP_PEEK, OP_POKE, OP_CALL, OP_REGS } op_t;

static struct {
   bool     registered;
   rp_state_t state;
   char     error[96];
   rp_regs_t stopped, result;
   /* the transfer under way */
   op_t     op;
   bool     waiting;
   rp_range_t ranges[RP_MAX_RANGES];
   unsigned nranges;
   unsigned ri;                /* the range under way, and how far into it */
   uint32_t rdone;
   uint32_t length, done;      /* all the ranges' bytes, and how many are done */
   unsigned next_ri;           /* where the call under way leaves them */
   uint32_t next_rdone, next_done;
   int      bank;
   uint8_t  seq;
   uint32_t t0, limit;
   /* how the agent is asked in */
   int      rung;              /* the rung asked for, then the one the stop came in by */
   bool     oneshot;           /* in, the transfer, and out again */
   bool     irq_on;
   uint8_t  src;               /* the probe's slot, for nIRQ */
   /* the NMI */
   bool     nmi_low;
   uint32_t nmi_us;
   uint32_t left_us;           /* when the agent last said it was going */
   unsigned nmi_sent;
   uint32_t peeked;
   uint32_t resets;            /* BBC resets seen: each one loses the install */
} R;

_Static_assert(RP_REPORT == SERVICES_RAMPROBE_FIRST && RP_DATA + RP_MAX_TRANSFER - 1u == SERVICES_RAMPROBE_LAST,
               "the probe's place in the services area, as services.h records it");

static uint8_t *J(uint32_t off) { return &Pi1MHz->JIM_ram[DISC_RAM_BASE + off]; }

static void fail(const char *why)
{
   size_t n = strlen(why);
   if (n >= sizeof R.error) n = sizeof R.error - 1u;
   memcpy(R.error, why, n);
   R.error[n] = '\0';
}

static void window_write(uint32_t off, const uint8_t *bytes, size_t n)
{
   for (size_t i = 0; i < n; i++)
      Pi1MHz_MemoryWrite(WINDOW(off + i), bytes[i]);
}

/* GPFSEL1 holds nIRQ's pin too, which the FIQ may change: hold it off. */
static void nmi_line(bool low)
{
   unsigned int cpsr = _disable_interrupts_cspr();
   Pi1MHz_SetnNMI(low ? ASSERT_NMI : CLEAR_NMI);
   _set_interrupts(cpsr);
   R.nmi_low = low;
}

/* Rungs 0 and 1: a hook goes in only while its first byte is BIT, not JMP (and for rung 1,
   nIRQ held).  One byte, so the 6502 sees one or the other, never half of each. */
static void ask(int rung)
{
   if (rung == RP_RUNG_IRQ) {
      Pi1MHz_MemoryWrite(RP_HOOK_IRQ2, RP_WANTED);
      R.irq_on = true;
      Pi1MHz_nIRQ_ASSERT(R.src);
   } else if (rung == RP_RUNG_EVENT) {
      Pi1MHz_MemoryWrite(RP_HOOK_EVENT, RP_WANTED);
   }
}

/* Nothing asked of the BBC any more: what every way out of a halt does, served or not. */
static void unask(void)
{
   Pi1MHz_MemoryWrite(RP_HOOK_EVENT, RP_IDLE);
   Pi1MHz_MemoryWrite(RP_HOOK_IRQ2, RP_IDLE);
   if (R.irq_on) {
      R.irq_on = false;
      Pi1MHz_nIRQ_CLEAR(R.src);
   }
   if (R.nmi_low) nmi_line(false);
}

/* ---- code the agent runs at &FDA0 ---------------------------------------- */

/* Select a sideways bank, keeping the OS's copy true; the old one on the stack. */
static size_t emit_bank_in(uint8_t *c, size_t n, int bank)
{
   c[n++] = 0xA5; c[n++] = 0xF4;                          /* LDA &F4 */
   c[n++] = 0x48;                                         /* PHA */
   c[n++] = 0xA9; c[n++] = (uint8_t)bank;                 /* LDA #bank */
   c[n++] = 0x85; c[n++] = 0xF4;                          /* STA &F4 */
   c[n++] = 0x8D; c[n++] = 0x30; c[n++] = 0xFE;           /* STA &FE30 */
   return n;
}
static size_t emit_bank_out(uint8_t *c, size_t n)
{
   c[n++] = 0x68;                                         /* PLA */
   c[n++] = 0x85; c[n++] = 0xF4;                          /* STA &F4 */
   c[n++] = 0x8D; c[n++] = 0x30; c[n++] = 0xFE;           /* STA &FE30 */
   return n;
}

/* Point the services port at a services offset, as the agent does for its report: three writes,
   the top byte last (that loads the data register).  The agent saved the program's pointer on
   the way in and puts it back on the way out. */
static size_t emit_port(uint8_t *c, size_t n, uint32_t offset)
{
   c[n++] = 0xA9; c[n++] = (uint8_t)offset;               /* LDA #lo */
   c[n++] = 0x8D; c[n++] = 0xA6; c[n++] = 0xFC;           /* STA &FCA6 */
   c[n++] = 0xA9; c[n++] = (uint8_t)(offset >> 8);        /* LDA #mid */
   c[n++] = 0x8D; c[n++] = 0xA7; c[n++] = 0xFC;           /* STA &FCA7 */
   c[n++] = 0xA9; c[n++] = (uint8_t)(offset >> 16);       /* LDA #hi */
   c[n++] = 0x8D; c[n++] = 0xA8; c[n++] = 0xFC;           /* STA &FCA8 */
   return n;
}

/* A loop of `count` bytes (0 means 256): memory to the port, or the port to memory. */
static size_t emit_copy(uint8_t *c, size_t n, uint16_t address, unsigned count, bool to_port)
{
   c[n++] = 0xA2; c[n++] = 0x00;                          /* LDX #0 */
   size_t top = n;
   if (to_port) {
      c[n++] = 0xBD; c[n++] = (uint8_t)address; c[n++] = (uint8_t)(address >> 8);  /* LDA addr,X */
      c[n++] = 0x8D; c[n++] = 0xA9; c[n++] = 0xFC;                                 /* STA &FCA9 */
   } else {
      c[n++] = 0xAD; c[n++] = 0xA9; c[n++] = 0xFC;                                 /* LDA &FCA9 */
      c[n++] = 0x9D; c[n++] = (uint8_t)address; c[n++] = (uint8_t)(address >> 8);  /* STA addr,X */
   }
   c[n++] = 0xE8;                                         /* INX */
   c[n++] = 0xE0; c[n++] = (uint8_t)count;                /* CPX #count */
   c[n++] = 0xD0; c[n] = (uint8_t)(top - (n + 1u)); n++;  /* BNE top */
   return n;
}

static void send_command(uint8_t command)
{
   R.seq = (uint8_t)(R.seq + 1u) ? (uint8_t)(R.seq + 1u) : 1u;
   Pi1MHz_MemoryWrite(WINDOW(MB_COMMAND), command);
   Pi1MHz_MemoryWrite(WINDOW(MB_SEQ), R.seq);            /* last: it sets the agent off */
}

/* Code for the agent to run at &FDA0, in its own page. */
static void send_call(const uint8_t *code, size_t n, uint8_t a, uint8_t x, uint8_t y, uint32_t limit)
{
   window_write(MB_CODE, code, n);
   Pi1MHz_MemoryWrite(WINDOW(MB_A), a);
   Pi1MHz_MemoryWrite(WINDOW(MB_X), x);
   Pi1MHz_MemoryWrite(WINDOW(MB_Y), y);
   *J(RP_REPORT + RP_SEQ_AT) = 0;
   send_command(CMD_CALL);
   R.waiting = true;
   R.t0 = Pi1MHz_now_us;
   R.limit = limit;
}

/* As much as one call's code holds: a copy loop for each piece of up to CHUNK bytes, from as
   many ranges as fit.  The port moves on by itself, so the bytes land back to back. */
#define COPY_LEN 13u                      /* emit_copy's loop */
static void send_chunk(void)
{
   uint8_t c[RP_CODE_MAX];
   size_t n = 0;
   unsigned ri = R.ri;
   uint32_t rdone = R.rdone, done = R.done;
   const size_t tail = 1u + (R.bank != RP_NO_BANK ? 6u : 0u);    /* the bank back, RTS */
   n = emit_port(c, n, RP_DATA + done);
   if (R.bank != RP_NO_BANK) n = emit_bank_in(c, n, R.bank);
   do {
      uint32_t left = R.ranges[ri].length - rdone;
      unsigned count = left >= CHUNK ? CHUNK : (unsigned)left;
      n = emit_copy(c, n, (uint16_t)(R.ranges[ri].address + rdone), count, R.op == OP_PEEK);
      done += count;
      rdone += count;
      if (rdone == R.ranges[ri].length) { ri++; rdone = 0; }
   } while (ri < R.nranges && n + COPY_LEN + tail <= RP_CODE_MAX);
   R.next_ri = ri;
   R.next_rdone = rdone;
   R.next_done = done;
   if (R.bank != RP_NO_BANK) n = emit_bank_out(c, n);
   c[n++] = 0x60;                                         /* RTS */
   send_call(c, n, 0, 0, 0, STEP_LIMIT_US);
}

static void start_resume(void)
{
   R.state = RP_RESUMING;
   R.t0 = Pi1MHz_now_us;
   send_command(CMD_RESUME);
}

static void read_result(void)
{
   const uint8_t *r = J(RP_REPORT + RP_CALL_AT);
   R.result.a = r[0];
   R.result.x = r[1];
   R.result.y = r[2];
   R.result.p = r[3];
}

/* ---- the poll ------------------------------------------------------------- */

/* The SD card's 6502code.bin has this build's probe pages: each its own number, and the agent's
   RP_VERSION.  It is loaded apart from the kernel, so the two can differ. */
#define MISMATCH "the SD card's Pi1MHz/6502code.bin isn't this firmware's (pages 22 and 23): copy this build's"
static bool pages_ok(void)
{
   const uint8_t *agent = helpers_page(RP_HELPER_AGENT);
   return agent != NULL && helpers_page_number(RP_HELPER_AGENT) == RP_HELPER_AGENT
       && helpers_page_number(RP_HELPER_INSTALL) == RP_HELPER_INSTALL
       && agent[RP_VERSION_AT] == RP_VERSION;
}

static bool start_halt(int rung);

static void ramprobe_poll(void)
{
   if (R.nmi_low) nmi_line(false);          /* one pass low: the 6502 wants the edge */

   switch (R.state) {
   case RP_RUNNING:
      /* In when nothing asked: a hook that read BIT just before a failed rung put JMP back.  Let
         it go on at once, so a failure leaves the BBC as it was. */
      if (*J(RP_REPORT + RP_HALTED_AT) == 1u && ramprobe_installed()) {
         *J(RP_REPORT + RP_HALTED_AT) = 2u;          /* seen: not again */
         start_resume();
      }
      break;

   case RP_HALTING:
      if (*J(RP_REPORT + RP_HALTED_AT) == 1u) {
         const uint8_t *r = J(RP_REPORT);
         unask();
         R.stopped = (rp_regs_t){ .y = r[0], .x = r[1], .a = r[2], .p = r[3], .pc = (uint16_t)(r[4] | (r[5] << 8)), .s = r[6] };
         R.state = RP_HALTED;
         if (R.oneshot) {
            R.state = RP_WORKING;
            send_chunk();
         }
      } else if (R.rung != RP_RUNG_NMI) {
         if (Pi1MHz_now_us - R.t0 >= (R.rung == RP_RUNG_EVENT ? EVENT_WAIT_US : IRQ_WAIT_US)) {
            unask();
            fail(R.rung == RP_RUNG_EVENT
                 ? "rung 0 (the vsync event) didn't answer in 300 ms: events off, or EVNTV taken?"
                 : "rung 1 (nIRQ, IRQ2V) didn't answer in 40 ms: interrupts off, or IRQ2V taken?");
            R.oneshot = false;
            R.rung = RP_RUNG_NONE;
            R.state = RP_RUNNING;
         }
      } else if (Pi1MHz_now_us - R.nmi_us >= NMI_EVERY_US) {
         if (R.nmi_sent >= NMI_TRIES) {
            unask();
            fail("rung 2: no answer to the NMI: its JMP at &0D00 may be gone (a disc access, or a program)");
            R.oneshot = false;
            R.rung = RP_RUNG_NONE;
            R.state = RP_RUNNING;
         } else {
            R.nmi_us = Pi1MHz_now_us;
            R.nmi_sent++;
            nmi_line(true);
         }
      }
      break;

   case RP_WORKING:
      if (!R.waiting) break;
      if (*J(RP_REPORT + RP_SEQ_AT) == R.seq) {
         R.waiting = false;
         read_result();
         if (R.op == OP_CALL) {
            R.state = RP_HALTED;
         } else if (R.op == OP_REGS) {
            start_resume();
         } else {
            R.ri = R.next_ri;
            R.rdone = R.next_rdone;
            R.done = R.next_done;
            if (R.ri < R.nranges) {
               send_chunk();
            } else {
               if (R.op == OP_PEEK) R.peeked = R.length;
               R.state = RP_HALTED;
               if (R.oneshot) {
                  R.oneshot = false;
                  start_resume();
               }
            }
         }
      } else if (Pi1MHz_now_us - R.t0 >= R.limit) {
         /* The machine is off somewhere in that code: nothing to go back to. */
         fail("the code didn't return: the machine may need a Break");
         R.waiting = false;
         R.oneshot = false;
         R.state = RP_RUNNING;
      }
      break;

   case RP_RESUMING:
      if (*J(RP_REPORT + RP_HALTED_AT) == 0u) {
         R.state = RP_RUNNING;
         R.left_us = Pi1MHz_now_us;
      } else if (Pi1MHz_now_us - R.t0 >= STEP_LIMIT_US) {
         fail("the agent didn't leave");
         R.state = RP_RUNNING;
         R.left_us = Pi1MHz_now_us;      /* it may yet: no NMI into it for LEAVE_US either */
      }
      break;

   default:
      break;
   }
}

/* ---- the calls ------------------------------------------------------------ */

static bool need_halted(void)
{
   if (R.state == RP_HALTED) return true;
   fail(R.state == RP_OFF ? "off" : "the machine isn't stopped");
   return false;
}

static bool start_halt(int rung)
{
   if (R.state != RP_RUNNING) { fail(R.state == RP_OFF ? "off" : "busy"); return false; }
   if (rung < RP_RUNG_EVENT || rung > RP_RUNG_NMI) { fail("bad rung: 0, 1 or 2"); return false; }
   if (!pages_ok()) {
      fail(MISMATCH);
      return false;
   }
   if (*J(RP_REPORT + RP_INSTALLED_AT) != RP_INSTALLED) {
      fail("not installed: type X%=23:CALL &FC88 on the BBC");
      return false;
   }
   memset(J(RP_REPORT), 0, RP_INSTALLED_AT);
   R.rung = rung;
   R.state = RP_HALTING;
   R.t0 = Pi1MHz_now_us;
   if (rung == RP_RUNG_NMI && Pi1MHz_now_us - R.left_us < LEAVE_US) {
      /* The agent has said it's going but may still be on its way out, between that and
         rp_exit's RTI: an NMI there would nest a stop inside it.  The poll sends it when due. */
      R.nmi_sent = 0u;
      R.nmi_us = R.left_us + LEAVE_US - NMI_EVERY_US;
   } else if (rung == RP_RUNG_NMI) {
      R.nmi_sent = 1u;
      R.nmi_us = Pi1MHz_now_us;
      nmi_line(true);
   } else {
      ask(rung);
   }
   return true;
}

bool ramprobe_halt(int rung)
{
   R.error[0] = '\0';
   if (R.state == RP_HALTED) return true;
   return start_halt(rung);
}

bool ramprobe_resume(const rp_resume_t *how)
{
   R.error[0] = '\0';
   if (!need_halted()) return false;
   if (how != NULL && (how->set_pc || how->set_p || how->set_a || how->set_x || how->set_y)) {
      /* Rung 0 stops inside the OS's vsync event, with the OS's IRQ frames under its own: its
         registers are the OS's, and a new PC would leave them on the stack, interrupts off. */
      if (R.rung == RP_RUNG_EVENT) {
         fail("rung 0 stops inside the OS: its registers aren't the program's; halt by rung 1 or 2 to set them");
         return false;
      }
      /* Run as a call first, writing them where the agent left them on the stack: with S as
         stopped, the frame at S+1 to S+3, A at S, X at S-1, Y at S-2 (6502code.asm, page 22). */
      uint8_t c[RP_CODE_MAX];
      size_t n = 0;
      const uint8_t s = R.stopped.s;
      #define SET(k, v) do { uint16_t at_ = (uint16_t)(0x100u + (uint8_t)(s + (k))); \
         c[n++] = 0xA9; c[n++] = (uint8_t)(v);                                      /* LDA #v */ \
         c[n++] = 0x8D; c[n++] = (uint8_t)at_; c[n++] = (uint8_t)(at_ >> 8); } while (0)  /* STA */
      if (how->set_pc) { SET(2, how->pc); SET(3, how->pc >> 8); }
      if (how->set_p) SET(1, how->p);
      if (how->set_a) SET(0, how->a);
      if (how->set_x) SET(-1, how->x);
      if (how->set_y) SET(-2, how->y);
      #undef SET
      c[n++] = 0x60;
      R.op = OP_REGS;
      R.state = RP_WORKING;
      send_call(c, n, 0, 0, 0, STEP_LIMIT_US);
      return true;
   }
   start_resume();
   return true;
}

/* Stopped: the transfer now.  Running, with a rung: in by it, the transfer, out again. */
static bool start_transfer(op_t op, const rp_range_t *ranges, unsigned count, int bank, int rung,
                           const uint8_t *bytes)
{
   bool now = R.state == RP_HALTED;
   if (!now && (rung == RP_RUNG_NONE || R.state != RP_RUNNING)) return need_halted();
   if (count == 0u || count > RP_MAX_RANGES) { fail("bad ranges: 1 to 32"); return false; }
   uint32_t total = 0;
   for (unsigned i = 0; i < count; i++) {
      uint32_t address = ranges[i].address, length = ranges[i].length;
      if (length == 0u || length > RP_MAX_TRANSFER || address + length > 0x10000u) {
         fail("bad address or length");
         return false;
      }
      /* The I/O pages, both ways: reading FRED, JIM or SHEILA has side effects (&FCA9 moves the
         services port the transfer itself uses; a VIA or ACIA read clears interrupt flags under
         the running OS), and the agent runs from &FD00.  /bbc/run is for deliberate I/O. */
      if (address < 0xFF00u && address + length > 0xFC00u) { fail("that's I/O: &FC00-&FEFF (use /bbc/run)"); return false; }
      /* The NMI's JMP: the rest of the probe is in FRED, out of reach. */
      if (op == OP_POKE && address < RP_NMI_JMP + 3u && address + length > RP_NMI_JMP) {
         fail("that's the probe's NMI JMP at &0D00-&0D02");
         return false;
      }
      total += length;
   }
   if (total > RP_MAX_TRANSFER) { fail("more than 64 KB in all"); return false; }
   if (bank != RP_NO_BANK && (bank < 0 || bank > 15)) { fail("bad bank"); return false; }
   R.op = op;
   memcpy(R.ranges, ranges, count * sizeof *ranges);
   R.nranges = count;
   R.ri = 0;
   R.rdone = 0;
   R.length = total;
   R.done = 0;
   R.bank = bank;
   R.peeked = 0;
   if (!now) {
      R.oneshot = true;
      if (!start_halt(rung)) { R.oneshot = false; return false; }
   }
   /* Only now, every check passed and the probe installed: the data area is the probe's. */
   if (bytes != NULL) memcpy(J(RP_DATA), bytes, total);
   if (!now) return true;
   R.state = RP_WORKING;
   send_chunk();
   return true;
}

bool ramprobe_peek_ranges(const rp_range_t *ranges, unsigned count, int bank, int rung)
{
   R.error[0] = '\0';
   return start_transfer(OP_PEEK, ranges, count, bank, rung, NULL);
}

bool ramprobe_peek(uint16_t address, uint32_t length, int bank, int rung)
{
   const rp_range_t one = { address, length };
   return ramprobe_peek_ranges(&one, 1u, bank, rung);
}

bool ramprobe_poke_ranges(const rp_range_t *ranges, unsigned count, const uint8_t *bytes, int rung)
{
   R.error[0] = '\0';
   return start_transfer(OP_POKE, ranges, count, RP_NO_BANK, rung, bytes);
}

bool ramprobe_poke(uint16_t address, const uint8_t *bytes, uint32_t length, int rung)
{
   const rp_range_t one = { address, length };
   return ramprobe_poke_ranges(&one, 1u, bytes, rung);
}

bool ramprobe_run(const uint8_t *code, uint32_t length, uint8_t a, uint8_t x, uint8_t y)
{
   R.error[0] = '\0';
   if (!need_halted()) return false;
   if (length == 0u || length > RP_CODE_MAX) { fail("code too long"); return false; }
   R.op = OP_CALL;
   R.state = RP_WORKING;
   send_call(code, length, a, x, y, CALL_LIMIT_US);
   return true;
}

bool ramprobe_call(uint16_t address, uint8_t a, uint8_t x, uint8_t y)
{
   const uint8_t c[3] = {0x4C, (uint8_t)address, (uint8_t)(address >> 8)};   /* JMP: its RTS comes home */
   return ramprobe_run(c, sizeof c, a, x, y);
}

/* ---- what it says ----------------------------------------------------------- */

rp_state_t ramprobe_state(void) { return R.state; }

const char *ramprobe_state_name(void)
{
   static const char *const Names[] = { "off", "running", "halting", "halted", "working", "resuming" };
   /* Not "running" before the install: the Pi knows nothing of the BBC until then, not
      even that one is there (a BASIC prompt makes no 1MHz bus cycles). */
   if (R.state == RP_RUNNING && !ramprobe_installed()) return "waiting";
   return Names[R.state];
}

/* The FRED code, from page 23, with both hooks idle until the install writes the old vectors
   into them; only those four bytes are the BBC's to write.  None of it if 6502code.bin has no
   probe pages.  rp_in's PHA only if the pages are this build's: the install checks for it.  The
   rest is written even when they aren't, so that an install from another build that doesn't
   check (an earlier one) hooks its own code, not whatever FRED held; and start_halt refuses
   to use it. */
static void put_fred(void)
{
   static const struct { uint8_t at, len; } Blocks[] = {
      { RP_FRED_A, RP_FRED_A_LEN }, { RP_FRED_B, RP_FRED_B_LEN }, { RP_FRED_C, RP_FRED_C_LEN },
   };
   if (helpers_page_number(RP_HELPER_INSTALL) != RP_HELPER_INSTALL) {
      Pi1MHz_MemoryWrite(RP_FRED_A + RP_IN_AT, 0x00u);
      return;
   }
   const uint8_t *page = helpers_page(RP_HELPER_INSTALL);
   const uint8_t *code = page + RP_FRED_IN_PAGE;
   for (unsigned b = 0; b < sizeof Blocks / sizeof Blocks[0]; b++)
      for (unsigned i = 0; i < Blocks[b].len; i++)
         Pi1MHz_MemoryWrite(Blocks[b].at + i, *code++);
   if (pages_ok()) Pi1MHz_MemoryWrite(RP_FRED_A + RP_IN_AT, RP_IN_OP);   /* page 23 has a 0 there */
   for (unsigned i = 1; i <= 2u; i++) {
      Pi1MHz_Register_Memory(WRITE_FRED, RP_HOOK_EVENT + i, Pi1MHz_EmulatedMemoryByte);
      Pi1MHz_Register_Memory(WRITE_FRED, RP_HOOK_IRQ2 + i, Pi1MHz_EmulatedMemoryByte);
   }
}

bool ramprobe_installed(void) { return R.state != RP_OFF && pages_ok() && *J(RP_REPORT + RP_INSTALLED_AT) == RP_INSTALLED; }
const char *ramprobe_error(void) { return R.error; }
uint32_t ramprobe_resets(void) { return R.resets; }
int ramprobe_rung(void) { return R.rung; }
const rp_regs_t *ramprobe_stopped(void) { return &R.stopped; }
const rp_regs_t *ramprobe_result(void) { return &R.result; }

uint32_t ramprobe_report_at(void) { return RP_REPORT; }

const uint8_t *ramprobe_data(uint32_t *length)
{
   *length = R.peeked;
   return J(RP_DATA);
}

/* Only reached when the emulator table enables it (ramprobe_addr=-1 in Pi1MHz.cfg turns it off,
   and then the /bbc routes aren't there).  Typing the install line on the BBC is the opt-in for
   anything to happen to the BBC; until then this registers a poll that reads one byte of JIM,
   and the routes answer "not installed". */
void ramprobe_init(uint8_t instance, uint8_t address)
{
   (void)address;
   R.src = instance;
   /* A BBC reset (or the Pi's start): the vectors are the OS's again, and whatever was under
      way is gone. */
   if (R.registered) R.resets++;
   /* Its report says installed no longer.  Written only if the install had written it: the area
      is the probe's only once the line is typed (services.h). */
   if (*J(RP_REPORT + RP_INSTALLED_AT) == RP_INSTALLED) memset(J(RP_REPORT), 0, RP_INSTALLED_AT + 1u);
   put_fred();
   unask();
   R.rung = RP_RUNG_NONE;
   R.oneshot = false;
   R.state = RP_RUNNING;
   R.waiting = false;
   R.error[0] = '\0';
   if (!pages_ok()) fail(MISMATCH);      /* GET /bbc says so before anyone tries a halt */
   R.peeked = 0;
   /* Every time: a BBC reset empties the poll table (init_emulator), and each emulator
      registers again.  Registering once left the probe with no poll after the first Break. */
   Pi1MHz_Register_Poll(ramprobe_poll, "ramprobe");
   R.registered = true;
}
