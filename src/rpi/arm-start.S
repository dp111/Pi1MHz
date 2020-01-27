
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

.equ    SCTLR_ENABLE_DATA_CACHE,        0x4
.equ    SCTLR_ENABLE_BRANCH_PREDICTION, 0x800
.equ    SCTLR_ENABLE_INSTRUCTION_CACHE, 0x1000

.section ".text.startup"

.global _start
.global _get_cpsr
.global _get_stack_pointer
.global _set_interrupts
.global _enable_unaligned_access
.global _data_memory_barrier

.global _interrupt_vector_h

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
    ldr pc, _interrupt_vector_h

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

.ltorg

.section ".text.arm_irq_handler"
arm_irq_handler:
    subs    pc, lr, #4

.section ".text._reset_"

_reset_:

    mrc     p15,0,r4,c1,c0,0
    // Enable caches and branch prediction
    orr     r4,#SCTLR_ENABLE_BRANCH_PREDICTION + SCTLR_ENABLE_INSTRUCTION_CACHE
    // System Control Register = R0
    mcr     p15,0,r4,c1,c0,0

    // Set the CPACR for access to CP10 and CP11, and clear the ASEDIS and D32DIS bits:
    ldr     r4, =(0xf << 20)
    mcr     p15, 0, r4, c1, c0, 2

    // Enable VFP ------------------------------------------------------------
    // Set the FPEXC EN bit to enable the NEON MPE:
    mov     r4, #0x40000000
    vmsr    fpexc, r4

#ifdef HAS_MULTICORE

    // enter in HYP mode and need to force a switch to SVC mode
    //
    // The logs show:
    // SVC mode: cpsr ends with 1d3
    // HYP mode: cpsr ends with 1a3
    adr     lr , _reset_continue
    msr     spsr_cxsf, #CPSR_IRQ_INHIBIT | CPSR_FIQ_INHIBIT | CPSR_MODE_SVC
    .word 0xE12EF30E  // msr_elr_hyp lr
    .word 0xE160006E  // eret
_reset_continue:


#endif

    // We enter execution in supervisor mode. For more information on
    // processor modes see ARM Section A2.2 (Processor Modes)

    ldr     r4,=_start
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

// import fiq setup from fiq.s this is a #define
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

    stmia   r4!,{r6,r7,r8,r9}
    bic     r4,r4,#0x3F       // 64 byte align

bssclearloop:
    cmp     r4,r5
    stmia   r4!,{r6,r7,r8,r9}
    blt     bssclearloop
    b       kernel_main

.section ".text._get_stack_pointer"
_get_stack_pointer:
    mov     r0, sp
    mov     pc, lr

.section ".text._get_cpsr"
_get_cpsr:
    mrs     r0, cpsr
    mov     pc, lr

.section ".text._set_interrupts"
_set_interrupts:
    and     r0, r0, #CPSR_IRQ_INHIBIT | CPSR_FIQ_INHIBIT    // extract the IRQ/FIQ bits from the value of cpsr passed in
    mrs     r1, cpsr
    bic     r1, r1, #CPSR_IRQ_INHIBIT | CPSR_FIQ_INHIBIT
    orr     r1, r1, r0
    msr     cpsr_c, r1
    mov     pc, lr

.section ".text._undefined_instruction_handler_"
_undefined_instruction_handler_:
    stmfd    sp!, {r0-r12, lr}
    mrs      r0, spsr             // Get spsr.
    stmfd    sp!, {r0}            // Store spsr onto stack.
    mov      r0, sp
    b       undefined_instruction_handler

.section ".text._prefetch_abort_handler_"
_prefetch_abort_handler_:
    stmfd    sp!, {r0-r12, lr}
    mrs      r0, spsr             // Get spsr.
    stmfd    sp!, {r0}            // Store spsr onto stack.
    mov      r0, sp
    b       prefetch_abort_handler

.section ".text._data_abort_handler_"
_data_abort_handler_:
    stmfd    sp!, {r0-r12, lr}
    mrs      r0, spsr             // Get spsr.
    stmfd    sp!, {r0}            // Store spsr onto stack.
    mov      r0, sp
    b       data_abort_handler

.section ".text._swi_handler_"
_swi_handler_:
    stmfd    sp!, {r0-r12, lr}
    mrs      r0, spsr             // Get spsr.
    stmfd    sp!, {r0}            // Store spsr onto stack.
    mov      r0, sp
    b       swi_handler

.section ".text._enable_unaligned_access"
_enable_unaligned_access:
    mrc      p15, 0, r0, c1, c0, 0   // read SCTLR
    bic      r0, r0, #2              // A (no unaligned access fault)
    orr      r0, r0, #1 << 22        // U (v6 unaligned access model)
    mcr      p15, 0, r0, c1, c0, 0   // write SCTLR
    mov      pc, lr

.section ".text._data_memory_barrier"
_data_memory_barrier:
#if defined(RPI2) || defined(RPI3)
    dmb
#else
    mov      r0, #0
    mcr      p15, 0, r0, c7, c10, 5
#endif
    mov      pc, lr
