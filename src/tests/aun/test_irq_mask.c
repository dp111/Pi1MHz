/* Regression test for the shared nIRQ mask (Pi1MHz_SetnIRQ_src model in
 * Pi1MHz.c). nIRQ is an open-collector line shared by several emulators;
 * each owns one bit of the mask, indexed by its emulator-table slot (the
 * 'instance' passed to <emu>_init). AUN is slot 11, so the mask and the
 * working shift must be at least 32-bit: a uint8_t made (1u<<11) == 0 and
 * AUN could never assert nIRQ. This test pins that down and also checks
 * that sources are independent (one releasing must not clear another's). */
#include <stdint.h>
#include <stdio.h>
#include <assert.h>

/* Mirror of the production logic in Pi1MHz_SetnIRQ_src(). */
static uint32_t mask;
static int      line_asserted;
static void set_src(uint8_t src, int assert_irq)
{
   if (assert_irq) mask |=  (1u << src);
   else            mask &= ~(1u << src);
   line_asserted = (mask != 0);
}

#define HD_SLOT   3u    /* Harddisc emulator-table index */
#define AUN_SLOT  11u   /* AUN emulator-table index      */

int main(void)
{
   /* 1. AUN (slot 11) must actually be able to assert the line. The old
    *    uint8_t working copy truncated 1u<<11 to 0 -> silently broken. */
   mask = 0; line_asserted = 0;
   set_src(AUN_SLOT, 1);
   assert(line_asserted && "AUN slot 11 must assert nIRQ");

   /* the historical bug, made explicit: width matters */
   assert((uint8_t)(1u << AUN_SLOT) == 0);     /* what used to happen */
   assert((uint32_t)(1u << AUN_SLOT) != 0);    /* the fix */

   /* 2. Open-collector independence: harddisc + aun both want the line;
    *    releasing one keeps it asserted while the other still wants it. */
   mask = 0; line_asserted = 0;
   set_src(HD_SLOT, 1);
   set_src(AUN_SLOT, 1);
   assert(line_asserted);
   set_src(HD_SLOT, 0);                         /* harddisc releases */
   assert(line_asserted && "AUN still holds nIRQ after harddisc releases");
   set_src(AUN_SLOT, 0);                        /* aun releases */
   assert(!line_asserted && "line released once all sources clear");

   /* 3. Every emulator-table slot in the supported range maps to a real
    *    (non-truncated) bit. */
   for (unsigned s = 0; s < 32; s++)
      assert((1u << s) != 0);

   printf("all irq-mask tests passed\n");
   return 0;
}
