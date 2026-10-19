/*
  serial_redirect.c - the BBC's RS423 stream, redirected to the Pi

  The 6850 at &FE08 is not on the 1MHz bus, so the Pi cannot be it.  What
  the Pi can do is stand in for it one level up: a 49-byte stub in FRED
  hooks INSV for the RS423 output buffer (2) and IRQ1V for the input buffer
  (1), and the Pi feeds and drains both.  Everything downstream of the
  buffers - OSRDCH, *FX2, ADVAL(-2), OSBYTE 145 - works unchanged because
  buffer 1 really does fill.  Helper 19 (6502code.asm) hooks the vectors;
  BREAK unhooks them.  Design record: docs/dev/serial-modem-plan.md.

  The stub, at base b (default &FCCC; Serial_addr=-1 disables it):

    +00 E0 02     CPX #2         INSV: ours?     [write: TX data port]
    +02 D0 05     BNE +09
    +04 18/38     CLC/SEC        TX flag, kept by the Pi: carry IS the answer
    +05 8D b FC   STA b+0        hand the byte over (dropped if flag said full)
    +08 60        RTS
    +09 4C .. ..  JMP old_insv   stashed by the install code
    +0C 4C/2C     JMP/BIT        RX gate, kept by the Pi: 2C = byte waiting
                                                [write: consume one byte]
    +0D .. ..     old_irq1v      stashed by the install code
    +0F 8A        TXA            [write: buffer 1 full - back off]
    +10 48 98 48  PHA TYA PHA
    +13 A9 xx     LDA #byte      the RX FIFO head, kept by the Pi
    +15 A2 01     LDX #1
    +17 20 b+9 FC JSR b+9        -> the MOS's own INSV
    +1A 90 05     BCC +21
    +1C 8D b+F FC STA b+F        full: tell the Pi, keep the byte
    +1F B0 03     BCS +24
    +21 8D b+C FC STA b+C        inserted: consume it
    +24 68 A8     PLA TAY
    +26 68 AA     PLA TAX
    +28 A5 FC     LDA &FC        the MOS's saved A   [write: 1 hooked, 0 not]
    +2A 40/4C     RTI/JMP        exit, kept by the Pi: see below
    +2B .. ..     old_irq1v      stashed by the install code
    +2D 8D .. ..  STA addr       trips only: the Pi sets addr and A (see below)
    +30 60        RTS            C as it came: either report ends the trip

  Two single-byte flags carry all the flow control, and each is flipped
  between two same-length opcodes over fixed operands, so whatever the 6502
  fetches is a whole valid instruction.  Order matters on both: the TX flag
  goes to SEC in the same FIQ that fills the FIFO, and the RX byte is
  written before the gate opens.

  nIRQ is level-sensitive, so while it is asserted and the Beeb cannot take
  the byte (buffer 1 full, nobody reading) the machine would re-enter IRQ
  for ever.  The stub reports that case, and the Pi then closes the gate,
  drops nIRQ and retries later.  nIRQ is never asserted before the install
  code has said the vectors are hooked: an unclaimed interrupt that never
  clears would hang the Beeb just the same.

  Leaving by RTI is ~9x faster per byte than chaining to the MOS (105 vs
  910 us on a Master, which offers every unclaimed interrupt to the ROMs),
  but while the gate is open every re-entry lands here, so the timer, vsync
  and keyboard wait for the whole burst.  So the Pi makes every
  SER_CHAIN_EVERY'th exit a JMP to the old IRQ1V: the MOS then services
  whatever else is pending.  The exit is rewritten on each consume, ~15 us
  of 1 MHz fetches before the 6502 reaches it; a late write only means one
  exit of the other kind, both being whole instructions over fixed bytes.

  What the Beeb sends goes to the modem (serial_modem.c), and what the modem
  answers comes back; this file only moves the bytes.

  Telnet mode (helper 20, 6502code.asm): the same stub, hooked the same way,
  but the helper writes 2 to the control port (+28) where helper 19 writes 1.
  The Pi then edits the operand of the stub's LDX #1 (+16) to 0, so what it
  sends is inserted into the MOS keyboard buffer instead of the RS423 input
  buffer, and the byte stream belongs to telnetd.c, not the modem.  Like the
  other flags it is one byte between two whole instructions' operands.

  ESCAPE in telnet mode.  The Beeb's ESCAPE is a condition (bit 7 of &FF, set
  by the keyboard scan), not a character in the buffer, so no byte inserted
  with INSV can raise it.  serial_redirect_escape() makes the next trip through
  the stub an "escape trip": the Pi points the stub's JSR (its low byte at +18)
  at the routine at +2D instead of at INSV, points the routine's STA at &FF and
  gives it &80 for the byte.  The routine stores that and returns with whatever
  carry it came with, so the stub ends with its consume or its full write; in a
  trip the Pi takes either as the end, and points the JSR back at INSV in the
  same FIQ.  Nothing is taken from the FIFO, and nothing goes through the keyboard
  buffer, so a buffer full of type-ahead cannot hold it up.  It is a store to
  zero page and not an OSBYTE call because the stub runs in IRQ context, and the
  MOS keeps an OSBYTE call's A, X and Y in zero page where an interrupted OSBYTE
  would find them changed.  The escape flush the keyboard does is done by
  discarding the FIFO.

  RS423 idle.  Each character the MOS sends to RS423 (OSWRCH with *FX3 bit 0)
  first examines buffer 2, finds it empty - the stub never puts anything in it -
  and so starts the transmitter: it clears bit 7 of &24F ("RS423 busy") and
  enables the 6850's transmit interrupt (OS 1.2 &E0E8 -> &E170).  Only that
  interrupt sets the bit again, and a 6850 with nothing on the RS423 port holds
  CTS high, which keeps transmit-empty and its interrupt off.  The cassette and
  ROM filing systems wait for that bit before every operation (&FB1A), so after
  any output *ROM, *TAPE and *. hung until ESCAPE.  So once output has been
  quiet for SER_IDLE_US the Pi makes one "idle trip", as the escape one but
  storing &80 in &24F: what the missing interrupt would have done, buffer 2
  being empty.

  SHIFT in telnet mode.  Paged mode (VDU 14) waits at the end of each page until SHIFT is
  pressed, and the MOS reads SHIFT from the key matrix through KEYV, never from the buffer,
  so a waiting COPY or anything typed ahead cannot hold it up - nor can a byte stand for it.
  serial_redirect_shift() sets the SHIFT byte of the auto-start ROM's workspace, which the Pi
  keeps in FRED below the stub (SER_WS_OFF): the ROM's KEYV handler answers the MOS's "is
  SHIFT pressed" yes once when it finds it set, and clears it.  No Beeb RAM is written.
*/
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "Pi1MHz.h"
#include "rpi/asm-helpers.h"
#include "rpi/systimer.h"
#include "serial_redirect.h"
#include "serial_modem.h"
#include "telnetd.h"

