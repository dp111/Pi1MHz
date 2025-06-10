#include "rpi.h"
#include "asm-helpers.h"
#include "auxuart.h"

/* From here: https://www.raspberrypi.org/forums/viewtopic.php?f=72&t=53862*/
_Noreturn static void reboot_now(void)
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
      c = '0' + c;
   } else {
      c = 'A' + c - 10;
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
