
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

.global _start_vectors

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

_start_vectors:
    B _reset_
    B _undefined_instruction_handler_
    B _swi_handler_
    B _prefetch_abort_handler_
    B _data_abort_handler_
    B _reset_
    B arm_irq_handler  //ldr pc, _interrupt_vector_h

#ifdef FIQ_VECTOR_CODE
    // include FIQ handler at the address directly
    #include "../FIQ.s"
#else
    ldr pc,_fiq_vector_h
    _fiq_vector_h:                  .word   arm_fiq_handler
#endif

arm_irq_handler:
    subs    lr, lr, #4
    push    {r0, r1, r2, r3, ip, lr}
    // Save the caller-saved VFP state as well.  The C this calls does use it:
    // the VDU queue is drained from here, and PLOT (vdu_25) alone compiles to
    // ~155 VFP instructions touching s0 and s12-s18, while the main loop it
    // interrupts uses the same registers for BeebSID's filter and other float
    // code.  Nothing here declares floating point - the compiler reaches for
    // spare VFP registers on its own - so this cannot be kept safe by
    // inspecting the C, and a rebuild can reintroduce it anywhere.
    // d8-d15 are callee-saved and the generated code pushes those itself, so
    // d0-d7 plus FPSCR is the whole of what is at risk.  r1 is pushed purely
    // to keep the stack 8-byte aligned for the call.
    vpush   {d0-d7}
    vmrs    r0, fpscr
    push    {r0, r1}
    bl      IRQHandler_main
    pop     {r0, r1}
    vmsr    fpscr, r0
    vpop    {d0-d7}
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

    // Start from a known MMU state.  At a cold boot the GPU hands over with
    // the MMU and caches already off, so this is a no-op.  After a kernel.now
    // chain-boot it is not: the previous kernel is still using its own page
    // table, whose live copy sits in ITS .noinit - and if the two images place
    // .noinit differently, that table falls inside the range the bss clear
    // below is about to zero.  The clear then wipes the table it is running
    // on, sweeps on undisturbed while the TLB still covers the section being
    // written, and dies the moment it touches an address it has to walk for -
    // a Data Abort at exactly the next 1 MB boundary.  Turning the MMU off
    // first makes the incoming kernel independent of whatever the outgoing one
    // left behind.  (_copyandreboot also hands over with it off now; this is
    // the half that protects us from an older kernel that does not.)
    bl      _early_mmu_off_and_vectors
    nop                                // keep this pair two instructions wide

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


.global _copyandreboot

// we copy our code to below &8000 and then jump to it.
// R0 = source
// R1 = length
_copyandreboot:
    push {r0, r1}
    //ldr r0, =_start_vectors - 0x100
    //adr r1, _copyandreboot_code
    //mov r2, # ((_fast_scroll_end - _copyandreboot_code + 63) & ~63)
    //bl _fast_scroll
    pop {r1, r2}
    ldr r0, =_start_vectors

    //B _start_vectors - 0x100
_copyandreboot_code:
    BL _fast_scroll

    // flush caches and branch predictor to make sure we execute the new code
    mov r0,#0
    mcr p15, 0, r0, c7, c5, 0

// flush BTB icache dcache it might be better to just invalidate a few lines
#if (__ARM_ARCH >= 7 )

    mrc     p15, 1, r0, c0, c0, 1      @ Read CLIDR (Cache Level ID Register)
    mov     r10, #0                    @ Start at Cache Level 0

loop_level:
    add     r2, r10, r10, lsr #1       @ Calculate level shift
    mov     r1, r0, lsr r2             @ Get bits for this level
    and     r1, r1, #7                 @ Mask to get cache type
    cmp     r1, #2                     @ Does it have a data cache?
    blt     next_level                 @ Skip if no data cache

    @ Select this cache level in CSSELR
    mcr     p15, 2, r10, c0, c0, 0     @ Write CSSELR
    isb                                @ Instruction Synchronization Barrier
    mrc     p15, 1, r1, c0, c0, 0      @ Read CCSIDR (Current Cache Size ID)

    @ Extract Way/Set geometry
    and     r2, r1, #7                 @ Line size (log2 words - 2)
    add     r2, r2, #4                 @ Actual line size offset
    ldr     r4, =0x3ff
    and     r4, r4, r1, lsr #3         @ Max Way number
    clz     r5, r4                     @ Way shift (bit position)
    ldr     r7, =0x7fff
    and     r7, r7, r1, lsr #13        @ Max Set number

loop_set:
    mov     r9, r4                     @ Reset Way counter
loop_way:
    @ Combine Way, Set, and Level into a single register for DCCISW
    orr     r11, r10, r9, lsl r5       @ Insert Way
    orr     r11, r11, r7, lsl r2       @ Insert Set
    mcr     p15, 0, r11, c7, c14, 2    @ DCCISW (Clean & Invalidate by Set/Way)
    subs    r9, r9, #1                 @ Decrement Way
    bge     loop_way
    subs    r7, r7, #1                 @ Decrement Set
    bge     loop_set