#define SER_TX_PORT    0x00u
#define SER_TX_FLAG    0x04u
#define SER_INSV_OLD   0x0Au
#define SER_GATE       0x0Cu
#define SER_IRQ1V_OLD  0x0Du
#define SER_FULL_PORT  0x0Fu
#define SER_RX_BYTE    0x14u
#define SER_INBUF      0x16u     /* the LDX # operand: the MOS buffer the Pi's bytes go into */
#define SER_JSR        0x18u     /* the JSR's low byte: INSV's trampoline, or the escape routine */
#define SER_CONTROL    0x28u
#define SER_EXIT       0x2Au
#define SER_EXIT_OLD   0x2Bu
#define SER_INSV_TRAMP 0x09u     /* the JMP old_insv that the JSR normally calls */
#define SER_ESC_ROUTINE 0x2Du    /* STA addr / RTS: the trips' routine */
#define SER_TRIP_ADDR  0x2Eu     /* ... and its addr */
#define SER_ESC_BYTE   0x80u     /* what the escape trip stores in &FF */
#define SER_MOS_ESCFLAG 0x00FFu
#define SER_MOS_RSBUSY  0x024Fu  /* bit 7 set: RS423 output idle (what the idle trip stores: &80) */
#define SER_IDLE_US      2000u   /* output quiet this long: the idle trip */
/* The auto-start ROM's workspace, 8 bytes kept by the Pi just below the stub's escape routine
   (&FCB7-&FCBE with the stub at &FCCC): the ROM owns no Beeb RAM.  +0 the MOS's REMV routine,
   +2 its KEYV, +4 its RDCHV (the ROM's claims go on to them), +6 SHIFT for paged mode (set
   here, read and cleared by the ROM), +7 1 when telnetd is on (the ROM hooks the Beeb in only
   then). */
#define SER_WS_OFF     21u
#define SER_WS_LEN     8u
#define SER_WS_SHIFT   6u
#define SER_WS_TELNET  7u

/* The escape trip's routine, just below the stub (FRED, which the Pi serves): the MOS's own way
   of raising ESCAPE (OS 1.2 &E674), which also tells a second processor - the escape flag lives
   in both machines, and a program on the second processor reads its own:
      STA &FF / BIT &027A / BPL out / JMP &0403 / out: RTS
   &027A bit 7 is the MOS's "Tube present"; &0403 is the Tube host code's "escape changed"
   entry, which reads &FF and passes it across, and ends with RTS. */
/* 13 below, so it ends next to &FCCA/&FCCB, the Pi's FX status register, whose writes are
   kept for reading back: an *FX write there would replace the routine's RTS. */
#define SER_ESCR_OFF   13u
static const uint8_t ser_escr[11] = { 0x85, 0xFF, 0x2C, 0x7A, 0x02, 0x10, 0x03, 0x4C, 0x03, 0x04, 0x60 };
#define SER_STUB_LEN   0x31u

#define OP_CLC  0x18u
#define OP_SEC  0x38u
#define OP_JMP  0x4Cu
#define OP_BIT  0x2Cu
#define OP_RTI  0x40u

/* Every this many bytes the stub chains to the MOS instead of RTI, so other
   interrupts wait at most this many fast trips (~1.7 ms at 105 us each). */
#define SER_CHAIN_EVERY  16u

/* No consume or full report for this long with the gate open means the
   stub is not being run (interrupts off, or the vectors reset behind our
   back without a BREAK): close it rather than hold nIRQ indefinitely. */
