/* memset for AArch32, tuned for the Raspberry Pi bare-metal targets.
 *
 * Copyright (c) 2026 Pi1MHz contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The head/tail/bulk structure follows arm-mem's memset-v6l.S (3-clause BSD,
 * (c) Raspberry Pi Foundation and RISC OS Open Ltd -- see arm-mem/README.md),
 * with two deliberate departures documented at the bottom of this comment.
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
 * Three properties are load-bearing; do not "simplify" any of them away:
 *
 * 1. The head and tail are BRANCHLESS. Sizes below ~32 bytes dominate the call
 *    sites, and an ARM1176 branch mispredict is 5-7 cycles -- more than the
 *    stores themselves. `movs Rd, Rn, lsl #31` lands bit0 in N and bit1 in C,
 *    so one instruction yields two alignment decisions; `lsl #29` does the same
 *    for bits 2 and 3. Everything then falls out as conditional stores.
 *
 * 2. Alignment is to 16 bytes, not 4. A 16-byte STM at 16-byte alignment lies
 *    wholly inside one 32-byte ARM1176 cache line, so bulk stores never
 *    straddle a line and never provoke two partial-line write allocations.
 *
 * 3. It returns `bx lr`, NOT `pop {pc}`. The v11 hardware timing suite measured
 *    pop{pc} at ~13.13 cycles against ~9.13 for a register return: load-to-PC
 *    defeats the return-stack predictor and costs +4. arm-mem's original ends
 *    `pop {S, pc}`; that is the first departure. The second is that r0 is never
 *    used as the working pointer (ip is), so unlike arm-mem we need not push
 *    and reload S just to return it -- only r4 and lr are saved.
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
	and	r1, r1, #255		/* only the low byte of c is used */
	mov	ip, r0			/* working pointer; r0 must survive as the return value */
	orr	r1, r1, r1, lsl #8
	cmp	r2, #31
	orr	r1, r1, r1, lsl #16	/* r1 = c replicated across a word */
	mov	r3, r1
	blo	.Lshort			/* too small to reach the 16-byte loop */

	push	{r4, lr}
	sub	r2, r2, #16		/* bias by one iteration; simplifies the loop test */

	/* Bring ip up to a 16-byte boundary, branchlessly. r4 and lr are still
	   free as scratch here -- they are not loaded with the fill pattern
	   until .Lsetup, which is why that setup is delayed.

	   The whole leading run is (-ip) & 15, so r2 is adjusted ONCE rather
	   than once per conditional store, and the same `ands` doubles as the
	   already-aligned test. r2 >= 15 on entry and r4 <= 15, so it cannot go
	   negative. Storing 1, then 2, then 4, then 8 keeps every access
	   naturally aligned. */
	rsb	r4, ip, #0
	ands	r4, r4, #15		/* leading bytes until aligned; Z if none */
	beq	.Lsetup
	sub	r2, r2, r4
	movs	lr, r4, lsl #31		/* N = bit0 (1 byte), C = bit1 (2 bytes) */
	strbmi	r1, [ip], #1
	strhcs	r1, [ip], #2
	movs	lr, r4, lsl #29		/* N = bit2 (4 bytes), C = bit3 (8 bytes) */
	strmi	r1, [ip], #4
	stmiacs	ip!, {r1, r3}

.Lsetup:
	mov	r4, r1
	mov	lr, r1
.Lbulk:
	stmia	ip!, {r1, r3, r4, lr}
	subs	r2, r2, #16
	bhs	.Lbulk
	pop	{r4, lr}

	/* 0..15 bytes left. r2 still carries the -16 bias, but 16 has no bits
	   below bit 4, so r2's low nibble is the true remainder either way --
	   which is what lets .Lshort share this tail. */
.Ltail:
	movs	r2, r2, lsl #29		/* N = bit2 (4), C = bit3 (8) */
	stmiacs	ip!, {r1, r3}
	strmi	r1, [ip], #4
	movs	r2, r2, lsl #2		/* N = bit0 (1), C = bit1 (2) */
	strhcs	r1, [ip], #2
	strbmi	r1, [ip]
	bx	lr

.Lshort:
	/* n <= 30, so at most one 16-byte block plus the shared tail, and no
	   callee-saved register is needed. Only word alignment is worth
	   reaching at this size. */
	tst	ip, #3
	beq	.Lshort_aligned
.Lshort_head:
	subs	r2, r2, #1
	bxlo	lr			/* n ran out before we reached alignment */
	strb	r1, [ip], #1
	tst	ip, #3
	bne	.Lshort_head
.Lshort_aligned:
	tst	r2, #16
	stmiane	ip!, {r1, r3}
	stmiane	ip!, {r1, r3}
	b	.Ltail

	.size	memset, . - memset
