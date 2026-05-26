//
// Pi1MHz FIQ handler
//
// (c) Dominic Plunkett
//
// FIQ code sits at FIQ vector
// This code is directly included at the FIQ entry
// So don't put any code before it.
// and is triggered by fred or jim access

#include "Pi1MHz.h"
#include "rpi/base.h"

#define FIQ_SETUP_CODE 1
.macro FIQ_SETUP_M
   LDR     R12,= (PERIPHERAL_BASE + 0x00B844)
   LDR     r11,= Pi1MHz_VPU_RETURN
   mov     r10, # ADDRBUS_MASK>>ADDRBUS_SHIFT
   orr     r10,r10,#NPCFC_MASK>>ADDRBUS_SHIFT
.endm

.macro DMB_MACRO
#if (__ARM_ARCH == 6 )
   // dmb   // Only needed on ArmV6 systems
    mov      r9, #0
    mcr      p15, 0, r9, c7, c10, 5
#endif

.endm

//  r8  temp
//  r9  temp
//  r10 Address mask
//  r11 VPU return data
//  r12 doorbell reg
//  r13 stack
//  r14 return address
FIQstart:
   DMB_MACRO

   LDR      r8, [r11]       // get data posted going of chip
   DMB_MACRO                // Stall
   LDR      r9, [R12]       // read door bell to ack

   tst      r8, # RNW_MASK
   and      r9, r10, r8, LSR # ADDRBUS_SHIFT // isolate address bus and fred or jim

   orrne    r9, r9, # Pi1MHz_MEM_RNW     // set read flag ready for call back table

   mov      r9, r9 , LSL # 2
   ldr      r9, [r9, #Pi1MHz_CB_BASE]    // load call back pointer

//stall

   movs     r9, r9

   subeqs   pc, lr,#4   // no callback

   push    {r0-r3,r12, r14}
   mov     r0, r8, LSR # DATABUS_SHIFT
   blx     r9
   pop     {r0-r3,r12, r14}

   subs    pc, lr, #4
FIQend:
