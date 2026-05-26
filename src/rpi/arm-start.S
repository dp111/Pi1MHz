
//  Part of the Raspberry-Pi Bare Metal Tutorials
//  Copyright (c) 2013-2015, Brian Sidebotham
//  All rights reserved.
//
//  Redistribution and use in source and binary forms, with or without
//  modification, are permitted provided that the following conditions are met:
//
//  1. Redistributions of source code must retain the above copyright notice,
//      this list of conditions and the following disclaimer.
//
//  2. Redistributions in binary form must reproduce the above copyright notice,
//      this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//
//  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
//  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
//  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
//  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
//  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
//  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
//  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
//  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
//  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
//  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
//  POSSIBILITY OF SUCH DAMAGE.

#include "rpi.h"

.equ    STACK_BASE,          arm_stack
.equ    STACK_SIZE,          0x0010000

.equ    C0_SVC_STACK,        STACK_SIZE*1
.equ    C0_IRQ_STACK,        STACK_SIZE*2
.equ    C0_FIQ_STACK,        STACK_SIZE*3
.equ    C0_USER_STACK,       STACK_SIZE*4
.equ    C0_ABORT_STACK,      STACK_SIZE*5
.equ    C0_UNDEFINED_STACK,  STACK_SIZE*6

.equ    SCTLR_ENABLE_BRANCH_PREDICTION, 0x800
.equ    SCTLR_ENABLE_INSTRUCTION_CACHE, 0x1000

.section ".text.startup"

.global _start

//.global _interrupt_vector_h

// From the ARM ARM (Architecture Reference Manual). Make sure you get the
// ARMv5 documentation which includes the ARMv6 documentation which is the
// correct processor type for the Broadcom BCM2835. The ARMv6-M manuals
// available on the ARM website are for Cortex-M parts only and are very
// different.
//
// See ARM section A2.2 (Processor Modes)

.equ    CPSR_MODE_USER,         0x10
.equ    CPSR_MODE_FIQ,          0x11
.equ    CPSR_MODE_IRQ,          0x12
.equ    CPSR_MODE_SVC,          0x13
.equ    CPSR_MODE_ABORT,        0x17
.equ    CPSR_MODE_HYP,          0x1A
.equ    CPSR_MODE_UNDEFINED,    0x1B
.equ    CPSR_MODE_SYSTEM,       0x1F

.equ    CPSR_MODE_MASK,         0x1F

// See ARM section A2.5 (Program status registers)
.equ    CPSR_A_BIT,             0x100
.equ    CPSR_IRQ_INHIBIT,       0x80
.equ    CPSR_FIQ_INHIBIT,       0x40

_start:
    B _reset_
    ldr pc, _undefined_instruction_vector_h
    ldr pc, _software_interrupt_vector_h
    ldr pc, _prefetch_abort_vector_h
    ldr pc, _data_abort_vector_h
    B _reset_
    B arm_irq_handler  //ldr pc, _interrupt_vector_h

#ifdef FIQ_VECTOR_CODE
    // include FIQ handler at the address directly
    #include "../FIQ.s"
#else
    ldr pc,_fiq_vector_h
    _fiq_vector_h:                  .word   arm_fiq_handler
#endif

_interrupt_vector_h:                .word   arm_irq_handler
_undefined_instruction_vector_h:    .word   _undefined_instruction_handler_
_software_interrupt_vector_h:       .word   _swi_handler_
_prefetch_abort_vector_h:           .word   _prefetch_abort_handler_
_data_abort_vector_h:               .word   _data_abort_handler_

arm_irq_handler:
    subs    lr, lr, #4
    push    {r0, r1, r2, r3, ip, lr}
    bl      IRQHandler_main
    ldm     sp!, {r0, r1, r2, r3, ip, pc}^

_reset_:

    mrc     p15,0,r4,c1,c0,0
    // Enable caches and branch prediction
    orr     r4,#SCTLR_ENABLE_BRANCH_PREDICTION + SCTLR_ENABLE_INSTRUCTION_CACHE
    // System Control Register
    mcr     p15,0,r4,c1,c0,0

#if defined(RPI4)
   ldr      r4,=0xff842000
   mov      r9,#0
   str      r9,[r4]        // disable GIC on rpi4
#endif

    // Set the CPACR for access to all coprocessors
    mov     r4, #0x0fffffff
    mcr     p15, 0, r4, c1, c0, 2

    // Enable VFP ------------------------------------------------------------
    // Set the FPEXC EN bit to enable the NEON MPE:
    mov     r4, #0x40000000
    vmsr    fpexc, r4

#if (__ARM_ARCH >= 7 )
    // enter in HYP mode and need to force a switch to SVC mode
    //
    // The logs show:
    // SVC mode: cpsr ends with 1d3
    // HYP mode: cpsr ends with 1a3
    adr     lr , _reset_continue
    msr     spsr_cxsf, #CPSR_IRQ_INHIBIT | CPSR_FIQ_INHIBIT | CPSR_MODE_SVC
    msr     ELR_hyp , lr
    eret
_reset_continue:
#endif

    // We enter execution in supervisor mode. For more information on
    // processor modes see ARM Section A2.2 (Processor Modes)

    adr     r4,_start
    mcr     p15, 0, r4, c12, c0, 0     //set vectors to beginning of this code

    // Initialise Stack Pointers ---------------------------------------------
    ldr     r4,=STACK_BASE

    // We're going to use interrupt mode, so setup the interrupt mode
    // stack pointer which differs to the application stack pointer:
    CPS     #CPSR_MODE_IRQ
    add     sp, r4, #C0_IRQ_STACK

    // Also setup the stack used for FIQs
    CPS     #CPSR_MODE_FIQ
    add     sp, r4, #C0_FIQ_STACK

// import fiq setup from fiq.s this is a macro
#ifdef FIQ_SETUP_CODE
    FIQ_SETUP_M
#endif

    // Also setup the stack used for undefined exceptions
    CPS     #CPSR_MODE_UNDEFINED
    add     sp, r4, #C0_UNDEFINED_STACK

    // Also setup the stack used for prefetch and data abort exceptions
    CPS     #CPSR_MODE_ABORT
    add     sp, r4, #C0_ABORT_STACK

    // Finally, a user/system mode stack, although the application will likely reset this
    CPS     #CPSR_MODE_SYSTEM
    add     sp, r4, #C0_USER_STACK

    // Switch back to supervisor mode (our application mode) and
    // set the stack pointer
    CPS     #CPSR_MODE_SVC
    add     sp, r4, #C0_SVC_STACK

//
//  fast clear BSS loop
//

    ldr     r4,=__bss_start__
    ldr     r5,=__bss_end__

    mov     r6,#0
    mov     r7,#0
    mov     r8,#0
    mov     r9,#0

    bic     r4,r4,#0xF            // 16 byte align

bssclearloop:
    cmp     r4,r5
    stmia   r4!,{r6,r7,r8,r9}
    blt     bssclearloop
    b       kernel_main

.section ".text._exception_handlers_", "x"
_undefined_instruction_handler_:
    stmfd    sp!, {r0-r12, lr}
    adr      r2, undefined_instruction_txt
    b        exception_store_spsr_r1_4

_prefetch_abort_handler_:
    stmfd    sp!, {r0-r12, lr}
    adr      r2, prefetch_abort_txt
    b        exception_store_spsr_r1_4

_data_abort_handler_:
    stmfd    sp!, {r0-r12, lr}
    adr      r2, data_abort_txt
    mov      r1, #8
    b        exception_store_spsr

_swi_handler_:
    stmfd    sp!, {r0-r12, lr}
    adr      r2, unknown_swi_txt

exception_store_spsr_r1_4:
    mov      r1, #4
exception_store_spsr:
    mrs      r0, spsr             // Get spsr.
    stmfd    sp!, {r0}            // Store spsr onto stack.
    mov      r0, sp

    b        dump_info

undefined_instruction_txt:
.ascii "Undefined Instruction\0"

prefetch_abort_txt:
.ascii "Prefetch Abort\0"

data_abort_txt:
.ascii "Data Abort\0"

unknown_swi_txt:
.ascii "SWI\0"

// fast 1MHz page copy.

// Entry
// r0 destination
// r1 source

.section ".text.Pi1MHz_MemoryWritePage", "x"

.global Pi1MHz_MemoryWritePage
.align 5
    nop
    nop
    nop
    nop
    nop
Pi1MHz_MemoryWritePage:
    ldrd r2, [r1], #8 // get data
    push {r4, r5, r6, r7, r14}
    mov r12, r1

    mov r4,     #Pi1MHz_MEM_BASE & 0xFF000000
    orr r4, r4, #Pi1MHz_MEM_BASE & 0x00FF0000
    orr r6, r4, #Pi1MHz_MEM_BASE & 0x0000FF00

    add r5, r0, #0x100 // Pi1Mhz_memory

    mov r7, #0xFF000000
    mov r14, #2
    orr r7, r7, #0xFF00
    add r6, r6, r0, LSL #1
outer_copy_loop:
    mov r4, #256/16 // loop counter
    PLD [r12,#32]
    PLD [r12,#64]
    PLD [r12,#96]
    PLD [r12,#128]
copy_loop:

    subs r4, r4, #1

    strd r2, [r5], #8

    and r0, r2, #0xFF
    mov r1, r2, LSR #24
    orr r0, r0, r2, LSL #8
    orr r2, r7, r2, LSR #16
    orr r0, r0, r7
    orr r1, r2, r1, LSL #16

    str r0, [r6], #4
    str r1, [r6], #4

    and r0, r3, #0xFF
    mov r1, r3, LSR #24
    orr r0, r0, r3, LSL #8
    orr r3, r7, r3, LSR #16

    orr r1, r3, r1, LSL #16

    ldrd r2, [r12], #8 // get data

    orr r0, r0, r7
    str r0, [r6], #4
    str r1, [r6], #4

    bne copy_loop

    subs r14,r14,#1
    bne outer_copy_loop
    pop {r4, r5, r6, r7, pc}

.section ".text._fast_scroll", "x"
.global _fast_scroll
// R0 = dst
// R1 = src
// R2 = number of bytes, must be a multiple of 64
// Note: R7 reserved by Fast 6502 FIQ handler, so avoid
_fast_scroll:
        push    {r4, r5, r6, r8, r9, r10, r11, lr}
        bic     r2, r2, #0x4F
_fast_scroll_loop:
        ldmia   r1!,{r3, r4, r5, r6, r8, r9, r10, r11}
        subs    r2, r2, #64
        stmia   r0!,{r3, r4, r5, r6, r8, r9, r10, r11}
        ldmia   r1!,{r3, r4, r5, r6, r8, r9, r10, r11}
        stmia   r0!,{r3, r4, r5, r6, r8, r9, r10, r11}
        bne     _fast_scroll_loop
        pop     {r4, r5, r6, r8, r9, r10, r11, pc}

.ltorg