#define SER_STALL_US    100000u
#define SER_BACKOFF_US    2000u       /* telnet: 20 ms, with the keyboard buffer holding 32 bytes, capped a paste at ~1.3 kB/s */
#define SER_BACKOFF_MODEM_US 20000u   /* the modem (helper 19): as it always was */

#define SER_FIFO_SIZE   1024u      /* power of two */
#define SER_FIFO_MASK   (SER_FIFO_SIZE - 1u)

typedef struct {
   uint8_t           buf[SER_FIFO_SIZE];
   volatile uint32_t head;          /* consumer */
   volatile uint32_t tail;          /* producer */
} ser_fifo_t;

static ser_fifo_t tx;               /* Beeb -> Pi: FIQ produces, poll consumes */
static ser_fifo_t rx;               /* Pi -> Beeb: poll produces, FIQ consumes */

static uint8_t           ser_base;
static uint8_t           ser_irq_src;
static bool              ser_enabled;
static volatile bool     ser_hooked;      /* the install code says INSV/IRQ1V are ours */
static volatile bool     ser_telnet;      /* ... and by helper 20: keyboard buffer, telnetd's */
static volatile bool     ser_esc_req;     /* an ESCAPE for the Beeb is waiting for a trip */
static volatile bool     ser_brk_req;     /* ... and it is a BREAK: the trip's routine jumps through the reset vector */
static volatile bool     ser_brk_trip;    /* ... and the exit of the stub, after it, is JMP (&FFFC) */
static volatile bool     ser_esc_active;  /* the trip in progress is the escape one */
static volatile bool     ser_idle_req;    /* the Beeb's RS423 busy flag wants setting: an idle trip is waiting */
static volatile bool     ser_idle_active; /* the trip in progress is the idle one */
static volatile uint32_t ser_idle_tx;     /* ser_tx_count when the last idle trip was armed */
static volatile uint32_t ser_shifts;
static volatile uint32_t ser_tx_us;       /* the last byte the Beeb sent */
static volatile bool     ser_gate_open;   /* +0C is BIT: nIRQ asserted, byte at +14 */
static volatile uint32_t ser_gate_us;     /* gate opened, or last consume */
static volatile bool     ser_backoff;     /* the Beeb said buffer 1 is full */
static volatile uint32_t ser_backoff_us;

static volatile uint32_t ser_tx_count, ser_rx_count, ser_tx_drops, ser_fulls, ser_stalls, ser_escapes, ser_idles;
static volatile uint8_t  ser_beeb_mode;   /* the Beeb's screen MODE, as helper 20 reported it when it hooked in */
static volatile uint8_t  ser_beeb_col, ser_beeb_row;   /* ... and its text cursor */
static volatile uint32_t ser_mode_gen;    /* counts the reports */
static volatile uint32_t ser_reset_gen;   /* counts the resets of the BBC */
static volatile bool     ser_scan_ok;     /* the auto-start ROM has taken REMV: the screen can be asked for */
static volatile bool     ser_keys_told;   /* ... and tells each cursor key, COPY or RETURN a program takes (&24) */
static volatile uint32_t ser_key_gen;     /* counts those */
static volatile uint32_t ser_key_other_gen; /* ... and every other byte a program takes (&25, build 13) */
static volatile bool     ser_lit_ok;      /* the ROM reads a byte after LITKEY as it is (build 13, &26) */
static volatile uint32_t ser_unseen_gen;  /* counts the ROM's "printed where RS423 could not see" (&22) */
#ifdef DEBUG
/* Per-byte cost of the IRQ path: the gap between consecutive consumes while
   the gate stays open, i.e. one whole trip through the stub and back. */
static volatile uint32_t ser_gap_n, ser_gap_sum, ser_gap_max, ser_gap_min;

/* What happened on the stub, newest last: a ring of events that stops 24 events after the
   first stall, so what led up to a Beeb that stopped taking the trips is kept. */
#define SER_LOG_N 256u
static struct { uint32_t us; char ev; uint8_t d; } ser_log_ring[SER_LOG_N];
static volatile uint32_t ser_log_n, ser_log_stop;
static void ser_log(char ev, uint8_t d)
{
   uint32_t n = ser_log_n;
   if (ser_log_stop != 0u && n >= ser_log_stop)
      return;
   ser_log_ring[n % SER_LOG_N].us = Pi1MHz_now_us;
   ser_log_ring[n % SER_LOG_N].ev = ev;
   ser_log_ring[n % SER_LOG_N].d = d;
   ser_log_n = n + 1u;
}
#define SER_LOG(ev, d) ser_log((ev), (uint8_t)(d))
#else
#define SER_LOG(ev, d) ((void)0)
#endif

static inline uint32_t fifo_used(const ser_fifo_t *f) { return f->tail - f->head; }

static void ser_write(uint8_t off, uint8_t data)
{
   Pi1MHz_MemoryWrite_FIQ((uint32_t)ser_base + off, data);
}

