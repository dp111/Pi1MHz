/* memset for AArch32, tuned for the Raspberry Pi bare-metal targets.
 *
 * Copyright (c) 2026 Pi1MHz contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * newlib has no memset.S in libc/machine/arm at all -- only the __aeabi_memset
 * variants, which live under a different symbol and take their arguments in a
 * different order (dest, n, c). So the plain memset in libc is a byte-at-a-time
 * loop on every target we build. It is also our single most-called function
 * (~500 call sites), which makes it the cheapest large win available.
 *
 * This is deliberately architecture-neutral: plain ARM integer instructions
 * only, no NEON, no VFP, no ARMv6+ opcodes. One file serves ARM1176 and
 * ARMv8-A alike, so there is nothing to gate and nothing to keep in sync.
 * On ARM1176 an STM-based fill measures ~1458 MB/s (tinymembench, BCM2835)
 * because stores retire into the write buffer without a read to stall on.
 *
 * Assembled as ARM state and returns with bx, so it is interworking-safe even
 * if a caller is built for Thumb.
 *
 * void *memset(void *s, int c, size_t n);   returns s
 */

	.syntax unified
	.arm
	.text
	.align	2
	.global	memset
	.type	memset, %function

memset:
	mov	ip, r0			/* working pointer; r0 must survive as the return value */
	and	r1, r1, #255		/* only the low byte of c is used */
	cmp	r2, #8
	blo	.Lbytes			/* tiny fills are not worth any setup */

	/* Replicate the fill byte across a whole word. */
	orr	r1, r1, r1, lsl #8
	orr	r1, r1, r1, lsl #16

	/* Bring the destination up to a word boundary. r2 >= 8 here, and at
	   most 3 bytes are consumed, so r2 cannot underflow. */
	rsb	r3, ip, #0
	ands	r3, r3, #3
	beq	.Laligned
	sub	r2, r2, r3
.Lhead:
	strb	r1, [ip], #1
	subs	r3, r3, #1
	bne	.Lhead

.Laligned:
	cmp	r2, #32
	blo	.Lwords			/* not worth spilling registers for */

	/* Bulk fill, 32 bytes per iteration -- one ARM1176 cache line.
	   r1 and r3-r9 are eight registers all holding the replicated value;
	   stm stores them in register order, which does not matter here. */
	push	{r4-r9}
	mov	r3, r1
	mov	r4, r1
	mov	r5, r1
	mov	r6, r1
	mov	r7, r1
	mov	r8, r1
	mov	r9, r1
.Lbig:
	subs	r2, r2, #32
	stmia	ip!, {r1, r3, r4, r5, r6, r7, r8, r9}
	cmp	r2, #32
	bhs	.Lbig
	pop	{r4-r9}

.Lwords:
	cmp	r2, #4
	blo	.Lbytes
.Lwordloop:
	str	r1, [ip], #4
	sub	r2, r2, #4
	cmp	r2, #4
	bhs	.Lwordloop

	/* 0..3 bytes left. Reached directly for tiny fills too, where r1 has
	   not been replicated -- harmless, since strb only uses the low byte. */
.Lbytes:
	cmp	r2, #0
	bxeq	lr
.Lbyteloop:
	strb	r1, [ip], #1
	subs	r2, r2, #1
	bne	.Lbyteloop
	bx	lr

	.size	memset, . - memset
