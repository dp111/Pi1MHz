
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
#include "lowmem.h"

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
#if (__ARM_ARCH < 7)
    // VFP RunFast (FPSCR.DN | FPSCR.FZ, no traps).  In the reset IEEE mode the
    // ARM1176's VFP11 hands subnormal results to "support code" through the
    // Undefined vector - and there is none, so a BeebSID filter decaying
    // towards zero crashed V1.34 on the Pi Zero/1.  The A53 handles them.
    mov     r4, #0x03000000
    vmsr    fpscr, r4
#endif

#if (__ARM_ARCH >= 7 )
    // The firmware enters a cold boot in HYP mode and we need SVC.  A
    // kernel.now chain-boot arrives from the outgoing kernel's main loop,
    // already in SVC, and "msr ELR_hyp" / "eret" executed outside Hyp mode
    // are not defined - on the A53 that killed every chain-boot between the
    // jump and the first line of the new kernel (2026-09-11).  So: only
    // leave Hyp mode when actually in it.
    //
    // The logs show:
    // SVC mode: cpsr ends with 1d3
    // HYP mode: cpsr ends with 1a3
    mrs     r4, cpsr
    and     r4, r4, #CPSR_MODE_MASK
    cmp     r4, #CPSR_MODE_HYP
    bne     _reset_continue
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
    // chain-boot from an older kernel it may not be: such a kernel can jump
    // here still using its own page table, whose live copy sat in ITS .noinit
    // - and if the two images place .noinit differently, that table falls
    // inside the range the bss clear below is about to zero.  The clear then
    // wipes the table it is running on, sweeps on undisturbed while the TLB
    // still covers the section being written, and dies the moment it touches
    // an address it has to walk for - a Data Abort at exactly the next 1 MB
    // boundary.  Turning the MMU off first makes the incoming kernel
    // independent of whatever the outgoing one left behind.  (The copier at
    // LOWMEM_CHAINBOOT_COPIER hands over with it off; this is the half that
    // protects us from a kernel that does not.)
    bl      _early_mmu_off_and_vectors

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

// kernel.now: restart into the image at r0, r1 bytes long (a non-zero
// multiple of 64 - chainboot_request pads it).  Called from chainboot_poll
// with interrupts off, the audio DMA stopped and the D-cache already cleaned
// and turned off (disable_data_cache), so every store here goes straight to
// RAM.  Never returns.
//
// The image goes where this kernel runs, so the copy cannot run from this
// kernel: it would be writing over its own loop.  The copier below is first
// copied to its own page under the kernel, LOWMEM_CHAINBOOT_COPIER, and runs
// from there.  Nothing it uses lies in the copy's path - it has no stack, the
// page table it translates through is at LOWMEM_PAGE_TABLE, and the copy only
// writes from LOWMEM_KERNEL_BASE up - so the hand-over no longer depends on
// the incoming image's layout or size (up to CHAINBOOT_MAX_IMAGE) at all.
_copyandreboot:
    ldr     r2, =_start_vectors            // where the image goes: where we run
    adr     r3, _chainboot_copier
    adr     r4, _chainboot_copier_end
    mov     r5, #LOWMEM_CHAINBOOT_COPIER
1:
    ldr     r6, [r3], #4
    str     r6, [r5], #4
    cmp     r3, r4
    blo     1b

    // Freshly written code is about to run.  The D-cache is off and was
    // cleaned and invalidated (both levels on the A53) by disable_data_cache,
    // so the copier is already in RAM; what is left is the instruction side:
    // drain the stores, then drop any stale I-cache line, branch prediction
    // and prefetched instruction for that page.
    mov     r6, #0
#if (__ARM_ARCH >= 7)
    dsb
    mcr     p15, 0, r6, c7, c5, 0          // ICIALLU: invalidate I-cache
    mcr     p15, 0, r6, c7, c5, 6          // BPIALL: invalidate branch predictor
    dsb
    isb
#else
    mcr     p15, 0, r6, c7, c10, 4         // DSB: drain the write buffer
    // ARM1176 erratum 411920: Invalidate Entire Instruction Cache can miss
    // lines.  The workaround is four in a row with interrupts off (they are),
    // then at least 11 NOPs.
    mcr     p15, 0, r6, c7, c5, 0
    mcr     p15, 0, r6, c7, c5, 0
    mcr     p15, 0, r6, c7, c5, 0
    mcr     p15, 0, r6, c7, c5, 0
    .rept   11
    mov     r0, r0                         // a real NOP, not the v6K hint
    .endr
    mcr     p15, 0, r6, c7, c5, 6          // flush the branch target cache
    mcr     p15, 0, r6, c7, c10, 4         // DSB
    mcr     p15, 0, r6, c7, c5, 4          // flush prefetch buffer (ISB)
#endif
    mov     r5, #LOWMEM_CHAINBOOT_COPIER
    bx      r5                             // r0 = src, r1 = length, r2 = dst