/* Close the RX gate and drop our nIRQ.  FIQ context, or FIQ masked. */
static void ser_gate_close(void)
{
   ser_write(SER_GATE, OP_JMP);
   SER_LOG('x', ser_gate_open);
   ser_gate_open = false;
   Pi1MHz_nIRQ_CLEAR(ser_irq_src);
   if (ser_esc_active) {                     /* an escape trip that never finished: INSV again, ask again */
      ser_esc_active = false;
      ser_esc_req = true;
      ser_write(SER_JSR, (uint8_t)(ser_base + SER_INSV_TRAMP));
   }
   if (ser_idle_active) {                    /* ... and the same for an idle trip */
      ser_idle_active = false;
      ser_idle_req = true;
      ser_write(SER_JSR, (uint8_t)(ser_base + SER_INSV_TRAMP));
   }
}

/* Point the stub's JSR at its trip routine, storing byte at addr. */
static void ser_trip_arm(uint16_t addr, uint8_t byte)
{
   ser_write(SER_TRIP_ADDR, (uint8_t)addr);
   ser_write(SER_TRIP_ADDR + 1u, (uint8_t)(addr >> 8));
   ser_write(SER_JSR, (uint8_t)(ser_base + SER_ESC_ROUTINE));
   ser_write(SER_RX_BYTE, byte);
}

/* Make the next trip through the stub the idle one (the RS423 busy flag). */
static void ser_idle_arm(void)
{
   ser_idle_req = false;
   ser_idle_active = true;
   ser_idle_tx = ser_tx_count;
   ser_trip_arm(SER_MOS_RSBUSY, 0x80u);
}

/* What the next trip through an open gate is: a trip first, then a byte.
   False when there is nothing to send. */
static bool ser_arm_next(void);

/* Make the next trip through the stub the ESCAPE one.  The call target and the
   byte are both written before the gate opens, or while it is open and the
   consume that ended the last trip is being answered. */
static void ser_esc_arm(void)
{
   ser_esc_req = false;
   ser_esc_active = true;
   /* The routine stores &80 in &FF, and the trip ends as any other: the Beeb's consume
      (or full) write clears our nIRQ.  A BREAK instead turns the stub's last jump, which
      hands the interrupt on to the MOS, into JMP (&FFFC): the Beeb is reset with nothing
      left asserted (a reset taken inside the trip itself leaves nIRQ low and hangs it). */
   ser_brk_trip = ser_brk_req;               /* the consume that ends this trip sets the exit */
   ser_brk_req = false;
   if (ser_base >= SER_ESCR_OFF) {           /* the MOS's way, the Tube told too */
      ser_write(SER_JSR, (uint8_t)(ser_base - SER_ESCR_OFF));
      ser_write(SER_RX_BYTE, SER_ESC_BYTE);
   } else {
      ser_trip_arm(SER_MOS_ESCFLAG, SER_ESC_BYTE);
   }
}

static bool ser_arm_next(void)
{
   if (ser_esc_req) {
      ser_esc_arm();
      SER_LOG('E', 0);
   } else if (ser_idle_req && ser_telnet) {    /* telnet mode only: the modem (helper 19) runs as it always did */
      ser_idle_arm();
      SER_LOG('I', 0);
   } else if (fifo_used(&rx) != 0u) {
      ser_write(SER_RX_BYTE, rx.buf[rx.head & SER_FIFO_MASK]);
      SER_LOG('K', rx.buf[rx.head & SER_FIFO_MASK]);
   } else
      return false;
   return true;
}

/* ---- FIQ: bus writes to the stub ------------------------------------- */

static void ser_tx_byte(unsigned int gpio)
{
   if (fifo_used(&tx) >= SER_FIFO_SIZE) {    /* the flag said full: C=1, the MOS retries */
      ser_tx_drops++;
      return;
   }
   tx.buf[tx.tail & SER_FIFO_MASK] = (uint8_t)GET_DATA(gpio);
   tx.tail++;
   ser_tx_count++;
   ser_tx_us = Pi1MHz_now_us;
   SER_LOG('t', GET_DATA(gpio));
   if (fifo_used(&tx) >= SER_FIFO_SIZE)
      ser_write(SER_TX_FLAG, OP_SEC);
}

