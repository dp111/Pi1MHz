    .syntax unified
    .arch   armv6

/*
 * Fast 2-D IDCT for ARM1176JZF-S — pipeline-scheduled, LDRD-optimised
 *
 * void jpeg_idct_ifast(int16_t *coef, uint8_t *output, int stride);
 *
 *   coef   – 64 x int16, dequantised (destroyed)
 *   output – 8x8 uint8 pixels (+128 bias, clamped 0..255)
 *   stride – byte distance between output rows
 *
 * ARM1176 pipeline hazards observed:
 *   MUL/MLA:  result gap 3, Rm/Rs read 1 early
 *   SMULxy/SMLAxy/SMUAD/SMUSDX: result gap 2, Rm/Rs read 1 early
 *   PKHBT/PKHTB: result gap 0, Rm (last reg) read 1 early
 *   LDR/LDRSH: result gap 2 (Dcache hit)
 *   LDRD:      result gap 2 (loads 2 words in 1 cycle)
 *   Shifted-reg operand (incl. ASR/LSL Rd,Rm,#imm): Rm read 1 early
 *   usat: result gap 1, Rm read 1 early
 *
 *   "read 1 early" means: if producer has gap N for normal consumer,
 *   an early-read consumer needs gap N+1.
 *
 * Approach: row IDCT reads int16, writes int32 to a 256-byte stack
 * workspace. Column IDCT reads int32 from workspace, writes uint8
 * pixels to the output buffer. This avoids in-place widening issues.
 *
 * LDRD loads constant pairs from an 8-byte-aligned table into
 * even/odd register pairs (r0/r1, r2/r3).
 *
 * Chen-Wang constants (x2048*sqrt(2)):
 *   W1=2841 W2=2676 W3=2408 W5=1609 W6=1108 W7=565
 *
 * Callee-saved framework registers:
 *   r9  = const_table pointer
 *   r10 = source pointer (int16 coef for row pass, workspace for col)
 *   r11 = output base pointer (preserved during row pass)
 *
 * Stack frame layout (after prologue):
 *   [sp+0   .. sp+255] = int32 workspace (8 rows x 8 cols x 4 bytes)
 *   [sp+256]           = stride
 *   [sp+260 .. sp+295] = saved {r4-r11, lr}
 *
 * Constant table layout (8-byte aligned for LDRD):
 *   [0]+[4]   = CW7, CW1mW7      → ldrd r0,r1,[r9,#0]
 *   [8]+[12]  = CW1pW7, CW3      → ldrd r0,r1,[r9,#8]
 *   [16]+[20] = CW3mW5, CW3pW5   → ldrd r0,r1,[r9,#16]
 *   [24]+[28] = CW6, CW2pW6      → ldrd r0,r1,[r9,#24]
 *   [32]+[36] = CW2mW6, C181     → ldrd r0,r1,[r9,#32]
 */

    .equ CW7,     565
    .equ CW1mW7,  2276
    .equ CW1pW7,  3406
    .equ CW3,     2408
    .equ CW3mW5,  799
    .equ CW3pW5,  4017
    .equ CW6,     1108
    .equ CW2pW6,  3784
    .equ CW2mW6,  1568

    .text
    .align  3                         @ 8-byte aligned for LDRD
const_table:
    .word   CW7           @ [0]
    .word   CW1mW7        @ [4]
    .word   CW1pW7        @ [8]
    .word   CW3           @ [12]
    .word   CW3mW5        @ [16]
    .word   CW3pW5        @ [20]
    .word   CW6           @ [24]
    .word   CW2pW6        @ [28]
    .word   CW2mW6        @ [32]
    .word   181           @ [36]

    .global jpeg_idct_ifast
    .type   jpeg_idct_ifast, %function
    .func   jpeg_idct_ifast
