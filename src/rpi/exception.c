#include "rpi.h"
#include "asm-helpers.h"
#include "auxuart.h"
#include "cache.h"

/* Persistent crash record, sharing the .noinit boot-stage block
   (rpi/mailbox.c, words 4-11): .noinit survives the watchdog reset that
   ends dump_info and is untouched by the loader and BSS zeroing.  Read
   back by the /status page - the serial dump below needs a cable this Pi
   does not usually have attached.
   Layout: magic, type char, faulting pc, spsr, DFAR, DFSR, boot stage at the
   time, count of faults since power-on. */
#define CRASH_BASE (RPI_BootStageBlock() + 4)
#define CRASH_MAGIC 0xC7A54ADEu

const volatile unsigned int *RPI_LastCrash(void)
{
   return (CRASH_BASE[0] == CRASH_MAGIC) ? (const volatile unsigned int *)CRASH_BASE : 0;
}

/* From here: https://www.raspberrypi.org/forums/viewtopic.php?f=72&t=53862*/
_Noreturn void reboot_now(void)
{
  const int PM_PASSWORD = 0x5a000000;
  const int PM_RSTC_WRCFG_FULL_RESET = 0x00000020;
  unsigned int *PM_WDOG = (unsigned int *) (PERIPHERAL_BASE + 0x00100024);
  unsigned int *PM_RSTC = (unsigned int *) (PERIPHERAL_BASE + 0x0010001c);

  /* timeout = 1/16th of a second? (whatever)*/
  *PM_WDOG = PM_PASSWORD | 1;
  *PM_RSTC = PM_PASSWORD | PM_RSTC_WRCFG_FULL_RESET;
  while (1);
}

static void dump_digit(unsigned char c) {
   c &= 15;
   if (c < 10) {
      c = (unsigned char) ('0' + c);
   } else {
      c = (unsigned char) ('A' + c - 10);
   }
   RPI_AuxMiniUartWriteForce((uint8_t)c);
}

static void dump_hex(unsigned int value) {
  int nibbles = sizeof(value) * 2 ;
  for (int i = nibbles; i != 0 ; i--) {
   dump_digit( (uint8_t) (value >> ( (nibbles * 4) - 4) ) );
   value <<= 4;
  }
}

static void dump_binary(unsigned int value) {
  int bits = (sizeof(value) * 8);
  for (int i = bits; i != 0; i--) {
    RPI_AuxMiniUartWriteForce((uint8_t)('0' + (value >> (bits - 1))));
    value <<= 1;
  }
}

static void dump_string(const char *string) {
  char c;
  while ((c = *string++) != 0) {
    RPI_AuxMiniUartWriteForce((uint8_t)c);
  }
}

/* printf isn't used as it is buffered and uses IRQs which might be disabled */
// cppcheck-suppress unusedFunction
_Noreturn void dump_info(unsigned int *context, int offset, const char *type) {
  unsigned int *addr;
  const unsigned int *reg;
  unsigned int flags;

  /* context point into the exception stack, at flags, followed by registers 0 .. 13 */
  reg = context + 1;

  /* Record the crash where the next boot can report it, before attempting
     the UART dump (whose FIFO polling could itself wedge). */
  {
    uint32_t dfar, dfsr;
    __asm__ volatile ("mrc p15, 0, %0, c6, c0, 0" : "=r" (dfar));
    __asm__ volatile ("mrc p15, 0, %0, c5, c0, 0" : "=r" (dfsr));
    CRASH_BASE[1] = (uint32_t)(unsigned char)type[0];   /* U/P/D/S */
    CRASH_BASE[2] = (reg[13] & ~3u) - (uint32_t)offset; /* faulting pc */
    CRASH_BASE[3] = *context;                           /* spsr */
    CRASH_BASE[4] = dfar;
    CRASH_BASE[5] = dfsr;
    CRASH_BASE[6] = RPI_BootStageBlock()[1];               /* boot stage */
    CRASH_BASE[7] = (CRASH_BASE[0] == CRASH_MAGIC) ? CRASH_BASE[7] + 1u : 1u;
    CRASH_BASE[0] = CRASH_MAGIC;
    {
      const volatile unsigned int *blk = RPI_BootStageBlock();
      _clean_cache_area((const void *)(uintptr_t)blk, 64);
    }
  }
  dump_string("\r\n\r\n");
  dump_string(type);
  dump_string(" at ");
  /* The stacked LR points one or two words after the exception address */
  addr = (unsigned int *)((reg[13] & ~3u) - (uint32_t)offset);
  dump_hex((unsigned int)addr);
#if (__ARM_ARCH >= 7 )
  dump_string(" on core ");
  dump_digit((unsigned char)_get_core());
#endif
  dump_string("\r\nRegisters:\r\n");
  for (int i = 0; i <= 13; i++) {
    int j = (i < 13) ? i : 14; /* slot 13 actually holds the link register */
    dump_string("  r[");
    RPI_AuxMiniUartWriteForce((uint8_t)('0' + (j / 10)));
    RPI_AuxMiniUartWriteForce((uint8_t)('0' + (j % 10)));
    dump_string("]=");
    dump_hex(reg[i]);
    dump_string("\r\n");
  }
  dump_string("Memory:\r\n");
  /* Only dereference the window when the faulting pc lies in kernel RAM: a
     wild pc (a common crash class) would make these reads fault again -
     recursing into dump_info in ABT mode and overwriting the crash
     record's pc with our own - or wedge the bus on a strongly-ordered
     peripheral read. 512 MB is the largest SDRAM any supported Pi has;
     the crash record above is already written and cache-cleaned. */
  if ((uint32_t)addr >= 0x8000u && (uint32_t)addr < 0x20000000u) {
    for (int i = -4; i <= 4; i++) {
      dump_string("  ");
      dump_hex((unsigned int) (addr + i));
      RPI_AuxMiniUartWriteForce('=');
      dump_hex(*(addr + i));
      if (i == 0) {
        dump_string(" <<<<<< \r\n");
      } else {
        dump_string("\r\n");
      }
    }
  } else {
    dump_string("  (pc outside RAM - window skipped)\r\n");
  }
  /* The flags are pointed to by context, before the registers */
  flags = *context;
  dump_string("Flags: \r\n  NZCV--------------------IFTMMMMM\r\n  ");
  dump_binary(flags);
  dump_string(" (");
  /* The last 5 bits of the flags are the mode */
  switch (flags & 0x1f) {
  case 0x10:
    dump_string("User");
    break;
  case 0x11:
    dump_string("FIQ");
    break;
  case 0x12:
    dump_string("IRQ");
    break;
  case 0x13:
    dump_string("Supervisor");
    break;
  case 0x17:
    dump_string("Abort");
    break;
  case 0x1B:
    dump_string("Undefined");
    break;
  case 0x1F:
    dump_string("System");
    break;
  default:
    dump_string("Illegal");
    break;
  };
  dump_string(" Mode)\r\nHalted waiting for reset\r\n");

  reboot_now();
}
