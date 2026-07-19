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
	mov	r1, r0			/* r1 = scanning pointer; r0 must survive as base for the length */
	tst	r1, #3
	beq	.Laligned

	/* Misaligned head: at most 3 bytes, scanned one at a time. */
.Lhead:
	ldrb	r2, [r1]
	cmp	r2, #0
	beq	.Ldone
	add	r1, r1, #1
	tst	r1, #3
	bne	.Lhead

.Laligned:
	/* Build the two magic constants in scratch registers only -- this is
	   a leaf function and must not touch r4+ without saving them. */
	mov	r3, #1
	orr	r3, r3, r3, lsl #8
	orr	r3, r3, r3, lsl #16	/* r3 = 0x01010101 */

.Lwordloop:
	ldr	r2, [r1], #4
	sub	ip, r2, r3
	bic	ip, ip, r2
	ands	ip, ip, r3, lsl #7	/* ip &= 0x80808080 */
	beq	.Lwordloop

	/* The word just consumed (now 4 bytes before r1) contains a NUL. */
	sub	r1, r1, #4
.Ltail:
	ldrb	r2, [r1]
	cmp	r2, #0
	addne	r1, r1, #1
	bne	.Ltail

.Ldone:
	sub	r0, r1, r0
	bx	lr

	.size	strlen, . - strlen

#endif