jpeg_idct_ifast:
    stmdb   sp!, {r4-r11, lr}
    sub     sp, sp, #260              @ workspace(256) + stride(4)
    str     r2, [sp, #256]            @ save stride

    mov     r10, r0                    @ r10 = coef ptr (int16 source)
    mov     r11, r1                    @ r11 = output ptr (saved for col pass)
    adr     r9, const_table

    /* ============================================================= */
    /*  ROW PASS: 8 rows, int16 -> int32                             */
    /* ============================================================= */
    mov     r0, sp                     @ r0 = workspace write ptr
    mov     r8, #8                     @ row counter

.Lrow_loop:
    /* Load 8 int16 coefficients (ascending address order) */
    ldrsh   r12, [r10, #0]             @ d0
    ldrsh   r4, [r10, #2]              @ d1
    ldrsh   r3, [r10, #4]              @ d2
    ldrsh   r7, [r10, #6]              @ d3
    ldrsh   r1, [r10, #8]              @ d4
    ldrsh   r6, [r10, #10]             @ d5
    ldrsh   r2, [r10, #12]             @ d6
    ldrsh   r5, [r10, #14]             @ d7

    /* Zero-AC shortcut (r2,r5 last — loaded most recently) */
    orr     r14, r4, r7
    orr     r14, r14, r1
    orr     r14, r14, r3
    orr     r14, r14, r6
    orr     r14, r14, r2
    orrs    r14, r14, r5
    beq     .Lrow_dc

    /* Save workspace ptr (r0) and counter (r8) — needed for scratch */
    stmdb   sp!, {r0, r8}

    /* Register map: r12=d0 r1=d4 r2=d6 r3=d2 r4=d1 r5=d7 r6=d5 r7=d3 */

    /* --- Even setup --- */
    lsl     r12, r12, #11             @ d0 << 11
    add     r12, r12, #128            @ x0 = (d0<<11)+128
    add     r14, r12, r1, lsl #11    @ e8 = x0 + (d4<<11)
    sub     r12, r12, r1, lsl #11    @ e0 = x0 - (d4<<11)
    stmdb   sp!, {r12, r14}           @ push {e0, e8}
    /* r12, r14 now free */

    /* --- W7 butterfly: d1=r4, d7=r5 ---
     * W7sum = CW7*(d1+d7)
     * o4 = W7sum + CW1mW7*d1
     * o5 = W7sum - CW1pW7*d7
     */
    ldrd    r0, r1, [r9, #0]          @ r0=CW7, r1=CW1mW7
    add     r8, r4, r5                @ sum17                       (LDRD gap 1)
    nop                                @                             (LDRD gap 2)
    nop                                @ (MUL reads r0 early: LDRD→MUL gap 3)
    mul     r12, r0, r8               @ W7sum = CW7*sum17           [+3]
    add     r8, r6, r7                @ sum53 (for W3 later)         [1]
    nop                                @                              [2]
    nop                                @                              [3]
    nop                                @ (smlabb reads r12 early → MUL→early gap 4)

    smlabb  r4, r1, r4, r12           @ o4 = W7sum + CW1mW7*d1      [+2]
    ldrd    r0, r1, [r9, #8]          @ r0=CW1pW7, r1=CW3          [1]
    nop                                @                              [2]
    nop                                @ (LDRD→smulbb early-read 3)
    /* r4 ready; r0,r1 ready for early-read */

    smulbb  r14, r0, r5               @ CW1pW7 * d7                 [+2]
    nop                                @                              [1]
    nop                                @                              [2]
    /* r14 ready */
    sub     r5, r12, r14              @ o5 = W7sum - CW1pW7*d7

    /* Live: r4=o4 r5=o5 r6=d5 r7=d3 r2=d6 r3=d2 r1=CW3 r8=sum53 */
    /* Free: r0 r12 r14 */

    /* --- W3 butterfly: d5=r6, d3=r7 ---
     * W3sum = CW3*(d5+d3)          r1=CW3, r8=sum53
     * o6 = W3sum - CW3mW5*d5
     * o7 = W3sum - CW3pW5*d3
     */
    mul     r12, r1, r8               @ W3sum = CW3*sum53           [+3]
    ldrd    r0, r1, [r9, #16]         @ r0=CW3mW5, r1=CW3pW5      [1]
    nop                                @                              [2]
    nop                                @ (MUL gap 3; LDRD gap 2)     [3]
    nop                                @ (LDRD→smulbb early-read 3)
    smulbb  r14, r0, r6               @ CW3mW5 * d5                 [+2]
    nop                                @                              [1]
    nop                                @                              [2]
    /* r12 ready (gap 7 from mul); r14 ready (gap 2 from smulbb) */
    sub     r6, r12, r14              @ o6 = W3sum - CW3mW5*d5

    smulbb  r14, r1, r7               @ CW3pW5 * d3                 [+2]
    add     r8, r3, r2                @ sum26 = d2+d6 (for W6)      [1]
    nop                                @ (smulbb gap 2, LDRD follows)
    /* r14 ready */
    sub     r7, r12, r14              @ o7 = W3sum - CW3pW5*d3
    ldrd    r0, r1, [r9, #24]         @ r0=CW6, r1=CW2pW6

    /* Live: r4=o4 r5=o5 r6=o6 r7=o7 r2=d6 r3=d2 */
    /* Free: r12 r14 r8=sum26 r0=CW6 r1=CW2pW6 */

    /* --- W6 butterfly: d6=r2, d2=r3 ---
     * W6sum = CW6*(d2+d6)   r0=CW6, r1=CW2pW6, r8=sum26
     * e2 = W6sum - CW2pW6*d6
     * e3 = W6sum + CW2mW6*d2
     */
    nop                                @ (LDRD gap 1)
    nop                                @ (LDRD gap 2, MUL reads early → LDRD→MUL gap 3)
    mul     r12, r0, r8               @ W6sum = CW6*sum26           [+3]
    smulbb  r14, r1, r2               @ CW2pW6 * d6                 [+2]
    ldrd    r0, r1, [r9, #32]         @ r0=CW2mW6, r1=C181
    /* --- Stage 2 odd butterflies (fill mul/ldrd gaps) --- */
    add     r8, r4, r6                @ x1 = o4 + o6
    sub     r4, r4, r6                @ o4' = o4 - o6
    add     r6, r5, r7                @ x6 = o5 + o7
    sub     r5, r5, r7                @ o5' = o5 - o7
    /* r12(6), r14(5), r0/r1(4) all ready */
    sub     r2, r12, r14              @ e2 = W6sum - CW2pW6*d6

    /* ---- STAGE 3: even + 181 rotation ---- */
    smlabb  r3, r0, r3, r12           @ e3 = W6sum + CW2mW6*d2      [+2]
    add     r12, r4, r5               @ sum45                       [1]
    sub     r4, r4, r5                @ dif45                       [2]
    ldmia   sp!, {r0, r14}            @ r0=e0, r14=e8               [3]
    nop                                @ (add r12 → mul r12 early-read gap 4)
    mul     r12, r1, r12              @ 181*sum45                   [+3]
    nop                                @ (ldm gap 2; smlabb r3 gap 2 ok)
    /* r0, r14, r3 all ready */
    add     r7, r14, r3               @ x7 = e8 + e3
    sub     r14, r14, r3              @ x8 = e8 - e3
    add     r3, r0, r2                @ x3 = e0 + e2
    sub     r0, r0, r2                @ x0 = e0 - e2
    /* r12 ready = 181*sum45 (5 gap from mul) */
    add     r12, r12, #128
    mul     r4, r1, r4                @ 181*dif45 (fills asr r12 early-read gap 1) [+3]
    asr     r5, r12, #8              @ x2 → r5 (r12 gap 1 ✅)
    ldmia   sp!, {r1, r12}            @ r1=ws_ptr r12=counter       [1]
    nop                                @                              [2]
    nop                                @                              [3]
    /* r4 ready = 181*dif45 */
    add     r4, r4, #128
    add     r2, r7, r8                @ x7+x1 start ws[0] (fills asr r4 early gap 1)
    asr     r4, r4, #8                @ x4 (r4 gap 1 ✅)

    /*
     * Final values:
     *   r7=x7  r14=x8  r3=x3  r0=x0
     *   r8=x1  r5=x2   r4=x4  r6=x6
     *
     * row[N] = (xA±xB)>>8  stored to workspace
     *
     * asr Rd,Rm,#imm reads Rm 1 early (shifted-reg operand)
     *   → add Rd,... then asr Rd,Rd,#8 needs gap 1
     * Interleave paired rows to fill the gap.
     */

    /* ws[0] = (x7+x1)>>8, ws[7] = (x7-x1)>>8 (r2 already = x7+x1) */
    sub     r7, r7, r8                @ (gap 1 for r2)
    asr     r2, r2, #8
    asr     r7, r7, #8                @ (gap 1 for r7)
    str     r2, [r1, #0]              @ ws[0]
    str     r7, [r1, #28]             @ ws[7]

    /* ws[1] = (x3+x2)>>8, ws[6] = (x3-x2)>>8 */
    add     r2, r3, r5
    sub     r3, r3, r5                @ (gap 1 for r2)
    asr     r2, r2, #8
    asr     r3, r3, #8                @ (gap 1 for r3)
    str     r2, [r1, #4]              @ ws[1]
    str     r3, [r1, #24]             @ ws[6]

    /* ws[2] = (x0+x4)>>8, ws[5] = (x0-x4)>>8 */
    add     r2, r0, r4
    sub     r0, r0, r4                @ (gap 1 for r2)
    asr     r2, r2, #8
    asr     r0, r0, #8                @ (gap 1 for r0)
    str     r2, [r1, #8]              @ ws[2]
    str     r0, [r1, #20]             @ ws[5]

    /* ws[3] = (x8+x6)>>8, ws[4] = (x8-x6)>>8 */
    add     r2, r14, r6
    sub     r14, r14, r6              @ (gap 1 for r2)
    asr     r2, r2, #8
    asr     r14, r14, #8              @ (gap 1 for r14)
    str     r2, [r1, #12]             @ ws[3]
    str     r14, [r1, #16]            @ ws[4]

    mov     r0, r1                     @ restore ws_ptr → r0
    mov     r8, r12                    @ restore counter → r8
    b       .Lrow_next

.Lrow_dc:
    /* DC-only: fill 8 int32 with d0<<3 */
    lsl     r2, r12, #3
    mov     r3, r2
    strd    r2, r3, [r0, #0]           @ ws[0],ws[1]
    strd    r2, r3, [r0, #8]           @ ws[2],ws[3]
    strd    r2, r3, [r0, #16]          @ ws[4],ws[5]
    strd    r2, r3, [r0, #24]          @ ws[6],ws[7]

.Lrow_next:
    add     r10, r10, #16             @ next int16 row
    add     r0, r0, #32              @ next int32 workspace row
    subs    r8, r8, #1
    bne     .Lrow_loop

    /* ============================================================= */
    /*  COLUMN PASS: 8 columns, int32 -> uint8                       */
    /* ============================================================= */
    /* r10 = workspace base = sp */
    /* r11 = output base (preserved from entry) */
    mov     r10, sp
    mov     r0, #0                     @ column offset (bytes: 0,4,...,28)

.Lcol_loop:
    /* Load 8 column values (int32, row stride = 32 bytes) */
    add     r12, r10, r0              @ r12 = workspace + col_off
    ldr     r4, [r12], #32            @ d0 (row 0)
    ldr     r5, [r12], #32            @ d1 (row 1)
    ldr     r14, [r12], #32           @ d2 (row 2)
    ldr     r7, [r12], #32            @ d3 (row 3)
    ldr     r1, [r12], #32            @ d4 (row 4)
    ldr     r6, [r12], #32            @ d5 (row 5)
    ldr     r2, [r12], #32            @ d6 (row 6)
    ldr     r3, [r12]                 @ d7 (row 7)

    /* Rearrange to: r0_=d0 r1=d4 r2=d6 r3_=d2 r4_=d1 r5_=d7 r6=d5 r7=d3 */
    /* Currently: r4=d0 r5=d1 r14=d2 r7=d3 r1=d4 r6=d5 r2=d6 r3=d7 */
    /* Save col offset before clobbering r0 */
    stmdb   sp!, {r0, r11}            @ save {col_off, output_base}

    mov     r0, r4                     @ r0 = d0
    mov     r4, r5                     @ r4 = d1
    mov     r5, r3                     @ r5 = d7
    mov     r3, r14                    @ r3 = d2
    /* r1=d4, r2=d6, r6=d5, r7=d3 — already correct */

    /* Zero-AC shortcut */
    orr     r12, r4, r5
    orr     r14, r6, r7
    orr     r12, r12, r1
    orr     r14, r14, r2
    orr     r12, r12, r3
    orrs    r12, r12, r14
    beq     .Lcol_dc

    /* r0=d0 r1=d4 r2=d6 r3=d2 r4=d1 r5=d7 r6=d5 r7=d3 */

    /* --- Even setup --- */
    lsl     r0, r0, #8                @ x0 = d0 << 8
    add     r0, r0, #8192             @ x0 += 8192 (rounding)
    lsl     r1, r1, #8                @ x1 = d4 << 8
    add     r14, r0, r1               @ e8
    sub     r0, r0, r1                @ e0
    stmdb   sp!, {r0, r14}            @ push {e0, e8}

    /* --- W7 butterfly --- */
    add     r8, r4, r5                @ sum17
    ldrd    r0, r1, [r9, #0]          @ r0=CW7, r1=CW1mW7
    nop                                @ (LDRD gap 1)
    nop                                @ (LDRD gap 2)
    nop                                @ (LDRD→MUL early-read gap 3)
    mul     r12, r0, r8               @ W7sum                       [+3]
    add     r8, r6, r7                @ sum53                        [1]
    nop                                @                              [2]
    nop                                @                              [3]
    add     r12, r12, #4              @ W7s = W7sum + 4              (MUL→mla early gap 4)

    mla     r4, r1, r4, r12           @ o4_pre                      [+3]
    ldrd    r0, r1, [r9, #8]          @ r0=CW1pW7, r1=CW3          [1]
    nop                                @                              [2]
    nop                                @ (mla gap 3; LDRD→MUL early gap 3) [3]
    nop                                @ (mla→asr early-read gap 4)
    asr     r4, r4, #3                @ o4 = o4_pre >> 3

    mul     r14, r0, r5               @ CW1pW7*d7                   [+3]
    nop                                @                              [1]
    nop                                @                              [2]
    nop                                @                              [3]
    sub     r5, r12, r14              @ o5_pre = W7s - CW1pW7*d7
    nop                                @ (sub→asr early-read gap 1)
    asr     r5, r5, #3                @ o5

    /* Live: r4=o4 r5=o5 r6=d5 r7=d3 r2=d6 r3=d2 r1=CW3 r8=sum53 */

    /* --- W3 butterfly --- */
    mul     r12, r1, r8               @ W3sum                       [+3]
    ldrd    r0, r1, [r9, #16]         @ r0=CW3mW5, r1=CW3pW5      [1]
    nop                                @                              [2]
    nop                                @ (MUL gap 3; LDRD gap 2)     [3]
    nop                                @ (LDRD→MUL early-read gap 3)
    add     r12, r12, #4              @ W3s

    mul     r14, r0, r6               @ CW3mW5*d5                   [+3]
    nop                                @                              [1]
    nop                                @                              [2]
    nop                                @                              [3]
    sub     r6, r12, r14              @ o6_pre
    nop                                @ (sub→asr early-read gap 1)
    asr     r6, r6, #3                @ o6

    mul     r14, r1, r7               @ CW3pW5*d3                   [+3]
    add     r8, r3, r2                @ sum26 (for W6)               [1]
    nop                                @                              [2]
    nop                                @                              [3]
    sub     r7, r12, r14              @ o7_pre
    nop                                @ (sub→asr early-read gap 1)
    asr     r7, r7, #3                @ o7
    ldrd    r0, r1, [r9, #24]         @ r0=CW6, r1=CW2pW6

    /* --- W6 butterfly --- */
    nop                                @ (LDRD gap 1)
    nop                                @ (LDRD gap 2)
    nop                                @ (LDRD→MUL early-read gap 3)
    mul     r12, r0, r8               @ W6sum                       [+3]
    nop                                @                              [1]
    nop                                @                              [2]
    nop                                @                              [3]
    add     r12, r12, #4              @ W6s

    mul     r14, r1, r2               @ CW2pW6*d6                   [+3]
    ldrd    r0, r1, [r9, #32]         @ r0=CW2mW6, r1=C181         [1]
    nop                                @                              [2]
    nop                                @                              [3]
    sub     r2, r12, r14              @ e2_pre
    nop                                @ (sub→asr early-read gap 1)
    asr     r2, r2, #3                @ e2

    mla     r3, r0, r3, r12           @ e3_pre                      [+3]
    add     r8, r4, r6                @ x1 = o4+o6                  [1]
    sub     r4, r4, r6                @ o4'                         [2]
    add     r6, r5, r7                @ x6 = o5+o7                  [3]
    nop                                @ (mla→asr early-read gap 4)
    asr     r3, r3, #3                @ e3
    sub     r5, r5, r7                @ o5'

    /* --- 181 rotation --- */
    add     r0, r4, r5                @ sum45
    sub     r4, r4, r5                @ dif45
    /* r1=C181 already loaded by LDRD above */
    nop                                @ gap for r1
    mul     r0, r1, r0                @ 181*sum45                   [+3]
    ldmia   sp!, {r5, r14}            @ r5=e0 r14=e8                [1]
    nop                                @                              [2]
    add     r7, r14, r3               @ x7 = e8+e3                  [3]
    add     r0, r0, #128
    sub     r14, r14, r3              @ x8'= e8-e3 (fills asr r0 early gap 1)
    asr     r0, r0, #8                @ x2 (r0 gap 1 ✅)

    mul     r4, r1, r4                @ 181*dif45                   [+3]
    add     r3, r5, r2                @ x3 = e0+e2                  [1]
    sub     r5, r5, r2                @ x0'= e0-e2                  [2]
    nop                                @                              [3]
    add     r4, r4, #128
    ldmia   sp!, {r2, r12}            @ r2=col_off r12=output_base  (fills asr r4 early gap 1)
    asr     r4, r4, #8                @ x4 (r4 gap 1 ✅)

    /*
     * r7=x7 r14=x8' r3=x3 r5=x0' r8=x1 r0=x2 r4=x4 r6=x6
     * r2=col_off (byte offset into ws row = pixel column index)
     * r12=output_base
     * pixel col = r2>>2 (since workspace stores 4 bytes per int32)
     *
     * usat Rd,#imm,Rm: result gap 1, Rm read 1 early
     *   → ALU→usat(Rm) needs gap 1; usat→consumer(Rd) needs gap 1
     * Strategy: interleave paired rows (N and 7-N) so the paired
     * sub/asr/add fills both usat gaps.  Zero nops needed.
     */

    /* Start row 0 computation early to fill ldmia result gap 2 */
    add     r11, r7, r8               @ x7+x1 (ldmia gap 1)
    sub     r7, r7, r8                @ x7-x1 for row 7 (ldmia gap 2; r8 free)
    nop                                @ (ldmia→asr r2 early-read gap 3)
    asr     r1, r2, #2                @ pixel column (r2 from ldmia gap 3 ✅)
    add     r2, r12, r1               @ r2 = output_base + pixel_col (r12 ldmia gap 3 ✅)
    ldr     r12, [sp, #256]           @ stride

    /* Output 8 pixels: ((val >> 14) + 128) clamped 0..255 */
    /* r2=output+pixel_col, post-indexed writeback advances by stride */

    /* Row 0 — precompute row 7 (x7-x1) to fill usat gaps */
    asr     r11, r11, #14
    asr     r7, r7, #14               @ row 7 prep
    add     r11, r11, #128
    add     r7, r7, #128              @ row 7 prep (ALU→usat Rm early gap 1)
    usat    r11, #8, r11              @ (usat result gap 1)
    add     r8, r3, r0                @ x3+x2 start row 1 (fills usat gap)
    strb    r11, [r2], r12            @ row 0, r2 += stride

    /* Row 1 — precompute row 6 (x3-x2) */
    sub     r3, r3, r0                @ x3-x2 for row 6 (r0 free)
    asr     r8, r8, #14
    asr     r3, r3, #14               @ row 6 prep
    add     r8, r8, #128
    add     r3, r3, #128              @ row 6 prep (ALU→usat gap 1)
    usat    r8, #8, r8                @ (usat result gap 1)
    add     r11, r5, r4               @ x0'+x4 start row 2 (fills usat gap)
    strb    r8, [r2], r12             @ row 1, r2 += stride

    /* Row 2 — precompute row 5 (x0'-x4) */
    sub     r5, r5, r4                @ x0'-x4 for row 5 (r4 free)
    asr     r11, r11, #14
    asr     r5, r5, #14               @ row 5 prep
    add     r11, r11, #128
    add     r5, r5, #128              @ row 5 prep (ALU→usat gap 1)
    usat    r11, #8, r11              @ (usat result gap 1)
    add     r8, r14, r6               @ x8'+x6 start row 3 (fills usat gap)
    strb    r11, [r2], r12            @ row 2, r2 += stride

    /* Row 3 — precompute row 4 (x8'-x6) and clamp together */
    sub     r14, r14, r6              @ x8'-x6 for row 4 (r6 free)
    asr     r8, r8, #14
    asr     r14, r14, #14             @ row 4 prep
    add     r8, r8, #128
    add     r14, r14, #128            @ row 4 prep (ALU→usat gap 1 for r8)
    usat    r8, #8, r8                @ (usat result gap 1)
    usat    r14, #8, r14              @ row 4 clamp (fills r8 usat gap; ALU→usat gap 1 ✅)
    strb    r8, [r2], r12             @ row 3 (r8 from usat gap 1 ✅), r2 += stride

    /* Rows 4-5 (precomputed: r14=row4 clamped, r5=row5 +128) */
    usat    r5, #8, r5                @ row 5 clamp (fills r14 usat gap 1)
    strb    r14, [r2], r12            @ row 4 (r14 from usat gap 2 ✅), r2 += stride
    strb    r5, [r2], r12             @ row 5 (r5 from usat gap 2 ✅), r2 += stride

    /* Rows 6-7 (precomputed: r3=row6 +128, r7=row7 +128) */
    usat    r3, #8, r3                @ row 6 clamp
    usat    r7, #8, r7                @ row 7 clamp (fills r3 usat gap 1)
    strb    r3, [r2], r12             @ row 6 (r3 from usat gap 1 ✅), r2 += stride
    strb    r7, [r2]                  @ row 7 (r7 from usat gap 2 ✅)

    b       .Lcol_next

.Lcol_dc:
    /* DC-only column: (d0+32)>>6 + 128 clamped */
    add     r0, r0, #32
    ldmia   sp!, {r12, r14}           @ r12=col_off r14=output_base
    asr     r0, r0, #6                @ (ldmia gap 1)
    add     r0, r0, #128              @ (ldmia gap 2)
    nop                                @ (ldmia→asr r12 early-read gap 3)
    asr     r1, r12, #2               @ pixel column (r12 from ldmia gap 3 ✅)
    usat    r0, #8, r0                @ (ALU→usat Rm early gap 1 ✅)
    ldr     r12, [sp, #256]           @ stride
    add     r14, r14, r1              @ r14 = output_base + pixel_col (LDR gap 1)
    strb    r0, [r14], r12            @ row 0 (r0 usat gap 2 ✅; r12 LDR gap 2 ✅)
    strb    r0, [r14], r12            @ row 1
    strb    r0, [r14], r12            @ row 2
    strb    r0, [r14], r12            @ row 3
    strb    r0, [r14], r12            @ row 4
    strb    r0, [r14], r12            @ row 5
    strb    r0, [r14], r12            @ row 6
    strb    r0, [r14]                 @ row 7

.Lcol_next:
    /* Recover col_off: r1 = pixel_col, so col_off = r1 << 2 */
    lsl     r0, r1, #2                @ col_off = pixel_col * 4
    /* Reload r11 (output base) from saved frame */
    ldr     r11, [sp, #288]           @ output base from saved r11

    add     r0, r0, #4                @ next column
    cmp     r0, #32
    blt     .Lcol_loop

    /* Epilogue */
    add     sp, sp, #260              @ deallocate workspace + stride slot
    ldmia   sp!, {r4-r11, pc}

    .endfunc
    .size   jpeg_idct_ifast, . - jpeg_idct_ifast
