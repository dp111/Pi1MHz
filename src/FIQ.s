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
#include "rpi/rpi-base.h"

#define GPFSEL0 (PERIPHERAL_BASE + 0x200000)  // controls GPIOs 0..9

#define GPFSEL0_OFFSET      0x00
#define GPSET0_OFFSET       0x1C
#define GPCLR0_OFFSET       0x28 
#define GPLEV0_OFFSET       0x34
#define GPEDS0_OFFSET       0x40
#define GPFEN0_OFFSET       0x58

//-----------------------------------------------------------------------
// Setup registers for FIQ handler
// this gets inserted into the code after the stack is setup
//  R11 is preset to be the GPIO base

#define FIQ_SETUP     LDR     r11,=GPFSEL0

.macro DMB_MACRO

#if defined(RPI2) || defined(RPI3)
    dmb
#else
    mov      r8, #0
    mcr      p15, 0, r8, c7, c10, 5
#endif

.endm

// Pi1MHz_Memory is defined to be at 0x0000 - 0x1ff
// Pi1MHz_callback_table is defined to be at 0x00000200 - 0x1200

//  r8  temp
//  r9  temp
//  r10 temp
//  r11 is preset to be the GPIO base
//  r12 gpio read
//  r13 stack 
//  r14 return address

    DMB_MACRO

checkforvalidcycle:
    LDR     r12, [r11, # GPLEV0_OFFSET]    // this will be slow as we are going off chip

    mov     r9, # NPCFD_MASK + NPCFC_MASK  // Clear event status register ( acknowledge )
    str     r9, [r11, # GPEDS0_OFFSET]

    tst     r12, # CLK1MHZ_MASK    
    beq     waitforclkhigh
                                           // going to be a two cycle access so mask the first one
waitforclklow:
    LDR     r12, [r11, # GPLEV0_OFFSET]    // this will be slow as we are going off chip

    tst     r12, # NPCFD_MASK
    tstne   r12, # NPCFC_MASK
    bne     FIQexit                        // remove glitch

    tst     r12, # CLK1MHZ_MASK
    bne     waitforclklow

waitforclkhigh:
    LDR     r12, [r11, # GPLEV0_OFFSET]    // this will be slow as we are going off chip

    // Address mask includes FC00 which is next to bit 7 of the address bus.
    // it will be low for for FC00 access and high for FD00
    mov     r9, # (ADDRBUS_MASK)>>ADDRBUS_SHIFT
    orr     r9,r9,#NPCFC_MASK>>ADDRBUS_SHIFT

    tst     r12, # NPCFD_MASK
    tstne   r12, # NPCFC_MASK
    bne     FIQexit                        // remove glitch

    tst     r12, # CLK1MHZ_MASK
    beq     waitforclkhigh
    
    and     r10, r9, r12, LSR # ADDRBUS_SHIFT // isolate address bus and fred or jim
    
    tst     r12, # RNW_MASK
    
    ldrneb  r8, [r10, #Pi1MHz_MEM_BASE]    // get byte to write out
    
// stall if writing
                                           // set databus to be outputs
    ldrne   r9, =DATABUS_TO_OUTPUTS | TEST_PINS_OUTPUTS
    orrne   r10, r10, # Pi1MHz_MEM_RNW     // set write flag ready for call back table

    movne   r8, r8, LSL # DATABUS_SHIFT
    strne   r8, [r11, # GPSET0_OFFSET]     // set up data

    strne   r9, [r11, # GPFSEL0_OFFSET]    // set data bus to be outputs

                                           // wait for clock edge
waitforclklow2:    
    LDR     r9, [r11, # GPLEV0_OFFSET]     // this will be slow as we are going off chip

    // setup R8 in stall ready to make Data bus inputs
    mov     r8, # TEST_PINS_OUTPUTS
    
// 1 stall 

    tst     r9, # CLK1MHZ_MASK
    streq   r8, [ r11, # GPFSEL0_OFFSET]   // set data bus to be inputs release databus
    bne     waitforclklow2
    
    mov     r10, r10 , LSL # 2
    ldr     r10, [r10, #Pi1MHz_CB_BASE]    // load call back pointer
    mov     r8, # DATABUS_MASK
    str     r8, [ r11, # GPCLR0_OFFSET]    // Clear outputs
                                           // check for call back
    movs    r10, r10

    beq     checkforvalidcycle             // no callback

    push    {r0-r3, r14}
    mov     r0, r12, LSR # DATABUS_SHIFT
    blx     r10
    pop     {r0-r3, r14}
    B       checkforvalidcycle

FIQexit:
    DMB_MACRO
    subs    pc, lr, #4
