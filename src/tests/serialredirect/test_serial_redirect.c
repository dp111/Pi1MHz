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
   serial_redirect_break();
   tick(10);
   CHECK(gate_open() && stub(OFF_EXIT) == 0x6C && stub(OFF_EXITOLD) == 0xFC && stub(OFF_EXITOLD + 1u) == 0xFF,
         "the exit is JMP (&FFFC) before the gate opens");
   bus_write(OFF_GATE, 0);
   CHECK(!gate_open() && !nirq, "the trip taken: nothing more asserted");
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
   serial_redirect_break();
   tick(10);
   tick(100001);
   CHECK(!gate_open() && stub(OFF_EXIT) == 0x6C, "stalled: shut, the exit still JMP (&FFFC)");
   tick(600000);
   CHECK(gate_open(), "asked again");
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