static void ser_rx_consume(unsigned int gpio)
{
   (void)gpio;
   SER_LOG('c', (ser_gate_open ? 1u : 0u) | (ser_esc_active ? 2u : 0u) | (ser_idle_active ? 4u : 0u));
   if (!ser_gate_open)
      return;
   if (ser_esc_active) {                     /* that trip was the escape one: no byte left the FIFO */
      ser_esc_active = false;
      ser_escapes++;
      ser_write(SER_JSR, (uint8_t)(ser_base + SER_INSV_TRAMP));
   } else if (ser_idle_active) {             /* ... the idle one: nor here */
      ser_idle_active = false;
      ser_idles++;
      ser_write(SER_JSR, (uint8_t)(ser_base + SER_INSV_TRAMP));
   } else {
      if (fifo_used(&rx) == 0u)
         return;
      rx.head++;
      ser_rx_count++;
   }
#ifdef DEBUG
   {
      uint32_t now = RPI_GetSystemTime();
      static uint32_t last;
      static uint32_t last_count;
      if (last_count == ser_rx_count - 1u && ser_gap_n != 0xFFFFFFFFu) {
         uint32_t gap = now - last;
         if (gap < 10000u) {              /* same burst */
            ser_gap_n++;
            ser_gap_sum += gap;
            if (gap > ser_gap_max) ser_gap_max = gap;
            if (ser_gap_min == 0u || gap < ser_gap_min) ser_gap_min = gap;
         }
      }
      last = now;
      last_count = ser_rx_count;
   }
#endif
   ser_gate_us = Pi1MHz_now_us;
   if (ser_brk_trip) {                       /* nIRQ goes low below, then the Beeb jumps to its reset */
      ser_brk_trip = false;
      ser_write(SER_EXIT, 0x6Cu);
      ser_write(SER_EXIT_OLD, 0xFCu);
      ser_write(SER_EXIT_OLD + 1u, 0xFFu);
   } else {
      ser_write(SER_EXIT, (ser_rx_count % SER_CHAIN_EVERY) == 0u ? OP_JMP : OP_RTI);
      SER_LOG('j', (ser_rx_count % SER_CHAIN_EVERY) == 0u);
   }
   if (!ser_arm_next())                      /* a trip or a byte: the gate stays open */
      ser_gate_close();
}

static void ser_rx_full(unsigned int gpio)
{
   SER_LOG('f', (ser_esc_active ? 2u : 0u) | (ser_idle_active ? 4u : 0u));
   if (ser_esc_active || ser_idle_active) {  /* a trip's routine returns the carry it came with */
      ser_rx_consume(gpio);
      return;
   }
   ser_fulls++;
   ser_gate_close();
   ser_backoff = true;
   ser_backoff_us = Pi1MHz_now_us;
}

/* The Beeb has been reset behind our back (a software BREAK, which never touches the
   reset line, or the BREAK we sent): the escape trip that was running will never finish
   and the stub's routine is the JMP (&FFFC) one.  Put the stub's call target and routine
   back to the ordinary ones and forget the keys in flight. */
static void ser_after_break(void)
{
   ser_gate_close();
   ser_esc_active = false;
   ser_esc_req = false;
   ser_idle_active = false;
   ser_idle_req = true;                         /* whatever was sent before, the busy flag starts right */
   ser_brk_req = false;
   ser_backoff = false;
   ser_brk_trip = false;
   ser_write(SER_EXIT, OP_RTI);                 /* the chain operand is the Beeb's, written when it hooked in */
   ser_write(SER_JSR, (uint8_t)(ser_base + SER_INSV_TRAMP));
   rx.head = rx.tail;
}

static void ser_control(unsigned int gpio)
{
   uint32_t v = GET_DATA(gpio);
   SER_LOG('h', v);

   /* Helper 20 says where the Beeb's cursor and what screen MODE it has, in three
      bytes sent ahead of the mode value that also means "telnet": &80 + column
      and &E0 + row, then &10 + MODE.  They are information, not a change of mode. */
   if (v == 0x22u) {                          /* the ROM: a ROM filing system operation, whose output the MOS keeps from RS423 */
      ser_unseen_gen++;
      return;
   }
   if (v == 0x24u) {                          /* the ROM: a program took a cursor key, COPY or RETURN */
      ser_key_gen++;
      return;
   }
   if (v == 0x25u) {                          /* ... or any other byte (build 13) */
      ser_key_other_gen++;
      return;
   }
   if (v == 0x20u || v == 0x21u || v == 0x23u || v == 0x26u) {   /* the auto-start ROM: a reset, seen or not on the reset line */
      ser_after_break();                         /* a soft reset leaves the screen as it was: not a new screen */
      ser_scan_ok = (v != 0x20u);                /* &21, &23, &26: and it can be asked for the screen */
      ser_keys_told = (v == 0x23u || v == 0x26u);   /* &23 (build 11): and it tells the keys taken */
      ser_lit_ok = (v == 0x26u);                 /* &26 (build 13): every key taken, and LITKEY bytes as they are */
      return;
   }
   if (v >= 0x80u) {
      if (v >= 0xE0u)
         ser_beeb_row = (uint8_t)(v & 0x1Fu);
      else
         ser_beeb_col = (uint8_t)(v & 0x7Fu);
      return;
   }
   bool telnet = (v == 2u) || ((v & 0xF8u) == 0x10u);   /* 2, or &10 + the Beeb's screen MODE (helper 20) */

   if ((v & 0xF8u) == 0x10u) {
      ser_after_break();                        /* a helper has just hooked in: nothing before it is in flight */
      ser_beeb_mode = (uint8_t)(v & 7u);
      ser_mode_gen++;                           /* the report is complete */
   }

   ser_hooked = (v != 0u);
   if (telnet != ser_telnet) {
      /* Which MOS buffer the stub inserts into: 0 the keyboard, 1 RS423 input.
         What is queued for the other one goes with the change. */
      ser_telnet = telnet;
      ser_gate_close();
      ser_write(SER_INBUF, (uint8_t)(telnet ? 0u : 1u));
      rx.head = rx.tail;
      ser_esc_req = false;
   }
   if (!ser_hooked) {
      ser_gate_close();
      ser_esc_req = false;
      ser_idle_req = false;
   }
}