// The copier: runs at LOWMEM_CHAINBOOT_COPIER, so it must be position
// independent - relative branches inside itself only, no literal pool, no
// symbol addresses - and must not touch the stack, which is in the copy's
// path (arm_stack is in .noinit).
//
// The ascending copy is safe although source and destination can overlap:
// the source is a heap buffer, and the heap starts at this kernel's _end, so
// src > dst always.  Each block is read before the (lower) block it lands
// on is written, so an overlap only ever overwrites bytes already copied.
//
// In:  r0 = source, r1 = length (a non-zero multiple of 64), r2 = destination
//      and entry point of the incoming image.
_chainboot_copier:
    mov     r12, r2                        // the entry point, kept for the jump
2:
    ldmia   r0!, {r3-r10}
    stmia   r2!, {r3-r10}
    ldmia   r0!, {r3-r10}
    stmia   r2!, {r3-r10}
    subs    r1, r1, #64
    bhi     2b                             // stops at 0, and can never wrap

#if (__ARM_ARCH >= 7)
    // Invalidate - never clean - every data/unified cache level by set/way,
    // so the new kernel starts with nothing cached.  disable_data_cache
    // cleaned everything before the cache went off; a clean here could only
    // write some line left over from before the copy back over the image.
    mrc     p15, 1, r0, c0, c0, 1          // CLIDR
    and     r3, r0, #0x07000000            // LoC, bits 26:24
    mov     r10, #0                        // cache level 0 (x2)
3:
    add     r2, r10, r10, lsr #1           // 3 x level: this level's CLIDR field
    mov     r1, r0, lsr r2
    and     r1, r1, #7                     // cache type at this level
    cmp     r1, #2
    blt     6f                             // no data cache here
    mcr     p15, 2, r10, c0, c0, 0         // CSSELR: select this level
    isb
    mrc     p15, 1, r1, c0, c0, 0          // CCSIDR
    and     r2, r1, #7                     // log2(line length) - 4
    add     r2, r2, #4                     // set shift
    movw    r4, #0x3ff
    and     r4, r4, r1, lsr #3             // max way number
    clz     r5, r4                         // way shift
    movw    r7, #0x7fff
    and     r7, r7, r1, lsr #13            // max set number
4:
    mov     r9, r4                         // ways, from the top
5:
    orr     r11, r10, r9, lsl r5
    orr     r11, r11, r7, lsl r2
    mcr     p15, 0, r11, c7, c6, 2         // DCISW
    subs    r9, r9, #1
    bge     5b
    subs    r7, r7, #1
    bge     4b
6:
    add     r10, r10, #2
    cmp     r3, r10, lsl #23               // compare with LoC
    bgt     3b
    dsb
#else
    // Invalidate, never clean: see the ARMv7 note above.
    mov     r0, #0
    mcr     p15, 0, r0, c7, c6, 0          // invalidate entire D-cache
    mcr     p15, 0, r0, c7, c10, 4         // DSB
#endif

    // Hand over in the state the GPU leaves a cold boot in: MMU and caches
    // off, nothing stale in the TLB, I-cache or branch predictor.  This page
    // is identity mapped, so turning the MMU off under it is seamless.
    mrc     p15, 0, r0, c1, c0, 0
    bic     r0, r0, #0x0001                // M: MMU off
    bic     r0, r0, #0x0004                // C: data cache off
    bic     r0, r0, #0x1000                // I: instruction cache off
    mcr     p15, 0, r0, c1, c0, 0
    mov     r0, #0
#if (__ARM_ARCH >= 7)
    isb
    mcr     p15, 0, r0, c8, c7, 0          // TLBIALL
    mcr     p15, 0, r0, c7, c5, 0          // ICIALLU
    mcr     p15, 0, r0, c7, c5, 6          // BPIALL
    dsb
    isb
#else
    mcr     p15, 0, r0, c7, c5, 4          // flush prefetch buffer
    mcr     p15, 0, r0, c8, c7, 0          // invalidate entire TLB
    mcr     p15, 0, r0, c7, c5, 0          // invalidate I-cache (now off)
    mcr     p15, 0, r0, c7, c5, 6          // flush the branch target cache
    mcr     p15, 0, r0, c7, c10, 4         // DSB
    mcr     p15, 0, r0, c7, c5, 4          // flush prefetch buffer
#endif
    // A register branch to the image's own start.  Never "mov pc, #symbol":
    // a MOV immediate cannot carry a symbol, and it once assembled as
    // "mov pc, #0" - a jump into the firmware's ARM stub, which killed every
    // chain-boot on the Pi 3 family (Zero 2 W, 2026-09-11).
    bx      r12
_chainboot_copier_end:

.if (_chainboot_copier_end - _chainboot_copier) > LOWMEM_CHAINBOOT_COPIER_SIZE
.error "the kernel.now copier no longer fits its page at LOWMEM_CHAINBOOT_COPIER"
.endif

.global _fast_scroll
// Block copy, used by the framebuffer's scroll (screen_modes.c).
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

// Bring the MMU and caches down to the state the GPU hands over in at a cold
// boot.  Then point the vector base at this image.  Called from _reset_ before
// the .bss clear; a no-op on a cold boot, and the thing that saves us on a
// chain-boot from an older kernel - see the note at the call site.
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
