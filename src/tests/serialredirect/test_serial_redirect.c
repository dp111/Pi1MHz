/*
   Host tests for serial_redirect.c: the stub's flow control and its trips (escape, BREAK,
   RS423 idle), driven through the FRED write callbacks the FIQ would call and the poll the
   main loop would call.  The Beeb is not simulated: each step does what the stub would do
   (a consume or full write) and the test reads back what the Pi wrote into the stub.

   With TRACE set it prints every stub write and nIRQ change, so two versions of the file can
   be compared step for step (run_tests.sh does that against git's previous version when
   asked: SER_TRACE_BASE=<rev>).
*/
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Pi1MHz.h"
#include "rpi/asm-helpers.h"
#include "serial_redirect.h"
#include "serial_modem.h"
#include "telnetd.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
   printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bool trace;

/* ---- the Pi side the file talks to ------------------------------------ */
uint32_t Pi1MHz_now_us;
static uint8_t fred[256];
static callback_func_ptr cb[256];
static func_ptr polls[8];
static unsigned npolls;
static bool nirq;

void Pi1MHz_MemoryWrite(uint32_t addr, uint8_t data)
{
   if (trace && fred[addr & 0xFFu] != data)
      printf("    W %02X=%02X\n", (unsigned)(addr & 0xFFu), data);
   fred[addr & 0xFFu] = data;
}
void Pi1MHz_MemoryWrite_FIQ(uint32_t addr, uint8_t data) { Pi1MHz_MemoryWrite(addr, data); }
uint8_t Pi1MHz_MemoryRead(uint32_t addr) { return fred[addr & 0xFFu]; }
void Pi1MHz_EmulatedMemoryByte(unsigned int gpio) { (void)gpio; }
void Pi1MHz_Register_Memory(unsigned int access, unsigned int addr, callback_func_ptr fn)
{
   if (access == WRITE_FRED)
      cb[addr & 0xFFu] = fn;
}
void Pi1MHz_Register_Poll(func_ptr fn, const char *name)
{
   (void)name;
   for (unsigned i = 0; i < npolls; i++)
      if (polls[i] == fn)
         return;
   polls[npolls++] = fn;
}
void Pi1MHz_nIRQ_ASSERT(uint8_t src)
{
   (void)src;
   if (trace && !nirq) printf("    nIRQ on\n");
   nirq = true;
}
void Pi1MHz_nIRQ_CLEAR(uint8_t src)
{
   (void)src;
   if (trace && nirq) printf("    nIRQ off\n");
   nirq = false;
}
unsigned int _disable_interrupts_cspr(void) { return 0; }
void _restore_cpsr(unsigned int cpsr) { (void)cpsr; }
uint32_t RPI_GetSystemTime(void) { return Pi1MHz_now_us; }

/* The stream's two users: not run here (their own suites test them). */
void modem_reset(void) { }
void modem_attach(void) { }
void telnetd_attach(void) { }
void fb_edit_cursor_arm(void) { }
void fb_edit_cursor_keys_told(bool told) { (void)told; }
bool telnetd_enabled(void) { return true; }

/* ---- the Beeb's side --------------------------------------------------- */
#define B 0xCCu                         /* the stub's base (&FCCC) */
#define OFF_TX      0x00u
#define OFF_TXFLAG  0x04u
#define OFF_GATE    0x0Cu
#define OFF_FULL    0x0Fu
#define OFF_RXBYTE  0x14u
#define OFF_INBUF   0x16u
#define OFF_JSR     0x18u
#define OFF_CONTROL 0x28u
#define OFF_EXIT    0x2Au
#define OFF_EXITOLD 0x2Bu
#define OFF_ROUTINE 0x2Du                /* the trips' routine: STA (or LDA, JMP) addr */
#define OFF_TRIPLO  0x2Eu
#define OFF_TRIPHI  0x2Fu
#define ESCR        ((uint8_t)(B - 13u)) /* the escape routine, borrowed by CTRL-BREAK */
#define WS_SHIFT    ((uint8_t)(B - 21u + 6u))

static uint8_t stub(uint8_t off) { return fred[(B + off) & 0xFFu]; }
static void bus_write(uint8_t off, uint8_t data)
{
   uint8_t a = (uint8_t)(B + off);
   if (trace) printf("   beeb %02X<-%02X\n", off, data);
   if (cb[a] != NULL)
      cb[a](data);                       /* GET_DATA: the data bus is the low byte */
}
static void poll(void)
{
   for (unsigned i = 0; i < npolls; i++)
      polls[i]();
}
static void step(const char *what) { if (trace) printf("  -- %s\n", what); }
static void tick(uint32_t us) { Pi1MHz_now_us += us; poll(); }