next_level:
    add     r10, r10, #2               @ Increment Level (Level 0, 2, 4...)
    cmp     r3, r10, lsl #23           @ Compare with LoC
    bgt     loop_level


finished:

   DSB
   mov r0,#0
   MCR p15, 0, r0, c7, c5, 0 // invalidate I&BTB cache

   DSB
   ISB

#else

// Pizero Cache flush code

   // clean and invalidate all the cache
   mov r0,#0
   // Instruction count here is load-bearing: _fast_scroll must keep the
   // address the outgoing kernel calls, so this block cannot grow.  The DSB
   // that would belong here is done in _chainboot_mmu_off instead.
   MCR p15, 0, r0, c7, c14, 0 //; clean+invalidate entire data cache
   MCR p15, 0, r0, c7, c5, 0 //; invalidate I cache and BTB
   MCR p15, 0, r0, c7, c5, 4 // flush prefetch buffer

#endif

// Hand over with the MMU and caches off - see _chainboot_mmu_off.  This must
// stay a single instruction: _copyandreboot_code runs IN PLACE while
// _fast_scroll copies the new image over it, so every byte from here back to
// _copyandreboot_code has to keep the address the outgoing kernel expects.
// Growing this region shifts _fast_scroll and the copy overwrites the loop
// mid-flight; the out-of-line routine lives past _fast_scroll_end instead.
    b _chainboot_mmu_off


// and then restart
.global _fast_scroll
// R0 = dst
// R1 = src
// R2 = number of bytes, must be a multiple of 64
_fast_scroll:
        push    {r4, r5, r6, r8, r9, r10, r11, lr}
        bic     r2, r2, #0x3F
_fast_scroll_loop:
        ldmia   r1!,{r3, r4, r5, r6, r8, r9, r10, r11}
        subs    r2, r2, #64
        stmia   r0!,{r3, r4, r5, r6, r8, r9, r10, r11}
        ldmia   r1!,{r3, r4, r5, r6, r8, r9, r10, r11}
        stmia   r0!,{r3, r4, r5, r6, r8, r9, r10, r11}
        bne     _fast_scroll_loop
        pop     {r4, r5, r6, r8, r9, r10, r11, pc}
_fast_scroll_end:

// Everything below is deliberately placed AFTER _fast_scroll_end.
//
// A kernel.now flash copies the incoming image over the outgoing kernel while
// the outgoing kernel is executing from the region being overwritten -
// _copyandreboot_code runs in place and _fast_scroll copies straight across
// it.  That only survives because the bytes there are the same in both images.
// So nothing may be inserted at or before _fast_scroll_end: doing so shifts
// the copy loop, the outgoing kernel executes whatever landed at its old
// address, and the flash dies with no output at all.  Past _fast_scroll_end
// the copy has already finished, so code here is free to differ.

// Bring the MMU and caches down to the state the GPU hands over in at a cold
// boot.  Then point the vector base at this image.  Called from _reset_ before
// the .bss clear; a no-op on a cold boot, and the thing that saves us on a
// chain-boot - see the note at the call site.
_early_mmu_off_and_vectors:
    mrc     p15, 0, r4, c1, c0, 0
    bic     r4, r4, #0x0001            // M: MMU off
    bic     r4, r4, #0x0004            // C: data cache off
    bic     r4, r4, #0x1000            // I: instruction cache off
    mcr     p15, 0, r4, c1, c0, 0
    mov     r4, #0
    mcr     p15, 0, r4, c8, c7, 0      // invalidate entire TLB
    mcr     p15, 0, r4, c7, c5, 0      // invalidate I-cache and BTB
    mcr     p15, 0, r4, c7, c10, 4     // DSB
    mcr     p15, 0, r4, c7, c5, 4      // flush prefetch buffer
    ldr     r4, =_start_vectors
    mcr     p15, 0, r4, c12, c0, 0     // vector base = start of this image
    bx      lr

// The outgoing half of the same handover: drop the MMU before branching into
// the freshly copied image, so it never runs a single instruction under this
// kernel's page table.  The incoming kernel protects itself as well, but an
// older kernel in the field does not, and this is the side that fixes it for
// the image being replaced.  Reached by the one-instruction branch that ends
// _copyandreboot_code; the caches were cleaned there, so the copy is in RAM
// and not stranded in a line that turning the cache off would discard.
_chainboot_mmu_off:
    mrc     p15, 0, r0, c1, c0, 0
    bic     r0, r0, #0x0001            // M: MMU off
    bic     r0, r0, #0x0004            // C: data cache off
    bic     r0, r0, #0x1000            // I: instruction cache off
    mcr     p15, 0, r0, c1, c0, 0
    mov     r0, #0
    mcr     p15, 0, r0, c8, c7, 0      // invalidate entire TLB
    mcr     p15, 0, r0, c7, c10, 4     // DSB
    mcr     p15, 0, r0, c7, c5, 4      // flush prefetch buffer
    mov     pc, #_start_vectors

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

.ltorg
