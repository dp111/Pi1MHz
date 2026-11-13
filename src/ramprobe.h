/* ramprobe.h - stop, inspect and run code on a BBC with no ROM fitted.

   One line typed on the BBC (X%=23:CALL &FC88) points EVNTV and IRQ2V at hooks
   the Pi serves in FRED, and puts a JMP for the NMI at &0D00.  From then on the
   Pi can stop the machine wherever it is (by the NMI even with interrupts off) in
   an agent that runs from the Pi's own memory through the &FD00 window.  While
   it is stopped the Pi can read and write memory and call code, then let it go
   on, anywhere.  See docs/dev/ramprobe.md.

   Typing the install line on the BBC is the opt-in.  Every call here is from the main loop
   and returns at once; the work happens in the poll, and ramprobe_state() says
   when it is over. */
#ifndef RAMPROBE_H
#define RAMPROBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The probe's 6502 code is helpers 22 and 23 in 6502code/6502code.asm, loaded from 6502code.bin
   with the other helpers.  These are the places in it that the Pi writes or reads; the ASSERTs
   after page 22 there check the addresses, so a change on one side wants one on the other. */
#define RP_HELPER_AGENT    22u      /* each page's number is its last byte, which rp_in waits on */
#define RP_HELPER_INSTALL  23u      /* the line typed: X%=23:CALL &FC88 */
#define RP_VERSION_AT     0xFEu     /* in page 22: RP_VERSION, so the SD card's 6502code.bin is this build's */
#define RP_VERSION         1u
#define RP_BACK_SEL       0xFDu     /* &FC88: the agent's way out (BACK_SEL), helpers.c's HELPER_BACK */
#define RP_REPORT   0xE00000u       /* services offset of the report (services.h records it) */
#define RP_HALTED_AT    7u          /* in the report: the stopped Y, X, A, P, PC, S, then 1 while halted */
#define RP_CALL_AT      8u          /* a call's A, X, Y, P, then its sequence number */
#define RP_INSTALLED_AT 13u
#define RP_INSTALLED    0x5Au
/* The code outside the agent's page is in FRED, in three blocks the Pi copies there from the
   end of page 23 (RP_FRED_IN_PAGE, A then B then C) at every reset.  The BBC's RAM holds only
   the NMI's JMP at &0D00. */
#define RP_FRED_IN_PAGE 0xCFu
#define RP_FRED_A       0x6Fu       /* &FC6F: the IRQ2V hook, the way in, the event's way on */
#define RP_IN_AT        5u          /* rp_in in block A: 0 in page 23, the Pi's PHA here (RP_IN_OP) */
#define RP_IN_OP        0x48u
#define RP_FRED_A_LEN   24u
#define RP_FRED_B       0x8Fu       /* &FC8F: the EVNTV hook */
#define RP_FRED_B_LEN   17u
#define RP_FRED_C       0xB0u       /* &FCB0: the way out */
#define RP_FRED_C_LEN   7u
#define RP_HOOK_IRQ2    RP_FRED_A   /* each hook's first byte: JMP (idle), or BIT (wanted) */
#define RP_HOOK_EVENT   RP_FRED_B
#define RP_IDLE         0x4Cu
#define RP_WANTED       0x2Cu
#define RP_NMI_JMP      0x0D00u     /* JMP rp_in, 3 bytes: the only BBC RAM the probe has */
#define RP_AGENT_JSR    0x61u       /* the call's JSR address, in the agent's page */

#define RP_MAX_TRANSFER 0x10000u   /* the most one peek or poke can move */
#define RP_NO_BANK      (-1)

typedef enum {
   RP_OFF,        /* not running (no JIM RAM) */
   RP_RUNNING,    /* the BBC is running, installed or not */
   RP_HALTING,    /* the NMI has gone: waiting for the agent */
   RP_HALTED,     /* holding in the agent, nothing under way */
   RP_WORKING,    /* holding, with a call, peek or poke under way */
   RP_RESUMING    /* told to go on: waiting for the agent to leave */
} rp_state_t;

/* The CPU as it was stopped, or as a call left it (pc and s unused then). */
typedef struct {
   uint8_t a, x, y, p, s;
   uint16_t pc;
} rp_regs_t;

typedef struct {
   bool     set_pc, set_a, set_x, set_y, set_p;
   uint16_t pc;
   uint8_t  a, x, y, p;
} rp_resume_t;