static bool gate_open(void) { return stub(OFF_GATE) == 0x2Cu; }   /* BIT: a byte or trip waits */
static bool jsr_insv(void) { return stub(OFF_JSR) == (uint8_t)(B + 0x09u); }
static bool trip_to(uint8_t op, uint16_t addr)   /* the JSR on the trips' routine, op addr */
{
   return stub(OFF_JSR) == (uint8_t)(B + OFF_ROUTINE) && stub(OFF_ROUTINE) == op
          && stub(OFF_TRIPLO) == (uint8_t)addr && stub(OFF_TRIPHI) == (uint8_t)(addr >> 8);
}

int main(int argc, char **argv)
{
   trace = (argc > 1 && strcmp(argv[1], "trace") == 0);
   Pi1MHz_now_us = 1000000u;

   step("init");
   serial_redirect_init(3, B);
   CHECK(serial_redirect_address() == B, "the stub is at &FCCC");
   CHECK(!nirq && !gate_open(), "nothing asserted before it is hooked");

   step("hook: column 5, row 3, MODE 7");
   bus_write(OFF_CONTROL, 0x85);
   bus_write(OFF_CONTROL, 0xE3);
   bus_write(OFF_CONTROL, 0x17);
   CHECK(serial_redirect_telnet_attached(), "telnet mode");
   CHECK(serial_redirect_beeb->mode == 7 && serial_redirect_beeb->col == 5 && serial_redirect_beeb->row == 3,
         "the report: MODE 7 at 5,3");
   CHECK(stub(OFF_INBUF) == 0, "bytes go into the keyboard buffer");
   tick(10);
   CHECK(gate_open() && !jsr_insv(), "a hook starts with the idle trip (the busy flag starts right)");
   bus_write(OFF_GATE, 0);
   CHECK(!gate_open() && jsr_insv(), "which ends with its consume");

   step("two keys");
   serial_redirect_write((const uint8_t *)"AB", 2);
   tick(10);
   CHECK(gate_open() && nirq && stub(OFF_RXBYTE) == 'A', "A offered");
   bus_write(OFF_GATE, 0);
   CHECK(gate_open() && stub(OFF_RXBYTE) == 'B', "then B");
   bus_write(OFF_GATE, 0);
   CHECK(!gate_open() && !nirq, "then the gate shuts");

   step("the Beeb's buffer full");
   serial_redirect_write((const uint8_t *)"C", 1);
   tick(10);
   bus_write(OFF_FULL, 0);
   CHECK(!gate_open() && !nirq, "full: the gate shuts");
   tick(1000);
   CHECK(!gate_open(), "and stays shut during the backoff");
   tick(1500);
   CHECK(gate_open() && stub(OFF_RXBYTE) == 'C', "C offered again after it");
   bus_write(OFF_GATE, 0);

   step("ESCAPE");
   serial_redirect_write((const uint8_t *)"xyz", 3);
   serial_redirect_escape();
   tick(10);
   CHECK(gate_open() && !jsr_insv() && stub(OFF_RXBYTE) == 0x80, "the escape trip armed");
   bus_write(OFF_GATE, 0);
   CHECK(jsr_insv(), "its consume puts the JSR back on INSV");
   CHECK(!gate_open(), "and the type-ahead went with the ESCAPE");

   step("ESCAPE ended by a full write (the routine keeps the carry)");
   serial_redirect_escape();
   tick(10);
   bus_write(OFF_FULL, 0);
   CHECK(jsr_insv() && !gate_open(), "a full write ends a trip as a consume does");

   step("ESCAPE that stalls: asked for again");
   serial_redirect_escape();
   tick(10);
   tick(100001);
   CHECK(!gate_open() && jsr_insv(), "stalled: shut, the JSR back on INSV");
   tick(400000);
   CHECK(!gate_open(), "the long backoff after a stall");
   tick(200000);
   CHECK(gate_open() && !jsr_insv(), "then the escape trip again");
   bus_write(OFF_GATE, 0);
   CHECK(jsr_insv(), "and it ends");

   step("drop_keys while a trip is in flight");
   serial_redirect_escape();
   tick(10);
   serial_redirect_drop_keys(0xFB);
   CHECK(gate_open(), "the trip in flight finishes as it is");
   bus_write(OFF_GATE, 0);
   CHECK(jsr_insv() && !gate_open(), "and ends");

   step("idle trip");
   bus_write(OFF_TX, 'p');
   CHECK(serial_redirect_pending(), "a byte from the Beeb");
   { uint8_t c; CHECK(serial_redirect_read(&c, 1) == 1 && c == 'p', "read"); }
   tick(10);
   CHECK(stub(OFF_TXFLAG) == 0x18, "TX flag CLC");
   tick(2100);
   CHECK(gate_open() && !jsr_insv() && stub(OFF_RXBYTE) == 0x80, "the idle trip armed after 2 ms quiet");
   bus_write(OFF_GATE, 0);
   CHECK(jsr_insv() && !gate_open(), "and ends");
   tick(5000);
   CHECK(!gate_open(), "once per burst of output");

   step("BREAK");
   uint8_t chain_lo = stub(OFF_EXITOLD), chain_hi = stub(OFF_EXITOLD + 1u);
   serial_redirect_break(SER_BRK_PLAIN);
   tick(10);
   CHECK(!gate_open(), "not before 2 ms: a trip may still be in the stub");
   tick(2000);
   CHECK(gate_open() && stub(OFF_EXIT) == 0x6C && stub(OFF_EXITOLD) == 0xFC && stub(OFF_EXITOLD + 1u) == 0xFF,
         "the exit is JMP (&FFFC) before the gate opens");
   CHECK(stub(OFF_JSR) == ESCR && fred[ESCR] == 0x8E && stub(OFF_INBUF) == 0x00 && stub(OFF_RXBYTE) == 0xA0,
         "the second processor pulsed (STA &FEE0 &A0, then &20), &00 to the IER");
   bus_write(OFF_GATE, 0);
   CHECK(!gate_open() && !nirq, "the trip taken: nothing more asserted");
   CHECK(fred[ESCR] == 0x85 && jsr_insv(), "the escape routine back");
   serial_redirect_write((const uint8_t *)"q", 1);
   tick(10);
   CHECK(!gate_open(), "no nIRQ while the Beeb resets");
   tick(3000001);
   CHECK(stub(OFF_EXIT) == 0x40 && stub(OFF_EXITOLD) == chain_lo && stub(OFF_EXITOLD + 1u) == chain_hi,
         "no reset came in 3 s: the exit and its operand back");
   CHECK(!serial_redirect_telnet_attached(), "and the stub counted unhooked (no ROM hooked it in again)");

   step("hook again; BREAK that stalls stays a BREAK");
   bus_write(OFF_CONTROL, 0x80);
   bus_write(OFF_CONTROL, 0xE0);
   bus_write(OFF_CONTROL, 0x17);
   serial_redirect_break(SER_BRK_PLAIN);
   tick(2100);
   tick(100001);
   CHECK(!gate_open() && stub(OFF_EXIT) == 0x6C, "stalled: shut, the exit still JMP (&FFFC)");
   CHECK(fred[ESCR] == 0x85 && jsr_insv(), "the escape routine back meanwhile");
   tick(600000);
   CHECK(gate_open() && stub(OFF_RXBYTE) == 0xA0 && stub(OFF_JSR) == ESCR, "asked again, as the BREAK's trip");
   bus_write(OFF_GATE, 0);
   CHECK(!gate_open() && stub(OFF_EXIT) == 0x6C, "taken as a BREAK");

   step("the ROM reports the reset");
   bus_write(OFF_CONTROL, 0x26);
   CHECK(serial_redirect_beeb->rom, "REMV claimed: rom");
   CHECK(stub(OFF_EXIT) == 0x40 && jsr_insv(), "the stub as it was");
   bus_write(OFF_CONTROL, 0x20);
   CHECK(!serial_redirect_beeb->rom, "REMV not claimed");
   bus_write(OFF_CONTROL, 0x24);
   bus_write(OFF_CONTROL, 0x25);
   bus_write(OFF_CONTROL, 0x22);
   CHECK(serial_redirect_beeb->key_gen == 1 && serial_redirect_beeb->key_other_gen == 1
         && serial_redirect_beeb->unseen_gen == 1, "keys taken and a ROM FS operation counted");

   step("BREAK with the ROM: the second processor pulsed as well, as the BREAK key's reset line");
   bus_write(OFF_CONTROL, 0x26);
   bus_write(OFF_CONTROL, 0x17);
   tick(10);
   bus_write(OFF_GATE, 0);                        /* the idle trip a hook starts with */
   CHECK(!gate_open(), "hooked, quiet");
   serial_redirect_break(SER_BRK_PLAIN);
   tick(2100);
   CHECK(gate_open() && stub(OFF_JSR) == ESCR && fred[ESCR] == 0x8E && stub(OFF_INBUF) == 0x00
         && stub(OFF_RXBYTE) == 0xA0 && stub(OFF_ROUTINE) == 0x8D && stub(OFF_TRIPLO) == 0xE0
         && stub(OFF_TRIPHI) == 0xFE && stub(OFF_EXIT) == 0x6C,
         "STA &FEE0 (&A0), then (&20), &00 to the IER, the exit JMP (&FFFC)");
   bus_write(OFF_GATE, 0);
   CHECK(!gate_open() && fred[ESCR] == 0x85 && jsr_insv(), "taken: the escape routine back");
   bus_write(OFF_CONTROL, 0x26);
   tick(10);
   if (gate_open())
      bus_write(OFF_GATE, 0);                     /* (the idle trip the report starts) */
   bus_write(OFF_CONTROL, 0x17);
   tick(10);
   CHECK(gate_open() && trip_to(0x8D, 0x024F), "hooked in again: no release trip, the idle one");
   bus_write(OFF_GATE, 0);
   CHECK(!gate_open() && jsr_insv(), "and nothing more");

   step("CTRL-BREAK: the VIA's interrupt enables off too, the second processor pulsed");
   serial_redirect_break(SER_BRK_CTRL);
   tick(2100);
   CHECK(gate_open() && stub(OFF_JSR) == ESCR && fred[ESCR] == 0x8E && fred[(uint8_t)(ESCR + 3u)] == 0x8D
         && fred[(uint8_t)(ESCR + 8u)] == 0x4C && fred[(uint8_t)(ESCR + 9u)] == (uint8_t)(B + OFF_ROUTINE)
         && stub(OFF_INBUF) == 0x7F && stub(OFF_RXBYTE) == 0xA0 && stub(OFF_ROUTINE) == 0x8D
         && stub(OFF_TRIPLO) == 0xE0 && stub(OFF_TRIPHI) == 0xFE,
         "STX &FE4E (&7F), STA &FEE0 (&A0), AND #&7F, JMP to STA &FEE0 (&20)");
   bus_write(OFF_GATE, 0);
   CHECK(fred[ESCR] == 0x85 && stub(OFF_INBUF) == 0 && jsr_insv(), "the escape routine and LDX #0 back");
   bus_write(OFF_CONTROL, 0x26);
   bus_write(OFF_CONTROL, 0x17);
   tick(10);
   CHECK(gate_open() && trip_to(0x8D, 0x024F), "pulsed: no release trip, only the idle one");
   bus_write(OFF_GATE, 0);

   step("SHIFT-BREAK: SHIFT for the auto-boot test, held for a second");
   serial_redirect_break(SER_BRK_SHIFT);
   tick(2100);
   bus_write(OFF_GATE, 0);
   CHECK(fred[WS_SHIFT] == 0, "not before the ROM has hooked in");
   bus_write(OFF_CONTROL, 0x26);
   CHECK(fred[WS_SHIFT] == 1, "the ROM's reset report: SHIFT set");
   fred[WS_SHIFT] = 0;                            /* the ROM's KEYV took it */
   tick(10);
   CHECK(fred[WS_SHIFT] == 1, "taken: pressed still");
   tick(1000001);
   CHECK(fred[WS_SHIFT] == 0, "let go after a second");
   bus_write(OFF_CONTROL, 0x17);
   tick(10);
   CHECK(gate_open() && trip_to(0x8D, 0x024F), "pulsed: no release trip, the idle one");
   bus_write(OFF_GATE, 0);
   CHECK(!gate_open(), "quiet");

   step("the ESCAPE key, the MOS's defaults: four reads, then ESCAPE");
   static const uint16_t vars[4] = { 0x026C, 0x0275, 0x0258, 0x02C5 };
   serial_redirect_write((const uint8_t *)"ab", 2);
   serial_redirect_escape_key();
   tick(10);
   for (unsigned i = 0; i < 4; i++) {
      CHECK(gate_open() && trip_to(0xAD, vars[i]), "read %u: LDA &%04X", i, vars[i]);
      bus_write(OFF_GATE, i == 0 ? 27 : 0);
   }
   CHECK(stub(OFF_ROUTINE) == 0x8D, "the routine STA again");
   CHECK(serial_redirect_escape_result() == 0, "not decided before the poll");
   tick(10);
   CHECK(serial_redirect_escape_result() == SER_ESC_RAISED, "ESCAPE");
   CHECK(serial_redirect_escape_result() == 0, "told once");
   tick(10);
   CHECK(gate_open() && stub(OFF_JSR) == ESCR && stub(OFF_RXBYTE) == 0x80, "the escape trip");
   bus_write(OFF_GATE, 0);
   CHECK(!gate_open(), "and the keys before it went");

   step("the ESCAPE key under *FX229,1: a character");
   serial_redirect_escape_key();
   tick(10);
   for (unsigned i = 0; i < 4; i++)
      bus_write(OFF_GATE, i == 0 ? 27 : i == 1 ? 1 : 0);
   tick(10);
   CHECK(serial_redirect_escape_result() == SER_ESC_CHAR && !gate_open(), "a key for the caller, no trip");

   step("the ESCAPE key under *FX200,1: ignored");
   serial_redirect_escape_key();
   tick(10);
   for (unsigned i = 0; i < 4; i++)
      bus_write(OFF_GATE, i == 0 ? 27 : i == 2 ? 1 : 0);
   tick(10);
   CHECK(serial_redirect_escape_result() == SER_ESC_OFF && !gate_open(), "nothing");

   step("the ESCAPE key with escape event 6 enabled");
   serial_redirect_escape_key();
   tick(10);
   for (unsigned i = 0; i < 4; i++)
      bus_write(OFF_GATE, i == 0 ? 27 : i == 3 ? 1 : 0);
   tick(10);
   CHECK(gate_open() && trip_to(0x4C, 0xFFBF) && fred[(uint8_t)(B + OFF_INBUF - 1u)] == 0xA0 && stub(OFF_INBUF) == 6
         && stub(OFF_RXBYTE) == 27, "LDY #6 / JMP OSEVEN with A = 27");
   bus_write(OFF_GATE, 0);
   CHECK(serial_redirect_escape_result() == SER_ESC_EVENT, "taken by a handler: no ESCAPE");
   CHECK(fred[(uint8_t)(B + OFF_INBUF - 1u)] == 0xA2 && stub(OFF_INBUF) == 0 && stub(OFF_ROUTINE) == 0x8D && jsr_insv(),
         "LDX #0 and STA back");
   CHECK(!gate_open(), "quiet");

   step("... and no handler took it (C set): ESCAPE");
   serial_redirect_escape_key();
   tick(10);
   for (unsigned i = 0; i < 4; i++)
      bus_write(OFF_GATE, i == 0 ? 27 : i == 3 ? 1 : 0);
   tick(10);
   bus_write(OFF_FULL, 0);
   CHECK(serial_redirect_escape_result() == SER_ESC_RAISED, "the flag");
   CHECK(gate_open() && stub(OFF_JSR) == ESCR, "the escape trip next");
   bus_write(OFF_GATE, 0);

   step("the ESCAPE key's reads never come back: ESCAPE anyway");
   serial_redirect_escape_key();
   tick(10);
   tick(100001);                                  /* stalled: shut, the read asked again later */
   CHECK(!gate_open() && stub(OFF_ROUTINE) == 0x8D && jsr_insv(), "the read put back");
   tick(400000);
   CHECK(serial_redirect_escape_result() == SER_ESC_RAISED, "ESCAPE after 0.4 s");
   CHECK(gate_open() && stub(OFF_JSR) == ESCR, "the escape trip at once (an ESCAPE ends the backoff)");
   bus_write(OFF_GATE, 0);

   step("the TX flag: only the FIQ changes it");
   for (unsigned i = 0; i < 1024; i++)
      bus_write(OFF_TX, (uint8_t)i);
   CHECK(stub(OFF_TXFLAG) == 0x38, "full: SEC");
   { uint8_t c; CHECK(serial_redirect_read(&c, 1) == 1, "one read"); }
   tick(10);
   CHECK(stub(OFF_TXFLAG) == 0x38, "room again, but SEC stays until the 6502's next write");
   bus_write(OFF_TX, 'r');                        /* the byte it was told no for: not taken */
   CHECK(stub(OFF_TXFLAG) == 0x18, "that write puts CLC back");
   bus_write(OFF_TX, 'r');                        /* the MOS's retry */
   {
      static uint8_t all[1100];
      size_t n = serial_redirect_read(all, sizeof all);
      CHECK(n == 1024 && all[1022] == 0xFF && all[1023] == 'r', "each byte once (%zu)", n);
   }
   tick(10000);
   bus_write(OFF_GATE, 0);                        /* (the idle trip after the output) */

   step("modem mode, then unhooked");
   bus_write(OFF_CONTROL, 0x01);
   CHECK(!serial_redirect_telnet_attached() && stub(OFF_INBUF) == 1, "helper 19: RS423 input buffer");
   bus_write(OFF_CONTROL, 0x00);
   serial_redirect_write((const uint8_t *)"z", 1);
   tick(30000);
   CHECK(!gate_open() && !nirq, "unhooked: never asserted");

   printf("%d checks, %d failures\n", checks, failures);
   return failures != 0;
}
