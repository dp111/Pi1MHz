/*
  serial_redirect.c - the BBC's RS423 stream, redirected to the Pi

  The 6850 at &FE08 is not on the 1MHz bus, so the Pi cannot be it.  What
  the Pi can do is stand in for it one level up: a 45-byte stub in FRED
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

#define SER_TX_PORT    0x00u
#define SER_TX_FLAG    0x04u
#define SER_INSV_OLD   0x0Au
#define SER_GATE       0x0Cu
#define SER_IRQ1V_OLD  0x0Du
#define SER_FULL_PORT  0x0Fu
#define SER_RX_BYTE    0x14u
#define SER_CONTROL    0x28u
#define SER_EXIT       0x2Au
#define SER_EXIT_OLD   0x2Bu
#define SER_STUB_LEN   0x2Du

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
#define SER_BACKOFF_US   20000u

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
static volatile bool     ser_gate_open;   /* +0C is BIT: nIRQ asserted, byte at +14 */
static volatile uint32_t ser_gate_us;     /* gate opened, or last consume */
static volatile bool     ser_backoff;     /* the Beeb said buffer 1 is full */
static volatile uint32_t ser_backoff_us;

static volatile uint32_t ser_tx_count, ser_rx_count, ser_tx_drops, ser_fulls, ser_stalls;
#ifdef DEBUG
/* Per-byte cost of the IRQ path: the gap between consecutive consumes while
   the gate stays open, i.e. one whole trip through the stub and back. */
static volatile uint32_t ser_gap_n, ser_gap_sum, ser_gap_max, ser_gap_min;
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
   ser_gate_open = false;
   Pi1MHz_nIRQ_CLEAR(ser_irq_src);
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
   if (fifo_used(&tx) >= SER_FIFO_SIZE)
      ser_write(SER_TX_FLAG, OP_SEC);
}

static void ser_rx_consume(unsigned int gpio)
{
   (void)gpio;
   if (!ser_gate_open || fifo_used(&rx) == 0u)
      return;
   rx.head++;
   ser_rx_count++;
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
   ser_write(SER_EXIT, (ser_rx_count % SER_CHAIN_EVERY) == 0u ? OP_JMP : OP_RTI);
   if (fifo_used(&rx) != 0u)
      ser_write(SER_RX_BYTE, rx.buf[rx.head & SER_FIFO_MASK]);   /* gate stays open */
   else
      ser_gate_close();
}

static void ser_rx_full(unsigned int gpio)
{
   (void)gpio;
   ser_fulls++;
   ser_gate_close();
   ser_backoff = true;
   ser_backoff_us = Pi1MHz_now_us;
}

static void ser_control(unsigned int gpio)
{
   ser_hooked = (GET_DATA(gpio) != 0u);
   if (!ser_hooked)
      ser_gate_close();
}

/* ---- the byte API, main loop only ------------------------------------ */

static bool ser_tx_drained;       /* the TX flag may need to go back to CLC */

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

/* ---- poll loop -------------------------------------------------------- */

static void serial_redirect_poll(void)
{
   modem_poll(Pi1MHz_now_us);

   if (ser_tx_drained) {
      ser_tx_drained = false;
      unsigned int cpsr = _disable_interrupts_cspr();   /* against a FIQ refilling it */
      if (fifo_used(&tx) < SER_FIFO_SIZE)
         ser_write(SER_TX_FLAG, OP_CLC);
      _restore_cpsr(cpsr);
   }

   if (ser_gate_open) {
      if ((Pi1MHz_now_us - ser_gate_us) > SER_STALL_US) {
         unsigned int cpsr = _disable_interrupts_cspr();
         if (ser_gate_open && (Pi1MHz_now_us - ser_gate_us) > SER_STALL_US) {
            ser_stalls++;
            ser_gate_close();
            ser_backoff = true;
            ser_backoff_us = Pi1MHz_now_us;
         }
         _restore_cpsr(cpsr);
      }
      return;
   }

   if (ser_backoff) {
      if ((Pi1MHz_now_us - ser_backoff_us) < SER_BACKOFF_US)
         return;
      ser_backoff = false;
   }

   if (ser_hooked && fifo_used(&rx) != 0u) {
      unsigned int cpsr = _disable_interrupts_cspr();
      if (ser_hooked && !ser_gate_open) {
         ser_write(SER_RX_BYTE, rx.buf[rx.head & SER_FIFO_MASK]);   /* byte first ... */
         ser_write(SER_GATE, OP_BIT);                              /* ... then the gate */
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
      OP_JMP, 0x00, 0x00      /* +2A JMP old_irq1v */
   };
   static const uint8_t rel[] = { 0x06, 0x18, 0x1D, 0x22 };   /* low bytes relative to b */

   ser_enabled = false;
   /* The stub's branches and its install code's ADC assume one page. */
   if ((unsigned int)address + SER_STUB_LEN > 0x100u)
      return;

   ser_base = address;
   ser_irq_src = instance;
   ser_hooked = false;             /* a reset has put the MOS vectors back */
   ser_backoff = false;
   ser_gate_open = false;
   Pi1MHz_nIRQ_CLEAR(ser_irq_src);
   tx.head = tx.tail = 0u;
   rx.head = rx.tail = 0u;
   ser_tx_drained = false;
   modem_reset();                  /* a BBC reset hangs up, as a DTR drop would */

   for (uint8_t i = 0u; i < SER_STUB_LEN; i++)
      Pi1MHz_MemoryWrite((uint32_t)address + i, stub[i]);
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

   Pi1MHz_Register_Poll(serial_redirect_poll, "serial");
   ser_enabled = true;
}

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
   snprintf(buf, len, "&FC%02X %s gate %s tx %lu rx %lu drop %lu full %lu stall %lu q %lu/%lu",
            (unsigned int)ser_base, ser_hooked ? "hooked" : "not hooked",
            ser_gate_open ? "open" : "shut",
            (unsigned long)ser_tx_count, (unsigned long)ser_rx_count,
            (unsigned long)ser_tx_drops, (unsigned long)ser_fulls,
            (unsigned long)ser_stalls,
            (unsigned long)fifo_used(&tx), (unsigned long)fifo_used(&rx));
#ifdef DEBUG
   size_t n = strlen(buf);
   if (n < len && ser_gap_n != 0u)
      snprintf(buf + n, len - n, " gap us %lu/%lu/%lu n%lu",
               (unsigned long)ser_gap_min, (unsigned long)(ser_gap_sum / ser_gap_n),
               (unsigned long)ser_gap_max, (unsigned long)ser_gap_n);
#endif
}