void        ramprobe_init(uint8_t instance, uint8_t address);
rp_state_t  ramprobe_state(void);
const char *ramprobe_state_name(void);
bool        ramprobe_installed(void);   /* the install line typed since the last reset */
const char *ramprobe_error(void);       /* why the last thing failed, or "" */
uint32_t    ramprobe_resets(void);      /* BBC resets since the Pi started: a client's cue to recover */
uint32_t    ramprobe_stops(void);       /* times the BBC has been stopped since the Pi started */

/* How the agent is asked in.  The caller picks; only that rung is tried, and if it
   doesn't answer in its time the call fails, leaving nothing asked of the BBC.
     RP_RUNG_EVENT  the vsync event (EVNTV): ordinary programs; no NMI.  300 ms.
     RP_RUNG_IRQ    the Pi's nIRQ, taken by IRQ2V: events off, or EVNTV taken.  40 ms.
     RP_RUNG_NMI    the NMI: anything, even with interrupts off.  3 tries, 40 ms apart. */
#define RP_RUNG_EVENT 0
#define RP_RUNG_IRQ   1
#define RP_RUNG_NMI   2
#define RP_RUNG_NONE  (-1)          /* peek, poke: only if already stopped */
int ramprobe_rung(void);            /* the rung the last stop came in by, or RP_RUNG_NONE */

/* All false, with ramprobe_error() set, if it can't start in this state. */
bool ramprobe_halt(int rung);
bool ramprobe_resume(const rp_resume_t *how);    /* NULL: as it was stopped */
/* With the machine stopped, rung is ignored.  With it running and a rung given: in by that
   rung, the transfer, and out again, all in one (the BBC held only for the transfer). */
bool ramprobe_peek(uint16_t address, uint32_t length, int bank, int rung);  /* bank: RP_NO_BANK, or 0-15 */
bool ramprobe_poke(uint16_t address, const uint8_t *bytes, uint32_t length, int rung);
/* Several ranges in one stop, their bytes back to back in the data buffer (ramprobe_data) in the
   order given: up to RP_MAX_RANGES of them, RP_MAX_TRANSFER bytes in all.  Each is checked as a
   single one would be before anything is done.  A poke takes its bytes the same way. */
typedef struct { uint16_t address; uint32_t length; } rp_range_t;
#define RP_MAX_RANGES 32u
bool ramprobe_peek_ranges(const rp_range_t *ranges, unsigned count, int bank, int rung);
bool ramprobe_poke_ranges(const rp_range_t *ranges, unsigned count, const uint8_t *bytes, int rung);
bool ramprobe_call(uint16_t address, uint8_t a, uint8_t x, uint8_t y);
/* Runs code of up to RP_CODE_MAX bytes, at &FDA0 in the agent's page; it ends with RTS. */
#define RP_CODE_MAX 0x50u
bool ramprobe_run(const uint8_t *code, uint32_t length, uint8_t a, uint8_t x, uint8_t y);

const rp_regs_t *ramprobe_stopped(void);    /* as stopped */
const rp_regs_t *ramprobe_result(void);     /* the last call's A, X, Y, P */
const uint8_t   *ramprobe_data(uint32_t *length);   /* the last peek's bytes */

uint32_t ramprobe_report_at(void);   /* JIM address of the report; its data buffer is 0x100 on */

/* Shared memory: the first 4 MB of the services area's program area (docs/advanced.md), through
   which a remote application on the network and its own code on the BBC exchange data while the
   BBC runs, with no stop.  The application writes and reads it over HTTP (POST and GET
   /bbc/shared); the BBC reaches it through the services port (&FCA6-&FCA9: a 24-bit services
   offset, then a data register that moves on by itself) or through the &FD00 window, whose page
   registers (&FCFD-&FCFF) take the JIM address that ramprobe_shared_jim() gives.  Both show the
   application's newer bytes only once the BBC sets them again: the window is a copy of the page
   made when a page register is written, and the port holds its next byte ready.  The probe
   writes it only for those routes and never interprets its bytes: what goes where is the
   application's choice.  services.h records it. */
#define RP_SHARED      0x800000u
#define RP_SHARED_SIZE 0x400000u
uint8_t *ramprobe_shared(void);
uint32_t ramprobe_shared_jim(void);  /* its JIM address: DISC_RAM_BASE + RP_SHARED */

#endif