/* ---- the byte API, main loop only ------------------------------------ */

static bool ser_tx_drained;       /* the TX flag may need to go back to CLC */

bool serial_redirect_pending(void)
{
   return fifo_used(&tx) != 0u;
}

size_t serial_redirect_read(uint8_t *dst, size_t max)
{
   size_t n = 0u;
   while (n < max && fifo_used(&tx) != 0u) {
      dst[n++] = tx.buf[tx.head & SER_FIFO_MASK];
      tx.head++;
   }
   if (n != 0u)
      ser_tx_drained = true;
   return n;
}

bool serial_redirect_scan_ok(void)
{
   return ser_scan_ok;
}

bool serial_redirect_keys_told(void)
{
   return ser_keys_told;
}

uint32_t serial_redirect_key_gen(void)
{
   return ser_key_gen;
}

uint32_t serial_redirect_key_other_gen(void)
{
   return ser_key_other_gen;
}

bool serial_redirect_lit_ok(void)
{
   return ser_lit_ok;
}

uint32_t serial_redirect_rx_count(void)
{
   return ser_rx_count;
}

uint32_t serial_redirect_tx_count(void)
{
   return ser_tx_count;
}

/* Counts the auto-start ROM's reports of a ROM filing system operation (service
   call &0D): the cassette and ROM filing systems own the 6850 while they run, and
   the MOS sends nothing to RS423 then (OS 1.2 &E0DC), so what they print - *CAT,
   *OPT 1 messages - is on the Beeb's screen and nowhere else. */
uint32_t serial_redirect_unseen_gen(void)
{
   return ser_unseen_gen;
}

uint32_t serial_redirect_reset_gen(void)
{
   return ser_reset_gen;
}

uint32_t serial_redirect_mode_gen(void)
{
   return ser_mode_gen;
}

uint8_t serial_redirect_beeb_mode(void)
{
   return ser_beeb_mode;
}

void serial_redirect_beeb_cursor(uint8_t *col, uint8_t *row)
{
   *col = ser_beeb_col;
   *row = ser_beeb_row;
}

size_t serial_redirect_room(void)
{
   return SER_FIFO_SIZE - fifo_used(&rx);
}

size_t serial_redirect_write(const uint8_t *src, size_t len)
{
   size_t n = 0u;
   while (n < len && fifo_used(&rx) < SER_FIFO_SIZE) {
      rx.buf[rx.tail & SER_FIFO_MASK] = src[n++];
      __asm volatile("" ::: "memory");   /* the byte before the tail: a FIQ may read it at once */
      rx.tail++;
   }
   return n;
}

/* Raise the Beeb's ESCAPE condition, as its ESCAPE key does, and throw away what
   was typed ahead, as that does too.  Telnet mode only: in modem mode ESC is
   data.  Takes effect on the next trip through the stub, a few hundred
   microseconds later when the Beeb is taking interrupts. */
void serial_redirect_escape(void)
{
   if (!ser_hooked || !ser_telnet)
      return;
   unsigned int cpsr = _disable_interrupts_cspr();
   ser_gate_close();               /* a byte may be in flight; the FIFO is about to go */
   rx.head = rx.tail;
   ser_esc_req = true;
   ser_backoff = false;            /* the escape trip never touches the keyboard buffer */
   _restore_cpsr(cpsr);
}

/* SHIFT for the Beeb, pressed once: the ROM's SHIFT byte in its workspace, which its KEYV
   handler reads and clears (see the top).  Telnet mode only, and only useful with the
   auto-start ROM (serial_redirect_scan_ok). */
void serial_redirect_shift(void)
{
   if (!ser_hooked || !ser_telnet || ser_base < SER_WS_OFF)
      return;
   Pi1MHz_MemoryWrite((uint32_t)ser_base - SER_WS_OFF + SER_WS_SHIFT, 1u);
   ser_shifts++;
}

/* BREAK the Beeb, as its BREAK key does (a soft reset: memory kept, the language
   restarts).  The stub runs a one-off routine on the Beeb's own interrupt, as for
   ESCAPE, that jumps through the reset vector; the reset is seen by serial_redirect_init
   and whatever hooks the daemon in on a reset (the auto-start ROM) puts the session
   back.  Telnet mode only. */
void serial_redirect_break(void)
{
   if (!ser_hooked || !ser_telnet)
      return;
   unsigned int cpsr = _disable_interrupts_cspr();
   ser_gate_close();
   rx.head = rx.tail;
   ser_brk_req = true;
   ser_esc_req = true;
   ser_backoff = false;
   _restore_cpsr(cpsr);
}

/* ---- poll loop -------------------------------------------------------- */

