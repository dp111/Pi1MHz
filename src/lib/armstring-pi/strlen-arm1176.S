/* strlen for ARM1176 (ARMv6).
 *
 * Copyright (c) 2026 Pi1MHz contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * newlib's strlen.S dispatch requires Thumb-2 + DSP or ARMv7-A, so ARM1176
 * (ARMv6, no Thumb-2) falls through to a 14-byte byte loop for our single
 * hottest read-only string call (~300 call sites). arm-mem has no ARMv6
 * strlen either, so this is written from scratch.
 *
 * Standard word-at-a-time zero-byte test: for a word w,
 *   (w - 0x01010101) & ~w & 0x80808080
 * is non-zero iff w contains a byte equal to 0x00. 0x01010101 and 0x80808080
 * are built with shift/orr (no ARM immediate encodes a repeated byte
 * pattern), which keeps this to plain ARMv6 integer opcodes -- no NEON, no
 * Thumb-2, no literal pool.
 *
 * Reads up to 3 bytes past the terminating NUL once word-aligned, which is
 * the same assumption every mainstream libc's word-at-a-time strlen makes.
 *
 * On every other target this file emits nothing; newlib's NEON+Thumb-2
 * strlen.S (lib/armstring/strlen.S) supplies ARMv7-A instead. Exactly one of
 * the two ever defines the symbol.
 *
 * size_t strlen(const char *s);
 */

	.syntax unified
	.arm

#if __ARM_ARCH == 6

	.text
	.align	2
	.global	strlen
	.type	strlen, %function

strlen:
	/* Build the magic constant in scratch registers only -- this is a leaf
	   function and must not touch r4+ without saving them. It is hoisted
	   above the alignment test on purpose: each `orr` reads r3 as a SHIFTED
	   operand, which on ARM1176 is an Early-Reg read costing 2 cycles rather
	   than 1 (silicon-confirmed, v12 timing suite rows 4-9), so the chain is
	   ~5 cycles and wants the head test overlapping it. It cannot be made
	   shorter: no ARM immediate encodes a repeated byte pattern, and ARM1176
	   is ARMv6 with no movw.

	   The interleave below is exact, and the order matters: each `orr` needs
	   ONE independent instruction after its producer to cover the 2-cycle
	   Early-Reg latency, so `mov r1, r0` covers the first and `tst` covers
	   the second. Putting both `orr`s together costs a stall. The `beq`
	   cannot move above the second `orr` -- the aligned path branches
	   straight into the word loop and needs r3 fully built. `orr` does not
	   set flags, so sitting between the `tst` and the `beq` is harmless. */
	mov	r3, #1
	mov	r1, r0			/* r1 = scanning pointer; r0 must survive as base for the length */
	orr	r3, r3, r3, lsl #8
	tst	r1, #3
	orr	r3, r3, r3, lsl #16	/* r3 = 0x01010101 */
	beq	.Lwordloop

	/* Misaligned head: at most 3 bytes, scanned one at a time. */
.Lhead:
	ldrb	r2, [r1]
	cmp	r2, #0
	beq	.Ldone
	add	r1, r1, #1
	tst	r1, #3
	bne	.Lhead

.Lwordloop:
	ldr	r2, [r1], #4
	sub	ip, r2, r3
	bic	ip, ip, r2
	ands	ip, ip, r3, lsl #7	/* ip &= 0x80808080 */
	beq	.Lwordloop

	/* The word just consumed (now 4 bytes before r1) contains a NUL, and ip
	   holds 0x80 in the lanes that were zero. The LOWEST set lane is always
	   the first NUL: a borrow out of the subtraction only ever propagates
	   upward, and the `bic` above already removed the false hits in lanes
	   whose own byte was >= 0x81. Little-endian, so lane 0 is the lowest
	   address.

	   Testing the lanes beats re-reading the bytes: the old byte loop paid
	   the 3-cycle load-use latency up to four times plus a mispredict on
	   exit. The chain below self-terminates -- once one `tst` yields NE the
	   remaining `tsteq`s do not execute, so the flags stay NE and no further
	   `addeq` fires. */
	sub	r1, r1, #4
	tst	ip, #0x000000FF
	addeq	r1, r1, #1
	tsteq	ip, #0x0000FF00
	addeq	r1, r1, #1
	tsteq	ip, #0x00FF0000
	addeq	r1, r1, #1

.Ldone:
	sub	r0, r1, r0
	bx	lr

	.size	strlen, . - strlen

#endif
