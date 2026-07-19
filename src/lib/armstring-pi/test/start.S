/* Test-harness startup. FOR TESTING ONLY -- not firmware code.
 *
 * The MMU has to be on before the string routines can be tested. With the MMU
 * off, all memory is Strongly Ordered, and unaligned accesses to Strongly
 * Ordered memory always fault regardless of SCTLR.A / SCTLR.U. arm-mem's memcpy
 * deliberately does unaligned accesses -- correct, since the real firmware
 * enables them -- so it data-aborts under an MMU-off harness.
 *
 * Flat identity map, 4096 x 1MB sections, Normal write-back, domain 0 as
 * manager. No permission checks, no exception handling, no device/normal
 * distinction -- none of which matters for a test that only touches RAM and
 * talks to the host via semihosting.
 */

	.syntax unified
	.arm
	.section .text.start,"ax"
	.global _start

	/* section descriptor: AP=0b11 (full access), TEX=0, C=1, B=1, type=section */
	.equ	SECTION_ATTR, 0xC0E

_start:
	/* Build the identity map. The MMU is still off, so these stores go
	   straight to physical memory. */
	ldr	r0, =_page_table
	ldr	r2, =SECTION_ATTR
	mov	r1, #0
1:	orr	r3, r2, r1, lsl #20		/* descriptor for section r1 */
	str	r3, [r0, r1, lsl #2]
	add	r1, r1, #1
	cmp	r1, #4096
	blo	1b

	mov	r1, #0
	mvn	r3, #0
	mcr	p15, 0, r3, c3, c0, 0		/* DACR: all domains manager */
	ldr	r0, =_page_table
	mcr	p15, 0, r0, c2, c0, 0		/* TTBR0 */
	mcr	p15, 0, r1, c2, c0, 2		/* TTBCR = 0: always use TTBR0 */

	mcr	p15, 0, r1, c8, c7, 0		/* invalidate unified TLB */
	mcr	p15, 0, r1, c7, c5, 0		/* invalidate instruction cache */

	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #(1 << 1)		/* A = 0: no alignment faults */
	orr	r0, r0, #(1 << 22)		/* U = 1: unaligned access    */
	orr	r0, r0, #(1 << 0)		/* M = 1: MMU on              */
	orr	r0, r0, #(1 << 2)		/* C = 1: D-cache             */
	orr	r0, r0, #(1 << 12)		/* I = 1: I-cache             */
	mcr	p15, 0, r0, c1, c0, 0
	mcr	p15, 0, r1, c7, c5, 4		/* prefetch flush (ISB)       */

	ldr	sp, =_stack_top

	/* zero .bss so a restart cannot accumulate counters */
	ldr	r0, =__bss_start
	ldr	r1, =__bss_end
	mov	r2, #0
2:	cmp	r0, r1
	strlo	r2, [r0], #4
	blo	2b

	bl	main
	b	.