static void serial_redirect_poll(void)
{
   /* The byte stream is the modem's, or telnetd's once helper 20 has put the
      stub in telnet mode (telnetd also serves its network side every pass). */
   bool telnet = ser_telnet && ser_hooked;
   if (!telnet)
      modem_poll(Pi1MHz_now_us);
   telnetd_poll(Pi1MHz_now_us, telnet);

   if (ser_tx_drained) {
      ser_tx_drained = false;
      unsigned int cpsr = _disable_interrupts_cspr();   /* against a FIQ refilling it */
      if (fifo_used(&tx) < SER_FIFO_SIZE)
         ser_write(SER_TX_FLAG, OP_CLC);
      _restore_cpsr(cpsr);
   }

   /* Output has stopped: give the Beeb its RS423 idle flag back (see the top; telnet mode only). */
   if (telnet && !ser_idle_req && !ser_idle_active && ser_tx_count != ser_idle_tx
       && (Pi1MHz_now_us - ser_tx_us) > SER_IDLE_US)
      ser_idle_req = true;

   if (ser_gate_open) {
      if ((Pi1MHz_now_us - ser_gate_us) > SER_STALL_US) {
         unsigned int cpsr = _disable_interrupts_cspr();
         if (ser_gate_open && (Pi1MHz_now_us - ser_gate_us) > SER_STALL_US) {
            ser_stalls++;
#ifdef DEBUG
            if (ser_log_stop == 0u)
               ser_log_stop = ser_log_n + 24u;
#endif
            SER_LOG('S', 0);
            ser_gate_close();
            ser_backoff = true;
            ser_backoff_us = Pi1MHz_now_us;
         }
         _restore_cpsr(cpsr);
      }
      return;
   }

   if (ser_backoff) {
      if ((Pi1MHz_now_us - ser_backoff_us) < (ser_telnet ? SER_BACKOFF_US : SER_BACKOFF_MODEM_US))
         return;
      ser_backoff = false;
   }

   if (ser_hooked && (fifo_used(&rx) != 0u || ser_esc_req || ser_idle_req)) {
      unsigned int cpsr = _disable_interrupts_cspr();
      if (ser_hooked && !ser_gate_open && ser_arm_next()) {      /* the trip or byte first ... */
         ser_write(SER_GATE, OP_BIT);                              /* ... then the gate */
         SER_LOG('o', 0);
         ser_gate_open = true;
         ser_gate_us = Pi1MHz_now_us;
         Pi1MHz_nIRQ_ASSERT(ser_irq_src);
      }
      _restore_cpsr(cpsr);
   }
}

/* ---- init (boot, and again on every BBC reset) ------------------------ */

void serial_redirect_init(uint8_t instance, uint8_t address)
{
   ser_reset_gen++;
   SER_LOG('R', address);
   ser_scan_ok = false;                          /* the MOS has put its vectors back: the ROM says again if it can */
   ser_keys_told = false;
   ser_lit_ok = false;
   ser_brk_req = false;
   static const uint8_t stub[SER_STUB_LEN] = {
      0xE0, 0x02,             /* +00 CPX #2      */
      0xD0, 0x05,             /* +02 BNE +09     */
      OP_CLC,                 /* +04 TX flag     */
      0x8D, 0x00, 0xFC,       /* +05 STA b+0     */
      0x60,                   /* +08 RTS         */
      OP_JMP, 0x00, 0x00,     /* +09 JMP old_insv */
      OP_JMP, 0x00, 0x00,     /* +0C RX gate, old_irq1v */
      0x8A,                   /* +0F TXA         */
      0x48, 0x98, 0x48,       /* +10 PHA TYA PHA */
      0xA9, 0x00,             /* +13 LDA #byte   */
      0xA2, 0x01,             /* +15 LDX #1      */
      0x20, 0x09, 0xFC,       /* +17 JSR b+9     */
      0x90, 0x05,             /* +1A BCC +21     */
      0x8D, 0x0F, 0xFC,       /* +1C STA b+F     */
      0xB0, 0x03,             /* +1F BCS +24     */
      0x8D, 0x0C, 0xFC,       /* +21 STA b+C     */
      0x68, 0xA8, 0x68, 0xAA, /* +24 PLA TAY PLA TAX */
      0xA5, 0xFC,             /* +28 LDA &FC     */
      OP_JMP, 0x00, 0x00,     /* +2A JMP old_irq1v */
      0x8D, 0xFF, 0x00,       /* +2D STA &00FF   the trips' routine: the Pi sets addr */
      0x60                    /* +30 RTS         */
   };
   static const uint8_t rel[] = { 0x06, 0x18, 0x1D, 0x22 };   /* low bytes relative to b */

   ser_enabled = false;
   /* The stub's branches and its install code's ADC assume one page. */
   if ((unsigned int)address + SER_STUB_LEN > 0x100u)
      return;

   ser_base = address;
   ser_irq_src = instance;
   ser_hooked = false;             /* a reset has put the MOS vectors back */
   ser_telnet = false;             /* ... and the stub below is in RS423 mode again */
   ser_esc_req = false;
   ser_esc_active = false;
   ser_idle_req = false;
   ser_idle_active = false;
   ser_backoff = false;
   ser_gate_open = false;
   Pi1MHz_nIRQ_CLEAR(ser_irq_src);
   tx.head = tx.tail = 0u;
   rx.head = rx.tail = 0u;
   ser_tx_drained = false;
   modem_reset();                  /* a BBC reset hangs up, as a DTR drop would */

   for (uint8_t i = 0u; i < SER_STUB_LEN; i++)
      Pi1MHz_MemoryWrite((uint32_t)address + i, stub[i]);
   if (address >= SER_ESCR_OFF)
      for (unsigned int i = 0u; i < sizeof ser_escr; i++)
         Pi1MHz_MemoryWrite((uint32_t)address - SER_ESCR_OFF + i, ser_escr[i]);
   for (unsigned int i = 0u; i < sizeof rel; i++)
      Pi1MHz_MemoryWrite((uint32_t)address + rel[i], (uint8_t)(address + stub[rel[i]]));

   Pi1MHz_Register_Memory(WRITE_FRED, address + SER_TX_PORT,   ser_tx_byte);
   Pi1MHz_Register_Memory(WRITE_FRED, address + SER_GATE,      ser_rx_consume);
   Pi1MHz_Register_Memory(WRITE_FRED, address + SER_FULL_PORT, ser_rx_full);
   Pi1MHz_Register_Memory(WRITE_FRED, address + SER_CONTROL,   ser_control);
   static const uint8_t stash[] = { SER_INSV_OLD, SER_INSV_OLD + 1u,
                                    SER_IRQ1V_OLD, SER_IRQ1V_OLD + 1u,
                                    SER_EXIT_OLD, SER_EXIT_OLD + 1u };
   for (unsigned int i = 0u; i < sizeof stash; i++)
      Pi1MHz_Register_Memory(WRITE_FRED, (unsigned int)address + stash[i], Pi1MHz_EmulatedMemoryByte);
   if (address >= SER_WS_OFF) {                 /* the auto-start ROM's workspace (see SER_WS_OFF) */
      /* Cleared only at the first init after the Pi starts.  This runs again on every Beeb reset,
         up to a poll pass after it, by when the ROM may already have kept the MOS's routines here:
         clearing them then sent REMV to &0000. */
      static bool ws_cleared;
      for (unsigned int i = 0u; i < SER_WS_LEN; i++) {
         if (!ws_cleared)
            Pi1MHz_MemoryWrite((uint32_t)address - SER_WS_OFF + i, 0u);
         Pi1MHz_Register_Memory(WRITE_FRED, (unsigned int)address - SER_WS_OFF + i, Pi1MHz_EmulatedMemoryByte);
      }
      ws_cleared = true;
      Pi1MHz_MemoryWrite((uint32_t)address - SER_WS_OFF + SER_WS_TELNET,   /* telnetd on: the ROM may hook in */
                         telnetd_enabled() ? 1u : 0u);
   }

   Pi1MHz_Register_Poll(serial_redirect_poll, "serial");
   ser_enabled = true;
}

