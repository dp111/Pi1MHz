/* Regression test for the shared nIRQ mask in Pi1MHz.c.  nIRQ is an
 * open-collector line shared by several emulators; each owns one bit of the
 * mask, indexed by its emulator-table slot (the 'instance' passed to
 * <emu>_init).  AUN is slot 11: when the mask was a uint8_t, 1u<<11 was lost
 * and AUN could never assert nIRQ.
 *
 * run_tests.sh extracts the REAL Pi1MHz_nirq_mask, Pi1MHz_SetnIRQ_src,
 * Pi1MHz_nIRQ_ASSERT and Pi1MHz_nIRQ_CLEAR verbatim from a copy of Pi1MHz.c
 * into nirq.inc; this file supplies only the hardware they touch.  That
 * every slot has a bit is a compile-time fact, so it is a _Static_assert in
 * Pi1MHz.c next to the mask, not a check here. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>

/* ---- stubs for what the extracted code calls ---- */
#define NIRQ_PIN (12)
typedef unsigned int rpi_gpio_pin_t;
typedef enum { FS_INPUT = 0, FS_OUTPUT } rpi_gpio_alt_function_t;

static int irqs_off;                 /* nesting of the FIQ/IRQ guard */
static unsigned int _disable_interrupts_cspr(void) { irqs_off++; return 0x1Fu; }
static void _restore_cpsr(unsigned int cpsr) { assert(cpsr == 0x1Fu); irqs_off--; }

static int gpfsel_writes;            /* RPI_SetGpioPinFunction calls */
static rpi_gpio_alt_function_t pin_fn = FS_INPUT;
static void RPI_SetGpioPinFunction(rpi_gpio_pin_t gpio, rpi_gpio_alt_function_t func)
{
   assert(gpio == NIRQ_PIN);
   assert(irqs_off > 0 && "GPFSEL RMW must run inside the interrupt guard");
   gpfsel_writes++;
   pin_fn = func;
}

#include "nirq.inc"

#define HD_SLOT   3u    /* Harddisc emulator-table index */
#define AUN_SLOT  11u   /* AUN emulator-table index      */

static void reset(void) { Pi1MHz_nirq_mask = 0; pin_fn = FS_INPUT; gpfsel_writes = 0; }

int main(void)
{
   /* 1. AUN (slot 11) really asserts the line, and its bit is kept. */
   reset();
   Pi1MHz_nIRQ_ASSERT((uint8_t)AUN_SLOT);
   assert(pin_fn == FS_OUTPUT && "AUN slot 11 must assert nIRQ");
   assert(Pi1MHz_nirq_mask == (1u << AUN_SLOT));
   Pi1MHz_nIRQ_CLEAR((uint8_t)AUN_SLOT);
   assert(pin_fn == FS_INPUT && Pi1MHz_nirq_mask == 0u);

   /* 2. Open-collector independence: releasing one source keeps the line
    *    asserted while another still wants it. */
   reset();
   Pi1MHz_nIRQ_ASSERT((uint8_t)HD_SLOT);
   Pi1MHz_nIRQ_ASSERT((uint8_t)AUN_SLOT);
   assert(pin_fn == FS_OUTPUT);
   Pi1MHz_nIRQ_CLEAR((uint8_t)HD_SLOT);
   assert(pin_fn == FS_OUTPUT && "AUN still holds nIRQ after harddisc releases");
   Pi1MHz_nIRQ_CLEAR((uint8_t)AUN_SLOT);
   assert(pin_fn == FS_INPUT && "line released once all sources clear");

   /* 3. GPFSEL is written only when "any source asserted" changes: the SCSI
    *    DMA loops re-assert once per byte. */
   reset();
   for (int i = 0; i < 256; i++)
      Pi1MHz_nIRQ_ASSERT((uint8_t)HD_SLOT);
   Pi1MHz_nIRQ_ASSERT((uint8_t)AUN_SLOT);
   Pi1MHz_nIRQ_CLEAR((uint8_t)HD_SLOT);
   assert(gpfsel_writes == 1);
   Pi1MHz_nIRQ_CLEAR((uint8_t)AUN_SLOT);
   Pi1MHz_nIRQ_CLEAR((uint8_t)AUN_SLOT);
   assert(gpfsel_writes == 2 && pin_fn == FS_INPUT);

   /* 4. Every bit below the diag pin-level bit (31) is its own source. */
   for (unsigned s = 0; s < 31u; s++) {
      reset();
      Pi1MHz_nIRQ_ASSERT((uint8_t)s);
      assert(Pi1MHz_nirq_mask == (1u << s) && pin_fn == FS_OUTPUT);
   }
   assert(irqs_off == 0);

   printf("all irq-mask tests passed\n");
   return 0;
}
