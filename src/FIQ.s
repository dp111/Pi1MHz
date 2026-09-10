//
// Pi1MHz FIQ handler
//
// (c) Dominic Plunkett
//
// FIQ code sits at FIQ vector
// This code is directly included at the FIQ entry
// So don't put any code before it.
// and is triggered by fred or jim access
//
// Post ring (docs/dev/bus-post-ring.md): the VPU writes each bus cycle's
// GPIO sample, tagged with a 5-bit sequence number in bits 27-31, into one
// of eight registers, then rings the doorbell.  This handler acknowledges
// the bell FIRST (the data is in the ring, so a bell rung after the
// acknowledge simply re-enters), then drains every slot whose tag matches
// its own sequence.  The VPU stores the entry before it rings and the entry
// is there by the time the FIQ reads it: a measurement build that re-read an
// unwritten slot up to 64 times never found one that landed within four
// reads (every hit was the Beeb's NEXT cycle arriving), so the handler does
// not re-read.  The drain ends at the first slot not yet written; it cannot
// run for ever because one bus cycle per microsecond is slower than one
// pass of this loop.

#include "Pi1MHz.h"
#include "rpi/base.h"

#define FIQ_SETUP_CODE 1
.macro FIQ_SETUP_M
   mov     r12, #ADDRBUS_MASK>>(ADDRBUS_SHIFT-2)
   orr     r12, r12, #NPCFC_MASK>>(ADDRBUS_SHIFT-2)
   LDR     r11, =Pi1MHz_POST_RING     // ring base
   mov     r10, #0                    // consumer sequence: the VPU starts at tag 0 too
   mov     r9, #0                     // the entry DMB needs a zero: every exit leaves r9 zero
.endm

.macro DMB_MACRO reg
#if (__ARM_ARCH == 6 )
   // dmb   // Only needed on ArmV6 systems
    mcr      p15, 0, \reg, c7, c10, 5
#endif

.endm

// FIQ-banked registers: nothing here is saved or restored, and they hold
// their values from one FIQ to the next.
//  r8  temp: the next slot index (computed early), then the entry being dispatched
//  r9  temp: the doorbell read, tag difference, callback; ZERO on entry and
//      exit (the DMB wants a zero register; the common exit leaves one for free)
//  r10 C, the sequence tag of the next entry to consume, pre-shifted to bits
//      27-31 (like the VPU's r10); bits 27-29 are the slot, bits 30-31 the lap
//  r11 ring base
//  r12 address mask (a constant; saved round the callback because C code
//      treats r12 as scratch)
//  r13 stack
//  r14 return address
FIQstart:
   DMB_MACRO r9             // r9 == 0.  This finishes any outstanding read that might be
                            // inflight e.g. from the foreground task
   and      r8, r10, #0x38000000       // first slot = C & 7, still up at bits 27-29; early, so the
                                       // shifted index in the entry load below never interlocks

// Acknowledge the doorbell first (read to clear).  The value is not used and r9
// is not written again until the first entry has arrived, so the read stays in
// flight with nothing waiting on it.
   mov      r9,     #(PERIPHERAL_BASE + 0x00B844) & 0xff000000
#if  ((PERIPHERAL_BASE + 0x00B844) & 0x00ff0000)
   orr      r9, r9, #(PERIPHERAL_BASE + 0x00B844) & 0x00ff0000
#endif
   orr      r9, r9, #(PERIPHERAL_BASE + 0x00B844) & 0x0000ff00
   LDR      r9, [r9, #(PERIPHERAL_BASE + 0x00B844) & 0x000000ff]       // read door bell to ack

drain:
   LDR      r8, [r11, r8, LSR #25]     // LSR #25 is slot * 4: the entry (stalls: off chip)
   sub      r9, r8, r10                // tag difference in bits 27-31 (no borrow: C's low 27 bits are 0)
   tst      r9, #0xF8000000
   bne      notmine                    // not C's: notmine reuses the difference

// Dispatch: the callback table is indexed by address bus + fred/jim + RnW.
   tst      r8, # RNW_MASK
   and      r9, r12, r8, LSR # ADDRBUS_SHIFT - 2 // isolate address bus and fred or jim
   orrne    r9, r9, # Pi1MHz_MEM_RNW<<2     // set read flag ready for call back table
   ldr      r9, [r9, #Pi1MHz_CB_BASE]       // load call back pointer
   add      r10, r10, #0x08000000           // C += 1 (wraps through bit 31), in the load's shadow
   cmp      r9, #0
   andeq    r8, r10, #0x38000000            // no callback: next slot index ...
   beq      drain                           // ... and straight on

   push    {r0-r3,r12, r14}
   mov     r0, r8, LSR # DATABUS_SHIFT
   and     r8, r10, #0x38000000             // next slot index; r8 is callee-saved so it survives the call
   blx     r9
   pop     {r0-r3,r12, r14}
   b       drain

notmine:
// The tag is not C's.  The slot bits always agree (an entry in slot s carries
// slot s), so the difference is in the lap: entry lap - C lap = 3 means the
// slot still holds LAST lap's entry, i.e. not written yet, and the drain is
// done - the common way out of every FIQ, so it falls through to done with
// r9 already zero for the DMB.  1 or 2 means the VPU is a lap or two ahead:
// entries are gone.  Count it and leave C alone: the VPU's tag comes back
// round and the stream resyncs itself (each bell in between counts again,
// so /status shows overrun FIQs, not events).
   eor      r9, r9, #0xC0000000         // lap difference 3 -> 0 in the tag bits
   ands     r9, r9, #0xF8000000
   bne      overrun
done:
   DMB_MACRO r9             // r9 == 0: the doorbell ack has actually left the core; r9 stays zero for the next entry
   subs     pc, lr, #4

overrun:
   ldr      r9, =Pi1MHz_fiq_ovr_first_us   // BREAK forensics: when the first overrun after a reset happened
   ldr      r8, [r9]
   cmp      r8, #0                          // 0 = none since the nRST IRQ cleared it
   bne      counted
   ldr      r8, =(PERIPHERAL_BASE + 0x3004) // system timer CLO
   ldr      r8, [r8]
   str      r8, [r9]
counted:
   ldr      r9, =Pi1MHz_fiq_overruns
   ldr      r8, [r9]
   add      r8, r8, #1
   str      r8, [r9]
   mov      r9, #0
   b        done
FIQend:
.ltorg