#ifdef DEBUG
/* The event ring for /status: "ms.us ev data" each, oldest first, times relative to the
   newest.  ev: t TX byte, o gate open, x gate shut, c consume (1 open 2 esc 4 idle),
   f full, K/I/E armed key/idle/escape, j exit (1 JMP), S stall, h control, R init. */
void serial_redirect_log(char *buf, size_t len)
{
   uint32_t n = ser_log_n;
   uint32_t first = n > SER_LOG_N ? n - SER_LOG_N : 0u;
   uint32_t last_us = n != 0u ? ser_log_ring[(n - 1u) % SER_LOG_N].us : 0u;
   size_t used = 0u;
   buf[0] = '\0';
   used += (size_t)snprintf(buf, len, "n %lu stop %lu:", (unsigned long)n, (unsigned long)ser_log_stop);
   for (uint32_t i = first; i < n && used + 24u < len; i++) {
      uint32_t ago = last_us - ser_log_ring[i % SER_LOG_N].us;
      used += (size_t)snprintf(buf + used, len - used, " -%lu.%03lu%c%02X",
                               (unsigned long)(ago / 1000u), (unsigned long)(ago % 1000u),
                               ser_log_ring[i % SER_LOG_N].ev, ser_log_ring[i % SER_LOG_N].d);
   }
}
#endif

uint8_t serial_redirect_address(void)
{
   return ser_enabled ? ser_base : 0u;
}

void serial_redirect_status(char *buf, size_t len)
{
   if (!ser_enabled) {
      snprintf(buf, len, "off");
      return;
   }
   snprintf(buf, len, "&FC%02X %s gate %s tx %lu rx %lu drop %lu full %lu stall %lu esc %lu idle %lu shift %lu q %lu/%lu",
            (unsigned int)ser_base,
            !ser_hooked ? "not hooked" : ser_telnet ? "hooked (telnet)" : "hooked",
            ser_gate_open ? "open" : "shut",
            (unsigned long)ser_tx_count, (unsigned long)ser_rx_count,
            (unsigned long)ser_tx_drops, (unsigned long)ser_fulls,
            (unsigned long)ser_stalls, (unsigned long)ser_escapes, (unsigned long)ser_idles, (unsigned long)ser_shifts,
            (unsigned long)fifo_used(&tx), (unsigned long)fifo_used(&rx));
#ifdef DEBUG
   size_t n = strlen(buf);
   if (n < len && ser_gap_n != 0u)
      snprintf(buf + n, len - n, " gap us %lu/%lu/%lu n%lu",
               (unsigned long)ser_gap_min, (unsigned long)(ser_gap_sum / ser_gap_n),
               (unsigned long)ser_gap_max, (unsigned long)ser_gap_n);
#endif
}
