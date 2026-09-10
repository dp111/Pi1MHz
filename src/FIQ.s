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
// its own sequence.  The VPU stores the entry before it rings, and its
// stores land in order, so the entry a bell announces is always there:
// no retry.  The drain ends at the first slot not yet written; it cannot
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
//  r9  temp: the doorbell read, tag test, callback; ZERO on entry and exit
//      (the DMB wants a zero register, so the exit leaves one)
//  r10 C, the sequence tag of the next entry to consume, pre-shifted to bits
//      27-31 (like the VPU's r10); bits 27-29 are the slot, bits 30-31 the lap
//  r11 ring base
//  r12 address mask (a constant; saved round the callback because C code
//      treats r12 as scratch); bit 31 = "consumed something in this FIQ"
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
   eor      r9, r8, r10                // bits 27-31 clear if the tag is C's
   tst      r9, #0xF8000000
   bne      notmine

// Dispatch: the callback table is indexed by address bus + fred/jim + RnW.
dispatch:
   orr      r12, r12, #0x80000000           // something consumed in this FIQ (see notwritten; cleared at done).
                                            // Harmless in the mask below: r8 LSR #14 has bit 31 clear.
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
// slot still holds LAST lap's entry, i.e. not written yet.  1 or 2 means the
// VPU is a lap or two ahead: entries are gone.  Count it and leave C alone:
// the VPU's tag comes back round and the stream resyncs itself (each bell in
// between counts again, so /status shows overrun FIQs, not events).
   sub      r9, r8, r10                 // lap difference in bits 30-31 (bits 27-29 cancel)
   eor      r9, r9, #0xC0000000         // 3 -> 0
   tst      r9, #0xC0000000
   beq      notwritten

overrun:
   ldr      r9, =Pi1MHz_fiq_overruns
   ldr      r8, [r9]
   add      r8, r8, #1
   str      r8, [r9]
   b        done

notwritten:
// Not written yet.  If this FIQ has consumed something, that is the normal
// end of the drain.  If it has consumed NOTHING, the bell that woke us
// announced an entry that has not landed yet: the entry and the bell are
// stores to different peripheral blocks and the entry can arrive after the
// bell (seen in the counter builds).  Re-read the slot, bounded; if it never
// lands we leave and the next bell finds it.
   tst      r12, #0x80000000
   bne      done
   mov      r9, #16
   push     {r9}                        // re-read budget
retry:
   and      r8, r10, #0x38000000
   LDR      r8, [r11, r8, LSR #25]
   eor      r9, r8, r10
   tst      r9, #0xF8000000
   beq      landed
   ldr      r9, [sp]
   subs     r9, r9, #1
   str      r9, [sp]
   bne      retry
   add      sp, sp, #4
   b        done
landed:
   add      sp, sp, #4
   b        dispatch

done:
   bic      r12, r12, #0x80000000       // consumed flag off for the next FIQ
   mov      r9, #0
   DMB_MACRO r9             // the doorbell ack has actually left the core; r9 left zero for the next entry
   subs     pc, lr, #4
FIQend:
.ltorg
